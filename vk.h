#pragma once
#include "external/tracy/public/tracy/TracyC.h"
#include <bits/time.h>
#include <time.h>
#define IMGUI_IMPL_VULKAN_MINIMUM_IMAGE_SAMPLER_POOL_SIZE (1)
#define IMGUI_IMPL_VULKAN_USE_VOLK
#define CIMGUI_USE_GLFW
#define CIMGUI_DEFINE_ENUMS_AND_STRUCTS
#define CIMGUI_USE_VULKAN
#define CGLM_ALL_UNALIGNED
#include "external/cglm/include/cglm/vec3.h"
#include "external/cglm/include/cglm/cglm.h"
#include "external/cglm/include/cglm/types.h"
#include "stb/stb_image_write.h"
#include "vk_default.h"
#ifdef Status
#undef Status
#endif
#include "gpu_timer.h"

#include "mu/mu.h"

#include "tinytypes.h"
#include "slangtypes.h"

#include "external/cimgui/cimgui.h"
#include "helpers.h"
#include "external/cimgui/cimgui_impl.h"

#include "offset_allocator.h"

#include "external/stb/stb_image.h"
#include <stdint.h>
// Fallback for older Vulkan headers without VK_KHR_shader_non_semantic_info
#ifndef VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_NON_SEMANTIC_INFO_FEATURES_KHR
#define VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_NON_SEMANTIC_INFO_FEATURES_KHR 1000333000
#endif

#ifndef VK_KHR_SHADER_NON_SEMANTIC_INFO_EXTENSION_NAME
#define VK_KHR_SHADER_NON_SEMANTIC_INFO_EXTENSION_NAME "VK_KHR_shader_non_semantic_info"
#endif

#ifndef VK_PHYSICAL_DEVICE_SHADER_NON_SEMANTIC_INFO_FEATURES_KHR
#define VK_PHYSICAL_DEVICE_SHADER_NON_SEMANTIC_INFO_FEATURES_KHR 1
typedef struct VkPhysicalDeviceShaderNonSemanticInfoFeaturesKHR
{
    VkStructureType sType;
    void*           pNext;
    VkBool32        shaderNonSemanticInfo;
} VkPhysicalDeviceShaderNonSemanticInfoFeaturesKHR;
#endif


#define BINDLESS_TEXTURE_BINDING 0
#define BINDLESS_SAMPLER_BINDING 1
#define BINDLESS_STORAGE_IMAGE_BINDING 2
#define BINDLESS_GLOBAL_UBO_BINDING 3
#define MAX_MIPS 16
#define MAX_SWAPCHAIN_IMAGES 8

#define MAX_PIPELINES 256
#define MAX_BINDLESS_SAMPLERS 256
#define MAX_BINDLESS_TEXTURES 65536
#define MAX_BINDLESS_STORAGE_BUFFERS 65536
#define MAX_BINDLESS_UNIFORM_BUFFERS 16384
#define MAX_BINDLESS_STORAGE_IMAGES 16384
#define MAX_BINDLESS_VERTEX_BUFFERS 65536
#define MAX_BINDLESS_INDEX_BUFFERS 65536
#define MAX_BINDLESS_MATERIALS 65536
#define MAX_BINDLESS_TRANSFORMS 65536


typedef uint32_t TextureID;

typedef struct
{
    VkImage       image;
    VkImageView   view;
    VmaAllocation allocation;

    // metadata
    uint32_t width;
    uint32_t height;
    uint32_t mip_count;

    VkFormat format;

    bool valid;
} Texture;

typedef struct
{
    VkInstance               instance;
    VkDebugUtilsMessengerEXT debug_messenger;
    VkAllocationCallbacks*   allocatorcallbacks;
} InstanceContext;


typedef uint32_t SamplerID;
typedef enum DefaultSamplerID
{
    SAMPLER_LINEAR_WRAP = 0,
    SAMPLER_LINEAR_CLAMP,
    SAMPLER_NEAREST_WRAP,
    SAMPLER_NEAREST_CLAMP,
    SAMPLER_LINEAR_WRAP_ANISO,
    SAMPLER_SHADOW,
    SAMPLER_COUNT
} DefaultSamplerID;

typedef struct DefaultSamplerTable
{
    SamplerID samplers[SAMPLER_COUNT];
} DefaultSamplerTable;

typedef struct Buffer
{
    VkBuffer        buffer;
    VkDeviceSize    buffer_size;
    VkDeviceAddress address;
    uint8_t*        mapping;
    VmaAllocation   allocation;
} Buffer;


typedef struct
{
    VkPhysicalDevice physical;
    //warm data
    VkDevice device;

    VkQueue present_queue;
    VkQueue graphics_queue;
    VkQueue compute_queue;
    VkQueue transfer_queue;

    uint32_t present_queue_index;
    uint32_t graphics_queue_index;
    uint32_t compute_queue_index;
    uint32_t transfer_queue_index;


    VkAllocationCallbacks* allocatorcallbacks;

} DeviceContext;

//Touched rarely.

typedef struct VkFeatureChain
{
    VkPhysicalDeviceFeatures2 core;

    VkPhysicalDeviceVulkan11Features v11;
    VkPhysicalDeviceVulkan12Features v12;
    VkPhysicalDeviceVulkan13Features v13;


    // ---- add this ----
    VkPhysicalDeviceMaintenance5FeaturesKHR          maintenance5;
    VkPhysicalDeviceShaderNonSemanticInfoFeaturesKHR shaderNonSemanticInfo;
} VkFeatureChain;


typedef struct
{
    VkPhysicalDevice physical;

    VkPhysicalDeviceProperties       properties;
    VkPhysicalDeviceFeatures         features;
    VkPhysicalDeviceMemoryProperties memory;

    VkFeatureChain feature_chain;

} DeviceInfo;
typedef struct
{

    uint32_t width;
    uint32_t height;

    const char* app_name;

    const char** instance_layers;
    const char** instance_extensions;
    const char** device_extensions;

    uint32_t instance_layer_count;
    uint32_t instance_extension_count;
    uint32_t device_extension_count;
    bool     enable_validation;
    bool     enable_gpu_based_validation;

    VkDebugUtilsMessageSeverityFlagsEXT validation_severity;
    VkDebugUtilsMessageTypeFlagsEXT     validation_types;
    bool                                use_custom_features;
    VkFeatureChain                      custom_features;
    VkPresentModeKHR                    swapchain_preferred_present_mode;
    VkFormat                            swapchain_preferred_format;

    VkColorSpaceKHR swapchain_preferred_color_space;

    VkImageUsageFlags swapchain_extra_usage_flags; /* Additional usage flags */
    bool              vsync;
    bool              enable_debug_printf; /* Enable VK_KHR_shader_non_semantic_info for shader debug printf */
    uint32_t          bindless_sampled_image_count;
    uint32_t          bindless_sampler_count;
    uint32_t          bindless_storage_image_count;
    bool              enable_pipeline_stats;


    VkDeviceSize size_of_cpu_pool;

    VkDeviceSize size_of_gpu_pool;

    VkDeviceSize size_of_staging_pool;
} RendererDesc;


