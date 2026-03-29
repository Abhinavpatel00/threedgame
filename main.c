#include "external/cglm/include/cglm/vec2.h"
#include "renderer.h"
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
#include "text_system.h"
#define DMON_IMPL
#include "external/dmon/dmon.h"

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

// TODO: memory bandwidth is crucial as fuck so we will optimize it for that
typedef struct Sprite2D
{
    // Rendering
    TextureID texture_id;
    vec2      position;
    vec2      scale;
    float     rotation;

    vec2 velocity;
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
PUSH_CONSTANT(SpritePushConstants, VkDeviceAddress instance_ptr; vec2 screen_size; uint32_t sampler_id; float view_proj[4][4];

);

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
void camera2d_update_matrices(Camera2D* cam)
{
    if(!cam->dirty)
        return;

    /*
    =========================================================
        CAMERA MATH (aka move world, not camera)
    =========================================================

    If camera moves RIGHT →
    world must move LEFT ←

    So we NEGATE camera position.

          Camera
            ↓
       +-----------+
       |   YOU     |
       +-----------+

    World shifts opposite direction
    */

    mat4 view = GLM_MAT4_IDENTITY_INIT;

    // translate world opposite of camera
    glm_translate(view, (vec3){-cam->position[0], -cam->position[1], 0.0f});

    // optional rotation (rare for 2D but you're fancy)
    glm_rotate(view, -cam->rotation, (vec3){0, 0, 1});

    glm_mat4_copy(view, cam->view_matrix);

    /*
    =========================================================
        ORTHOGRAPHIC PROJECTION
    =========================================================

        (0,0) top-left → (width,height)

        No perspective. No drama.
    */

    mat4 proj;
    glm_ortho(0.0f, (float)cam->viewport_width, (float)cam->viewport_height, 0.0f, -1.0f, 1.0f, proj);

    glm_mat4_copy(proj, cam->projection_matrix);

    /*
    =========================================================
        FINAL MATRIX
    =========================================================

        screen = projection * view * world
    */

    glm_mat4_mul(proj, view, cam->view_projection);

    cam->dirty = false;
}

#define MAX_DRAWS 1024
#define MAX_SPRITES 10000

typedef struct SpriteRenderer
{
    GPU_Quad2D* instances;
    uint32_t    instance_count;
    uint32_t    capacity;

    VkDrawIndirectCommand* draws;
    uint32_t               draw_count;

    BufferSlice instance_buffer;
    BufferSlice indirect_buffer;
    BufferSlice count_buffer;

    bool dirty;
} SpriteRenderer;
// lifecycle
void sprite_renderer_init(SpriteRenderer* r, uint32_t max_sprites);
void sprite_renderer_destroy(SpriteRenderer* r);

// per-frame
void sprite_begin(SpriteRenderer* r);
void sprite_submit(SpriteRenderer* r, Sprite2D* s);
void sprite_end(SpriteRenderer* r, VkCommandBuffer cmd);

// rendering

void sprite_render(SpriteRenderer* r, VkCommandBuffer cmd, const Camera* cam);
void sprite_renderer_init(SpriteRenderer* r, uint32_t max_sprites)
{
    r->capacity = max_sprites;

    r->instances = malloc(sizeof(GPU_Quad2D) * max_sprites);
    r->draws     = malloc(sizeof(VkDrawIndirectCommand) * MAX_DRAWS);

    r->instance_buffer = buffer_pool_alloc(&renderer.gpu_pool, sizeof(GPU_Quad2D) * max_sprites, 16);

    r->indirect_buffer = buffer_pool_alloc(&renderer.gpu_pool, sizeof(VkDrawIndirectCommand) * MAX_DRAWS, 16);

    r->count_buffer = buffer_pool_alloc(&renderer.cpu_pool, sizeof(uint32_t), 4);
}
void sprite_begin(SpriteRenderer* r)
{
    r->instance_count = 0;
    r->draw_count     = 0;
    r->dirty          = false;
}


void sprite_submit(SpriteRenderer* r, Sprite2D* s)
{


    // may use dynamic array with stb
    if(r->instance_count >= r->capacity)
        return;  // or assert if you're serious about life

    GPU_Quad2D* q = &r->instances[r->instance_count++];

    // pack data
    glm_vec2_copy(s->position, q->position);
    glm_vec2_copy(s->scale, q->scale);
    glm_vec4_copy(s->uv_rect, q->uv_rect);
    glm_vec4_copy(s->tint_color, q->tint);

    q->rotation   = s->rotation;
    q->depth      = s->depth;
    q->texture_id = s->texture_id;
    q->opacity    = s->tint_color[3];

    r->dirty = true;
}
static int compare_texture(const void* a, const void* b)
{
    const GPU_Quad2D* A = a;
    const GPU_Quad2D* B = b;
    return (int)A->texture_id - (int)B->texture_id;
}

void sprite_end(SpriteRenderer* r, VkCommandBuffer cmd)
{
    if(r->instance_count == 0)
        return;

    /*
        STEP 1: SORT

        Before:
            [tex1][tex2][tex1][tex3]  -> garbage batching

        After:
            [tex1][tex1][tex2][tex3]  -> perfect batching
    */
    qsort(r->instances, r->instance_count, sizeof(GPU_Quad2D), compare_texture);

    /*
        STEP 2: BUILD INDIRECT COMMANDS

        Each batch = one draw call
    */
    uint32_t start = 0;

    while(start < r->instance_count)
    {
        uint32_t tex   = r->instances[start].texture_id;
        uint32_t count = 1;

        for(uint32_t i = start + 1; i < r->instance_count; i++)
        {
            if(r->instances[i].texture_id != tex)
                break;
            count++;
        }

        VkDrawIndirectCommand* cmdi = &r->draws[r->draw_count++];

        cmdi->vertexCount   = 6;
        cmdi->instanceCount = count;
        cmdi->firstVertex   = 0;
        cmdi->firstInstance = start;

        start += count;
    }

    /*
        STEP 3: UPLOAD

        CPU → GPU
    */
    VkDeviceSize instance_size = sizeof(GPU_Quad2D) * r->instance_count;

    renderer_upload_buffer_to_slice(&renderer, cmd, r->instance_buffer, r->instances, instance_size, 16);

    VkDeviceSize indirect_size = sizeof(VkDrawIndirectCommand) * r->draw_count;

    renderer_upload_buffer_to_slice(&renderer, cmd, r->indirect_buffer, r->draws, indirect_size, 16);
    /*
        STEP 4: BARRIER

        COPY → SHADER READ
    */


    VkBufferMemoryBarrier2 barriers[2] = {// instance buffer
                                          {
                                              .sType         = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
                                              .srcStageMask  = VK_PIPELINE_STAGE_2_COPY_BIT,
                                              .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                                              .dstStageMask  = VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT,
                                              .dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
                                              .buffer        = r->instance_buffer.buffer,
                                              .offset        = r->instance_buffer.offset,
                                              .size          = instance_size,
                                          },

                                          // INDIRECT BUFFER (this one you ignored)
                                          {
                                              .sType         = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
                                              .srcStageMask  = VK_PIPELINE_STAGE_2_COPY_BIT,
                                              .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                                              .dstStageMask  = VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT,
                                              .dstAccessMask = VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT,
                                              .buffer        = r->indirect_buffer.buffer,
                                              .offset        = r->indirect_buffer.offset,
                                              .size          = indirect_size,
                                          }};

    VkDependencyInfo dep = {
        .sType                    = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .bufferMemoryBarrierCount = 2,
        .pBufferMemoryBarriers    = barriers,
    };

    vkCmdPipelineBarrier2(cmd, &dep);

    /*
        STEP 5: WRITE DRAW COUNT
    */
    *(uint32_t*)r->count_buffer.mapped = r->draw_count;
}
void sprite_render(SpriteRenderer* r, VkCommandBuffer cmd, const Camera* cam)
{
    if(r->draw_count == 0)
        return;
    VkBufferDeviceAddressInfo addr_info = {
        .sType  = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
        .buffer = r->instance_buffer.buffer,
    };

    SpritePushConstants push = {
        .instance_ptr = vkGetBufferDeviceAddress(renderer.device, &addr_info) + r->instance_buffer.offset,

        .screen_size = {(float)renderer.swapchain.extent.width, (float)renderer.swapchain.extent.height},

        .sampler_id = renderer.default_samplers.samplers[SAMPLER_LINEAR_WRAP_ANISO],
    };

    glm_mat4_ucopy(cam->view_proj, push.view_proj);
    vkCmdPushConstants(cmd, renderer.bindless_system.pipeline_layout, VK_SHADER_STAGE_ALL, 0, sizeof(SpritePushConstants), &push);
    vk_cmd_set_viewport_scissor(cmd, renderer.swapchain.extent);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g_render_pipelines.pipelines[pipelines.sprite]);

    vkCmdDrawIndirectCount(cmd, r->indirect_buffer.buffer, r->indirect_buffer.offset, r->count_buffer.buffer,
                           r->count_buffer.offset, r->draw_count, sizeof(VkDrawIndirectCommand));
}
static void update_sprite_movement(Sprite2D* s, float speed)
{
    /*
    =========================================================
        INPUT → VELOCITY → NORMALIZE → APPLY → POSITION
    =========================================================

           W
           ↑
       A ← + → D
           ↓
           S

    velocity = direction vector from input
    normalize = prevents faster diagonal movement
    position += velocity * speed * dt
    */

    vec2        velocity = {0.0f, 0.0f};
    GLFWwindow* window   = renderer.window;
    // --- INPUT ---
    if(glfwGetKey(window, GLFW_KEY_W) == GLFW_PRESS)
        velocity[1] -= 1.0f;

    if(glfwGetKey(window, GLFW_KEY_S) == GLFW_PRESS)
        velocity[1] += 1.0f;

    if(glfwGetKey(window, GLFW_KEY_A) == GLFW_PRESS)
        velocity[0] -= 1.0f;

    if(glfwGetKey(window, GLFW_KEY_D) == GLFW_PRESS)
        velocity[0] += 1.0f;

    // --- NORMALIZE ---
    float len = sqrtf(velocity[0] * velocity[0] + velocity[1] * velocity[1]);

    if(len > 0.0f)
    {
        velocity[0] /= len;
        velocity[1] /= len;
    }

    // --- APPLY MOVEMENT ---
    float dt = renderer.dt;
    s->position[0] += velocity[0] * speed * dt;
    s->position[1] += velocity[1] * speed * dt;
}
int main(void)
{
    graphics_init();

    const CameraMode app_camera_mode = CAMERA_MODE_2D;

    Camera cam = {0};
    if(app_camera_mode == CAMERA_MODE_2D)
    {
        camera_defaults_2d(&cam, renderer.swapchain.extent.width, renderer.swapchain.extent.height);
        cam.ortho_height_world = (float)renderer.swapchain.extent.height;
        cam.zoom               = 1.0f;
        camera2d_set_position(&cam, (float)renderer.swapchain.extent.width * 0.5f, (float)renderer.swapchain.extent.height * 0.5f);
    }
    else
    {
        camera_defaults_3d(&cam);
        camera3d_set_position(&cam, 11.0f, 3.3f, 8.6f);
        camera3d_set_rotation_yaw_pitch(&cam, glm_rad(5.7f), glm_rad(0.0f));
    }

    SpriteRenderer sprites;

    sprite_renderer_init(&sprites, 10000);
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
#define MAX_ENTITIES 100

    Sprite2D entities[MAX_ENTITIES];
    int      entity_count     = 0;
    uint32_t pp_frame_counter = 0;
    while(!glfwWindowShouldClose(renderer.window))
    {
        //MU_SCOPE_TIMER("GAME")
        {
            text_system_begin_frame();
            sprite_begin(&sprites);
            update_sprite_movement(&brick_sprite, 233.00);
            entities[0]     = brick_sprite;
            entity_count    = 1;
            cam.position[0] = brick_sprite.position[0] - cam.viewport_width * 0.5f;
            cam.position[1] = brick_sprite.position[1] - cam.viewport_height * 0.5f;

            for(int i = 0; i < entity_count; i++)
            {
                update_sprite_movement(&entities[i], 200.0f);
                sprite_submit(&sprites, &entities[i]);
            }
            vec4 text_color = {1.0f, 2.0f, 2.0f, 1.0f};
            draw_text_2d("Hello World", 40.0f, 40.0f, 0.5f, text_color, 0.0f);
        }


        //     MU_SCOPE_TIMER("GRAPHICS CPU")
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

                //  MU_SCOPE_TIMER("SPRITE CPU")
                {
                    sprite_end(&sprites, cmd);
                }
                bool text_ready = text_system_prepare_gpu_data(cmd);

                if(!text_ready)
                {
                    text_system_handle_prepare_failure();
                }
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


                // --- RENDER ---

                //     MU_SCOPE_TIMER("SP and TE CPU")
                {
                    sprite_render(&sprites, cmd, &cam);
                    text_system_render(cmd);
                }
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
