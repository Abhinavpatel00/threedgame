# Toon Shader Integration Plan (vk_toon_shader → threedgame)

Status: Draft v1 (March 2026)

## Goal

Integrate a toon/contour look inspired by `vk_toon_shader` into the existing renderer with minimal disruption to current architecture:

- keep the **single descriptor set layout** and **single pipeline layout**
- keep **bindless resource indexing**
- keep **offset/push-constant based draw path** (no per-draw VB/IB binding API changes)
- preserve current frame flow and SMAA/postprocess where possible

This plan targets an incremental rollout (MVP first, then quality upgrades).

---

## What to Reuse from `vk_toon_shader`

From the reference implementation, we only need the technique, not the framework:

1. Write an auxiliary per-pixel buffer containing:
   - encoded normal (2 channels)
   - depth metric
   - object ID
2. Run screen-space contour extraction using neighborhood comparisons:
   - object-ID contour pass
   - normal+depth contour pass
3. Composite contour result over the lit color buffer
4. Optionally anti-alias contour output (you already have SMAA)

---

## Current Engine Mapping

### Existing relevant flow

Current frame sequence in `main.c`:

1. Main 3D render (to `hdr_color` + `depth`)
2. `post_pass()` (HDR → LDR tonemap compute)
3. `pass_smaa()`
4. `pass_ldr_to_swapchain()`
5. `pass_imgui()`

### Existing systems we can leverage

- `RenderTarget` arrays per swapchain image (`vk.h` / `vk.c`)
- fullscreen graphics pass pattern (`pass_smaa()`)
- compute postprocess pass pattern (`post_pass()`)
- shared push constants through `renderer.bindless_system.pipeline_layout`
- Slang compilation flow from `shaders/*.slang` via `compileslang.sh`

---

## Integration Strategy (Phased)

## Phase 1 — MVP Data Path (single extra MRT + object contour)

Objective: Get visible toon outline quickly with lowest risk.

### 1. Add toon intermediate targets

Add per-frame render targets:

- `toon_data` (recommended `VK_FORMAT_R16G16B16A16_SFLOAT` or `R32G32B32A32_SFLOAT`)
  - stores: encoded normal.xy, depthMetric.z, objectIdAsFloat.w
- `toon_edges` (recommended `VK_FORMAT_R8_UNORM`)
  - stores contour mask/intensity

Touchpoints:

- `vk.h` renderer struct (new render target arrays + optional pipeline IDs)
- `vk.c` render target specs + `rt_create` + destroy/recreate path

### 2. Extend main mesh shader output to write toon data

Update `shaders/gltf_uber.slang`:

- keep `SV_Target0` as lit color
- add `SV_Target1` as encoded toon data
- pass stable object ID per draw (e.g. from draw index or explicit draw field)

Data encoding recommendation for MVP:

- normal.xy: compact encoded world-space normal
- depthMetric.z: linear eye/world depth (avoid min/max compute in MVP)
- objectID.w: `asfloat(object_id + 1)` (`0` reserved for background)

Touchpoints:

- `shaders/gltf_uber.slang`
- `main.c` (`GltfIndirectDrawData` may gain `object_id` for stability)
- `vk.c` glTF pipeline config (`color_attachment_count = 2`, formats `[hdr, toon_data]`)

### 3. Add fullscreen contour pass (object ID only first)

Create a new fullscreen graphics pass that samples `toon_data` and writes `toon_edges`.

- start with object-ID edge test (neighbor compare)
- expose one mode first: thick contour (`!=` in 8-neighborhood)

Touchpoints:

- new shader: `shaders/toon_contour_obj.slang`
- `vk.c` pipeline creation
- `passes.c` new function: `pass_toon_contour()`
- `passes.h` declaration

### 4. Add simple compositing pass

Blend contour mask onto `ldr_color` (after tonemap, before SMAA).

- black line with configurable strength (`line_color`, `line_strength`)
- optionally skip if feature disabled

Touchpoints:

- new shader: `shaders/toon_composite.slang`
- `vk.c` pipeline creation
- `passes.c` new function: `pass_toon_composite()`
- call order in `main.c`

### 5. Frame order update (MVP)

Proposed order:

