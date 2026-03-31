# Depth of Field Performance Analysis

Date: 2026-03-31  
Workspace: `threedgame`

## Scope

This document analyzes why depth of field is expensive in two places in this workspace:

1. **Bevy post-process bokeh/gaussian DOF path** (`bevy/crates/bevy_post_process/src/dof/*`)
2. **Current engine DOF shader path** (`shaders/postprocess.slang` + `passes.c`)

The analysis is static (code-path and complexity driven), with practical performance estimates and optimization priorities.

---

## Executive Summary

Depth of field is expensive here mainly because:

- **Your current DOF is full-resolution gather blur** in a single compute pass.
- It does **many texture reads per pixel** (color + depth taps), plus expensive transcendental math (`exp`, `pow` via tonemapping, etc.).
- DOF is **always enabled** and uses aggressive focus settings, so many pixels likely take the expensive path.
- Bevy’s bokeh path is visually better but also costly due to **multi-pass blur + variable kernel support**, and can become bandwidth-heavy when CoC grows.

If the goal is immediate speedup with minimal visual impact, the highest ROI is:

1. Move DOF to **half-resolution** (or variable resolution).
2. Build a **CoC prepass** and sample CoC instead of recomputing from depth repeatedly.
3. Add **adaptive tap count** + radius budget.
4. Keep bokeh quality for captures; use gaussian/adaptive for gameplay.

---

## Part A — Bevy `BokehDepthOfField` Path

### A.1 Pipeline structure and pass count

Relevant files:

- `bevy/crates/bevy_post_process/src/dof/dof.wgsl`
- `bevy/crates/bevy_post_process/src/dof/mod.rs`

Bevy implements two modes:

- **Gaussian mode**: 2 fullscreen passes
  - Horizontal blur
  - Vertical blur
- **Bokeh mode**: 2 fullscreen passes
  - Pass 0: vertical + diagonal (dual render target output)
  - Pass 1: diagonal + diagonal blend (dual input)

This is well-structured and renderer-friendly, but bokeh can still be expensive when CoC is large.

### A.2 Why bokeh can be expensive

In `dof.wgsl`, each pass computes CoC from depth and then executes directional box blurs with support:

- `support = round(coc * 0.5)`
- Loop runs from `0..support`

For large CoC, sample count scales linearly per direction.

For max CoC diameter = 64 (default in `DepthOfField::default`):

- `support ≈ 32` → **33 samples per directional blur**
- Bokeh pass 0 has 2 directional blurs → ~66 color samples
- Bokeh pass 1 has 2 directional blurs → ~66 color samples
- Combined bokeh path: **~132 color samples/pixel** + depth/C0 overhead

Even with texture cache locality, this is substantial memory bandwidth and texture-unit pressure.

### A.3 Why gaussian is cheaper than bokeh

`gaussian_blur.wgsl` uses a bilinear sampling trick (pairing adjacent taps), lowering fetch count for equivalent support.
Still not free, but generally cheaper than bokeh and easier to scale.

### A.4 Bevy-side observations

Strengths:

- Good pipeline specialization (`MULTISAMPLED`, `DUAL_INPUT`)
- Explicit auxiliary RT allocation only for bokeh
- Clear pass scheduling between bloom and tonemap

Costs:

- Fullscreen multi-pass bandwidth
- Per-pixel variable loop lengths
- Repeated depth/CoC computations across passes

---

## Part B — Current Engine DOF (`postprocess.slang`)

### B.1 Where cost happens

Relevant files:

- `shaders/postprocess.slang`
- `passes.c` (`post_pass`)

Current behavior:

- Full-resolution compute dispatch (`numthreads(16,16,1)` over full swapchain)
- DOF is enabled in push constants (`dof_enabled = 1`)
- Blur performed in `apply_dof()` with Poisson taps

### B.2 Per-pixel operation cost

In heavy-blur regions (`radius >= 0.75`), per pixel does:

- Center reads:
  - `1x` color read
  - `1x` depth read
- Loop (`TAP_COUNT = 16`):
  - `16x` color reads
  - `16x` depth reads
  - math: `exp`, lerp, abs, max, smoothstep path
- Post DOF:
  - `1x` bloom sample
  - exposure and ACES tonemap math
  - output store

Approx total texture reads/pixel in blurred areas:

- **35 reads/pixel** (17 color + 17 depth + 1 bloom)

