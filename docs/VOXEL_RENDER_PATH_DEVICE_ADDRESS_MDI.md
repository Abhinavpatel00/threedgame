# Voxel Rendering Path (Device Address + Multi Draw Indirect Count, No SSBO Descriptors)

## Why this document
This defines a voxel render path similar to the `meshing` reference flow and your `voxelmesh.txt` notes, but adapted to this renderer’s rules:

- Single bindless descriptor set + single pipeline layout.
- No per-mesh vertex/index buffer binding model.
- Geometry read through `VkDeviceAddress` passed via push constants.
- Draw submission through `vkCmdDrawIndirectCount` (multi-draw indirect count).
- No descriptor-backed SSBO requirement for voxel mesh data.

This is written to match current repo patterns in `main.c` and `shaders/triangle.slang`.

---

## 1) Current repo baseline (what already exists)

From current code:

- CPU builds `PackedFace[]` mesh.
- Faces are uploaded into a pooled buffer slice (`BufferSlice face_slice`).
- A 64-bit GPU address (`face_ptr`) is passed via push constants.
- Vertex shader expands each packed face into two triangles using `SV_VertexID`.
- Draw uses `vkCmdDrawIndirectCount` and `VkDrawIndirectCommand`.
- Descriptor set is bindless for textures/samplers, not for voxel face storage.

So you already have the core architecture needed for a GPU-driven voxel path.

---

## 2) Target architecture summary

### CPU side
1. Maintain chunk voxel data (`Chunk`) and generated face meshes (`ChunkMesh`).
2. For each visible/dirty chunk, generate packed faces (greedy or basic hidden-face culling).
3. Suballocate contiguous slices in large GPU buffers:
   - face data region (`PackedFace[]`)
   - indirect command region (`VkDrawIndirectCommand[]`)
   - draw count region (`uint32_t`)
4. Fill indirect commands for all visible chunks.
5. Set draw count = number of valid commands.

### GPU side
1. Push constants provide:
   - camera matrices/params
   - base `VkDeviceAddress` to face stream
   - texture/sampler ids
2. One graphics pipeline call:
   - `vkCmdDrawIndirectCount(...)`
3. Vertex shader pulls packed face records by address.
4. Fragment shader shades using bindless textures.

No SSBO descriptor binding needed for voxel geometry.

---

## 3) Data contracts

## 3.1 Packed voxel face format
Keep your current compact face layout (already in `triangle.slang`):

```c
typedef struct PackedFace {
    uint32_t data0;
    uint32_t data1;
} PackedFace;
```

Current decode semantics:

- `data0[0..7]`   => local `x`
- `data0[8..15]`  => local `y`
- `data0[16..27]` => local `z`
- `data0[28..30]` => face direction (0..5)
- `data1[0..15]`  => texture id
- `data1[16..23]` => chunk x (biased)
- `data1[24..31]` => chunk z (biased)

This is fine for a first full path.

### Optional v2 extension
If you need larger world coordinates:

- move chunk coordinate out of `data1` and into per-draw metadata encoded in indirect command (`firstInstance`) or a secondary metadata stream.

---

## 3.2 Per-draw command format (MDI count)
Use standard Vulkan indirect command array:

```c
typedef struct VkDrawIndirectCommand {
    uint32_t vertexCount;
    uint32_t instanceCount;
    uint32_t firstVertex;
    uint32_t firstInstance;
} VkDrawIndirectCommand;
```

Per chunk draw command:

- `vertexCount = face_count * 6`
- `instanceCount = 1`
- `firstVertex = global_face_offset * 6`
- `firstInstance = metadata_index_or_zero`

Where `global_face_offset` is in units of `PackedFace` records inside one big face stream.

Important: one global contiguous face stream allows all visible chunks to be rendered in one `vkCmdDrawIndirectCount` call.

---

## 3.3 Push constant layout (graphics)
Use one shared push block (CPU + shader), aligned and stable:

```c
PUSH_CONSTANT(VoxelPush,
    VkDeviceAddress face_stream_ptr; // base address to PackedFace stream
    uint32_t        total_face_count; // optional debug/guard
    float           aspect;

    vec3            cam_pos;
    uint32_t        pad0;

    vec3            cam_dir;
    uint32_t        pad1;

    float           view_proj[4][4];

    uint32_t        texture_id;
    uint32_t        sampler_id;

    uint32_t        frame_index;
    uint32_t        debug_flags;
);
```

