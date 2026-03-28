#include "external/cglm/include/cglm/vec2.h"
#include "tinytypes.h"
#include "vk.h"
#include <GLFW/glfw3.h>
#include <dirent.h>
#include <stdalign.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "stb/stb_perlin.h"
#include "text_baker.h"
#define DMON_IMPL
#include "external/dmon/dmon.h"

#include "voxel.h"
#include "external/tracy/public/tracy/TracyC.h"

#define RUN_ONCE_N(name) for(static bool name = true; name; name = false)
static bool voxel_debug     = true;
static bool take_screenshot = true;
static bool wireframe_mode  = false;

#define PRINT_FIELD(type, field)                                                                                       \
    printf("%-20s offset = %3zu  align = %2zu  size = %2zu\n", #field, offsetof(type, field),                          \
           _Alignof(((type*)0)->field), sizeof(((type*)0)->field))

#define PRINT_STRUCT(type) printf("\nSTRUCT %-20s size = %zu  align = %zu\n\n", #type, sizeof(type), _Alignof(type));

static inline size_t mu_ravel_index(const size_t* coord, const size_t* strides, size_t ndim)
{
    size_t index = 0;

    for(size_t i = 0; i < ndim; i++)
        index += coord[i] * strides[i];

    return index;
}


static inline void mu_unravel_index(size_t index, const size_t* dims, size_t ndim, size_t* coord)
{
    for(int i = ndim - 1; i >= 0; i--)
    {
        coord[i] = index % dims[i];
        index /= dims[i];
    }
}
typedef struct Sprite2D
{
    // Rendering
    TextureID texture_id;
    vec2      position;
    vec2      scale;
    float     rotation;

    // Appearance
    vec4  tint_color;  // For color modulation
    float depth;       // Z-order (0.0 to 1.0)

    // Texture mapping
    vec4 uv_rect;  // x,y = min UV, z,w = max UV (for atlas)

    // Transform
    uint32_t transform_offset;  // Push constant offset
    bool     dirty;
} Sprite2D;
typedef struct GPU_Quad2D
{
    // Instance data (vec4 aligned)
    vec2 position;
    vec2 scale;

    float    rotation;
    float    depth;
    float    opacity;
    uint32_t texture_id;

    // UV coordinates
    vec4 uv_rect;  // min_u, min_v, max_u, max_v

    // Color tint
    vec4 tint;
} GPU_Quad2D;

typedef struct SpritePushConstants
{
    VkDeviceAddress instance_ptr;
    vec2            screen_size;
    uint32_t        sampler_id;
    uint32_t        _pad;
} SpritePushConstants;


typedef struct Camera2D
{
    vec2  position;  // World position
    float zoom;      // Scale factor
    float rotation;  // Rotation in radians

    uint32_t viewport_width;
    uint32_t viewport_height;

    // Cached matrix (update when dirty)
    mat4 view_matrix;
    mat4 projection_matrix;
    mat4 view_projection;
    bool dirty;
} Camera2D;

void camera2d_update_matrices(Camera2D* cam);
#define MAX_DRAWS 1024
#define MAX_SPRITES 10000

typedef struct SpriteIndirectSystem
{
    GPU_Quad2D* instances;
    uint32_t    instance_count;

    VkDrawIndirectCommand* draws;
    uint32_t               draw_count;

    BufferSlice instance_buffer;
    BufferSlice indirect_buffer;
    BufferSlice count_buffer;

} SpriteIndirectSystem;

static SpriteIndirectSystem g_sprite_sys;
static GPU_Quad2D           g_sprite_instances_cpu[MAX_SPRITES];

typedef struct TextSystem
{
    TextureID  atlas_texture;
    BakedGlyph glyphs[96];
    uint32_t   atlas_width;
    uint32_t   atlas_height;
    float      pixel_height;
    bool       ready;
} TextSystem;

static TextSystem g_text_sys;

void draw_sprite(Sprite2D* s);

