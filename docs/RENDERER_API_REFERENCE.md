# Render API Reference

Complete documentation of the Vulkan-based renderer API including core structures, initialization, resource management, and rendering workflows.

## Table of Contents

1. [Overview](#overview)
2. [Core Concepts](#core-concepts)
3. [Initialization](#initialization)
4. [Resource Management](#resource-management)
5. [Pipeline System](#pipeline-system)
6. [Rendering Workflow](#rendering-workflow)
7. [Advanced Topics](#advanced-topics)

---

## Overview

This is a **bindless, single-descriptor-set** Vulkan renderer that prioritizes:
- **GPU efficiency** through bindless resource access
- **Memory efficiency** using offset allocators for suballocations
- **Simplicity** with a single descriptor set layout and pipeline layout for all pipelines
- **Performance** with reverse-Z infinite far plane and clustered shading support

### Key Features

- ✓ Bindless textures (65,536 texture slots)
- ✓ Bindless samplers (256 sampler slots)
- ✓ Bindless storage images and buffers
- ✓ Single descriptor set for all pipelines
- ✓ Dynamic buffer suballocation via offset allocator
- ✓ HDR/LDR rendering pipeline
- ✓ SMAA integration
- ✓ GPU-based profiling (vk_ext_query_result_status)
- ✓ ImGui integration
- ✓ Debug printf support (VK_KHR_shader_non_semantic_info)

---

## Core Concepts

### Bindless Model

The renderer uses a **single descriptor set** for all resources:

```c
#define BINDLESS_TEXTURE_BINDING 0
#define BINDLESS_SAMPLER_BINDING 1
#define BINDLESS_STORAGE_IMAGE_BINDING 2

#define MAX_BINDLESS_TEXTURES 65536
#define MAX_BINDLESS_SAMPLERS 256
#define MAX_BINDLESS_STORAGE_IMAGES 16384
#define MAX_BINDLESS_STORAGE_BUFFERS 65536
```

This means **no pipeline layout changes** when accessing different resources—everything is indexed at draw time via push constants or shader parameters.

### Push Constants

All pipelines share a uniform 256-byte push constant block. Your shaders receive:
- Transformation matrices
- Buffer offsets (via offset allocator)
- Texture IDs
- Custom data (per-pipeline)

### Offset Allocator

Large GPU buffers are suballocated via the offset allocator. Rather than creating individual `VkBuffer` objects, you:
1. Allocate from a pool (GPU, CPU, or staging)
2. Get back an offset and size
3. Pass the offset in push constants
4. Shaders read from `bufferDeviceAddress + offset`

---

## Initialization

### Renderer Descriptor

Configuration for renderer creation:

```c
typedef struct
{
    uint32_t width;
    uint32_t height;
    const char* app_name;

    // Instance/Device extensions
    const char** instance_layers;
    const char** instance_extensions;
    const char** device_extensions;
    uint32_t instance_layer_count;
    uint32_t instance_extension_count;
    uint32_t device_extension_count;

    // Validation
    bool enable_validation;
    bool enable_gpu_based_validation;
    VkDebugUtilsMessageSeverityFlagsEXT validation_severity;
    VkDebugUtilsMessageTypeFlagsEXT validation_types;

    // Swapchain
    VkPresentModeKHR swapchain_preferred_present_mode;
    VkFormat swapchain_preferred_format;
    VkColorSpaceKHR swapchain_preferred_color_space;
    bool vsync;

    // Features
    bool use_custom_features;
    VkFeatureChain custom_features;
    bool enable_debug_printf;
    bool enable_pipeline_stats;
    VkImageUsageFlags swapchain_extra_usage_flags;

    // Bindless counts
    uint32_t bindless_sampled_image_count;
    uint32_t bindless_sampler_count;
    uint32_t bindless_storage_image_count;

    // Memory pools
    VkDeviceSize size_of_cpu_pool;     // CPU-accessible buffer
    VkDeviceSize size_of_gpu_pool;     // GPU-only buffer
    VkDeviceSize size_of_staging_pool; // Upload staging buffer
} RendererDesc;
```

### Creating a Renderer

```c
Renderer renderer = {0};

RendererDesc desc = {
    .width = 1920,
    .height = 1080,
    .app_name = "My Game",
    .enable_validation = true,
    .enable_gpu_based_validation = false,
    .vsync = true,
    .swapchain_preferred_present_mode = VK_PRESENT_MODE_FIFO_KHR,
    .size_of_cpu_pool = 256 * 1024 * 1024,     // 256 MB
    .size_of_gpu_pool = 512 * 1024 * 1024,     // 512 MB
    .size_of_staging_pool = 64 * 1024 * 1024,  // 64 MB
};

renderer_create(&renderer, &desc);
```

### Cleaning Up

```c
renderer_destroy(&renderer);
```

---

## Resource Management

### Buffer Pools

Three types of buffer pools with different update patterns:

#### Linear Pool
Allocate-only, reset at frame boundaries. Ideal for per-frame CPU data.

```c
BufferPool cpu_pool;
buffer_pool_init(
    &renderer,
    BUFFER_POOL_LINEAR,
    &cpu_pool,
    256 * 1024 * 1024,  // 256 MB
    VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
    VMA_MEMORY_USAGE_CPU_TO_GPU,
    VMA_ALLOCATION_CREATE_MAPPED_BIT,
    10000  // max allocations
);

// Per frame
buffer_pool_linear_reset(&cpu_pool);

// Allocate
BufferSlice slice = buffer_pool_alloc(&cpu_pool, 1024, 256);
memcpy(slice.mapped, data, 1024);
```

#### Ring Pool
Circular buffer for streaming data. Track the tail offset as it cycles.

```c
buffer_pool_ring_free_to(&staging_pool, frame->staging_tail);
```

Frame-lifetime rule for staging ring slices:

- Staging slices are transient and frame-scoped.
- They are valid only until the frame fence signals.
- Do not keep `mapped` pointers or staging offsets across frames.

Upload helper split:

- `renderer_upload_buffer_to_slice(...)` for buffer-to-buffer staged copies.
- `renderer_upload_buffer(...)` when destination allocation should also be handled.
- `renderer_upload_texture_2d(...)` for texture copies (separate path due to image constraints).

Example staged buffer upload:

```c
BufferSlice dst = buffer_pool_alloc(&renderer.gpu_pool, sizeof(MaterialGpu), 16);
renderer_upload_buffer_to_slice(&renderer, cmd, dst, &material_gpu, sizeof(MaterialGpu), 16);
```

#### TLSF Pool
Offset allocator-based pool for random allocation/deallocation.

```c
BufferPool gpu_pool;
buffer_pool_init(
    &renderer,
    BUFFER_POOL_TLSF,
    &gpu_pool,
    512 * 1024 * 1024,
    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
    VMA_MEMORY_USAGE_GPU_ONLY,
    0,
    65536  // max allocations
);
```

### Direct Buffers

For persistent GPU buffers (not suballocated):

```c
Buffer my_buffer;
create_buffer(
    &renderer,
    1024,  // size
    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
    VMA_MEMORY_USAGE_GPU_ONLY,
    &my_buffer
);

// Use my_buffer.address in shaders via VK_KHR_buffer_device_address
// ...

destroy_buffer(&renderer, &my_buffer);
```

### Textures

#### Texture Structure

```c
typedef struct
{
    VkImage image;
    VkImageView view;
    VmaAllocation allocation;

    uint32_t width;
    uint32_t height;
    uint32_t mip_count;

    VkFormat format;
    bool valid;
} Texture;

typedef uint32_t TextureID;
```

#### Creating Textures

From file:
```c
TextureID tex_id = load_texture(&renderer, "assets/texture.png");
```

Programmatic creation:
```c
TextureCreateDesc desc = {
    .width = 512,
    .height = 512,
    .mip_count = 10,
    .format = VK_FORMAT_R8G8B8A8_SRGB,
    .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
    .debug_name = "MyTexture"
};

TextureID tex_id = create_texture(&renderer, &desc);
```

#### Accessing Textures in Shaders

All textures are bindless and indexed by `TextureID`:

```glsl
layout(set = 0, binding = 0) uniform sampler2D textures[];

vec4 color = texture(textures[texture_id], uv);
```

#### Destroying Textures

```c
destroy_texture(&renderer, tex_id);
```

### Samplers

#### Default Samplers

Pre-created samplers available for immediate use:

```c
typedef enum DefaultSamplerID
{
    SAMPLER_LINEAR_WRAP = 0,        // Linear filtering, wrapping
    SAMPLER_LINEAR_CLAMP,            // Linear filtering, clamp to edge
    SAMPLER_NEAREST_WRAP,            // Nearest filtering, wrapping
    SAMPLER_NEAREST_CLAMP,           // Nearest filtering, clamp to edge
    SAMPLER_LINEAR_WRAP_ANISO,       // Linear + anisotropic filtering
    SAMPLER_SHADOW,                  // Comparison sampler for shadow maps
    SAMPLER_COUNT
} DefaultSamplerID;

// Access via renderer->default_samplers.samplers[SAMPLER_LINEAR_WRAP]
```

#### Custom Samplers

```c
typedef struct
{
    VkFilter mag_filter;
    VkFilter min_filter;
    VkSamplerAddressMode address_u;
    VkSamplerAddressMode address_v;
    VkSamplerAddressMode address_w;
    VkSamplerMipmapMode mipmap_mode;
    float max_lod;
    const char* debug_name;
} SamplerCreateDesc;

SamplerCreateDesc desc = {
    .mag_filter = VK_FILTER_LINEAR,
    .min_filter = VK_FILTER_LINEAR,
    .address_u = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
    .address_v = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
    .address_w = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
    .mipmap_mode = VK_SAMPLER_MIPMAP_MODE_LINEAR,
    .max_lod = 12.0f,
    .debug_name = "PostProcessSampler"
};

SamplerID sampler_id = create_sampler(&renderer, &desc);
destroy_sampler(&renderer, sampler_id);
```

---

## Pipeline System

### Graphics Pipelines

#### Configuration Structure

```c
typedef struct
{
    // Shaders
    const char* vert_path;
    const char* frag_path;

    // Rasterization
    VkCullModeFlags cull_mode;
    VkFrontFace front_face;
    VkPolygonMode polygon_mode;

    VkPrimitiveTopology topology;

    // Depth
    bool depth_test_enable;
    bool depth_write_enable;
    VkCompareOp depth_compare_op;

    // Attachments and blending
    uint32_t color_attachment_count;
    const VkFormat* color_formats;
    VkFormat depth_format;

    ColorAttachmentBlend blends[MAX_COLOR_ATTACHMENTS];

    // Vertex input (optional)
    bool use_vertex_input;
    VertexBinding vertex_binding;
} GraphicsPipelineConfig;
```

#### Creating a Graphics Pipeline

```c
GraphicsPipelineConfig cfg = pipeline_config_default();

cfg.vert_path = "shaders/myshader.vert";
cfg.frag_path = "shaders/myshader.frag";
cfg.cull_mode = VK_CULL_MODE_BACK_BIT;
cfg.front_face = VK_FRONT_FACE_COUNTER_CLOCKWISE;

cfg.depth_test_enable = true;
cfg.depth_write_enable = true;
cfg.depth_compare_op = VK_COMPARE_OP_GREATER;  // Reverse-Z

cfg.color_attachment_count = 1;
cfg.color_formats = (VkFormat[]){ VK_FORMAT_R16G16B16A16_SFLOAT };
cfg.depth_format = VK_FORMAT_D32_SFLOAT;

cfg.blends[0] = blend_disabled();  // No blending

PipelineID pipeline_id = pipeline_create_graphics(&renderer, &cfg);
VkPipeline pipeline = pipeline_get(pipeline_id);
```

#### Blend Helpers

```c
// Standard alpha blending
ColorAttachmentBlend blend = blend_alpha();

// Additive blending
ColorAttachmentBlend blend = blend_additive();

// No blending
ColorAttachmentBlend blend = blend_disabled();
```

### Compute Pipelines

```c
PipelineID compute_id = pipeline_create_compute(&renderer, "shaders/compute.comp");
VkPipeline compute_pipeline = pipeline_get(compute_id);
```

### Hot Reloading

Shaders are automatically hot-reloaded when source files change:

```c
// Mark pipeline as needing rebuild
pipeline_mark_dirty("shaders/myshader.vert");

// Rebuild all dirty pipelines
pipeline_rebuild(&renderer);
```

---

## Render Targets

### Render Target Structure

A render target is an off-screen image with optional mip chain, typically used for:
- G-Buffer passes
- HDR intermediate targets
- Post-processing inputs

```c
typedef struct
{
    VkImage image;
    VmaAllocation allocation;

    VkImageView view;                    // Full mip chain
    VkImageView mip_views[RT_MAX_MIPS];  // Per-mip views

    VkFormat format;
    uint32_t width;
    uint32_t height;
    uint32_t layers;
    uint32_t mip_count;

    VkImageUsageFlags usage;
    VkImageAspectFlags aspect;

    ImageState mip_states[RT_MAX_MIPS];  // Sync state per mip

    uint32_t bindless_index;  // Shared for sampled/storage access

    const char* debug_name;
} RenderTarget;
```

### Creating Render Targets

```c
typedef struct
{
    uint32_t           width;
    uint32_t           height;
    uint32_t           layers;
    VkFormat           format;
    VkImageUsageFlags  usage;
    VkImageAspectFlags aspect;     // 0 = infer from format
    uint32_t           mip_count;  // 0 = auto-compute, 1 = no mips
    const char*        debug_name;
} RenderTargetSpec;

RenderTarget hdr_target;
RenderTargetSpec spec = {
    .width = 1920,
    .height = 1080,
    .layers = 1,
    .format = VK_FORMAT_R16G16B16A16_SFLOAT,
    .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | 
             VK_IMAGE_USAGE_SAMPLED_BIT |
             VK_IMAGE_USAGE_TRANSFER_DST_BIT,
    .mip_count = 1,
    .debug_name = "HDR_Color"
};

rt_create(&renderer, &hdr_target, &spec);

// Resize on window resize
rt_resize(&renderer, &hdr_target, new_width, new_height);

// Clean up
rt_destroy(&renderer, &hdr_target);
```

### Render Target Transitions

Image layout transitions with synchronization tracking:

```c
// Transition full mip chain
rt_transition_all(cmd, &hdr_target,
    VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
    VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
    VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);

// Transition single mip
rt_transition_mip(cmd, &hdr_target, 0,
    VK_IMAGE_LAYOUT_GENERAL,
    VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
    VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
```

---

## Swapchain Management

### Swapchain Structure

```c
typedef struct
{
    // Hot data
    ImageState states[MAX_SWAPCHAIN_IMAGES];
    VkImage images[MAX_SWAPCHAIN_IMAGES];
    uint32_t current_image;

    VkImageView image_views[MAX_SWAPCHAIN_IMAGES];
    VkSemaphore render_finished[MAX_SWAPCHAIN_IMAGES];
    TextureID bindless_index[MAX_SWAPCHAIN_IMAGES];

    // Cold data
    VkSwapchainKHR swapchain;
    VkSurfaceKHR surface;
    VkFormat format;
    VkColorSpaceKHR color_space;
    VkPresentModeKHR present_mode;
    VkExtent2D extent;
    uint32_t image_count;

    VkImageUsageFlags image_usage;

    bool vsync;
    bool needs_recreate;
} FlowSwapchain;
```

### Acquiring and Presenting

These are inline functions in the header:

```c
// Acquire next image
bool acquired = vk_swapchain_acquire(
    renderer.device,
    &renderer.swapchain,
    frame->image_available_semaphore,
    VK_NULL_HANDLE,
    UINT64_MAX);

if (!acquired) {
    // Swapchain needs recreation
    renderer.swapchain.needs_recreate = true;
}

// Present after command submission
vk_swapchain_present(renderer.present_queue,
    &renderer.swapchain,
    &renderer.swapchain.render_finished[renderer.swapchain.current_image],
    1);
```

### Swapchain Transitions

```c
// Transition to rendering target
image_transition_swapchain(cmd, &renderer.swapchain,
    VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
    VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
    VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);

// Transition to present
image_transition_swapchain(cmd, &renderer.swapchain,
    VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
    VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT,
    0);
```

---

## Rendering Workflow

### Per-Frame Structure

1. **Frame Start** - Synchronization, input, camera update
2. **Command Recording** - Bind resources, record draw calls
3. **Frame Submit** - Submit to GPU
4. **Present** - Show on screen

### Frame Start

```c
Camera cam = {
    .position = {0, 5, 10},
    .fov_y = glm_rad(45.0f),
    .near_z = 0.1f,
    .far_z = 1000.0f,
    .move_speed = 10.0f,
    .look_speed = 0.005f
};

// At frame start
frame_start(&renderer, &cam);

// frame_start handles:
// - Frame synchronization (wait for fence)
// - Input processing
// - Camera updates (WASD, mouse look)
// - Window resize detection
// - Swapchain recreation
// - Command buffer reset
```

### Recording Commands

```c
FrameContext* frame = &renderer.frames[renderer.current_frame];
VkCommandBuffer cmd = frame->cmdbuf;

VkCommandBufferBeginInfo begin_info = {
    .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
    .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT
};
vkBeginCommandBuffer(cmd, &begin_info);

// Transition depth to attachment
image_transition_simple(cmd, renderer.depth[renderer.swapchain.current_image].image,
    VK_IMAGE_ASPECT_DEPTH_BIT,
    VK_IMAGE_LAYOUT_UNDEFINED,
    VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL);

// Setup rendering
VkRenderingAttachmentInfo color_attachment = {
    .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
    .imageView = renderer.hdr_color[renderer.swapchain.current_image].view,
    .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
    .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
    .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
    .clearValue = {.color = {0.0f, 0.0f, 0.0f, 0.0f}}
};

VkRenderingAttachmentInfo depth_attachment = {
    .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
    .imageView = renderer.depth[renderer.swapchain.current_image].view,
    .imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
    .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
    .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
    .clearValue = {.depthStencil = {0.0f, 0}}  // Reverse-Z
};

VkRenderingInfo render_info = {
    .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
    .renderArea = {.offset = {0, 0}, .extent = renderer.swapchain.extent},
    .layerCount = 1,
    .colorAttachmentCount = 1,
    .pColorAttachments = &color_attachment,
    .pDepthAttachment = &depth_attachment
};

vkCmdBeginRendering(cmd, &render_info);

// Bind pipeline and descriptor set
vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, my_pipeline);
vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
    renderer.bindless_system.pipeline_layout, 0, 1,
    &renderer.bindless_system.set, 0, NULL);

// Setup viewport and scissor
vk_cmd_set_viewport_scissor(cmd, renderer.swapchain.extent);

// Push constants
typedef struct {
    mat4 view_proj;
    uint32_t vertex_buffer_offset;
} PushConstants;

PushConstants pc = {0};
glm_mat4_copy(cam.view_proj, pc.view_proj);
pc.vertex_buffer_offset = vertex_offset;

vkCmdPushConstants(cmd, renderer.bindless_system.pipeline_layout,
    VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(PushConstants), &pc);

// Draw
vkCmdDraw(cmd, vertex_count, 1, 0, 0);

vkCmdEndRendering(cmd);

vkEndCommandBuffer(cmd);
```

### Frame Submit

```c
submit_frame(&renderer);

// submit_frame:
// - Submits command buffer to graphics queue
// - Signals render_finished semaphore
// - Waits on image_available semaphore
// - Presents swapchain
```

---

## Advanced Topics

### Frustum Culling

The renderer computes a view-frustum from the camera's view-projection matrix:

```c
typedef struct
{
    vec4 planes[FrustumPlaneCount];  // ax + by + cz + d = 0
} Frustum;

// Updated automatically in frame_start()
// Access via: renderer.frustum.planes[LeftPlane/RightPlane/etc]
```

### GPU Profiling

Per-frame GPU command timing via `VkQueryResultStatusKHR`:

```c
typedef struct GpuProfiler {
    // Internal profiling data
} GpuProfiler;

// Collected per frame in frame_start()
GpuProfiler* frame_prof = &renderer.gpuprofiler[renderer.current_frame];
```

### Camera System

```c
typedef struct
{
    vec3 position;
    float near_z;
    vec3  cam_dir;

    float yaw;    // radians
    float pitch;  // radians

    float move_speed;
    float look_speed;
    float fov_y;
    float far_z;
    mat4  view_proj;
    
    bool mouse_captured;
    bool first_mouse;

    double last_mouse_x;
    double last_mouse_y;
} Camera;

// Controls:
// - Right Mouse Button: Capture/release mouse
// - WASD: Move
// - E/Q: Up/Down
// - =/-: Increase/decrease speed
// - Shift: 3x speed multiplier
```

### ImGui Integration

ImGui is automatically initialized and available:

```c
imgui_begin_frame();

igText("Hello, World!");
igSliderFloat("Some Value", &value, 0.0f, 1.0f, "%.3f", 0);

if (igButton("Click Me", (ImVec2){0, 0})) {
    // Handle button press
}

// Commands are recorded in the current render pass
```

### Image Transitions

#### Simple Transition

```c
void image_transition_simple(VkCommandBuffer cmd, VkImage image,
    VkImageAspectFlags aspect,
    VkImageLayout old_layout,
    VkImageLayout new_layout);
```

#### Advanced Transitions with Tracking

For complex synchronization with mip-level tracking:

```c
void cmd_transition_all_mips(VkCommandBuffer cmd,
    VkImage image,
    ImageState* state,
    VkImageAspectFlags aspect,
    uint32_t mipCount,
    VkPipelineStageFlags2 newStage,
    VkAccessFlags2 newAccess,
    VkImageLayout newLayout,
    uint32_t newQueueFamily);

void cmd_transition_mip(VkCommandBuffer cmd,
    VkImage image,
    ImageState* state,
    VkImageAspectFlags aspect,
    uint32_t mip,
    VkPipelineStageFlags2 newStage,
    VkAccessFlags2 newAccess,
    VkImageLayout newLayout,
    uint32_t newQueueFamily);
```

### Push Constants Macro

For type-safe, 256-byte aligned push constants:

```c
PUSH_CONSTANT(MyShaderPC,
    mat4 transform;
    uint32_t texture_id;
    float time;
    uint32_t flags;
);

// Creates:
// - MyShaderPC_init (minimal struct)
// - MyShaderPC (padded to 256 bytes)

MyShaderPC pc = {0};
glm_mat4_copy(matrix, pc.transform);
pc.texture_id = tex_id;

vkCmdPushConstants(cmd, layout,
    VK_SHADER_STAGE_ALL, 0, sizeof(MyShaderPC), &pc);
```

### Descriptor Set Layout

Single descriptor set shared by all pipelines:

```c
typedef struct Bindless
{
    VkDescriptorSetLayout set_layout;
    VkDescriptorPool pool;
    VkDescriptorSet set;
    VkPipelineLayout pipeline_layout;
} Bindless;

// Access via: renderer.bindless_system
```

---

## Example: Complete Render Loop

```c
#include "vk.h"

int main() {
    Renderer renderer = {0};
    RendererDesc desc = {
        .width = 1920,
        .height = 1080,
        .app_name = "My Renderer",
        .enable_validation = true,
        .size_of_gpu_pool = 512 * 1024 * 1024,
        .size_of_cpu_pool = 256 * 1024 * 1024,
        .size_of_staging_pool = 64 * 1024 * 1024,
    };

    renderer_create(&renderer, &desc);

    // Create pipeline
    GraphicsPipelineConfig cfg = pipeline_config_default();
    cfg.vert_path = "shaders/mesh.vert";
    cfg.frag_path = "shaders/mesh.frag";
    cfg.color_attachment_count = 1;
    cfg.color_formats = (VkFormat[]){ VK_FORMAT_R16G16B16A16_SFLOAT };
    cfg.depth_format = VK_FORMAT_D32_SFLOAT;
    cfg.depth_compare_op = VK_COMPARE_OP_GREATER;

    PipelineID pipeline_id = pipeline_create_graphics(&renderer, &cfg);

    // Load resources
    TextureID texture = load_texture(&renderer, "assets/texture.png");

    Camera camera = {
        .position = {0, 5, 10},
        .fov_y = glm_rad(45.0f),
        .near_z = 0.1f,
        .far_z = 1000.0f,
        .move_speed = 10.0f,
        .look_speed = 0.005f
    };

    // Main loop
    while (!glfwWindowShouldClose(renderer.window)) {
        frame_start(&renderer, &camera);

        FrameContext* frame = &renderer.frames[renderer.current_frame];
        VkCommandBuffer cmd = frame->cmdbuf;

        // Begin command buffer
        VkCommandBufferBeginInfo begin = {
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
            .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT
        };
        vkBeginCommandBuffer(cmd, &begin);

        // Transition and render
        VkRenderingAttachmentInfo color = {
            .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
            .imageView = renderer.hdr_color[renderer.swapchain.current_image].view,
            .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
            .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
            .clearValue = {.color = {0, 0, 0, 0}}
        };

        VkRenderingInfo render_info = {
            .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
            .renderArea = {.offset = {0, 0}, .extent = renderer.swapchain.extent},
            .layerCount = 1,
            .colorAttachmentCount = 1,
            .pColorAttachments = &color,
        };

        vkCmdBeginRendering(cmd, &render_info);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_get(pipeline_id));
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
            renderer.bindless_system.pipeline_layout, 0, 1,
            &renderer.bindless_system.set, 0, NULL);

        vk_cmd_set_viewport_scissor(cmd, renderer.swapchain.extent);

        // Draw call
        vkCmdDraw(cmd, 3, 1, 0, 0);

        vkCmdEndRendering(cmd);
        vkEndCommandBuffer(cmd);

        submit_frame(&renderer);
    }

    destroy_texture(&renderer, texture);
    renderer_destroy(&renderer);

    return 0;
}
```

---

## Performance Tips

1. **Minimize Push Constant Changes** - Group data efficiently in 256-byte blocks
2. **Batch by Texture** - Sort draw calls to minimize descriptor set changes
3. **Use Ring Buffers** - For streaming data (geometry, transforms)
4. **Leverage Reverse-Z** - More precision near camera, better for depth testing
5. **GPU Timeline Profiling** - Use `VkQueryResultStatusKHR` for frame analysis
6. **Avoid MIP Generation** - Pre-generate offline or use compute shaders
7. **Reuse Samplers** - Default sampler table covers most use cases

---

## Debugging

### Validation Layers
Enable in `RendererDesc.enable_validation` for comprehensive error checking.

### GPU-Based Validation
Slower but catches data races:
```c
.enable_gpu_based_validation = true
```

### Debug Printf
Enable shader debug output:
```c
.enable_debug_printf = true
```

Then in shaders:
```glsl
#extension GL_EXT_debug_printf : enable

debugPrintfEXT("Value: %f", value);
```

### Named Objects
Use `debug_name` parameters for better profiler/debugger output:
```c
rt.debug_name = "GBuffer_Albedo";
```

---

## API Stability

This API prioritizes **correctness and performance** over stability guarantees. Some internal structures may change between revisions. The public functions listed in this document are the stable interface.