typedef enum ImageStateValidity
{
    IMAGE_STATE_UNDEFINED = 0,
    IMAGE_STATE_VALID     = 1,
    IMAGE_STATE_EXTERNAL  = 2,
} ImageStateValidity;


typedef struct ALIGNAS(32) ImageState
{
    VkPipelineStageFlags2 stage;         // 8
    VkAccessFlags2        access;        // 8
    VkImageLayout         layout;        // 4
    uint32_t              queue_family;  // 4
    ImageStateValidity    validity;      // 4
    uint32_t              dirty_mips;    // 4
} ImageState;


typedef struct ALIGNAS(64) FlowSwapchain
{
    //hot
    ImageState states[MAX_SWAPCHAIN_IMAGES];
    VkImage    images[MAX_SWAPCHAIN_IMAGES];
    uint32_t   current_image;

    VkImageView image_views[MAX_SWAPCHAIN_IMAGES];
    VkSemaphore render_finished[MAX_SWAPCHAIN_IMAGES];
    TextureID   bindless_index[MAX_SWAPCHAIN_IMAGES];

    //cold
    VkSwapchainKHR   swapchain;
    VkSurfaceKHR     surface;
    VkFormat         format;
    VkColorSpaceKHR  color_space;
    VkPresentModeKHR present_mode;
    VkExtent2D       extent;
    uint32_t         image_count;

    VkImageUsageFlags image_usage;

    bool vsync;
    bool needs_recreate;
} FlowSwapchain;


typedef struct FlowSwapchainCreateInfo
{
    VkSurfaceKHR      surface;
    uint32_t          width;
    uint32_t          height;
    uint32_t          min_image_count;
    VkPresentModeKHR  preferred_present_mode;
    VkFormat          preferred_format;
    VkColorSpaceKHR   preferred_color_space; /* VK_COLOR_SPACE_SRGB_NONLINEAR_KHR default */
    VkImageUsageFlags extra_usage;           /* Additional usage flags */
    VkSwapchainKHR    old_swapchain;         /* For recreation */
} FlowSwapchainCreateInfo;

typedef struct
{
    VkCommandBuffer cmdbuf;
    VkCommandPool   cmdbufpool;
    VkSemaphore     image_available_semaphore;
    VkFence         in_flight_fence;
    uint32_t        staging_tail;
} FrameContext;

#pragma once


typedef struct Bindless
{
    VkDescriptorSetLayout set_layout;
    VkDescriptorPool      pool;
    VkDescriptorSet       set;

    VkPipelineLayout pipeline_layout;

} Bindless;


// ────────────────────────────────────────────────────────────────
// Render Targets
// ────────────────────────────────────────────────────────────────

#define RT_MAX_MIPS 13  // covers up to 4096x4096
#define RT_POOL_MAX 64
#define BLOOM_MIPS 5

typedef struct RenderTarget
{
    VkImage       image;
    VmaAllocation allocation;

    VkImageView view;                    // full mip chain
    VkImageView mip_views[RT_MAX_MIPS];  // per-mip views

    VkFormat format;
    uint32_t width;
    uint32_t height;
    uint32_t layers;
    uint32_t mip_count;

    VkImageUsageFlags  usage;
    VkImageAspectFlags aspect;

    ImageState mip_states[RT_MAX_MIPS];

    uint32_t bindless_index;                       // full-chain view for sampled/storage
    uint32_t mip_bindless_index[RT_MAX_MIPS];      // per-mip view indices (sampled/storage)

    const char* debug_name;

} RenderTarget;


typedef enum FrustumPlane
{
    TopPlane,
    BottomPlane,
    LeftPlane,
    RightPlane,
    NearPlane,
    FarPlane,
    FrustumPlaneCount
} FrustumPlane;

typedef struct Frustum
{
    //  ax + by + cz + d = 0
    //  vec =(a,b,c,d)
    vec4 planes[FrustumPlaneCount];
} Frustum;


#define MAX_FRAMES_IN_FLIGHT 3
//
//  shader decides which sampler to use.
//
// This is actually powerful. You can do things like:
//
// same texture
// different samplers
//
// Example:
//
// • albedo texture → linear filtering
// • pixel-art texture → nearest filtering
// • shadow map → comparison sampler

typedef enum BufferPoolType
{
    BUFFER_POOL_LINEAR,
    BUFFER_POOL_RING,
    BUFFER_POOL_TLSF
} BufferPoolType;

typedef struct BufferPool
{
    VkBuffer      buffer;
    VmaAllocation allocation;
    VkDeviceSize  size_bytes;

    void* mapped;

    BufferPoolType type;
    union
    {
        mu_linear_allocator linear;
        mu_ring_allocator   ring;
        OA_Allocator          tlsf;
    };

    // config
    VkBufferUsageFlags       usage;
    VmaMemoryUsage           memory_usage;
    VmaAllocationCreateFlags alloc_flags;
} BufferPool;

typedef struct BarrierBatch
{
    VkImageMemoryBarrier2 image_barriers[32];

    uint32_t image_count;
} BarrierBatch;

void flush_barriers( VkCommandBuffer cmd);

