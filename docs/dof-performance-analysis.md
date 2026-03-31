# Depth of Field Performance Analysis

Date: 2026-03-31  
Workspace: `threedgame`

## Scope

This document analyzes why DOF is expensive in:

1. `BokehDepthOfField/src/29_DepthOfField/Shaders/Vulkan` (reference implementation)
2. Your current engine path (`shaders/postprocess.slang` + `passes.c`)

It is a code-path and complexity analysis (no live GPU capture attached here), with concrete bottlenecks and optimization priorities.

## Executive Summary

Main reasons DOF is slow in your project:

1. Your current DOF is a full-resolution gather blur in one compute pass with many depth+color fetches per blurred pixel.
2. DOF is hard-enabled with aggressive focus values, so a large screen area likely enters expensive blur.
3. The reference BokehDepthOfField Vulkan paths are also expensive by design (multi-pass + many fullscreen barriers + heavy kernels), but they amortize cost by doing core blur work at half resolution.
4. There are likely implementation issues in the reference Vulkan sample that can hurt quality/perf on non-square targets.

Highest-ROI fix for your engine: move DOF blur to half resolution with adaptive tap count.

## Part A: Your Current DOF (`shaders/postprocess.slang`)

### A.1 Pipeline location

- DOF executes inside `post_pass()` in `passes.c` after bloom.
- The compute dispatch is full swapchain size.
- DOF is always enabled and fused with bloom-combine + exposure + tone mapping.

Key settings from `passes.c`:

- `dof_enabled = 1`
- `dof_focus_depth = 0.020f`
- `dof_focus_range = 0.015f`
- `dof_max_blur_radius = 6.0f`

These settings are narrow enough that many pixels can be out-of-focus.

### A.2 Shader cost model

In `apply_dof()`:

- center sample: 1 color + 1 depth
- if blurred: loop with `TAP_COUNT = 16`
- each tap: 1 color + 1 depth + CoC math + depth weight math (`exp`)
- outside loop: bloom sample + tone mapping math

Approximate blurred-pixel read cost:

- DOF core: `1+1 + 16*(1+1) = 34` texture reads
- bloom sample: `+1`
- total: about `35` reads/pixel in blurred regions

Upper-bound read volume:

- 1080p (2.07M px): ~72M reads/frame in this pass when blur coverage is high
- 4K (8.29M px): ~290M reads/frame when blur coverage is high

### A.3 Why it spikes frame time

1. Full-resolution processing at final output size.
2. Depth+color dual fetch per tap.
3. Exponential weighting in inner loop.
4. Blur eligibility can be broad due to narrow focus range.
5. DOF, bloom add, and tone map are fused, so DOF cost is paid inside your final post pass every frame.

## Part B: `BokehDepthOfField` Vulkan Shader Analysis

Reference files:

- `BokehDepthOfField/src/29_DepthOfField/Shaders/Vulkan/genCoc.frag`
- `.../maxfilterNearCoC.frag`
- `.../boxfilterNearCoC.frag`
- `.../circular_dof/*`
- `.../gather_based_dof/*`
- `.../singlepass/dof.frag`
- pass graph wiring in `BokehDepthOfField/src/29_DepthOfField/DepthOfField.cpp`

### B.1 Frame graph summary

Common steps:

1. Full-res scene render to HDR
2. Full-res CoC generation (`genCoc.frag`)

Then one of three methods:

1. Circular DOF
2. Practical Gather Based
3. Single Pass

Default selection is `gSelectedMethod = 0` (Circular DOF).

### B.2 Circular DOF path cost profile

Passes (after CoC generation):

1. Downres to half-res (3 MRT)
2. Near CoC max X
3. Near CoC max Y
4. Near CoC box X
5. Near CoC box Y
6. Horizontal pass (7 MRT outputs)
7. Vertical + composite to swapchain

Performance characteristics:

- Many fullscreen passes and barriers.
- Near-CoC filtering alone is 4 passes with kernel radius 6 (13 taps/pass).
- Horizontal stage writes 7 targets in one pass (bandwidth heavy).
- Vertical stage does reconstruction with complex-kernel math and multiple source textures.

Why still feasible:

- Most heavy blur work is at half resolution.

