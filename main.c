#include "renderer.h"
#include "passes.h"
#include "gltfloader.h"

#include <string.h>

#define GLTF_MODEL_PATH "assets/cubepets/Models/GLB format/animal-beaver.glb"

PUSH_CONSTANT(GltfMinimalPush,
              VkDeviceAddress pos_ptr;
              VkDeviceAddress idx_ptr;

              float model[4][4];
              float view_proj[4][4];

              vec4  color;
              uint32_t index_count;
              uint32_t _pad0;
              uint32_t _pad1;
              uint32_t _pad2;
);

typedef struct GltfGpuMesh
{
    BufferSlice position_slice;
    BufferSlice index_slice;
} GltfGpuMesh;

typedef struct GltfGpuModel
{
    GLTFContainer* cpu;
    GltfGpuMesh*   gpu_meshes;
    bool           uploaded;
} GltfGpuModel;


static VkDeviceAddress slice_device_address(const Renderer* r, BufferSlice slice)
{
    VkBufferDeviceAddressInfo info = {
        .sType  = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
        .buffer = slice.buffer,
    };
    return vkGetBufferDeviceAddress(r->device, &info) + slice.offset;
}

static bool gltf_gpu_model_init(GltfGpuModel* model)
{
    memset(model, 0, sizeof(*model));

    if(!loadGltf(GLTF_MODEL_PATH, GLTF_FLAG_LOAD_VERTICES | GLTF_FLAG_CALCULATE_BOUNDS, &model->cpu))
        return false;

    if(model->cpu->mesh_count == 0)
        return false;

    model->gpu_meshes = (GltfGpuMesh*)calloc(model->cpu->mesh_count, sizeof(GltfGpuMesh));
    if(!model->gpu_meshes)
        return false;

    for(uint32_t i = 0; i < model->cpu->mesh_count; ++i)
    {
        GLTFMesh* src = &model->cpu->meshes[i];
        if(!src->index || !src->attributes[GLTF_ATTRIBUTE_TYPE_POSITION])
            return false;

        VkDeviceSize pos_bytes = (VkDeviceSize)src->vertex_count * 3u * sizeof(float);
        VkDeviceSize idx_bytes = (VkDeviceSize)src->index_count * sizeof(uint32_t);

        model->gpu_meshes[i].position_slice = buffer_pool_alloc(&renderer.gpu_pool, pos_bytes, 16);
        model->gpu_meshes[i].index_slice    = buffer_pool_alloc(&renderer.gpu_pool, idx_bytes, 16);

        if(model->gpu_meshes[i].position_slice.buffer == VK_NULL_HANDLE || model->gpu_meshes[i].index_slice.buffer == VK_NULL_HANDLE)
            return false;
    }

    return true;
}

static void gltf_gpu_model_destroy(GltfGpuModel* model)
{
    if(model->gpu_meshes && model->cpu)
    {
        for(uint32_t i = 0; i < model->cpu->mesh_count; ++i)
        {
            if(model->gpu_meshes[i].position_slice.buffer != VK_NULL_HANDLE)
                buffer_pool_free(model->gpu_meshes[i].position_slice);
            if(model->gpu_meshes[i].index_slice.buffer != VK_NULL_HANDLE)
                buffer_pool_free(model->gpu_meshes[i].index_slice);
        }
    }

    free(model->gpu_meshes);
    freeGltf(model->cpu);
    memset(model, 0, sizeof(*model));
}

static void gltf_gpu_model_upload_once(GltfGpuModel* model, VkCommandBuffer cmd)
{
    if(model->uploaded)
        return;

    uint32_t barrier_count = model->cpu->mesh_count * 2u;
    VkBufferMemoryBarrier2* barriers = (VkBufferMemoryBarrier2*)calloc(barrier_count, sizeof(VkBufferMemoryBarrier2));
    if(!barriers)
        return;

    uint32_t b = 0;
    for(uint32_t i = 0; i < model->cpu->mesh_count; ++i)
    {
        GLTFMesh* src = &model->cpu->meshes[i];
        GltfGpuMesh* dst = &model->gpu_meshes[i];
        VkDeviceSize pos_bytes = (VkDeviceSize)src->vertex_count * 3u * sizeof(float);
        VkDeviceSize idx_bytes = (VkDeviceSize)src->index_count * sizeof(uint32_t);

        renderer_upload_buffer_to_slice(&renderer, cmd, dst->position_slice, src->attributes[GLTF_ATTRIBUTE_TYPE_POSITION], pos_bytes, 16);
        renderer_upload_buffer_to_slice(&renderer, cmd, dst->index_slice, src->index, idx_bytes, 16);

        barriers[b++] = (VkBufferMemoryBarrier2){
            .sType         = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
            .srcStageMask  = VK_PIPELINE_STAGE_2_COPY_BIT,
            .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
            .dstStageMask  = VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT,
            .dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
            .buffer        = dst->position_slice.buffer,
            .offset        = dst->position_slice.offset,
            .size          = pos_bytes,
        };
        barriers[b++] = (VkBufferMemoryBarrier2){
            .sType         = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
            .srcStageMask  = VK_PIPELINE_STAGE_2_COPY_BIT,
            .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
            .dstStageMask  = VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT,
            .dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
            .buffer        = dst->index_slice.buffer,
            .offset        = dst->index_slice.offset,
            .size          = idx_bytes,
        };
    }

    VkDependencyInfo dep = {
        .sType                    = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .bufferMemoryBarrierCount = b,
        .pBufferMemoryBarriers    = barriers,
    };
    vkCmdPipelineBarrier2(cmd, &dep);
    free(barriers);

    model->uploaded = true;
}