static bool text_system_init(const char* font_path, float pixel_height)
{
    memset(&g_text_sys, 0, sizeof(g_text_sys));
    g_text_sys.atlas_texture = UINT32_MAX;
    g_text_sys.pixel_height  = pixel_height;
    g_text_sys.atlas_width   = 1024;
    g_text_sys.atlas_height  = 1024;

    uint8_t* atlas_rgba = NULL;
    size_t   rgba_size  = 0;
    if(!text_bake_font_rgba(font_path, pixel_height, g_text_sys.atlas_width, g_text_sys.atlas_height, g_text_sys.glyphs,
                            &atlas_rgba, &rgba_size))
    {
        return false;
    }

    TextureCreateDesc desc = {
        .width     = g_text_sys.atlas_width,
        .height    = g_text_sys.atlas_height,
        .mip_count = 1,
        .format    = VK_FORMAT_R8G8B8A8_UNORM,
        .usage     = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
    };

    TextureID atlas_id = create_texture(&renderer, &desc);
    if(atlas_id == UINT32_MAX)
    {
        free(atlas_rgba);
        return false;
    }

    Texture*     atlas      = &textures[atlas_id];
    VkDeviceSize image_size = (VkDeviceSize)rgba_size;
    Buffer       staging    = {0};
    if(!create_buffer(&renderer, image_size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_AUTO_PREFER_HOST, &staging))
    {
        destroy_texture(&renderer, atlas_id);
        free(atlas_rgba);
        return false;
    }

    memcpy(staging.mapping, atlas_rgba, rgba_size);

    VkCommandBuffer      cmd      = vk_begin_one_time_cmd(renderer.device, renderer.one_time_gfx_pool);
    VkImageMemoryBarrier barrier1 = {
        .sType                           = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .oldLayout                       = VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout                       = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .srcAccessMask                   = 0,
        .dstAccessMask                   = VK_ACCESS_TRANSFER_WRITE_BIT,
        .image                           = atlas->image,
        .subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
        .subresourceRange.baseMipLevel   = 0,
        .subresourceRange.levelCount     = 1,
        .subresourceRange.baseArrayLayer = 0,
        .subresourceRange.layerCount     = 1,
    };

    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &barrier1);

    VkBufferImageCopy region = {
        .bufferOffset                    = 0,
        .bufferRowLength                 = 0,
        .bufferImageHeight               = 0,
        .imageSubresource.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
        .imageSubresource.mipLevel       = 0,
        .imageSubresource.baseArrayLayer = 0,
        .imageSubresource.layerCount     = 1,
        .imageOffset                     = {0, 0, 0},
        .imageExtent                     = {g_text_sys.atlas_width, g_text_sys.atlas_height, 1},
    };
    vkCmdCopyBufferToImage(cmd, staging.buffer, atlas->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    VkImageMemoryBarrier barrier2 = barrier1;
    barrier2.oldLayout            = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier2.newLayout            = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier2.srcAccessMask        = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier2.dstAccessMask        = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL, 0,
                         NULL, 1, &barrier2);

    vk_end_one_time_cmd(renderer.device, renderer.graphics_queue, renderer.one_time_gfx_pool, cmd);
    destroy_buffer(&renderer, &staging);
    free(atlas_rgba);

    g_text_sys.atlas_texture = atlas_id;
    g_text_sys.ready         = true;
    return true;
}

static void draw_text_2d(const char* text, float x, float y, float scale, vec4 color, float depth)
{
    if(!g_text_sys.ready || !text)
        return;

    float cursor_x = x;
    float baseline = y + g_text_sys.pixel_height * scale;

    for(const unsigned char* p = (const unsigned char*)text; *p; ++p)
    {
        if(*p == '\n')
        {
            cursor_x = x;
            baseline += g_text_sys.pixel_height * scale;
            continue;
        }

        if(*p == '\t')
        {
            cursor_x += g_text_sys.pixel_height * scale;
            continue;
        }

        if(*p < 32 || *p >= 128)
            continue;

        const BakedGlyph* glyph = &g_text_sys.glyphs[*p - 32];

        float x0 = cursor_x + glyph->xoff * scale;
        float y0 = baseline + glyph->yoff * scale;
        float x1 = x0 + (glyph->x1 - glyph->x0) * scale;
        float y1 = y0 + (glyph->y1 - glyph->y0) * scale;

        cursor_x += glyph->xadvance * scale;

        if((glyph->x1 - glyph->x0) <= 0 || (glyph->y1 - glyph->y0) <= 0)
            continue;

        const float atlas_w = (float)g_text_sys.atlas_width;
        const float atlas_h = (float)g_text_sys.atlas_height;
        Sprite2D s = {
            .texture_id       = g_text_sys.atlas_texture,
            .position         = {(x0 + x1) * 0.5f, (y0 + y1) * 0.5f},
            .scale            = {x1 - x0, y1 - y0},
            .rotation         = 0.0f,
            .tint_color       = {color[0], color[1], color[2], color[3]},
            .depth            = depth,
            .uv_rect          = {glyph->x0 / atlas_w, glyph->y1 / atlas_h, glyph->x1 / atlas_w, glyph->y0 / atlas_h},
            .transform_offset = 0,
            .dirty            = false,
        };

        draw_sprite(&s);
    }
}

