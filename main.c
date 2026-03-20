#include "external/debugbreak/debugbreak.h"
#include "tinytypes.h"
#include "flow/flow.h"
#include "vk_default.h"
#include "vk.h"
#include <GLFW/glfw3.h>
#include <dirent.h>
#include <stdalign.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <vulkan/vulkan_core.h>
#include "stb/stb_perlin.h"
#include "voxel.h"
#define DMON_IMPL
#include "external/dmon/dmon.h"
#include "external/tracy/public/tracy/TracyC.h"

#define RUN_ONCE_N(name) for(static bool name = true; name; name = false)
static bool voxel_debug     = true;
static bool take_screenshot = true;
static bool wireframe_mode  = false;
#define VALIDATION false
// imp gpu validation shows false positives may be bacause of data races

#define PRINT_FIELD(type, field)                                                                                       \
    printf("%-20s offset = %3zu  align = %2zu  size = %2zu\n", #field, offsetof(type, field),                          \
           _Alignof(((type*)0)->field), sizeof(((type*)0)->field))

#define PRINT_STRUCT(type) printf("\nSTRUCT %-20s size = %zu  align = %zu\n\n", #type, sizeof(type), _Alignof(type));

static inline size_t flow_ravel_index(const size_t* coord, const size_t* strides, size_t ndim)
{
    size_t index = 0;

    for(size_t i = 0; i < ndim; i++)
        index += coord[i] * strides[i];

    return index;
}


static inline void flow_unravel_index(size_t index, const size_t* dims, size_t ndim, size_t* coord)
{
    for(int i = ndim - 1; i >= 0; i--)
    {
        coord[i] = index % dims[i];
        index /= dims[i];
    }
}

static GPUMaterial gpu_materials[VOXEL_COUNT];
void               voxel_materials_init(Renderer* r)
{


    for(size_t i = 0; i < VOXEL_COUNT; i++)
    {
        VoxelMaterial* src = &voxel_materials[i];
        GPUMaterial*   dst = &gpu_materials[i];

        dst->tex_side   = 0;
        dst->tex_top    = 0;
        dst->tex_bottom = 0;

        if(src->face_tex[TEX_SIDE])
            dst->tex_side = load_texture(r, src->face_tex[TEX_SIDE]);

        if(src->face_tex[TEX_TOP])
            dst->tex_top = load_texture(r, src->face_tex[TEX_TOP]);

        if(src->face_tex[TEX_BOTTOM])
            dst->tex_bottom = load_texture(r, src->face_tex[TEX_BOTTOM]);
    }
}


typedef struct
{
    vec3  position;
    float radius;

    vec3  direction;
    float angle;

    vec3  color;
    float intensity;
} SpotLight;
typedef struct LightBeam
{
    float pos_width[4];   // xyz = position, w = width
    float sun_height[4];  // xyz = sun_dir,  w = height
    float misc[4];        // x = opacity
} LightBeam;
/*
 if(editing mode of light)
 {
may be just cast a ray from mouse pointer to screen and if it matches then activate cimgui gizmo to capture stuff from it

and also there should be save position to file option for that light so that     we can them just load it from from when editing isnt enabled and dmon watches for file changes anyway
 }

*/


typedef struct
{
    uint32_t fullscreen;
    uint32_t postprocess;
    uint32_t triangle;
    uint32_t triangle_wireframe;

    uint32_t beam;
    uint32_t sky;
} EnginePipelines;

static EnginePipelines pipelines;
static bool            upload_once_done = false;


static const VoxelType terrain_voxels[] = {VOXEL_STONE, VOXEL_GRASS

};
typedef struct
{
    float pos[3];
    float uv[2];
} Vertex;


static volatile bool shader_changed = false;
static char          changed_shader[256];
static void inline watch_cb(dmon_watch_id id, dmon_action action, const char* root, const char* filepath, const char* oldfilepath, void* user)
{
    if(action == DMON_ACTION_MODIFY || action == DMON_ACTION_CREATE)
    {
        if(strstr(filepath, ".slang"))
        {
            snprintf(changed_shader, sizeof(changed_shader), "%s", filepath);
            shader_changed = true;
        }
    }
}

/* voxel part starts  */

// instance data (packed)
//         │
//         ▼
// vertex shader
//         │
//         ├─ unpack bits
//         ├─ build face quad
//         ├─ rotate by normal
//         └─ add chunk position

// 31   26   21   16   13   10   7        0
// +----+----+----+----+----+----+-------------------+
// | x  | y  | z  | n  | h  | w  |material/texture id|
// +----+----+----+----+----+----+-------------------+
//  5b   5b   5b   3b   4b   4b      6b
//
//
static inline uint32_t pack_voxel_face(uint32_t x, uint32_t y, uint32_t z, uint32_t normal, uint32_t height, uint32_t width, uint32_t material)
{
    return ((x & 31) << 27) | ((y & 31) << 22) | ((z & 31) << 17) | ((normal & 7) << 14) | ((height & 15) << 10)
           | ((width & 15) << 6) | ((material & 63));
}