static void draw_gltf_model(VkCommandBuffer cmd, const Camera* cam, const GltfGpuModel* model)
{
    vk_cmd_set_viewport_scissor(cmd, renderer.swapchain.extent);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g_render_pipelines.pipelines[pipelines.gltf_minimal]);

    for(uint32_t i = 0; i < model->cpu->mesh_count; ++i)
    {
        GltfMinimalPush push = {0};
        push.pos_ptr = slice_device_address(&renderer, model->gpu_meshes[i].position_slice);
        push.idx_ptr = slice_device_address(&renderer, model->gpu_meshes[i].index_slice);
        push.index_count = model->cpu->meshes[i].index_count;

        glm_mat4_identity(push.model);
        memcpy(push.view_proj, cam->view_proj, sizeof(push.view_proj));

        push.color[0] = 0.86f;
        push.color[1] = 0.78f;
        push.color[2] = 0.63f;
        push.color[3] = 1.0f;

        vkCmdPushConstants(cmd, renderer.bindless_system.pipeline_layout, VK_SHADER_STAGE_ALL, 0, sizeof(GltfMinimalPush), &push);
        vkCmdDraw(cmd, push.index_count, 1, 0, 0);
    }
}

int main(void)
{
    graphics_init();

    Camera cam = {0};
    camera_defaults_3d(&cam);
    camera3d_set_position(&cam, 0.0f, 0.6f, 4.0f);
    camera3d_set_rotation_yaw_pitch(&cam, 0.0f, 0.0f);

    GltfGpuModel beaver = {0};
    if(!gltf_gpu_model_init(&beaver))
    {
        gltf_gpu_model_destroy(&beaver);
        renderer_destroy(&renderer);
        return 1;
    }

    while(!glfwWindowShouldClose(renderer.window))
    {
        TracyCFrameMark;

        pipeline_rebuild(&renderer);
        frame_start(&renderer, &cam);

        VkCommandBuffer cmd = renderer.frames[renderer.current_frame].cmdbuf;
        GpuProfiler*    frame_prof = &renderer.gpuprofiler[renderer.current_frame];

        vk_cmd_begin(cmd, false);
        gpu_profiler_begin_frame(frame_prof, cmd);
        {
		{
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, renderer.bindless_system.pipeline_layout, 0, 1,
                                    &renderer.bindless_system.set, 0, NULL);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, renderer.bindless_system.pipeline_layout, 0, 1,
                                    &renderer.bindless_system.set, 0, NULL);

            rt_transition_all(cmd, &renderer.depth[renderer.swapchain.current_image], VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                              VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                              VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT);

            rt_transition_all(cmd, &renderer.hdr_color[renderer.swapchain.current_image], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                              VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);

            image_transition_swapchain(cmd, &renderer.swapchain, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                       VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
            flush_barriers(cmd);
		}
            gltf_gpu_model_upload_once(&beaver, cmd);

            VkRenderingAttachmentInfo color = {
                .sType            = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                .imageView        = renderer.hdr_color[renderer.swapchain.current_image].view,
                .imageLayout      = renderer.hdr_color[renderer.swapchain.current_image].mip_states[0].layout,
                .loadOp           = VK_ATTACHMENT_LOAD_OP_CLEAR,
                .storeOp          = VK_ATTACHMENT_STORE_OP_STORE,
                .clearValue.color = {{0.10f, 0.12f, 0.15f, 1.0f}},
            };

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
            draw_gltf_model(cmd, &cam, &beaver);
            vkCmdEndRendering(cmd);

            post_pass();
            pass_smaa();
            pass_ldr_to_swapchain();

            image_transition_swapchain(cmd, &renderer.swapchain, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                                       VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, 0);
            flush_barriers(cmd);
        }
        vk_cmd_end(cmd);

        submit_frame(&renderer);
    }

    gltf_gpu_model_destroy(&beaver);
    renderer_destroy(&renderer);
    return 0;
}