typedef struct
{
  // frame mu
    uint32_t current_frame;
    float    dt;

    FrameContext frames[MAX_FRAMES_IN_FLIGHT];

    // frequently used GPU resources
    Buffer global_ubo[MAX_FRAMES_IN_FLIGHT];

    // barrier system (used constantly now)

//    BarrierBatch barrierbatch;
    // per-frame profiling (borderline hot)
    GpuProfiler gpuprofiler[MAX_FRAMES_IN_FLIGHT];

    // CPU timing (if used per frame)
    double cpu_frame_ns;
    double cpu_active_ns;
    double cpu_wait_ns;
    double      cpu_wait_accum_ns;
    double      cpu_prev_frame;



Frustum frustum; 
    // window + swapchain
    FlowSwapchain swapchain;
    GLFWwindow*   window;

    // GPU device stuff
    VkDevice device;
    VkPhysicalDevice physical_device;

    VkQueue present_queue;
    VkQueue graphics_queue;
    VkQueue compute_queue;
    VkQueue transfer_queue;

    uint32_t present_queue_index;
    uint32_t graphics_queue_index;
    uint32_t compute_queue_index;
    uint32_t transfer_queue_index;

    VkSurfaceKHR surface;

    // memory systems
    VmaAllocator vmaallocator;
    VkAllocationCallbacks* allocatorcallbacks;

    BufferPool cpu_pool;
    BufferPool gpu_pool;
    BufferPool staging_pool;

    // render targets (BIG = cold)
    RenderTarget depth[MAX_SWAPCHAIN_IMAGES];
    RenderTarget hdr_color[MAX_SWAPCHAIN_IMAGES];
    RenderTarget dof_half[MAX_SWAPCHAIN_IMAGES];
    RenderTarget ldr_color[MAX_SWAPCHAIN_IMAGES];
    RenderTarget bloom_chain[MAX_SWAPCHAIN_IMAGES];
    RenderTarget smaa_edges[MAX_SWAPCHAIN_IMAGES];
    RenderTarget smaa_weights[MAX_SWAPCHAIN_IMAGES];

    // pipelines / resources
    struct
    {
        uint32_t smaa_edge;
        uint32_t smaa_weight;
        uint32_t smaa_blend;
    } smaa_pipelines;

    DefaultSamplerTable default_samplers;

    // descriptors
    VkDescriptorPool imgui_pool;

    // textures
    TextureID dummy_texture;
    TextureID smaa_area_tex;
    TextureID smaa_search_tex;

    // misc systems
    Bindless bindless_system;
    InstanceContext instance;


    // command pools (rarely touched per draw)
    VkCommandPool one_time_gfx_pool;
    VkCommandPool transfer_pool;

    // debug / info
    DeviceInfo info;

    // readback (rare)
    Buffer readback_buffer;

    // GPU address (rarely used)
    VkDeviceAddress gpu_base_addr;

} Renderer;
typedef struct BufferSlice
{
    BufferPool*   pool;
    VkBuffer      buffer;
    VkDeviceSize  offset;
    VkDeviceSize  size;
    void*         mapped;
    OA_Allocation allocation;
} BufferSlice;

bool        buffer_pool_init(Renderer* r,

                             BufferPoolType           type,
                             BufferPool*              pool,
                             VkDeviceSize             size_bytes,
                             VkBufferUsageFlags       usage,
                             VmaMemoryUsage           memory_usage,
                             VmaAllocationCreateFlags alloc_flags,
                             oa_uint32                max_allocs);
void        buffer_pool_destroy(Renderer* r, BufferPool* pool);
void        buffer_pool_linear_reset(BufferPool* pool);
void        buffer_pool_ring_free_to(BufferPool* pool, uint32_t offset);
BufferSlice buffer_pool_alloc(BufferPool* pool, VkDeviceSize size_bytes, VkDeviceSize alignment);
void        buffer_pool_free(BufferSlice slice);

/*
Staging frame-lifetime rule:
- Slices allocated from `staging_pool` are transient.
- They remain valid only until the frame using them completes (fence signal).
- Do not cache `mapped` pointers or staging offsets across frames.
*/
bool renderer_upload_buffer_to_slice(Renderer*       r,
                                     VkCommandBuffer cmd,
                                     BufferSlice     dst_slice,
                                     const void*     src_data,
                                     VkDeviceSize    size_bytes,
                                     VkDeviceSize    staging_alignment);

/* Allocates destination from `gpu_pool`, stages upload, records `vkCmdCopyBuffer`, returns destination slice. */
BufferSlice renderer_upload_buffer(Renderer*       r,
                                   VkCommandBuffer cmd,
                                   const void*     src_data,
                                   VkDeviceSize    size_bytes,
                                   VkDeviceSize    staging_alignment,
                                   VkDeviceSize    dst_alignment);

#define MAX_VERTEX_ATTRS 8
typedef enum VertexFormat
{
    FMT_FLOAT,
    FMT_VEC2,
    FMT_VEC3,
    FMT_VEC4,
} VertexFormat;

typedef struct VertexAttr
{
    uint8_t      location;
    VertexFormat format;
} VertexAttr;

typedef struct VertexBinding
{
    uint16_t   stride;
    uint8_t    input_rate;  // VK_VERTEX_INPUT_RATE_VERTEX / INSTANCE
    uint8_t    attr_count;
    VertexAttr attrs[MAX_VERTEX_ATTRS];
} VertexBinding;

#define MAX_COLOR_ATTACHMENTS 8

typedef struct ColorAttachmentBlend
{
    bool blend_enable;

    VkBlendFactor src_color;
    VkBlendFactor dst_color;
    VkBlendOp     color_op;

    VkBlendFactor src_alpha;
    VkBlendFactor dst_alpha;
    VkBlendOp     alpha_op;

    VkColorComponentFlags write_mask;

} ColorAttachmentBlend;


typedef struct GraphicsPipelineConfig
{
    // Rasterization
    //
    const char*     vert_path;
    const char*     frag_path;
    VkCullModeFlags cull_mode;
    VkFrontFace     front_face;
    VkPolygonMode   polygon_mode;

    VkPrimitiveTopology topology;

    bool        depth_test_enable;
    bool        depth_write_enable;
    VkCompareOp depth_compare_op;

    uint32_t        color_attachment_count;
    const VkFormat* color_formats;
    VkFormat        depth_format;

    // Per-attachment blend state
    ColorAttachmentBlend blends[MAX_COLOR_ATTACHMENTS];
    // Vertex input (optional)
    bool          use_vertex_input;
    VertexBinding vertex_binding;

} GraphicsPipelineConfig;
static ColorAttachmentBlend blend_alpha(void)
{
    return (ColorAttachmentBlend){
        .blend_enable = true,

        .src_color = VK_BLEND_FACTOR_SRC_ALPHA,
        .dst_color = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
        .color_op  = VK_BLEND_OP_ADD,

        .src_alpha = VK_BLEND_FACTOR_ONE,
        .dst_alpha = VK_BLEND_FACTOR_ZERO,
        .alpha_op  = VK_BLEND_OP_ADD,

        .write_mask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
    };
}

static ColorAttachmentBlend blend_additive(void)
{
    return (ColorAttachmentBlend){
        .blend_enable = true,

        .src_color = VK_BLEND_FACTOR_ONE,
        .dst_color = VK_BLEND_FACTOR_ONE,
        .color_op  = VK_BLEND_OP_ADD,

        .src_alpha = VK_BLEND_FACTOR_ONE,
        .dst_alpha = VK_BLEND_FACTOR_ONE,
        .alpha_op  = VK_BLEND_OP_ADD,

        .write_mask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
    };
}