#define CHUNK_SIZE 32
#define CHUNK_AREA (CHUNK_SIZE * CHUNK_SIZE)
#define CHUNK_VOLUME (CHUNK_SIZE * CHUNK_SIZE * CHUNK_SIZE)

#define FLOW_FOR_3D(x, y, z, sx, sy, sz)                                                                               \
    for(size_t z = 0; z < (sz); z++)                                                                                   \
        for(size_t y = 0; y < (sy); y++)                                                                               \
            for(size_t x = 0; x < (sx); x++)

static FORCE_INLINE int voxel_index(int x, int y, int z)
{
    return x + y * CHUNK_SIZE + z * CHUNK_AREA;
}

static const int voxel_neighbors[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};


void generate_chunk(Voxel* chunk)
{
    for(int z = 0; z < CHUNK_SIZE; z++)
        for(int x = 0; x < CHUNK_SIZE; x++)
        {
            float wx = x;
            float wz = z;

            float h = stb_perlin_noise3(wx * 0.05f, 0, wz * 0.05f, 0, 0, 0);

            int height = (int)((h * 0.5f + 0.5f) * 20) + 10;

            for(int y = 0; y < CHUNK_SIZE; y++)
            {
                int idx = voxel_index(x, y, z);

                if(y < height - 1)
                    chunk[idx].type = VOXEL_STONE;
                else if(y == height - 1)
                    chunk[idx].type = VOXEL_DIAMOND_BLOCK;
                else
                    chunk[idx].type = VOXEL_AIR;
            }
        }
}

/* voxel part ends  */