void sprite_indirect_init()
{
    g_sprite_sys.instance_buffer = buffer_pool_alloc(&renderer.gpu_pool, sizeof(GPU_Quad2D) * MAX_SPRITES, 16);

    g_sprite_sys.indirect_buffer = buffer_pool_alloc(&renderer.cpu_pool, sizeof(VkDrawIndirectCommand) * MAX_DRAWS, 16);

    g_sprite_sys.count_buffer = buffer_pool_alloc(&renderer.cpu_pool, sizeof(uint32_t), 4);

    g_sprite_sys.instances = g_sprite_instances_cpu;
    g_sprite_sys.draws     = g_sprite_sys.indirect_buffer.mapped;
}
void draw_sprite(Sprite2D* s)
{
    if(g_sprite_sys.instance_count >= MAX_SPRITES)
        return;

    uint32_t id = g_sprite_sys.instance_count++;

    GPU_Quad2D* q = &g_sprite_sys.instances[id];

    glm_vec2_copy(s->position, q->position);
    glm_vec2_copy(s->scale, q->scale);
    q->rotation   = s->rotation;
    q->depth      = s->depth;
    q->texture_id = s->texture_id;
    glm_vec4_copy(s->uv_rect, q->uv_rect);
    glm_vec4_copy(s->tint_color, q->tint);
    q->opacity = s->tint_color[3];
}
void build_indirect_commands_2d()
{
    g_sprite_sys.draw_count = 0;

    uint32_t start = 0;

    while(start < g_sprite_sys.instance_count)
    {
        uint32_t tex = g_sprite_sys.instances[start].texture_id;

        uint32_t count = 1;

        // group same texture
        for(uint32_t i = start + 1; i < g_sprite_sys.instance_count; i++)
        {
            if(g_sprite_sys.instances[i].texture_id != tex)
                break;
            count++;
        }

        VkDrawIndirectCommand* cmd = &g_sprite_sys.draws[g_sprite_sys.draw_count++];

        cmd->vertexCount   = 6;
        cmd->instanceCount = count;
        cmd->firstVertex   = 0;
        cmd->firstInstance = start;

        start += count;
    }

    *(uint32_t*)g_sprite_sys.count_buffer.mapped = g_sprite_sys.draw_count;
}

void render_sprites(VkCommandBuffer cmd)
{
    if(g_sprite_sys.instance_count == 0)
        return;

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g_render_pipelines.pipelines[pipelines.sprite]);

    vk_cmd_set_viewport_scissor(cmd, renderer.swapchain.extent);

    VkBufferDeviceAddressInfo addr_info = {
        .sType  = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
        .buffer = g_sprite_sys.instance_buffer.buffer,
    };

    SpritePushConstants sprite_push = {
        .instance_ptr = vkGetBufferDeviceAddress(renderer.device, &addr_info) + g_sprite_sys.instance_buffer.offset,
        .screen_size  = {(float)renderer.swapchain.extent.width, (float)renderer.swapchain.extent.height},
        .sampler_id   = renderer.default_samplers.samplers[SAMPLER_LINEAR_WRAP_ANISO],
        ._pad         = 0,
    };

    vkCmdPushConstants(cmd, renderer.bindless_system.pipeline_layout, VK_SHADER_STAGE_ALL, 0, sizeof(SpritePushConstants), &sprite_push);

    vkCmdDrawIndirectCount(cmd, g_sprite_sys.indirect_buffer.buffer, g_sprite_sys.indirect_buffer.offset,
                           g_sprite_sys.count_buffer.buffer, g_sprite_sys.count_buffer.offset, MAX_DRAWS,
                           sizeof(VkDrawIndirectCommand));
}