Notes:

- Keep this in one shared header-like definition to avoid CPU/GPU drift.
- `face_stream_ptr` points to beginning of the face array; `firstVertex` selects per-draw range.

---

## 4) Meshing path (CPU)

## 4.1 Chunk meshing behavior (similar to `meshing` repo)
Recommended order per chunk:

1. Skip air voxels.
2. For non-air voxel, test 6 neighbors.
3. Emit only exposed faces.
4. Optional greedy merge per face plane.
5. Produce `PackedFace[]`.

This keeps triangle count low and maps directly to the address-based vertex pulling shader.

## 4.2 Face-direction split (optional high-FPS optimization)
As in `voxelmesh.txt`, you can keep 6 submeshes per chunk (`+X -X +Y -Y +Z -Z`):

- Build one command per non-empty face bucket.
- CPU chooses which direction buckets to draw based on camera/chunk relation.

This reduces wasted vertex work on backfacing regions before raster culling.

---

## 5) Buffer pool / allocator usage

Use existing big-buffer + offset allocator model.

Required slices per frame (or persistent ring regions):

1. `face_slice`: `PackedFace[max_faces_visible_frame]`
2. `indirect_slice`: `VkDrawIndirectCommand[max_draws]`
3. `count_slice`: `uint32_t`

Alignment recommendations:

- `PackedFace`: 16-byte alignment (safe, cache-friendly)
- indirect command slice: 16-byte alignment (required by command element size)
- count: 4-byte alignment

When using host-visible mapped memory:

- write face data
- write indirect commands
- write draw count
- flush mapped ranges (`vmaFlushAllocation`) covering all updated subranges

---

## 6) Multi-draw build algorithm (CPU)

```text
visible_draw_count = 0
face_write_cursor = 0

for each visible chunk:
    if chunk mesh empty: continue

    copy chunk faces -> face_slice[face_write_cursor ...]

    cmd = indirect[visible_draw_count]
    cmd.vertexCount   = chunk.face_count * 6
    cmd.instanceCount = 1
    cmd.firstVertex   = face_write_cursor * 6
    cmd.firstInstance = chunk.meta_index (or 0)

    face_write_cursor += chunk.face_count
    visible_draw_count += 1

count_buffer[0] = visible_draw_count
```

Then one draw call:

```c
vkCmdDrawIndirectCount(
    cmd,
    indirect_slice.buffer,
    indirect_slice.offset,
    count_slice.buffer,
    count_slice.offset,
    max_draw_count,
    sizeof(VkDrawIndirectCommand));
```

`max_draw_count` should be your allocated indirect capacity.

---

## 7) Shader path (address-based vertex pulling)

## 7.1 Vertex fetch model
Given `SV_VertexID` in each draw:

- `faceID_local = vertexID / 6`
- `corner = corner_lut[vertexID % 6]`
- `faceID_global = (firstVertex / 6) + faceID_local`
- `PackedFace f = ((PackedFace*)push.face_stream_ptr)[faceID_global]`

In Slang, `firstVertex` is available as base vertex semantic support (`SV_StartVertexLocation`) depending backend; if unavailable, preserve current approach where each draw uses per-draw `firstVertex` so `SV_VertexID` already includes it.

Your current shader already decodes `PackedFace` and computes world-space positions, so this is mostly a scaling change from one draw to many draws.

## 7.2 No-SSBO descriptor principle
The geometry stream is not fetched through `layout(binding=...) buffer` descriptors.
Instead:

- Push 64-bit address.
- Cast to pointer in shader.
- Pull records manually.

This preserves your “no SSBO descriptor” constraint while still enabling random-access structured reads.

---

## 8) Command buffer order and synchronization

## 8.1 Per-frame order (graphics relevant)
1. Update/upload stream slices (host writes + flush).
2. Begin command buffer.
3. Transition render targets.
4. Bind descriptor set (bindless textures/samplers).
5. Bind graphics pipeline.
6. Push `VoxelPush`.
7. `vkCmdDrawIndirectCount`.
8. Continue post/smaa/present.

## 8.2 Buffer visibility barrier (if needed)
If host writes are non-coherent and flushed, explicit host->shader visibility is usually satisfied by submission ordering and memory properties, but robust cross-platform path is:

- pipeline barrier before draw with
  - `srcStage = VK_PIPELINE_STAGE_2_HOST_BIT`
  - `srcAccess = VK_ACCESS_2_HOST_WRITE_BIT`
  - `dstStage = VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT`
  - `dstAccess = VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_2_SHADER_READ_BIT`

Apply to the pooled buffer range containing face + indirect + count slices (or whole buffer if simpler).

---

## 9) Frustum/streaming integration

## 9.1 Visibility input
Build visible chunk list from:

- camera frustum test against chunk AABB
- optional distance cap
- optional occlusion result (later)

Only visible chunks contribute indirect commands.

## 9.2 Dirty chunk remesh
For changed chunks:

- remesh on CPU worker thread(s)
- swap mesh pointer/version when complete
- include latest mesh in next frame’s stream packing

This is compatible with single-call MDI path because command generation is cheap and linear.

---

## 10) Suggested implementation phases

## Phase A (immediate; closest to existing code)
- Keep one global face stream per frame.
- Build N indirect commands (one per visible chunk).
- Draw through one `vkCmdDrawIndirectCount`.
- Keep chunk coords packed in `data1`.

## Phase B (more scalable metadata)
- Move chunk transform metadata from `data1` to per-draw source (`firstInstance` + metadata stream/address).
- Free bits in `PackedFace` for AO/light/material flags.

## Phase C (optional GPU build)
- Add compute culling/compaction to fill indirect buffer on GPU.
- Keep graphics shader and draw API unchanged.

---

## 11) Debug and validation checklist

1. If `count=0`, call still valid and draws nothing.
2. Clamp `count <= max_draw_count` on CPU before flush.
3. Validate each command:
   - `vertexCount % 6 == 0`
   - `firstVertex % 6 == 0`
4. Assert stream capacity:
   - `face_write_cursor <= MAX_STREAM_FACES`
   - `visible_draw_count <= MAX_DRAWS`
5. GPU markers:
   - `Main Pass: Voxel MDI` timing
   - draw count, face count in debug UI

---

## 12) Concrete integration points in this repo

- `main.c`
  - expand single-command write to command array build from visible chunk list
  - keep `vkCmdDrawIndirectCount` call; increase `maxDrawCount` to capacity
  - keep push constant path with `VkDeviceAddress`
- `shaders/triangle.slang`
  - preserve address-based `PackedFace*` read
  - ensure global face indexing works with multi-draw `firstVertex`
- allocator path (`offset_allocator` + buffer pool)
  - reserve per-frame capacities for faces + indirect + count

---

## 13) What this path intentionally does NOT require

- No additional descriptor set layout.
- No per-chunk descriptor updates.
- No per-chunk `vkCmdBindVertexBuffers`/`vkCmdBindIndexBuffer` calls.
- No SSBO descriptor for voxel faces.

---

## 14) Minimal pseudocode for frame submission

```c
// CPU
build_visible_chunk_list(camera);
pack_faces_and_indirect_commands(...);
flush_face_indirect_count_ranges(...);

// GPU record
vkCmdBindDescriptorSets(... bindless textures/samplers ...);
vkCmdBindPipeline(... voxel pipeline ...);
vkCmdPushConstants(..., &voxel_push);
vkCmdDrawIndirectCount(cmd,
                       indirect_slice.buffer, indirect_slice.offset,
                       count_slice.buffer,    count_slice.offset,
                       MAX_DRAWS,
                       sizeof(VkDrawIndirectCommand));
```

This is the intended steady-state draw path.

---

## 15) Recommended defaults

- `CHUNK_SIZE = 32` for gameplay chunks, keep render packing independent.
- `MAX_STREAM_FACES`: size for worst visible set (start conservative, profile).
- `MAX_DRAWS`: visible chunk cap per frame (e.g. 2k–8k depending world scale).
- Triple-buffer stream slices if CPU/GPU overlap causes hazards.

---

## 16) Final note

The important conceptual translation from the reference (`meshing` + `voxelmesh.txt`) is:

- Keep mesh data compact.
- Build one large face stream.
- Build many indirect commands.
- Submit once via `vkCmdDrawIndirectCount`.
- Replace SSBO-per-draw metadata with push-constant device address and draw-command parameters.

That is fully consistent with your renderer constraints and current code direction.