### B.3 Practical Gather-Based path cost profile

Passes (after CoC generation):

1. Downres to half-res (3 MRT)
2. Near CoC max X
3. Near CoC max Y
4. Near CoC box X
5. Near CoC box Y
6. Computation pass (near/far gather, many taps)
7. Filling pass (3x3 morphological fill)
8. Composite pass

Hotspots:

- `computation.frag` has large static sampling sets (48 offsets + center for near and far paths).
- Worst-case per half-res pixel in computation pass is very high (both near and far branches active).

### B.4 Single-Pass path cost profile

`singlepass/dof.frag` uses a golden-angle spiral loop:

- `radius` starts at 1.5 and grows with `radius += RAD_SCALE / radius`.
- Loop runs until `radius >= 20`.
- Each iteration samples color + CoC.

This can result in very high per-pixel sample counts, and it runs full-res.

### B.5 Observed issues in reference Vulkan code

1. Near-CoC render target dimension bug:
- In `DepthOfField.cpp`, near-CoC filter target height is set from width.
- `rtDesc.mHeight = pRenderTargetCoCDowres[0]->mDesc.mWidth;`
- Expected: use `mHeight`.

2. Filter step uses only width for both axes:
- In `maxfilterNearCoC.frag` and `boxfilterNearCoC.frag`:
- `vec2 step = vec2(1.0f / textureSize(...).r);`
- Vertical pass should use independent x/y texel size, not width-derived scalar for both.

Impact:

- Can distort kernel footprint for non-square targets.
- Can introduce extra work and blur inconsistency.

## Part C: Comparative Analysis (Reference vs Your Shader)

### C.1 Why your current path is slow right now

Compared to the reference multi-pass methods, your path is simpler but currently expensive because:

1. It is full-res for all DOF work.
2. It performs per-tap depth fetch + CoC logic directly in the final pass.
3. It is always active with aggressive focus settings.

### C.2 Why reference path can still be expensive

Reference methods reduce blur resolution but add:

1. More passes
2. More barriers
3. More intermediate render targets
4. Complex kernels and reconstructions

So reference is not “cheap by default”; it is quality-focused and requires tuning.

## Part D: Prioritized Optimization Plan for Your Engine

### D.1 Immediate changes (highest ROI)

1. Half-res DOF path
- Generate half-res CoC and half-res color input.
- Run blur on half-res.
- Composite into full-res final pass.

2. Adaptive taps
- Use CoC/radius tiers, e.g. 4/8/12/16 taps.
- Skip expensive path for tiny CoC.

3. Runtime quality presets
- Low: half-res + 4/8 taps
- Medium: half-res + 8/12 taps
- High: half-res + 12/16 taps

4. Relax focus defaults
- Broaden focus range so fewer pixels trigger strong blur.

### D.2 Medium changes

1. CoC prepass texture
- Precompute CoC once and sample it in DOF pass.

2. Cheaper depth similarity
- Replace `exp()` in inner loop with cheaper approximation/LUT.

3. Split DOF from tone-map path
- Keep DOF in its own compute stage for easier profiling and quality scaling.

### D.3 If you want Bokeh-style quality

1. Port a half-res near/far split (like reference circular/gather design).
2. Keep full-res composite only.
3. Avoid porting full complexity first; start with minimal 3-pass half-res architecture.

## Part E: Measurement Plan

Use your existing GPU scopes and add sub-scope timing around DOF internals.

1. Baseline with current settings.
2. Toggle `dof_enabled` off to isolate delta.
3. Sweep `dof_max_blur_radius` (0, 2, 4, 6).
4. Sweep resolution (720p, 1080p, 1440p, 4K).
5. After half-res DOF, re-run same sweep.

Expected signature of DOF bottleneck:

- Cost scales approximately with pixel count and blur coverage.

## Conclusion

Your current DOF is expensive because it is a full-resolution gather blur with depth-aware weighting in the final post compute pass, and it is always enabled with aggressive focus parameters. The `BokehDepthOfField` Vulkan implementation is also expensive in raw work but contains an important design advantage: heavy blur stages are half-resolution. For your engine, the fastest practical win is half-res DOF + adaptive taps + less aggressive focus defaults.
