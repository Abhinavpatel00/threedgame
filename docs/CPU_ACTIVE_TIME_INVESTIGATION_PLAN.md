# CPU Active Time Investigation Plan (Target: ~8 ms active)

## Current Measurement Model

Your renderer computes:

- `cpu_frame_ns = now - cpu_prev_frame`
- `cpu_wait_ns = cpu_wait_accum_ns`
- `cpu_active_ns = cpu_frame_ns - cpu_wait_ns`

So `CPU active` is **everything on CPU that is not explicitly counted as waits**.

## What Was Added

Frame-loop Tracy zones were added in `main.c` for:

1. `Frame Loop`
2. `Hot Reload + Pipeline Rebuild`
3. `frame_start`
4. `World Streaming Update`
5. `Record Command Buffer`
6. `ImGui CPU`
7. `Submit + Present`

This should let you see which stage dominates the ~8 ms active time.

## Profiling Procedure

1. Build and run with Tracy enabled.
2. Capture at least 300 frames in Tracy while camera is static.
3. Sort CPU zones by self time and total time.
4. Repeat with camera moving to compare.
5. Repeat once with `voxel_debug = true` and once with `voxel_debug = false`.

## Likely Hotspots To Validate

- `Record Command Buffer` can be high due to many Vulkan calls and transitions every frame.
- `ImGui CPU` may become non-trivial with debug UI text and per-frame formatting.
- `frame_start` includes input polling, camera math, frustum extraction, fence wait/acquire paths.
- `World Streaming Update` can spike when chunk rebuild/mesh copy/flush happens.
- `Hot Reload + Pipeline Rebuild` is expensive if shader-change path is triggered.

## Decision Tree

- If `Record Command Buffer` is dominant:
  - Reduce per-frame state transitions and redundant binds.
  - Cache unchanged command sequences where possible.
- If `ImGui CPU` is dominant:
  - Lower debug UI update frequency (e.g. every N frames) for heavy text blocks.
- If `frame_start` is dominant:
  - Split input/camera/frustum into separate Tracy zones for finer granularity.
- If `World Streaming Update` is dominant:
  - Lower per-frame build budget or move more work off the main thread.
- If no single zone dominates:
  - Add sub-zones inside `Record Command Buffer` for each pass (main/post/SMAA/blit/imgui render).

## Success Criteria

- Identify top 1-2 CPU zones covering at least 70% of CPU active time.
- Reduce `cpu_active_ms` from ~8 ms to a chosen target by optimizing those zones.
- Confirm improvement over the same scene and camera path.
