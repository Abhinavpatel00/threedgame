# Copilot Instructions

## Core renderer model (must follow)

- This renderer is **bindless-first**: one descriptor set layout + one pipeline layout shared by all pipelines.
- Descriptor bindings are standardized:
	- binding 0: `textures[]` (`Texture2D` sampled images)
	- binding 1: `samplers[]` (`SamplerState`)
	- binding 2: `storage_images[]` (`RWTexture2D<float4>`)
	- binding 3: `GlobalData` uniform buffer
- In shaders, include and use `shaders/common_bindless.slang` for shared bindings.

## Push constants and buffer usage

- Do **not** use traditional vertex/index buffer bindings for draw data.
- Use GPU buffer suballocation + offsets/device addresses passed through push constants.
- Keep push constants small and draw/pass-local only (resource IDs, offsets, tiny flags/scalars).
- Do **not** put camera/projection/time/frame globals into push constants.

## Global data policy

- Camera/projection/time/frame data belongs in `GlobalData` (binding 3) only.
- `GlobalData` is updated per-frame on CPU and consumed in shaders.
- If a shader currently pushes matrices/camera data, migrate it to `global_ubo` access.

## Shader conventions

- Prefer shared declarations from `common_bindless.slang`; avoid duplicating descriptor declarations per shader.
- Keep binding numbers consistent with Vulkan layout in `vk.c`.
- For new passes, define only pass-specific push constant fields.
- Preserve existing threadgroup sizes and memory access patterns unless optimization requires change.

## C code style and change strategy

- Keep changes minimal and local; avoid broad refactors unless requested.
- Match existing naming/style in touched files.
- Prefer straightforward C over abstraction-heavy patterns.
- No new dependencies unless clearly justified.
- Fix root cause, not superficial patches.

## Performance priorities

- Optimize for reduced memory bandwidth, fewer texture reads/writes, and fewer barriers.
- Avoid redundant transitions and unnecessary full-resolution passes.
- Prefer half-resolution/intermediate reuse for expensive post effects when quality allows.
- Keep hot-path branches and per-pixel work minimal.
- remember to keep push constant  sixteen byte aligned 
	

## Where to look first

- Pipeline/pass orchestration: `passes.c`
- Renderer/device/descriptors: `vk.c`, `vk.h`
- Frame loop and draw dispatch: `main.c`
- Shared GPU/CPU structs: `slangtypes.h`
- Shaders: `shaders/*.slang`

## Validation workflow for agents

- After shader edits: run `./compileslang.sh`.
- After C/Vulkan edits: run project build (`make` / existing build command).
- If changing descriptor bindings or push constants, verify both shader and host-side structs/layouts.

## Do / Don’t quick rules

- Do: use `GlobalData` for view/projection/camera/time.
- Do: use bindless IDs + offsets for resource access.
- Do: keep patches focused and performance-aware.
- Don’t: introduce per-shader custom descriptor layouts.
- Don’t: reintroduce vertex/index binding model for draw submission.
- Don’t: duplicate common descriptor declarations across shaders.


add helpers in helpers.h/.c for common tasks 