int main()
{
    VK_CHECK(volkInitialize());
    if(!is_instance_extension_supported("VK_KHR_wayland_surface"))
        glfwInitHint(GLFW_PLATFORM, GLFW_PLATFORM_X11);
    else
        glfwInitHint(GLFW_PLATFORM, GLFW_PLATFORM_WAYLAND);
    glfwInit();
    const char* dev_exts[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME, VK_EXT_ROBUSTNESS_2_EXTENSION_NAME};

    u32          glfw_ext_count = 0;
    const char** glfw_exts      = glfwGetRequiredInstanceExtensions(&glfw_ext_count);
    Renderer     renderer       = {0};

    {
        RendererDesc desc = {
            .app_name            = "My Renderer",
            .instance_layers     = NULL,
            .instance_extensions = glfw_exts,
            .device_extensions   = dev_exts,

            .instance_layer_count        = 0,
            .instance_extension_count    = glfw_ext_count,
            .device_extension_count      = 2,
            .enable_gpu_based_validation = VALIDATION,
            .enable_validation           = VALIDATION,

            .validation_severity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT
                                   | VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
            .validation_types = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT
                                | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT,
            .width  = 1362,
            .height = 749,

            .swapchain_preferred_color_space = VK_COLORSPACE_SRGB_NONLINEAR_KHR,
            .swapchain_preferred_format      = VK_FORMAT_B8G8R8A8_SRGB,
            .swapchain_extra_usage_flags     = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT
                                           | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,  // src for reading raw pixels
            .vsync               = false,
            .enable_debug_printf = false,  // Enable shader debug printf

            .bindless_sampled_image_count     = 65536,
            .bindless_sampler_count           = 256,
            .bindless_storage_image_count     = 16384,
            .enable_pipeline_stats            = false,
            .swapchain_preferred_present_mode = VK_PRESENT_MODE_IMMEDIATE_KHR,

            .size_of_cpu_pool     = MB(32),
            .size_of_gpu_pool     = MB(512),
            .size_of_staging_pool = MB(128),

        };

        renderer_create(&renderer, &desc);

        {
            GraphicsPipelineConfig cfg = pipeline_config_default();
            cfg.vert_path              = "compiledshaders/minimal_proc.vert.spv";
            cfg.frag_path              = "compiledshaders/minimal_proc.frag.spv";
            cfg.color_attachment_count = 1;
            cfg.color_formats          = &renderer.hdr_color[1].format;
            cfg.depth_test_enable      = false;
            cfg.depth_write_enable     = false;
            pipelines.fullscreen       = pipeline_create_graphics(&renderer, &cfg);
        }
        pipelines.postprocess = pipeline_create_compute(&renderer, "compiledshaders/postprocess.comp.spv");
        {
            GraphicsPipelineConfig cfg = pipeline_config_default();
            cfg.vert_path              = "compiledshaders/triangle.vert.spv";
            cfg.frag_path              = "compiledshaders/triangle.frag.spv";
            cfg.color_attachment_count = 1;
            cfg.color_formats          = &renderer.hdr_color[1].format;
            cfg.depth_format           = renderer.depth[1].format;
            cfg.polygon_mode           = VK_POLYGON_MODE_FILL;
            cfg.topology               = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
            pipelines.triangle         = pipeline_create_graphics(&renderer, &cfg);
        }
        {
            GraphicsPipelineConfig cfg   = pipeline_config_default();
            cfg.vert_path                = "compiledshaders/triangle.vert.spv";
            cfg.frag_path                = "compiledshaders/triangle.frag.spv";
            cfg.color_attachment_count   = 1;
            cfg.color_formats            = &renderer.hdr_color[1].format;
            cfg.depth_format             = renderer.depth[1].format;
            cfg.polygon_mode             = VK_POLYGON_MODE_LINE;
            cfg.topology                 = VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
            pipelines.triangle_wireframe = pipeline_create_graphics(&renderer, &cfg);
        }

        {

            GraphicsPipelineConfig beam = pipeline_config_default();

            beam.vert_path = "compiledshaders/light_beam.vert.spv";
            beam.frag_path = "compiledshaders/light_beam.frag.spv";

            beam.cull_mode              = VK_CULL_MODE_NONE;  // beams must be visible from both sides
            beam.depth_test_enable      = true;               // prevents beams behind geometry
            beam.depth_write_enable     = false;              // never write depth for transparent objects
            beam.color_attachment_count = 1;
            beam.color_formats          = &renderer.hdr_color[0].format;
            beam.depth_format           = renderer.depth[1].format;
            beam.blends[0]              = (ColorAttachmentBlend){.blend_enable = true,
                                                                 .src_color    = VK_BLEND_FACTOR_SRC_ALPHA,
                                                                 .dst_color    = VK_BLEND_FACTOR_ONE,
                                                                 .color_op     = VK_BLEND_OP_ADD,
                                                                 .src_alpha    = VK_BLEND_FACTOR_ONE,
                                                                 .dst_alpha    = VK_BLEND_FACTOR_ONE,
                                                                 .alpha_op     = VK_BLEND_OP_ADD,
                                                                 .write_mask   = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT
                                                                               | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT};

            pipelines.beam = pipeline_create_graphics(&renderer, &beam);

            // Blending
            // Use additive blending.
            //
            // finalColor = beamColor * alpha + sceneColor
            //
            // That gives the glowing light effect.
            //
            // Typical blend setup:
            //
            // src = SRC_ALPHA
            // dst = ONE
            //
            // Additive blending works better for light than standard alpha blending because light adds energy instead of blocking it.
        }


        {
        }
    }
    // Sky pipeline – fullscreen quad, no depth test/write
    {
        GraphicsPipelineConfig cfg = pipeline_config_default();
        cfg.vert_path              = "compiledshaders/sky.vert.spv";
        cfg.frag_path              = "compiledshaders/sky.frag.spv";
        cfg.color_attachment_count = 1;
        cfg.color_formats          = &renderer.hdr_color[1].format;
        cfg.depth_format           = renderer.depth[1].format;
        cfg.depth_test_enable      = false;
        cfg.depth_write_enable     = false;
        cfg.topology               = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
        pipelines.sky              = pipeline_create_graphics(&renderer, &cfg);
    }
    TextureID tex_id = load_texture(&renderer, "/home/lk/myprojects/flowgame/data/PNG/Tiles/greystone.png");

    /* buffer slices start  */

    BufferSlice indirect_slice = buffer_pool_alloc(&renderer.cpu_pool, sizeof(VkDrawIndirectCommand), 16);

    BufferSlice count_slice = buffer_pool_alloc(&renderer.cpu_pool, sizeof(uint32_t), 4);
    /* initialise voxel face storage then fill it with data and upload on gpu */


    voxel_materials_init(&renderer);
    BufferSlice mat_staging = buffer_pool_alloc(&renderer.staging_pool, sizeof(gpu_materials), 16);

    memcpy(mat_staging.mapped, gpu_materials, sizeof(gpu_materials));
    BufferSlice material_slice = buffer_pool_alloc(&renderer.gpu_pool, sizeof(gpu_materials), 16);


    static Voxel chunk[CHUNK_VOLUME];

    uint32_t* faces = NULL;

    generate_chunk(chunk);
    FLOW_FOR_3D(x, y, z, CHUNK_SIZE, CHUNK_SIZE, CHUNK_SIZE)
    {
        size_t idx = voxel_index(x, y, z);

        VoxelType type = chunk[idx].type;

        if(type == VOXEL_AIR)
            continue;

        for(int n = 0; n < 6; n++)
        {
            int nx = x + voxel_neighbors[n][0];
            int ny = y + voxel_neighbors[n][1];
            int nz = z + voxel_neighbors[n][2];

            bool visible = false;

            if(nx < 0 || ny < 0 || nz < 0 || nx >= CHUNK_SIZE || ny >= CHUNK_SIZE || nz >= CHUNK_SIZE)
            {
                visible = true;
            }
            else
            {
                int nidx = voxel_index(nx, ny, nz);

                if(chunk[nidx].type == VOXEL_AIR)
                    visible = true;
            }

            if(visible)
            {
                uint32_t packed = pack_voxel_face(x, y, z, n, 1, 1, type);

                arrput(faces, packed);
            }
        }
    }

    uint32_t    voxel_face_count = arrlen(faces);
    BufferSlice cpu_faces        = buffer_pool_alloc(&renderer.cpu_pool, voxel_face_count * sizeof(uint32_t), 4);

    uint32_t* cpu_face_data = cpu_faces.mapped;
    memcpy(cpu_faces.mapped, faces, voxel_face_count * sizeof(uint32_t));


    BufferSlice gpu_faces = buffer_pool_alloc(&renderer.gpu_pool, voxel_face_count * sizeof(uint32_t), 16);
    /* buffer slices ends  */

    VkDrawIndirectCommand* cpu_indirect = (VkDrawIndirectCommand*)indirect_slice.mapped;
    uint32_t*              cpu_count    = (uint32_t*)count_slice.mapped;


    // describe ONE draw call
    cpu_indirect[0].vertexCount   = 4;
    cpu_indirect[0].instanceCount = voxel_face_count;
    cpu_indirect[0].firstVertex   = 0;
    cpu_indirect[0].firstInstance = 0;

    // number of draws
    *cpu_count = 1;
#if 0
#define MAX_LIGHT_BEAM 64
    BufferSlice     light_beam = buffer_pool_alloc(&pool, MAX_LIGHT_BEAM * sizeof(LightBeam), 16);
    LightBeam*      cpu_beams  = (LightBeam*)light_beam.mapped;
    VkDeviceAddress beam_addr  = base_addr + light_beam.offset;


    u32 beam_count = 64;
    {
        const float beam_width  = 1.75f;
        const float beam_height = 14.0f;
        const float beam_alpha  = 1.25f;

        for(u32 i = 0; i < beam_count; ++i)
        {
            float x = 4.0f + (float)(i % 4) * 6.0f;
            float z = 2.0f + (float)(i / 4) * 6.0f;

            cpu_beams[i] = (LightBeam){
                .pos_width  = {x, 9.0f, z, beam_width},
                .sun_height = {0.12f, -1.0f, 0.08f, beam_height},
                .misc       = {beam_alpha, 0.0f, 0.0f, 0.0f},
            };
        }
    }
#endif
    /* device address */

    Camera cam = {

        .position   = {33.0f, 55.3f, 53.6f},
        .yaw        = glm_rad(5.7f),
        .pitch      = glm_rad(0.0f),
        .move_speed = 3.0f,
        .look_speed = 0.0025f,
        .fov_y      = glm_rad(75.0f),
        .near_z     = 0.05f,
        .far_z      = 2000.0f,

        .view_proj = GLM_MAT4_IDENTITY_INIT,
    };
    {
        glm_vec3_copy((vec3){11.0f, 3.3f, 8.6f}, cam.position);
    }
    glfwSetInputMode(renderer.window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);


    /* Push layout shared with older geometry shaders
         face_ptr=0, face_count=8, aspect=12, pad0=16, pad1=20, pad2=24, pad3=28,
         view_proj=32, texture_id=96, sampler_id=100 */
    // may be all push constant can be defined in one header that both gpu and cpu share


    VkDeviceAddress face_pointer     = renderer.gpu_base_addr + gpu_faces.offset;
    VkDeviceAddress material_pointer = renderer.gpu_base_addr + material_slice.offset;

    PUSH_CONSTANT(Push, VkDeviceAddress face_ptr;            //8
                  VkDeviceAddress mat_ptr; uint face_count;  //
                  float                         aspect;      //4

                  float _pad0[2];  // 24 → 32
                  vec3  cam_pos;   // camera world position
                  uint  pad1;      // alignment
                  vec3  cam_dir;   // camera forward (normalized)
                  uint  pad2;

                  float view_proj[4][4]; uint texture_id; uint sampler_id;

    );

    PUSH_CONSTANT(PostPush, uint32_t src_texture_id; uint32_t output_image_id; uint32_t sampler_id;

                  uint32_t width;


                  uint32_t height;

                  uint frame

                  ;

                  float exposure;

    );

    PUSH_CONSTANT(EdgePush, uint32_t texture_id; uint32_t sampler_id;);


    PUSH_CONSTANT(BlendPush, uint32_t color_tex; uint32_t weight_tex; uint32_t sampler_id; uint32_t pad;);

    PUSH_CONSTANT(WeightPush, uint32_t edge_tex; uint32_t area_tex; uint32_t search_tex; uint32_t sampler_id;);
    PUSH_CONSTANT(Lightbeampush, VkDeviceAddress beam_ptr; uint64_t pad; float view_proj[4][4]; uint texture_id; uint sampler_id;

    );

    PUSH_CONSTANT(SkyPush, float inv_proj[4][4]; float basis_right[4]; float basis_up[4]; float basis_back[4];
                  float time; float cirrus; float cumulus; float pad0;);

    dmon_init();
    dmon_watch("shaders", watch_cb, DMON_WATCHFLAGS_RECURSIVE, NULL);

    uint32_t pp_frame_counter = 0;
    while(!glfwWindowShouldClose(renderer.window))
    {


        TracyCFrameMark;
        TracyCZoneN(frame_loop_zone, "Frame Loop", 1);

        TracyCZoneN(hot_reload_zone, "Hot Reload + Pipeline Rebuild", 1);
        if(shader_changed)
        {
            shader_changed = false;
            printf("hello");
            system("./compileslang.sh");

            pipeline_mark_dirty(changed_shader);
        }
        pipeline_rebuild(&renderer);
        TracyCZoneEnd(hot_reload_zone);

        TracyCZoneN(frame_start_zone, "frame_start", 1);
        frame_start(&renderer, &cam);
        TracyCZoneEnd(frame_start_zone);

        TracyCZoneN(streaming_zone, "Frame Loop", 1);
        if(!voxel_debug)
        {
        }
        TracyCZoneEnd(streaming_zone);

        VkCommandBuffer cmd        = renderer.frames[renderer.current_frame].cmdbuf;
        GpuProfiler*    frame_prof = &renderer.gpuprofiler[renderer.current_frame];

        uint32_t current_image = renderer.swapchain.current_image;
        TracyCZoneN(record_cmd_zone, "Record Command Buffer", 1);
        vk_cmd_begin(cmd, false);
        {
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, renderer.bindless_system.pipeline_layout, 0,
                                    1, &renderer.bindless_system.set, 0, NULL);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, renderer.bindless_system.pipeline_layout, 0, 1,
                                    &renderer.bindless_system.set, 0, NULL);
            rt_transition_all(cmd, &renderer.depth[renderer.swapchain.current_image], VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                              VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                              VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT);
            rt_transition_all(cmd, &renderer.hdr_color[renderer.swapchain.current_image], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                              VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);

            image_transition_swapchain(cmd, &renderer.swapchain, VK_IMAGE_LAYOUT_GENERAL,
                                       VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
        }

        VkRenderingAttachmentInfo color = {.sType     = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                                           .imageView = renderer.hdr_color[renderer.swapchain.current_image].view,
                                           .imageLayout =
                                               renderer.hdr_color[renderer.swapchain.current_image].mip_states[0].layout,
                                           .loadOp           = VK_ATTACHMENT_LOAD_OP_CLEAR,
                                           .storeOp          = VK_ATTACHMENT_STORE_OP_STORE,
                                           .clearValue.color = {{0.1f, 0.1f, 0.1f, 1.0f}}};
        VkRenderingAttachmentInfo depth = {
            .sType                   = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
            .imageView               = renderer.depth[renderer.swapchain.current_image].view,
            .imageLayout             = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
            .loadOp                  = VK_ATTACHMENT_LOAD_OP_CLEAR,
            .storeOp                 = VK_ATTACHMENT_STORE_OP_STORE,
            .clearValue.depthStencil = {0.0f, 0},
        };

        VkRenderingInfo rendering = {
            .sType                = VK_STRUCTURE_TYPE_RENDERING_INFO,
            .renderArea.extent    = renderer.swapchain.extent,
            .layerCount           = 1,
            .colorAttachmentCount = 1,
            .pColorAttachments    = &color,
            .pDepthAttachment     = &depth,
        };


        TracyCZoneN(imgui_zone, "ImGui CPU", 1);
        {
            imgui_begin_frame();


            igBegin("Renderer Debug", NULL, 0);

            double cpu_frame_ms  = renderer.cpu_frame_ns / 1000000.0;
            double cpu_active_ms = renderer.cpu_active_ns / 1000000.0;
            double cpu_wait_ms   = renderer.cpu_wait_ns / 1000000.0;

            igText("CPU frame (wall): %.3f ms", cpu_frame_ms);
            igText("CPU active: %.3f ms", cpu_active_ms);
            igText("CPU wait: %.3f ms", cpu_wait_ms);
            igText("FPS: %.1f", cpu_frame_ms > 0.0 ? 1000.0 / cpu_frame_ms : 0.0);

            igSeparator();
            igSeparator();

            igText("Camera Position");
            igText("x: %.3f", cam.position[0]);
            igText("y: %.3f", cam.position[1]);
            igText("z: %.3f", cam.position[2]);

            igSeparator();

            igText("Yaw: %.3f", cam.yaw);
            igText("Pitch: %.3f", cam.pitch);

            igSeparator();
            igText("GPU Profiler");
            if(frame_prof->pass_count == 0)
            {
                igText("No GPU samples collected yet.");
            }
            for(uint32_t i = 0; i < frame_prof->pass_count; i++)
            {
                GpuPass* pass = &frame_prof->passes[i];
                igText("%s: %.3f ms", pass->name, pass->time_ms);
                if(frame_prof->enable_pipeline_stats)
                {
                    igText("  VS: %llu | FS: %llu | Prim: %llu", (unsigned long long)pass->vs_invocations,
                           (unsigned long long)pass->fs_invocations, (unsigned long long)pass->primitives);
                }
            }

            igEnd();
            igRender();
        }
        TracyCZoneEnd(imgui_zone);


        if(!upload_once_done)
        {
            upload_once_done = true;

            {
                VkBufferCopy copy = {.srcOffset = cpu_faces.offset,
                                     .dstOffset = gpu_faces.offset,
                                     .size      = voxel_face_count * sizeof(uint32_t)};

                vkCmdCopyBuffer(cmd, cpu_faces.buffer, gpu_faces.buffer, 1, &copy);
            }
            {
                VkBufferCopy mat_copy = {.srcOffset = mat_staging.offset, .dstOffset = material_slice.offset, .size = sizeof(gpu_materials)};

                vkCmdCopyBuffer(cmd, mat_staging.buffer, material_slice.buffer, 1, &mat_copy);
            }
        }

        gpu_profiler_begin_frame(frame_prof, cmd);
        {
            vkCmdBeginRendering(cmd, &rendering);
            GPU_SCOPE(frame_prof, cmd, "Main Pass", VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT)
            {


                // ── Sky pass ──────────────────────────────────────────
                {
                    vec3 forward = {
                        cosf(cam.pitch) * sinf(cam.yaw),
                        sinf(cam.pitch),
                        -cosf(cam.pitch) * cosf(cam.yaw),
                    };
                    glm_vec3_normalize(forward);

                    vec3 world_up = {0.0f, 1.0f, 0.0f};
                    vec3 right    = {0.0f};
                    vec3 up       = {0.0f};
                    glm_vec3_cross(forward, world_up, right);
                    glm_vec3_normalize(right);
                    glm_vec3_cross(right, forward, up);

                    float aspect = (float)renderer.swapchain.extent.width / (float)renderer.swapchain.extent.height;
                    mat4  proj   = GLM_MAT4_IDENTITY_INIT;
                    mat4  inv_proj;
                    camera_build_proj_reverse_z_infinite(proj, &cam, aspect);
                    proj[1][1] *= -1.0f;
                    glm_mat4_inv(proj, inv_proj);

                    SkyPush sky_push = {0};
                    memcpy(sky_push.inv_proj, inv_proj, sizeof(sky_push.inv_proj));
                    sky_push.basis_right[0] = right[0];
                    sky_push.basis_right[1] = right[1];
                    sky_push.basis_right[2] = right[2];
                    sky_push.basis_up[0]    = up[0];
                    sky_push.basis_up[1]    = up[1];
                    sky_push.basis_up[2]    = up[2];
                    sky_push.basis_back[0]  = -forward[0];
                    sky_push.basis_back[1]  = -forward[1];
                    sky_push.basis_back[2]  = -forward[2];
                    sky_push.time           = (float)glfwGetTime() * 0.2f;
                    sky_push.cirrus         = 0.4f;
                    sky_push.cumulus        = 0.8f;

                    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g_render_pipelines.pipelines[pipelines.sky]);
                    vk_cmd_set_viewport_scissor(cmd, renderer.swapchain.extent);
                    vkCmdPushConstants(cmd, renderer.bindless_system.pipeline_layout, VK_SHADER_STAGE_ALL, 0,
                                       sizeof(SkyPush), &sky_push);
                    vkCmdDraw(cmd, 4, 1, 0, 0);
                }

                static int prev_space = GLFW_RELEASE;

                int space = glfwGetKey(renderer.window, GLFW_KEY_SPACE);

                if(space == GLFW_PRESS && prev_space == GLFW_RELEASE)
                {
                    wireframe_mode = !wireframe_mode;
                }

                prev_space = space;

                // prev_space = (wireframe_mode ^= (space = glfwGetKey(renderer.window, GLFW_KEY_SPACE)) == GLFW_PRESS && prev_space == GLFW_RELEASE, space);

                if(!wireframe_mode)
                {
                    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g_render_pipelines.pipelines[pipelines.triangle]);
                }
                else
                {

                    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                      g_render_pipelines.pipelines[pipelines.triangle_wireframe]);
                };


                vk_cmd_set_viewport_scissor(cmd, renderer.swapchain.extent);

                Push push = {0};

                push.aspect     = (float)renderer.swapchain.extent.width / (float)renderer.swapchain.extent.height;
                push.texture_id = tex_id;
                push.sampler_id = renderer.default_samplers.samplers[SAMPLER_LINEAR_WRAP_ANISO];
                push.face_ptr   = face_pointer;
                push.face_count = voxel_face_count;
                push.mat_ptr    = material_pointer;
                glm_vec3_copy(cam.cam_dir, push.cam_dir);
                glm_vec3_copy(cam.position, push.cam_pos);
                glm_mat4_copy(cam.view_proj, push.view_proj);  // this one was already correct

                vkCmdPushConstants(cmd, renderer.bindless_system.pipeline_layout, VK_SHADER_STAGE_ALL, 0, sizeof(Push), &push);

                vkCmdDrawIndirectCount(cmd, indirect_slice.buffer, indirect_slice.offset, count_slice.buffer,
                                       count_slice.offset, 1024, sizeof(VkDrawIndirectCommand));
            }

            //
            // {
            //     Lightbeampush push = {0};
            //     push.beam_ptr      = beam_addr;
            //     glm_mat4_copy(cam.view_proj, push.view_proj);
            //
            //     vkCmdPushConstants(cmd, renderer.bindless_system.pipeline_layout, VK_SHADER_STAGE_ALL, 0,
            //                        sizeof(Lightbeampush), &push);
            //     vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, render_pipelines.pipelines[pipelines.beam]);
            //     //6 vertices × beam_count instances
            //     vkCmdDraw(cmd,
            //               6,           // vertices per quad
            //               beam_count,  // instances
            //               0, 0);
            // }
            //
            // //
            vkCmdEndRendering(cmd);
        }


        GPU_SCOPE(frame_prof, cmd, "POST", VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT)
        {
            rt_transition_all(cmd, &renderer.hdr_color[renderer.swapchain.current_image], VK_IMAGE_LAYOUT_GENERAL,
                              VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT);


            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g_render_pipelines.pipelines[pipelines.postprocess]);

            PostPush pp_push        = {0};
            pp_push.src_texture_id  = renderer.hdr_color[renderer.swapchain.current_image].bindless_index;
            pp_push.output_image_id = renderer.ldr_color[renderer.swapchain.current_image].bindless_index;
            pp_push.sampler_id      = renderer.default_samplers.samplers[SAMPLER_LINEAR_CLAMP];
            pp_push.width           = renderer.swapchain.extent.width;
            pp_push.height          = renderer.swapchain.extent.height;
            pp_push.frame           = pp_frame_counter++;

            pp_push.exposure = 1.2;
            vkCmdPushConstants(cmd, renderer.bindless_system.pipeline_layout, VK_SHADER_STAGE_ALL, 0, sizeof(PostPush), &pp_push);

            uint32_t gx = (pp_push.width + 15) / 16;
            uint32_t gy = (pp_push.height + 15) / 16;


            vkCmdDispatch(cmd, gx, gy, 1);
        }

        {
            rt_transition_all(cmd, &renderer.smaa_edges[current_image], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                              VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);

            VkRenderingAttachmentInfo color = {.sType            = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                                               .imageView        = renderer.smaa_edges[current_image].view,
                                               .imageLayout      = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                               .loadOp           = VK_ATTACHMENT_LOAD_OP_CLEAR,
                                               .storeOp          = VK_ATTACHMENT_STORE_OP_STORE,
                                               .clearValue.color = {{0, 0, 0, 0}}};

            VkRenderingInfo rendering = {.sType                = VK_STRUCTURE_TYPE_RENDERING_INFO,
                                         .renderArea.extent    = renderer.swapchain.extent,
                                         .layerCount           = 1,
                                         .colorAttachmentCount = 1,
                                         .pColorAttachments    = &color};

            vkCmdBeginRendering(cmd, &rendering);

            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g_render_pipelines.pipelines[renderer.smaa_pipelines.smaa_edge]);

            vk_cmd_set_viewport_scissor(cmd, renderer.swapchain.extent);

            EdgePush push   = {0};
            push.texture_id = renderer.ldr_color[current_image].bindless_index;
            push.sampler_id = renderer.default_samplers.samplers[SAMPLER_LINEAR_CLAMP];

            vkCmdPushConstants(cmd, renderer.bindless_system.pipeline_layout, VK_SHADER_STAGE_ALL, 0, sizeof(EdgePush), &push);

            vkCmdDraw(cmd, 3, 1, 0, 0);

            vkCmdEndRendering(cmd);
        }
        {
            rt_transition_all(cmd, &renderer.smaa_weights[current_image], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                              VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
            rt_transition_all(cmd, &renderer.smaa_edges[current_image], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                              VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
            VkRenderingAttachmentInfo color = {.sType            = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                                               .imageView        = renderer.smaa_weights[current_image].view,
                                               .imageLayout      = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                               .loadOp           = VK_ATTACHMENT_LOAD_OP_CLEAR,
                                               .storeOp          = VK_ATTACHMENT_STORE_OP_STORE,
                                               .clearValue.color = {{0, 0, 0, 0}}};

            VkRenderingInfo rendering = {.sType                = VK_STRUCTURE_TYPE_RENDERING_INFO,
                                         .renderArea.extent    = renderer.swapchain.extent,
                                         .layerCount           = 1,
                                         .colorAttachmentCount = 1,
                                         .pColorAttachments    = &color};

            vkCmdBeginRendering(cmd, &rendering);

            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g_render_pipelines.pipelines[renderer.smaa_pipelines.smaa_weight]);


            vk_cmd_set_viewport_scissor(cmd, renderer.swapchain.extent);
            WeightPush push = {0};

            push.edge_tex   = renderer.smaa_edges[current_image].bindless_index;
            push.area_tex   = renderer.smaa_area_tex;
            push.search_tex = renderer.smaa_search_tex;
            push.sampler_id = renderer.default_samplers.samplers[SAMPLER_LINEAR_CLAMP];

            vkCmdPushConstants(cmd, renderer.bindless_system.pipeline_layout, VK_SHADER_STAGE_ALL, 0, sizeof(WeightPush), &push);

            vkCmdDraw(cmd, 3, 1, 0, 0);

            vkCmdEndRendering(cmd);
        }

        rt_transition_all(cmd, &renderer.ldr_color[current_image], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                          VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);


        {
            VkRenderingAttachmentInfo color = {.sType       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                                               .imageView   = renderer.ldr_color[current_image].view,
                                               .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                               .loadOp      = VK_ATTACHMENT_LOAD_OP_LOAD,
                                               .storeOp     = VK_ATTACHMENT_STORE_OP_STORE};

            VkRenderingInfo rendering = {.sType                = VK_STRUCTURE_TYPE_RENDERING_INFO,
                                         .renderArea.extent    = renderer.swapchain.extent,
                                         .layerCount           = 1,
                                         .colorAttachmentCount = 1,
                                         .pColorAttachments    = &color};

            vkCmdBeginRendering(cmd, &rendering);

            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g_render_pipelines.pipelines[renderer.smaa_pipelines.smaa_blend]);


            vk_cmd_set_viewport_scissor(cmd, renderer.swapchain.extent);

            BlendPush push  = {0};
            push.color_tex  = renderer.ldr_color[current_image].bindless_index;
            push.weight_tex = renderer.smaa_weights[current_image].bindless_index;
            push.sampler_id = renderer.default_samplers.samplers[SAMPLER_LINEAR_CLAMP];

            vkCmdPushConstants(cmd, renderer.bindless_system.pipeline_layout, VK_SHADER_STAGE_ALL, 0, sizeof(BlendPush), &push);

            vkCmdDraw(cmd, 3, 1, 0, 0);


            vkCmdEndRendering(cmd);
        }

        rt_transition_all(cmd, &renderer.ldr_color[renderer.swapchain.current_image], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                          VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
        image_transition_swapchain(renderer.frames[renderer.current_frame].cmdbuf, &renderer.swapchain,
                                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_TRANSFER_BIT, 0);

        VkImageBlit blit = {
            .srcSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .mipLevel = 0, .baseArrayLayer = 0, .layerCount = 1},
            .srcOffsets = {{0, 0, 0}, {renderer.swapchain.extent.width, renderer.swapchain.extent.height, 1}},

            .dstSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .mipLevel = 0, .baseArrayLayer = 0, .layerCount = 1},
            .dstOffsets = {{0, 0, 0}, {renderer.swapchain.extent.width, renderer.swapchain.extent.height, 1}}};

        vkCmdBlitImage(cmd, renderer.ldr_color[renderer.swapchain.current_image].image,
                       VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, renderer.swapchain.images[renderer.swapchain.current_image],
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_NEAREST);


        {
            image_transition_swapchain(cmd, &renderer.swapchain, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                       VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);


            VkRenderingAttachmentInfo color = {.sType       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                                               .imageView   = renderer.swapchain.image_views[current_image],
                                               .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                               .loadOp      = VK_ATTACHMENT_LOAD_OP_LOAD,
                                               .storeOp     = VK_ATTACHMENT_STORE_OP_STORE};

            VkRenderingInfo rendering = {.sType                = VK_STRUCTURE_TYPE_RENDERING_INFO,
                                         .renderArea.extent    = renderer.swapchain.extent,
                                         .layerCount           = 1,
                                         .colorAttachmentCount = 1,
                                         .pColorAttachments    = &color};

            vkCmdBeginRendering(cmd, &rendering);
            {
                ImDrawData* draw_data = igGetDrawData();
                ImGui_ImplVulkan_RenderDrawData(draw_data, cmd, VK_NULL_HANDLE);
            }
            vkCmdEndRendering(cmd);
        }


        if(take_screenshot)
        {
            renderer_record_screenshot(&renderer, cmd);
        }
        image_transition_swapchain(renderer.frames[renderer.current_frame].cmdbuf, &renderer.swapchain,
                                   VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, 0);


        vk_cmd_end(renderer.frames[renderer.current_frame].cmdbuf);
        TracyCZoneEnd(record_cmd_zone);


        TracyCZoneN(submit_zone, "Submit + Present", 1);
        submit_frame(&renderer);
        TracyCZoneEnd(submit_zone);

        if(take_screenshot)
        {
            renderer_save_screenshot(&renderer);
        
            take_screenshot = false;
	}

        TracyCZoneEnd(frame_loop_zone);
    }


    printf(" renderer size is %zu", sizeof(Renderer));
    printf("Push size = %zu\n", sizeof(Push));
    printf("view_proj offset = %zu\n", offsetof(Push, view_proj));

    PRINT_FIELD(Push, face_ptr);
    PRINT_FIELD(Push, mat_ptr);
    PRINT_FIELD(Push, face_count);
    PRINT_FIELD(Push, aspect);
    PRINT_FIELD(Push, cam_pos);
    PRINT_FIELD(Push, pad1);
    PRINT_FIELD(Push, cam_dir);
    PRINT_FIELD(Push, pad2);
    PRINT_FIELD(Push, view_proj);
    PRINT_FIELD(Push, texture_id);
    PRINT_FIELD(Push, sampler_id);
    //    ANALYZE_STRUCT(ImageState);
    //renderer_destroy(&renderer);
    return 0;
}