static ColorAttachmentBlend blend_disabled(void)
{
    return (ColorAttachmentBlend){
        .blend_enable = false,

        .src_color = VK_BLEND_FACTOR_ONE,
        .dst_color = VK_BLEND_FACTOR_ZERO,
        .color_op  = VK_BLEND_OP_ADD,

        .src_alpha = VK_BLEND_FACTOR_ONE,
        .dst_alpha = VK_BLEND_FACTOR_ZERO,
        .alpha_op  = VK_BLEND_OP_ADD,

        .write_mask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
    };
}
static inline GraphicsPipelineConfig pipeline_config_default(void)
{
    GraphicsPipelineConfig cfg = {0};

    cfg.cull_mode    = VK_CULL_MODE_NONE;
    cfg.front_face   = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    cfg.polygon_mode = VK_POLYGON_MODE_FILL;

    cfg.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    cfg.depth_test_enable  = true;
    cfg.depth_write_enable = true;
    cfg.depth_compare_op   = VK_COMPARE_OP_GREATER;

    cfg.color_attachment_count = 0;
    cfg.color_formats          = NULL;
    cfg.depth_format           = VK_FORMAT_UNDEFINED;

    for(uint32_t i = 0; i < MAX_COLOR_ATTACHMENTS; i++)
        cfg.blends[i] = blend_disabled();

    cfg.use_vertex_input = false;

    return cfg;
}

VkPipeline create_graphics_pipeline(Renderer* renderer, const GraphicsPipelineConfig* cfg);
VkPipeline create_compute_pipeline(Renderer* renderer, const char* compute_path);

void vk_cmd_set_viewport_scissor(VkCommandBuffer cmd, VkExtent2D extent);


typedef enum SwapchainResult
{
    SWAPCHAIN_OK,
    SWAPCHAIN_SUBOPTIMAL,
    SWAPCHAIN_OUT_OF_DATE,
} SwapchainResult;
bool is_instance_extension_supported(const char* extension_name);
void renderer_create(Renderer* r, RendererDesc* desc);

void renderer_destroy(Renderer* r);
void vk_create_swapchain(VkDevice                       device,
                         VkPhysicalDevice               gpu,
                         FlowSwapchain*                 out_swapchain,
                         const FlowSwapchainCreateInfo* info,
                         VkQueue                        graphics_queue,
                         VkCommandPool                  one_time_pool,
                         Renderer*                      r);
void vk_swapchain_destroy(VkDevice device, FlowSwapchain* swapchain, mu_id_pool* id_pool);

void             vk_swapchain_recreate(VkDevice         device,
                                       VkPhysicalDevice gpu,
                                       FlowSwapchain*   sc,
                                       uint32_t         new_w,
                                       uint32_t         new_h,
                                       VkQueue          graphics_queue,
                                       VkCommandPool    one_time_pool,

                                       Renderer* r);
VkPresentModeKHR vk_swapchain_select_present_mode(VkPhysicalDevice physical_device, VkSurfaceKHR surface, bool vsync);


void image_transition_swapchain(VkCommandBuffer cmd, FlowSwapchain* sc, VkImageLayout new_layout, VkPipelineStageFlags2 new_stage, VkAccessFlags2 new_access);

void image_transition_simple(VkCommandBuffer cmd, VkImage image, VkImageAspectFlags aspect, VkImageLayout old_layout, VkImageLayout new_layout);


static inline VkImageSubresourceRange image_subresource_range(VkImageAspectFlags aspect, uint32_t baseMip, uint32_t mipCount)
{
    VkImageSubresourceRange range = {
        .aspectMask = aspect, .baseMipLevel = baseMip, .levelCount = mipCount, .baseArrayLayer = 0, .layerCount = VK_REMAINING_ARRAY_LAYERS};

    return range;
}
void cmd_transition_all_mips(VkCommandBuffer       cmd,
                             VkImage               image,
                             ImageState*           state,
                             VkImageAspectFlags    aspect,
                             uint32_t              mipCount,
                             VkPipelineStageFlags2 newStage,
                             VkAccessFlags2        newAccess,
                             VkImageLayout         newLayout,
                             uint32_t              newQueueFamilyi);

void cmd_transition_mip(VkCommandBuffer       cmd,
                        VkImage               image,
                        ImageState*           state,
                        VkImageAspectFlags    aspect,
                        uint32_t              mip,
                        VkPipelineStageFlags2 newStage,
                        VkAccessFlags2        newAccess,
                        VkImageLayout         newLayout,
                        uint32_t              newQueueFamily);


FORCE_INLINE bool vk_swapchain_acquire(VkDevice device, FlowSwapchain* sc, VkSemaphore image_available, VkFence fence, uint64_t timeout)
{
    ///  PFN_vkAcquireNextImage2KHR
    VkResult r = vkAcquireNextImageKHR(device, sc->swapchain, timeout, image_available, fence, &sc->current_image);

    if(r == VK_SUCCESS)
        return true;

    if(r == VK_SUBOPTIMAL_KHR || r == VK_ERROR_OUT_OF_DATE_KHR)
    {
        sc->needs_recreate = true;
        return false;
    }

    VK_CHECK(r);
    return false;
}

FORCE_INLINE bool vk_swapchain_present(VkQueue present_queue, FlowSwapchain* sc, const VkSemaphore* waits, uint32_t wait_count)
{
    VkPresentInfoKHR info = {.sType              = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
                             .waitSemaphoreCount = wait_count,
                             .pWaitSemaphores    = waits,
                             .swapchainCount     = 1,
                             .pSwapchains        = &sc->swapchain,
                             .pImageIndices      = &sc->current_image};

    VkResult r = vkQueuePresentKHR(present_queue, &info);

    if(r == VK_SUBOPTIMAL_KHR || r == VK_ERROR_OUT_OF_DATE_KHR)
    {
        sc->needs_recreate = true;
        return false;
    }

    VK_CHECK(r);
    return true;
}

//Instance → GPU selection → Query capabilities → Enable features → Create logical device → Store result
//
// Wait for frame fence
// Acquire swapchain image
// Reset frame command buffer
// Record command buffer
// Submit command buffer
// Present swapchain image
// Advance frame index


FORCE_INLINE void imgui_shutdown(void)
{
    ImGui_ImplVulkan_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    igDestroyContext(NULL);
}

FORCE_INLINE void imgui_begin_frame(void)
{
    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    igNewFrame();
}


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
    bool  mouse_captured;
    bool  first_mouse;

    double last_mouse_x;
    double last_mouse_y;

    // unified 2D/3D camera abstraction (backward-compatible with legacy fields above)
    uint32_t mode;        // CameraMode
    uint32_t projection;  // CameraProjection

    // 2D state
    vec2  position2d;
    float zoom;
    float rotation2d;
    float ortho_height_world;
    vec4  bounds2d;  // min_x, min_y, max_x, max_y
    bool  bounds2d_enabled;

    // shared viewport metadata
    uint32_t viewport_width;
    uint32_t viewport_height;

    // cached matrices
    mat4 view;
    mat4 proj;

} Camera;

typedef enum CameraMode
{
    CAMERA_MODE_2D = 0,
    CAMERA_MODE_3D = 1,
} CameraMode;