1. Main render (HDR + toon_data + depth)
2. `post_pass()` HDR→LDR
3. `pass_toon_contour()` toon_data→toon_edges
4. `pass_toon_composite()` (ldr + toon_edges → ldr)
5. `pass_smaa()`
6. `pass_ldr_to_swapchain()`
7. `pass_imgui()`

---

## Phase 2 — Quality Upgrade (normal+depth contour fusion)

Objective: Add intra-object creases and depth discontinuity lines.

### 1. Add normal+depth contour mode

Implement second contour shader path inspired by `contour_normaldepth.frag`:

- Sobel/Kroon-like normal gradient
- depth gradient term
- weighted fusion with tunables:
  - `normal_diff_coeff`
  - `depth_diff_coeff`

### 2. Depth normalization options

Choose one of these:

- **Option A (recommended initially):** use linear depth metric written in main pass; no min/max compute pass
- **Option B (later):** add compute min/max pass like `depthminmax.comp` for scene-adaptive depth scaling

If Option B:

- add tiny per-frame storage buffer for min/max
- add compute pipeline + dispatch before contour pass
- reset min/max each frame

### 3. Combine object and normal-depth edges

Either:

- single shader output with both contributions, or
- two masks merged in composite pass

Keep first merge simple: `edge = saturate(objEdge + ndEdge)`.

---

## Phase 3 — Controls, Debug, and Hardening

Objective: Make feature tunable and production-safe.

### Runtime controls (ImGui)

Add controls:

- enable toon outlines
- outline thickness/method
- line strength/color
- normal/depth contribution sliders
- optional debug views (`toon_data`, `toon_edges`)

### Robustness checklist

- swapchain resize: recreate toon render targets
- barriers/layout transitions for all new passes
- disable path fallback when resources unavailable
- verify no pipeline layout divergence

### Performance checklist

- profile with `GPU_SCOPE`
- compare cost at 1080p/1440p
- if needed, run contour at half-resolution then upscale

---

## Push Constant and ABI Notes

- Keep shared push constant contract stable across all pipelines.
- Add toon pass push structs in the same style as existing pass push constants (`passes.c`).
- Do not introduce per-pipeline descriptor set layouts.
- Continue passing buffer slice addresses/offset-derived pointers via push constants as currently done.

---

## File-Level Execution Plan

1. `vk.h`
   - add toon render targets and toon pipeline IDs to `Renderer`

2. `vk.c`
   - add `RenderTargetSpec` for `toon_data` and `toon_edges`
   - create/destroy/recreate them per swapchain image
   - create contour/composite pipelines
   - adjust glTF main pipeline color attachment formats/count

3. `shaders/gltf_uber.slang`
   - output second RT for encoded toon data

4. `shaders/toon_contour_obj.slang` (new)
   - object-ID edge extraction fullscreen fragment shader

5. `shaders/toon_contour_nd.slang` (new, phase 2)
   - normal+depth gradient extraction

6. `shaders/toon_composite.slang` (new)
   - blend contour mask into LDR

7. `passes.h` / `passes.c`
   - add new toon pass entry points and recording logic

8. `main.c`
   - update frame pass order
   - add optional toggles in debug UI

9. `compileslang.sh`
   - no script logic change expected (auto-compiles new `.slang` files)

---

## Validation Plan

## Functional

- outlines appear around object silhouettes in model grid
- no regressions in existing SMAA and screenshot path
- resize, alt-tab, and swapchain recreation remain stable

## Visual

- compare before/after captures:
  - baseline (no toon)
  - object contour only
  - object + normal/depth contour
- verify distant background is not over-outlined

## Technical

- no new descriptor set layout or pipeline layout variants
- push constant sizes remain within limits
- no validation errors for image layout transitions

---

## Suggested Delivery Slices

### Slice A (1 session)

- `toon_data` + `toon_edges` targets
- glTF writes toon_data MRT
- object contour pass + composite pass
- frame order integration

### Slice B (1 session)

- normal+depth contour path
- tuning UI controls

### Slice C (optional)

- depth min/max compute normalization
- optimization pass (half-res or kernel tuning)

---

## Done Criteria

Feature is considered integrated when:

- toon outlines can be toggled at runtime
- at least object-ID contour works on all loaded GLTFs
- no architecture rule is violated (single layout, bindless, offset/push-constant path)
- frame remains stable under resize and normal camera movement
