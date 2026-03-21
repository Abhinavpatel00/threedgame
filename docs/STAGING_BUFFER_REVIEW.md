# Staging Buffer System Review

This review is based on the current buffer-pool design in [vk.c](vk.c#L1773-L1782), the pool API in [vk.h](vk.h#L376-L475), and the current usage in [main.c](main.c#L383-L698).

## Summary

The current staging system is simple and effective:

- it uses a dedicated ring pool for upload data
- it keeps the staging memory persistently mapped
- it frees old upload space at the frame boundary with a fence-safe tail offset
- it already supports a clean copy path from staging to GPU-only memory

That is a solid baseline for a Vulkan renderer.

## What is already good

1. **Single-purpose staging pool**
   - Upload data is separated from CPU and GPU pools.
   - That keeps transfer traffic predictable.

2. **Persistently mapped memory**
   - Good for small and frequent uploads.
   - Avoids repeated map and unmap overhead.

3. **Frame-based reclamation**
   - `staging_tail` is captured before submit and released after the frame completes.
   - This is the right general pattern for ring-buffer uploads.

4. **Suballocation model matches the rest of the engine**
   - The staging pool fits the same pool/slice style used elsewhere.
   - That keeps the API consistent.

## Main concerns

### 1. The upload path is still manual

Right now the material upload is done by hand:

- allocate staging slice
- `memcpy` into mapped memory
- allocate destination slice
- record `vkCmdCopyBuffer`

This is correct, but it will become repetitive as more resources are uploaded.

### 2. The staging pool size is fixed up front

`size_of_staging_pool = MB(128)` may be fine for experimentation, but it is not yet tied to measured usage.

Possible outcomes:

- too large: wasted host-visible memory
- too small: upload stalls or failed allocations under heavy streaming

### 3. There is no explicit high-water tracking

The system does not appear to record:

- peak bytes used per frame
- number of failed allocations
- average upload volume

Without that, tuning the pool size is guesswork.

### 4. Alignment is currently ad hoc

The current call site uses 16-byte alignment for the staging slice, but different uploads may need different rules.

Examples:

- buffer copies need 4-byte alignment at minimum
- uniform buffer updates may want stricter alignment
- image uploads need row pitch and offset rules

A helper should compute this instead of hard-coding it at the call site.

### 5. The lifetime of staging slices is implicit

The staging slice is only safe until the frame fence signals.

That is currently enforced indirectly by the ring tail logic, but the rule should be made more explicit in the API or documentation.

## Suggestions

### 1. Add a dedicated upload helper

Create one function for common buffer uploads that does all of this:

- allocate a staging slice
- copy CPU data into mapped memory
- allocate the destination slice if needed
- emit `vkCmdCopyBuffer`
- return the destination slice or destination offset

This reduces boilerplate and makes upload behavior consistent.

### 2. Add staging statistics

Track these counters per frame and globally:

- bytes allocated
- bytes copied
- peak ring usage
- failed allocations
- number of uploads

This will make pool sizing and performance tuning much easier.

### 3. Make the staging pool size data-driven

Keep the default, but also log a warning if usage exceeds a threshold.

A good path is:

- start with the current fixed size
- print a warning near 75% and 90% usage
- once stable, choose a size based on real workload data

### 4. Separate buffer-copy and texture-upload helpers

Buffer uploads and texture uploads have different constraints.

Suggested API split:

- buffer upload helper
- texture upload helper

That keeps row-pitch and image-layout logic out of the generic buffer path.

### 5. Make alignment policy explicit

Add a small helper such as:

- `staging_alignment_for_buffer_copy()`
- `staging_alignment_for_uniform_upload()`
- `staging_alignment_for_texture_upload()`

This will avoid hidden assumptions in call sites.

### 6. Consider a transfer-queue upload path later

If uploads become larger or more frequent, consider recording copies on a transfer queue instead of the graphics queue.

That can help when the renderer starts doing more work per frame, especially for streaming assets.

### 7. Document the frame-lifetime rule

The staging API should clearly state:

- slices are transient
- they are valid only until the frame fence completes
- the caller must not keep pointers past the frame

This prevents accidental use-after-free style bugs.

### 8. Add debug checks for ring exhaustion

If the ring is full, fail loudly in debug builds and log the current usage.

That will make it easier to catch pathological upload spikes.

## Recommended next step

The best low-risk improvement is to add a small upload helper layer around the current ring pool.

That gives you:

- less duplicated code
- better alignment handling
- better observability
- easier future support for textures and larger streaming workloads

## Short verdict

The current staging system is good enough for a prototype and already follows the right Vulkan pattern. The main improvements now are not structural rewrites, but better abstraction, better metrics, and clearer lifetime rules.

## Implemented helper split

The renderer now exposes a clear API split:

- `renderer_upload_buffer_to_slice(...)`
   - Uses the staging ring for buffer uploads into an existing destination slice.
- `renderer_upload_buffer(...)`
   - Allocates destination from the GPU pool, stages upload, records copy, and returns the destination slice.
- `renderer_upload_texture_2d(...)`
   - Handles texture copy via a dedicated texture upload path.

This keeps texture-specific constraints (row packing, image copy region, image layout expectations) out of the generic buffer upload path.

## Frame-lifetime rule (explicit)

`staging_pool` slices are transient per-frame resources.

- Validity starts at allocation time in the recording frame.
- Validity ends when that frame's fence signals and the ring advances.
- Callers must not keep `mapped` pointers, staging offsets, or staging `BufferSlice` handles across frames.

Safe pattern:

1. Allocate staging slice while recording frame commands.
2. Copy CPU data into `slice.mapped`.
3. Record `vkCmdCopyBuffer` / `vkCmdCopyBufferToImage` in that frame.
4. Drop staging references after submit.