typedef enum CameraProjection
{
    CAMERA_PROJ_ORTHOGRAPHIC = 0,
    CAMERA_PROJ_PERSPECTIVE  = 1,
} CameraProjection;

static FORCE_INLINE void camera_build_proj_reverse_z_infinite(mat4 out_proj, Camera* cam, float aspect);

static FORCE_INLINE void camera_defaults_3d(Camera* cam)
{
    cam->mode       = CAMERA_MODE_3D;
    cam->projection = CAMERA_PROJ_PERSPECTIVE;

    glm_vec3_copy((vec3){0.0f, 1.8f, 4.0f}, cam->position);
    glm_vec3_copy((vec3){0.0f, 0.0f, -1.0f}, cam->cam_dir);

    cam->yaw        = 0.0f;
    cam->pitch      = 0.0f;
    cam->move_speed = 4.0f;
    cam->look_speed = 0.0025f;
    cam->fov_y      = glm_rad(75.0f);
    cam->near_z     = 0.05f;
    cam->far_z      = 2000.0f;

    cam->mouse_captured = false;
    cam->first_mouse    = true;
    cam->last_mouse_x   = 0.0;
    cam->last_mouse_y   = 0.0;

    cam->viewport_width  = 0;
    cam->viewport_height = 0;

    glm_mat4_identity(cam->view);
    glm_mat4_identity(cam->proj);
    glm_mat4_identity(cam->view_proj);
}

static FORCE_INLINE void camera_defaults_2d(Camera* cam, uint32_t viewport_width, uint32_t viewport_height)
{
    cam->mode       = CAMERA_MODE_2D;
    cam->projection = CAMERA_PROJ_ORTHOGRAPHIC;

    glm_vec2_zero(cam->position2d);
    cam->zoom              = 1.0f;
    cam->rotation2d        = 0.0f;
    cam->ortho_height_world = 10.0f;
    glm_vec4_zero(cam->bounds2d);
    cam->bounds2d_enabled = false;

    cam->near_z = -1.0f;
    cam->far_z  = 1.0f;

    cam->viewport_width  = viewport_width;
    cam->viewport_height = viewport_height;

    glm_mat4_identity(cam->view);
    glm_mat4_identity(cam->proj);
    glm_mat4_identity(cam->view_proj);
}

static FORCE_INLINE void camera_set_mode(Camera* cam, CameraMode mode)
{
    cam->mode = (uint32_t)mode;
}

static FORCE_INLINE void camera_set_projection(Camera* cam, CameraProjection projection)
{
    cam->projection = (uint32_t)projection;
}

static FORCE_INLINE void camera_set_viewport(Camera* cam, uint32_t width, uint32_t height)
{
    cam->viewport_width  = width;
    cam->viewport_height = height;
}

static FORCE_INLINE void camera2d_set_bounds(Camera* cam, float min_x, float min_y, float max_x, float max_y)
{
    cam->bounds2d[0]       = min_x;
    cam->bounds2d[1]       = min_y;
    cam->bounds2d[2]       = max_x;
    cam->bounds2d[3]       = max_y;
    cam->bounds2d_enabled  = true;
}

static FORCE_INLINE void camera2d_clear_bounds(Camera* cam)
{
    cam->bounds2d_enabled = false;
}

static FORCE_INLINE void camera2d_set_position(Camera* cam, float x, float y)
{
    cam->position2d[0] = x;
    cam->position2d[1] = y;
}

static FORCE_INLINE void camera2d_pan(Camera* cam, float dx_world, float dy_world)
{
    cam->position2d[0] += dx_world;
    cam->position2d[1] += dy_world;

    if(cam->bounds2d_enabled)
    {
        cam->position2d[0] = glm_clamp(cam->position2d[0], cam->bounds2d[0], cam->bounds2d[2]);
        cam->position2d[1] = glm_clamp(cam->position2d[1], cam->bounds2d[1], cam->bounds2d[3]);
    }
}

static FORCE_INLINE void camera2d_zoom(Camera* cam, float zoom_delta)
{
    cam->zoom = glm_clamp(cam->zoom + zoom_delta, 0.01f, 100.0f);
}

static FORCE_INLINE void camera3d_set_position(Camera* cam, float x, float y, float z)
{
    cam->position[0] = x;
    cam->position[1] = y;
    cam->position[2] = z;
}

static FORCE_INLINE void camera3d_set_rotation_yaw_pitch(Camera* cam, float yaw, float pitch)
{
    cam->yaw   = yaw;
    cam->pitch = glm_clamp(pitch, -glm_rad(89.0f), glm_rad(89.0f));
}

static FORCE_INLINE void camera_build_proj_standard(mat4 out_proj, const Camera* cam, float aspect)
{
    glm_perspective(cam->fov_y, aspect, cam->near_z, cam->far_z, out_proj);
}

static FORCE_INLINE void camera_extract_frustum(Frustum* out_frustum, const mat4 view_proj)
{
    out_frustum->planes[LeftPlane][0] = view_proj[0][3] + view_proj[0][0];
    out_frustum->planes[LeftPlane][1] = view_proj[1][3] + view_proj[1][0];
    out_frustum->planes[LeftPlane][2] = view_proj[2][3] + view_proj[2][0];
    out_frustum->planes[LeftPlane][3] = view_proj[3][3] + view_proj[3][0];

    out_frustum->planes[RightPlane][0] = view_proj[0][3] - view_proj[0][0];
    out_frustum->planes[RightPlane][1] = view_proj[1][3] - view_proj[1][0];
    out_frustum->planes[RightPlane][2] = view_proj[2][3] - view_proj[2][0];
    out_frustum->planes[RightPlane][3] = view_proj[3][3] - view_proj[3][0];

    out_frustum->planes[BottomPlane][0] = view_proj[0][3] + view_proj[0][1];
    out_frustum->planes[BottomPlane][1] = view_proj[1][3] + view_proj[1][1];
    out_frustum->planes[BottomPlane][2] = view_proj[2][3] + view_proj[2][1];
    out_frustum->planes[BottomPlane][3] = view_proj[3][3] + view_proj[3][1];

    out_frustum->planes[TopPlane][0] = view_proj[0][3] - view_proj[0][1];
    out_frustum->planes[TopPlane][1] = view_proj[1][3] - view_proj[1][1];
    out_frustum->planes[TopPlane][2] = view_proj[2][3] - view_proj[2][1];
    out_frustum->planes[TopPlane][3] = view_proj[3][3] - view_proj[3][1];

    out_frustum->planes[NearPlane][0] = view_proj[0][3] + view_proj[0][2];
    out_frustum->planes[NearPlane][1] = view_proj[1][3] + view_proj[1][2];
    out_frustum->planes[NearPlane][2] = view_proj[2][3] + view_proj[2][2];
    out_frustum->planes[NearPlane][3] = view_proj[3][3] + view_proj[3][2];

    out_frustum->planes[FarPlane][0] = view_proj[0][3] - view_proj[0][2];
    out_frustum->planes[FarPlane][1] = view_proj[1][3] - view_proj[1][2];
    out_frustum->planes[FarPlane][2] = view_proj[2][3] - view_proj[2][2];
    out_frustum->planes[FarPlane][3] = view_proj[3][3] - view_proj[3][2];

    forEach(i, FrustumPlaneCount)
    {
        vec4* p = &out_frustum->planes[i];
        float len = sqrtf((*p)[0] * (*p)[0] + (*p)[1] * (*p)[1] + (*p)[2] * (*p)[2]);
        if(len > 0.0f)
        {
            float inv = 1.0f / len;
            (*p)[0] *= inv;
            (*p)[1] *= inv;
            (*p)[2] *= inv;
            (*p)[3] *= inv;
        }
    }
}