static bool sprite_prepare_gpu_data(VkCommandBuffer cmd)
{
    if(g_sprite_sys.instance_count == 0)
        return true;

    build_indirect_commands_2d();

    VkDeviceSize instance_upload_bytes = sizeof(GPU_Quad2D) * g_sprite_sys.instance_count;
    if(!renderer_upload_buffer_to_slice(&renderer, cmd, g_sprite_sys.instance_buffer, g_sprite_sys.instances, instance_upload_bytes, 16))
        return false;

    VkBufferMemoryBarrier2 instance_barrier = {
        .sType         = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
        .srcStageMask  = VK_PIPELINE_STAGE_2_COPY_BIT,
        .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
        .dstStageMask  = VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT,
        .dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
        .buffer        = g_sprite_sys.instance_buffer.buffer,
        .offset        = g_sprite_sys.instance_buffer.offset,
        .size          = instance_upload_bytes,
    };

    VkDependencyInfo dep = {
        .sType                    = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .bufferMemoryBarrierCount = 1,
        .pBufferMemoryBarriers    = &instance_barrier,
    };
    vkCmdPipelineBarrier2(cmd, &dep);

    return true;
}

#include "renderer.h"
int main(void)
{
    graphics_init();
    sprite_indirect_init();

    text_system_init("/home/lk/myprojects/voxelfun/clusteredshading/assets/font/ttf/FiraCode-Bold.ttf", 48.0f);

    TextureID sprite_sheet_texture = load_texture(&renderer, "data/Spritesheets/spritesheet_tiles.png");
    if(sprite_sheet_texture == UINT32_MAX)
    {
        fprintf(stderr, "Failed to load sprite sheet data/Spritesheets/spritesheet_tiles.png\n");
        renderer_destroy(&renderer);
        return 1;
    }

    Texture* sprite_sheet = &textures[sprite_sheet_texture];

    const float atlas_w = (float)sprite_sheet->width;
    const float atlas_h = (float)sprite_sheet->height;

    Sprite2D brick_sprite = {
        .texture_id       = sprite_sheet_texture,
        .position         = {260.0f, 260.0f},
        .scale            = {128.0f, 128.0f},
        .rotation         = 0.0f,
        .tint_color       = {1.0f, 1.0f, 1.0f, 1.0f},
        .depth            = 0.0f,
        .uv_rect          = {512.0f / atlas_w, 256.0f / atlas_h, 640.0f / atlas_w, 384.0f / atlas_h},
        .transform_offset = 0,
        .dirty            = true,
    };

    /* buffer slices start  */

    BufferSlice indirect_slice = buffer_pool_alloc(&renderer.cpu_pool, sizeof(VkDrawIndirectCommand), 16);

    BufferSlice count_slice = buffer_pool_alloc(&renderer.cpu_pool, sizeof(uint32_t), 4);
    /* initialise voxel face storage then fill it with data and upload on gpu */

#if 0
    voxel_materials_init(&renderer);
    BufferSlice material_slice = buffer_pool_alloc(&renderer.gpu_pool, sizeof(gpu_materials), 16);


    static Voxel chunk[CHUNK_VOLUME];

    uint32_t* faces = NULL;

    generate_chunk(chunk);
    MU_FOR_3D(x, y, z, CHUNK_SIZE, CHUNK_SIZE, CHUNK_SIZE)
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
 
    VkDeviceAddress face_pointer     = renderer.gpu_base_addr + gpu_faces.offset;
    VkDeviceAddress material_pointer = renderer.gpu_base_addr + material_slice.offset;


#endif

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


    dmon_init();
    dmon_watch("shaders", watch_cb, DMON_WATCHFLAGS_RECURSIVE, NULL);

    uint32_t pp_frame_counter = 0;
    while(!glfwWindowShouldClose(renderer.window))
    {
        //MU_SCOPE_TIMER("GAME")
        {
            g_sprite_sys.instance_count = 0;
            draw_sprite(&brick_sprite);
            vec4 text_color = {1.0f, 1.0f, 1.0f, 1.0f};
            draw_text_2d("Hello World", 40.0f, 40.0f, 0.5f, text_color, 0.0f);
        }


        //    MU_SCOPE_TIMER("GRAPHICS CPU")
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
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, renderer.bindless_system.pipeline_layout,
                                        0, 1, &renderer.bindless_system.set, 0, NULL);
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, renderer.bindless_system.pipeline_layout,
                                        0, 1, &renderer.bindless_system.set, 0, NULL);
                rt_transition_all(cmd, &renderer.depth[renderer.swapchain.current_image], VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                                  VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                                  VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT);
                rt_transition_all(cmd, &renderer.hdr_color[renderer.swapchain.current_image], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                  VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);

                image_transition_swapchain(cmd, &renderer.swapchain, VK_IMAGE_LAYOUT_GENERAL,
                                           VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
                flush_barriers(cmd);

                sprite_prepare_gpu_data(cmd);
            }


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

            gpu_profiler_begin_frame(frame_prof, cmd);
            {

                VkRenderingAttachmentInfo color = {
                    .sType            = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                    .imageView        = renderer.hdr_color[renderer.swapchain.current_image].view,
                    .imageLayout      = renderer.hdr_color[renderer.swapchain.current_image].mip_states[0].layout,
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


                vkCmdBeginRendering(cmd, &rendering);


                //                 GPU_SCOPE(frame_prof, cmd, "Main Pass", VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT)
                //                 {
                //
                //b              RUN_ONCE_N(buffer_uplad)
                //             {
                //                 {
                //                     VkBufferCopy copy = {.srcOffset = cpu_faces.offset,
                //                                          .dstOffset = gpu_faces.offset,
                //                                          .size      = voxel_face_count * sizeof(uint32_t)};
                //                     vkCmdCopyBuffer(cmd, cpu_faces.buffer, gpu_faces.buffer, 1, &copy);
                //                 }
                //                 {
                //                     renderer_upload_buffer_to_slice(&renderer, cmd, material_slice, gpu_materials, sizeof(gpu_materials), 16);
                //                 }
                //             }
                //
                //
                //                     static int prev_space = GLFW_RELEASE;
                //
                //                     int space = glfwGetKey(renderer.window, GLFW_KEY_SPACE);
                //
                //                     if(space == GLFW_PRESS && prev_space == GLFW_RELEASE)
                //                     {
                //                         wireframe_mode = !wireframe_mode;
                //                     }
                //
                //                     prev_space = space;
                //
                //                     // prev_space = (wireframe_mode ^= (space = glfwGetKey(renderer.window, GLFW_KEY_SPACE)) == GLFW_PRESS && prev_space == GLFW_RELEASE, space);
                //
                //                     if(!wireframe_mode)
                //                     {
                //                         vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g_render_pipelines.pipelines[pipelines.triangle]);
                //                     }
                //                     else
                //                     {
                //
                //                         vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                //                                           g_render_pipelines.pipelines[pipelines.triangle_wireframe]);
                //                     };
                //
                //
                //                     vk_cmd_set_viewport_scissor(cmd, renderer.swapchain.extent);
                //
                //                     Push push = {0};
                //
                //                     push.aspect     = (float)renderer.swapchain.extent.width / (float)renderer.swapchain.extent.height;
                //                     push.texture_id = renderer.dummy_texture;
                //                     push.sampler_id = renderer.default_samplers.samplers[SAMPLER_LINEAR_WRAP_ANISO];
                //                     push.face_ptr   = face_pointer;
                //                     push.face_count = voxel_face_count;
                //                     push.mat_ptr    = material_pointer;
                //                     glm_vec3_copy(cam.cam_dir, push.cam_dir);
                //                     glm_vec3_copy(cam.position, push.cam_pos);
                //                     glm_mat4_copy(cam.view_proj, push.view_proj);
                //
                //                     vkCmdPushConstants(cmd, renderer.bindless_system.pipeline_layout, VK_SHADER_STAGE_ALL, 0, sizeof(Push), &push);
                //
                //                     vkCmdDrawIndirectCount(cmd, indirect_slice.buffer, indirect_slice.offset, count_slice.buffer,
                //                                            count_slice.offset, 1024, sizeof(VkDrawIndirectCommand));
                //                 }
                //
                //                 vkCmdEndRendering(cmd);
                render_sprites(cmd);
                vkCmdEndRendering(cmd);
            }
            //
#include "passes.h"
            post_pass();
            pass_smaa();
            pass_ldr_to_swapchain();
            pass_imgui();


            if(take_screenshot)
            {
                renderer_record_screenshot(&renderer, cmd);
            }
            image_transition_swapchain(renderer.frames[renderer.current_frame].cmdbuf, &renderer.swapchain,
                                       VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, 0);
            flush_barriers(cmd);

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
    }


    printf(" renderer size is %zu", sizeof(Renderer));
    printf("Push size = %zu\n", sizeof(Push));
    printf("view_proj offset = %zu\n", offsetof(Push, view_proj));

    renderer_destroy(&renderer);
    return 0;
}