At 1920×1080 (~2.07M pixels), worst-case upper bound is:

- ~72M texture reads/frame from this pass alone

At 3840×2160 (~8.29M pixels):

- ~290M texture reads/frame

These are worst-case approximations, but they explain the frame cost trend very well.

### B.3 Current parameter risk

In `passes.c`, DOF push values are currently fixed and aggressive:

- `dof_focus_depth = 0.020`
- `dof_focus_range = 0.015`
- `dof_max_blur_radius = 6.0`

With this narrow focus range, many pixels are likely out-of-focus, triggering expensive blur frequently.

### B.4 Additional pipeline-level costs

- `pass_bloom()` runs before `post_pass()`, so DOF runs after bloom down/up chain cost.
- Several full-image transitions/barriers around postprocess stages increase scheduling and memory pressure.
- DOF, bloom combine, tonemap are fused in one compute shader, which is convenient but reduces fine-grained quality/perf tuning.

---

## Part C — Why it is slow (ranked bottlenecks)

1. **Full-res gather DOF** in `postprocess.slang` with 16 taps and depth-tested weighting.
2. **Depth + color dual-sampling per tap**, causing high memory traffic.
3. **Transcendental math** in inner loops (`exp`) and per-pixel tone mapping.
4. **Always-on DOF** with narrow focus range (high blur coverage).
5. **No CoC prepass / no tile culling / no dynamic quality scaling**.

---

## Part D — Optimization Plan (highest impact first)

## D.1 Immediate wins (low risk)

1. **Half-resolution DOF buffer**
   - Run DOF on half-res color + half-res depth/CoC.
   - Upsample/composite into final post pass.
   - Typical gain: very large (often 2–4x for DOF stage).

2. **Adaptive tap budget**
   - Reduce `TAP_COUNT` based on CoC or a quality tier.
   - Example: CoC small → 4 taps, medium → 8 taps, large → 12/16.

3. **Runtime toggle/quality controls**
   - Expose DOF enable + quality in UI.
   - Avoid hardcoded always-on settings in shipping path.

## D.2 Medium effort

4. **CoC prepass texture**
   - Precompute CoC once (preferably half-res), sample CoC in blur shader.
   - Removes repeated CoC math from every depth tap path.

5. **Replace/approximate expensive `exp` weighting**
   - Use cheaper polynomial/LUT approximation for depth similarity.
   - Keeps edge behavior while cutting ALU cost.

6. **Far/near split with cheaper far blur**
   - Separate near and far fields and process differently.
   - Far field often tolerates more aggressive downsample.

## D.3 Advanced

7. **Tile classification / foveated DOF work skipping**
   - If max CoC in tile is below threshold, skip expensive kernel.

8. **Temporal accumulation for DOF**
   - Fewer taps per frame, accumulate over frames with reprojection.

9. **Dedicated bokeh pass graph (optional)**
   - Split bloom, DOF, tonemap into distinct quality-scalable stages.

---

## Part E — Suggested profiling methodology

You already have per-pass GPU scopes (`BLOOM`, `POST`, etc.). Use this process:

1. Baseline current scene with DOF on.
2. Disable DOF (`dof_enabled = 0`) and measure delta in `POST` pass.
3. Keep DOF on, set radius to 0/2/4/6 and collect slope.
4. Test 720p, 1080p, 1440p, 4K to verify pixel-scaling behavior.
5. Separate bloom from DOF in timings (optional temporary split shader or marker).

Expected signature of a fill/bandwidth-limited DOF:

- Cost scales near-linearly with pixel count and blur coverage.

---

## Part F — Recommended target architecture for this project

Given your existing bindless + push-constant model, practical next architecture:

1. **Pass 1**: CoC generation at half-res (R16F or R8 where acceptable)
2. **Pass 2**: Half-res DOF blur (adaptive taps)
3. **Pass 3**: Composite DOF + HDR + bloom
4. **Pass 4**: Tonemap (or keep tonemap fused with composite)

This keeps your current renderer style and gives clear quality/perf knobs.

---

## Quick Conclusion

Your current DOF is expensive because it is a full-resolution gather blur with many depth/color reads and expensive weighting math per pixel. Bevy’s bokeh implementation is also inherently costly due to variable-support multi-pass blur, but your current shader is likely the larger immediate hotspot in this project because it is always-on and full-res in one pass. Moving DOF to half-res + adaptive tap count will produce the biggest near-term win.