static FORCE_INLINE void camera_update_matrices(Camera* cam, float aspect, bool reverse_z)
{
    if(cam->mode == CAMERA_MODE_2D || cam->projection == CAMERA_PROJ_ORTHOGRAPHIC)
    {
        float clamped_zoom = cam->zoom < 0.01f ? 0.01f : cam->zoom;
        float ortho_height = cam->ortho_height_world / clamped_zoom;
        float half_h       = ortho_height * 0.5f;
        float half_w       = half_h * aspect;

        glm_mat4_identity(cam->view);
        glm_translate(cam->view, (vec3){-cam->position2d[0], -cam->position2d[1], 0.0f});
        glm_rotate(cam->view, -cam->rotation2d, (vec3){0.0f, 0.0f, 1.0f});

        glm_ortho(-half_w, half_w, -half_h, half_h, cam->near_z, cam->far_z, cam->proj);
        cam->proj[1][1] *= -1.0f;

        glm_mat4_mul(cam->proj, cam->view, cam->view_proj);
        return;
    }

    vec3 forward = {
        cosf(cam->pitch) * sinf(cam->yaw),
        sinf(cam->pitch),
        -cosf(cam->pitch) * cosf(cam->yaw),
    };
    glm_vec3_normalize(forward);
    glm_vec3_copy(forward, cam->cam_dir);

    vec3 world_up = {0.0f, 1.0f, 0.0f};
    vec3 right    = {0.0f, 0.0f, 0.0f};
    vec3 up       = {0.0f, 0.0f, 0.0f};
    glm_vec3_cross(forward, world_up, right);
    glm_vec3_normalize(right);
    glm_vec3_cross(right, forward, up);

    vec3 center;
    glm_vec3_add(cam->position, forward, center);
    glm_lookat(cam->position, center, up, cam->view);

    if(reverse_z)
    {
        camera_build_proj_reverse_z_infinite(cam->proj, cam, aspect);
    }
    else
    {
        camera_build_proj_standard(cam->proj, cam, aspect);
    }

    cam->proj[1][1] *= -1.0f;
    glm_mat4_mul(cam->proj, cam->view, cam->view_proj);
}

static FORCE_INLINE void camera_build_proj_reverse_z_infinite(mat4 out_proj, Camera* cam, float aspect)
{
    float f = 1.0f / tanf(cam->fov_y * 0.5f);
    float n = cam->near_z;

    glm_mat4_zero(out_proj);

    out_proj[0][0] = f / aspect;
    out_proj[1][1] = f;

    // Reverse-Z, infinite far
    out_proj[2][2] = 0.0f;
    out_proj[2][3] = -1.0f;

    out_proj[3][2] = n;
    out_proj[3][3] = 0.0f;
}

bool create_buffer(Renderer* r, VkDeviceSize size, VkBufferUsageFlags usage, VmaMemoryUsage memory_usage, Buffer* out);

void destroy_buffer(Renderer* r, Buffer* b);


typedef struct
{
    uint32_t          width;
    uint32_t          height;
    uint32_t          mip_count;
    VkFormat          format;
    VkImageUsageFlags usage;
    const char*       debug_name;
} TextureCreateDesc;

TextureID create_texture(Renderer* r, const TextureCreateDesc* desc);
void      destroy_texture(Renderer* r, TextureID id);

/* Texture upload helper kept separate from generic buffer uploads due to image row/layout constraints. */
bool renderer_upload_texture_2d(Renderer*       r,
                                VkCommandBuffer cmd,
                                Texture*        tex,
                                const void*     pixels,
                                VkDeviceSize    size_bytes,
                                uint32_t        width,
                                uint32_t        height,
                                uint32_t        mip_level);


TextureID load_texture(Renderer* r, const char* path);

TextureID load_texture_id_in_range(Renderer* r, const char* path, uint32_t max_id);
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

SamplerID create_sampler(Renderer* r, const SamplerCreateDesc* desc);
void      destroy_sampler(Renderer* r, SamplerID id);


MU_INLINE void rt_transition_mip(VkCommandBuffer cmd, RenderTarget* rt, uint32_t mip, VkImageLayout new_layout, VkPipelineStageFlags2 new_stage, VkAccessFlags2 new_access

)
{
    assert(mip < rt->mip_count);
    cmd_transition_mip(cmd, rt->image, &rt->mip_states[mip], rt->aspect, mip, new_stage, new_access, new_layout, VK_QUEUE_FAMILY_IGNORED);
}

MU_INLINE void rt_transition_all(VkCommandBuffer cmd, RenderTarget* rt, VkImageLayout new_layout, VkPipelineStageFlags2 new_stage, VkAccessFlags2 new_access)
{
    for(uint32_t mip = 0; mip < rt->mip_count; mip++)
    {
        ImageState* s = &rt->mip_states[mip];
        // Skip if already in target state
        if(s->validity == IMAGE_STATE_VALID && s->stage == new_stage && s->access == new_access && s->layout == new_layout)
        {
            continue;
        }
        cmd_transition_mip(cmd, rt->image, s, rt->aspect, mip, new_stage, new_access, new_layout, VK_QUEUE_FAMILY_IGNORED);
    }
}


typedef uint32_t RenderTargetID;
typedef struct RenderTargetSpec
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


bool rt_create(Renderer* r, RenderTarget* rt, const RenderTargetSpec* spec);
bool rt_resize(Renderer* r, RenderTarget* rt, uint32_t width, uint32_t height);
void rt_destroy(Renderer* r, RenderTarget* rt);


static inline uint64_t time_now_ns()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec;
}
#define RUN_ONCE for(static int _once = 1; _once; _once = 0)

