# Color Grading Support Plan

## Objective

Add runtime color grading to the post-process path in two stages:

1. Matrix grading MVP (no new asset pipeline dependency).
2. LUT grading follow-up (artist-authored looks).

The implementation must follow renderer constraints:

- Single descriptor set layout and single pipeline layout (bindless model).
- Per-frame data comes from suballocated GPU buffer slices.
- Slice offsets are provided through push constants.

---

## Scope

### In Scope

- Color grading in post-process compute shader.
- Runtime toggles/intensity and debug-visible active mode.
- Matrix grading first, LUT grading second.

### Out of Scope (for this plan)

- Full OCIO/ACES color-management workflow.
- LUT authoring tools or editor UI work.
- Per-object/per-cluster grading.

---

## Pipeline Order (Required)

Execute in this order:

1. Exposure
2. Tone map
3. Color grade (matrix or LUT)
4. Output transform / gamma

Reason: grading in display-referred space is predictable for look tuning.

---

## Technical Design

## Modes

- `0 = off`
- `1 = matrix`
- `2 = lut`

## Matrix mode fields

- `float3x3 grade_matrix`
- `float3 grade_offset`
- `float grade_intensity` (`[0,1]`)

Behavior:

- `graded = grade_matrix * color + grade_offset`
- `final = lerp(color, graded, grade_intensity)`

## LUT mode fields

- `uint lut_texture_id` (bindless id)
- `float lut_intensity` (`[0,1]`)

Behavior:

- Sample LUT with proper addressing.
- Blend result by `lut_intensity`.
- Fallback to matrix or off if LUT id is invalid.

---

## File Touchpoints

- `shaders/postprocess.comp.slang`
  - Add grading functions and mode switch.
  - Insert grading stage after tonemap.
- `slangtypes.h` (or existing shared CPU/GPU postprocess parameter definition)
  - Add grading parameters with explicit alignment/padding.
- `vk.c` / `main.c` (where postprocess params are populated)
  - Write grading fields into existing suballocated slice.
  - Preserve push-constant offset/range flow.
- `docs/rendering.md` (or nearest rendering notes)
  - Document controls, defaults, and mode semantics.

---

## Phase Plan

## Phase 1 — Data Plumbing

1. Extend shared postprocess parameter struct.
2. Verify C/Slang ABI compatibility (`sizeof`, offsets, 16-byte alignment).
3. Keep descriptor set/pipeline layout unchanged.

### Exit Criteria

- Shader reads valid default grading fields with mode `off`.

## Phase 2 — Matrix MVP

1. Add matrix grading function in `postprocess.comp.slang`.
2. Apply grading in required pipeline order.
3. Clamp/saturate final values to prevent invalid output.

### Exit Criteria

- Matrix grading visible in-frame and smoothly blendable via intensity.

## Phase 3 — Runtime Controls

1. Expose mode + intensity + matrix preset selection.
2. Add debug text/log showing active mode and key values.
3. Set neutral defaults to preserve baseline look.

### Exit Criteria

- User can toggle and tune grading live with no shader rebuild.

## Phase 4 — LUT Path

1. Load LUT texture using existing texture upload path.
2. Acquire/store bindless texture id.
3. Add LUT sampling branch in postprocess shader.
4. Add safe fallback when LUT is not available.

### Exit Criteria

- LUT mode works at runtime and falls back safely when invalid.

---

## Validation Checklist

- Disabled mode matches baseline output (visual parity).
- Matrix mode produces expected tint/contrast behavior.
- LUT mode shows no seams/precision artifacts with test LUT ramps.
- Validation layers remain clean.
- No new descriptor set layouts or pipeline layouts introduced.
- Push-constant slice offsets still match allocated ranges.

---

## Risks and Mitigations

- Struct mismatch between C and Slang.
  - Mitigate with shared definitions + offset/size assertions.
- Wrong pass order causing clipping/washed output.
  - Mitigate by keeping grade call in one documented post chain location.
- LUT sampling artifacts.
  - Mitigate with correct coordinate mapping and filtering tests.

---

## Milestones

- M1: Matrix mode integrated and runtime-toggleable.
- M2: Baseline parity confirmed when grading is off.
- M3: LUT mode integrated with fallback path.
- M4: Documentation updated.

---

## Definition of Done

Color grading support is complete when:

- Matrix grading ships in default post-process path.
- LUT grading works with bindless texture id and safe fallback.
- Disabled mode preserves existing visual output.
- Implementation adheres to single-layout bindless architecture and offset-based buffer slicing.