#define PUSH_CONSTANT(name, BODY)                                                                                      \
    typedef struct ALIGNAS(16) name##_init                                                                             \
    {                                                                                                                  \
        BODY                                                                                                           \
    } name##_init;                                                                                                     \
    enum                                                                                                               \
    {                                                                                                                  \
        name##_pad_size = 256 - sizeof(name##_init)                                                                    \
    };                                                                                                                 \
                                                                                                                       \
    typedef struct ALIGNAS(16) name                                                                                    \
    {                                                                                                                  \
        BODY uint8_t _pad[name##_pad_size];                                                                            \
    } name;                                                                                                            \
                                                                                                                       \
    _Static_assert(sizeof(name) == 256, "Push constant != 256");


static MU_INLINE void frame_start(Renderer* renderer, Camera* cam)
{
    TracyCZoneNC(ctx, "frame_start", 0x00FF00, 1);
    renderer->current_frame = (renderer->current_frame + 1) % MAX_FRAMES_IN_FLIGHT;
    uint64_t now            = time_now_ns();

    renderer->cpu_frame_ns  = now - renderer->cpu_prev_frame;
    renderer->cpu_wait_ns   = renderer->cpu_wait_accum_ns;
    renderer->cpu_active_ns = renderer->cpu_frame_ns - renderer->cpu_wait_ns;
    if(renderer->cpu_active_ns < 0.0)
        renderer->cpu_active_ns = 0.0;
    renderer->cpu_wait_accum_ns = 0.0;
    renderer->cpu_prev_frame    = now;


    renderer->dt = (float)renderer->cpu_frame_ns * 1e-9f;

    float dt = renderer->dt;
    glfwPollEvents();


    int fb_w, fb_h;
    glfwGetFramebufferSize(renderer->window, &fb_w, &fb_h);

    renderer->swapchain.needs_recreate |=
        fb_w != (int)renderer->swapchain.extent.width || fb_h != (int)renderer->swapchain.extent.height;

    if(cam)
    {
        camera_set_viewport(cam, (uint32_t)fb_w, (uint32_t)fb_h);

        if(cam->mode == CAMERA_MODE_3D)
        {
            if(glfwGetMouseButton(renderer->window, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS)
            {
                if(!cam->mouse_captured)
                {
                    glfwSetInputMode(renderer->window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
                    cam->mouse_captured = true;
                    cam->first_mouse    = true;
                }
            }
            else if(cam->mouse_captured)
            {
                glfwSetInputMode(renderer->window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
                cam->mouse_captured = false;
                cam->first_mouse    = true;
            }

            if(cam->mouse_captured)
            {
                double xpos, ypos;
                glfwGetCursorPos(renderer->window, &xpos, &ypos);

                if(cam->first_mouse)
                {
                    cam->last_mouse_x = xpos;
                    cam->last_mouse_y = ypos;
                    cam->first_mouse  = false;
                }

                float dx = (float)(xpos - cam->last_mouse_x);
                float dy = (float)(ypos - cam->last_mouse_y);

                cam->last_mouse_x = xpos;
                cam->last_mouse_y = ypos;

                cam->yaw += dx * cam->look_speed;
                cam->pitch -= dy * cam->look_speed;

                float limit = glm_rad(89.0f);
                cam->pitch  = glm_clamp(cam->pitch, -limit, limit);
            }

            vec3 forward = {
                cosf(cam->pitch) * sinf(cam->yaw),
                sinf(cam->pitch),
                -cosf(cam->pitch) * cosf(cam->yaw),
            };
            glm_vec3_normalize(forward);

            vec3 world_up = {0.0f, 1.0f, 0.0f};
            vec3 right    = {0.0f, 0.0f, 0.0f};
            glm_vec3_cross(forward, world_up, right);
            glm_vec3_normalize(right);

            float speed = cam->move_speed;
            if(glfwGetKey(renderer->window, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS)
                speed *= 3.0f;

            vec3 delta = {0.0f, 0.0f, 0.0f};
            if(glfwGetKey(renderer->window, GLFW_KEY_W) == GLFW_PRESS)
                glm_vec3_muladds(forward, speed * dt, delta);
            if(glfwGetKey(renderer->window, GLFW_KEY_S) == GLFW_PRESS)
                glm_vec3_muladds(forward, -speed * dt, delta);
            if(glfwGetKey(renderer->window, GLFW_KEY_D) == GLFW_PRESS)
                glm_vec3_muladds(right, speed * dt, delta);
            if(glfwGetKey(renderer->window, GLFW_KEY_A) == GLFW_PRESS)
                glm_vec3_muladds(right, -speed * dt, delta);
            if(glfwGetKey(renderer->window, GLFW_KEY_E) == GLFW_PRESS)
                glm_vec3_muladds(world_up, speed * dt, delta);
            if(glfwGetKey(renderer->window, GLFW_KEY_Q) == GLFW_PRESS)
                glm_vec3_muladds(world_up, -speed * dt, delta);

            glm_vec3_add(cam->position, delta, cam->position);
            glm_vec3_copy(forward, cam->cam_dir);
        }
    }

    /* ----------- window minimized ----------- */

    if(fb_w == 0 || fb_h == 0)
    {
        uint64_t wait_start = time_now_ns();
        glfwWaitEvents();
        renderer->cpu_wait_accum_ns += (double)(time_now_ns() - wait_start);
        return;
    }

    if(renderer->swapchain.needs_recreate)
    {
        uint64_t wait_start = time_now_ns();
        vkDeviceWaitIdle(renderer->device);
        renderer->cpu_wait_accum_ns += (double)(time_now_ns() - wait_start);

        vk_swapchain_recreate(renderer->device, renderer->physical_device, &renderer->swapchain, fb_w, fb_h,
                              renderer->graphics_queue, renderer->one_time_gfx_pool, renderer);

        forEach(i, renderer->swapchain.image_count)
        {
            rt_resize(renderer, &renderer->depth[i], fb_w, fb_h);

            rt_resize(renderer, &renderer->hdr_color[i], fb_w, fb_h);
            rt_resize(renderer, &renderer->dof_half[i], MAX(1u, fb_w / 2), MAX(1u, fb_h / 2));
        
            rt_resize(renderer, &renderer->ldr_color[i], fb_w, fb_h);

           rt_resize(renderer, &renderer->bloom_chain[i], fb_w, fb_h);

	}


        renderer->swapchain.needs_recreate = false;
    }

    if(cam)
    {
        float aspect = (float)renderer->swapchain.extent.width / (float)renderer->swapchain.extent.height;
        if(aspect <= 0.0f)
            aspect = 1.0f;

        camera_update_matrices(cam, aspect, true);
        camera_extract_frustum(&renderer->frustum, cam->view_proj);
    }

    {
        static uint32_t s_global_frame_count = 0;
        GlobalData      global_data          = {0};

        glm_mat4_identity(global_data.view);
        glm_mat4_identity(global_data.projection);
        glm_mat4_identity(global_data.viewproj);
        glm_mat4_identity(global_data.inv_view);
        glm_mat4_identity(global_data.inv_projection);
        glm_mat4_identity(global_data.inv_viewproj);

        if(cam)
        {
            glm_mat4_copy(cam->view, global_data.view);
            glm_mat4_copy(cam->proj, global_data.projection);
            glm_mat4_copy(cam->view_proj, global_data.viewproj);

            glm_mat4_inv(cam->view, global_data.inv_view);
            glm_mat4_inv(cam->proj, global_data.inv_projection);
            glm_mat4_inv(cam->view_proj, global_data.inv_viewproj);

            global_data.camera_pos[0] = cam->position[0];
            global_data.camera_pos[1] = cam->position[1];
            global_data.camera_pos[2] = cam->position[2];
            global_data.camera_pos[3] = cam->fov_y;

            global_data.camera_dir[0] = cam->cam_dir[0];
            global_data.camera_dir[1] = cam->cam_dir[1];
            global_data.camera_dir[2] = cam->cam_dir[2];
            global_data.camera_dir[3] = (float)renderer->swapchain.extent.width
                                        / (float)MAX(renderer->swapchain.extent.height, 1u);
        }

        global_data.time        = (float)((double)now * 1e-9);
        global_data.delta_time  = renderer->dt;
        global_data.frame_count = s_global_frame_count++;
        global_data.pad         = 0;

        Buffer* frame_ubo = &renderer->global_ubo[renderer->current_frame];
        if(frame_ubo->mapping)
            memcpy(frame_ubo->mapping, &global_data, sizeof(global_data));

        VkDescriptorBufferInfo ubo_info = {
            .buffer = frame_ubo->buffer,
            .offset = 0,
            .range  = sizeof(GlobalData),
        };

        VkWriteDescriptorSet write = {
            .sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet          = renderer->bindless_system.set,
            .dstBinding      = BINDLESS_GLOBAL_UBO_BINDING,
            .dstArrayElement = 0,
            .descriptorCount = 1,
            .descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
            .pBufferInfo     = &ubo_info,
        };

        vkUpdateDescriptorSets(renderer->device, 1, &write, 0, NULL);
    }

    /* -------- frame sync -------- */

    uint64_t wait_start = time_now_ns();
    vkWaitForFences(renderer->device, 1, &renderer->frames[renderer->current_frame].in_flight_fence, VK_TRUE, UINT64_MAX);
    renderer->cpu_wait_accum_ns += (double)(time_now_ns() - wait_start);

    vkResetFences(renderer->device, 1, &renderer->frames[renderer->current_frame].in_flight_fence);

    FrameContext* frame = &renderer->frames[renderer->current_frame];

    buffer_pool_linear_reset(&renderer->cpu_pool);
    buffer_pool_ring_free_to(&renderer->staging_pool, frame->staging_tail);

    GpuProfiler* frame_prof = &renderer->gpuprofiler[renderer->current_frame];

    wait_start = time_now_ns();
    gpu_profiler_collect(frame_prof, renderer->device);
    renderer->cpu_wait_accum_ns += (double)(time_now_ns() - wait_start);

    vkResetCommandPool(renderer->device, renderer->frames[renderer->current_frame].cmdbufpool, 0);

    wait_start = time_now_ns();
    vk_swapchain_acquire(renderer->device, &renderer->swapchain,
                         renderer->frames[renderer->current_frame].image_available_semaphore, VK_NULL_HANDLE, UINT64_MAX);
    renderer->cpu_wait_accum_ns += (double)(time_now_ns() - wait_start);

    TracyCZoneEnd(ctx);
}


static MU_INLINE void submit_frame(Renderer* r)
{
    TracyCZoneNC(ctx, "submit_frame", 0xFF0000, 1);
    FrameContext* f   = &r->frames[r->current_frame];
    uint32_t      img = r->swapchain.current_image;

    if(r->staging_pool.type == BUFFER_POOL_RING)
        f->staging_tail = r->staging_pool.ring.head;

    VkCommandBufferSubmitInfo cmd = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, .commandBuffer = f->cmdbuf};

    VkSemaphoreSubmitInfo wait = {.sType     = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
                                  .semaphore = f->image_available_semaphore,
                                  .stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT};

    VkSemaphoreSubmitInfo signal = {.sType     = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
                                    .semaphore = r->swapchain.render_finished[img],
                                    .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT};

    VkSubmitInfo2 submit = {.sType                    = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
                            .waitSemaphoreInfoCount   = 1,
                            .pWaitSemaphoreInfos      = &wait,
                            .commandBufferInfoCount   = 1,
                            .pCommandBufferInfos      = &cmd,
                            .signalSemaphoreInfoCount = 1,
                            .pSignalSemaphoreInfos    = &signal};

    VK_CHECK(vkQueueSubmit2(r->graphics_queue, 1, &submit, f->in_flight_fence));

    uint64_t wait_start = time_now_ns();
    vk_swapchain_present(r->present_queue, &r->swapchain, &r->swapchain.render_finished[r->swapchain.current_image], 1);
    r->cpu_wait_accum_ns += (double)(time_now_ns() - wait_start);

    TracyCZoneEnd(ctx);
}


bool sampler_create(Renderer* r, const VkSamplerCreateInfo* ci, uint32_t* out_sampler_id);


void renderer_record_screenshot(Renderer* r, VkCommandBuffer cmd);


void renderer_save_screenshot(Renderer* r);

typedef enum PipelineType
{
    PIPELINE_TYPE_GRAPHICS,
    PIPELINE_TYPE_COMPUTE
} PipelineType;

typedef struct PipelineEntry
{
    PipelineType type;

    union
    {
        GraphicsPipelineConfig graphics;

        struct
        {
            const char* path;
        } compute;
    };

    bool dirty;

} PipelineEntry;

typedef struct RendererPipelines
{
    VkPipeline    pipelines[MAX_PIPELINES];
    PipelineEntry entries[MAX_PIPELINES];
    uint32_t      count;
} RendererPipelines;


typedef uint32_t         PipelineID;
extern mu_id_pool      texture_pool;
extern mu_id_pool      sampler_pool;
extern Texture           textures[MAX_BINDLESS_TEXTURES];  // reference by textureid
extern VkSampler         samplers[MAX_BINDLESS_SAMPLERS];  // reference by samplerid
extern RendererPipelines g_render_pipelines;

PipelineID pipeline_create_graphics(Renderer* r, GraphicsPipelineConfig* cfg);
PipelineID pipeline_create_compute(Renderer* r, const char* path);

VkPipeline pipeline_get(PipelineID id);

void pipeline_mark_dirty(const char* changed_shader);
void pipeline_rebuild(Renderer* r);

void pipeline_cache_save(VkDevice device, VkPhysicalDevice phys, VkPipelineCache cache, const char* path);











