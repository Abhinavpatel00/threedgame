#include "renderer.h"


PUSH_CONSTANT(PostPush, uint32_t src_texture_id; uint32_t output_image_id; uint32_t depth_texture_id; uint32_t sampler_id;

              uint32_t bloom_texture_id;
              float bloom_intensity;

              uint32_t width;


              uint32_t height;

              uint frame

              ;

              float exposure;
              uint32_t dof_enabled;
              float    dof_focus_depth;
              float    dof_focus_range;
              float    dof_max_blur_radius;

);

PUSH_CONSTANT(BloomDownPush,
              uint32_t src_texture_id;
              uint32_t output_image_id;
              uint32_t sampler_id;
              uint32_t first_pass;
              uint32_t width;
              uint32_t height;
              float    threshold;
              float    threshold_knee;);

PUSH_CONSTANT(BloomUpPush,
              uint32_t src_texture_id;
              uint32_t output_image_id;
              uint32_t sampler_id;
              float    blend_factor;
              uint32_t width;
              uint32_t height;
              float    radius;
              float    pad0;);
PUSH_CONSTANT(EdgePush, uint32_t texture_id; uint32_t sampler_id;);


PUSH_CONSTANT(BlendPush, uint32_t color_tex; uint32_t weight_tex; uint32_t sampler_id; uint32_t pad;);

PUSH_CONSTANT(WeightPush, uint32_t edge_tex; uint32_t area_tex; uint32_t search_tex; uint32_t sampler_id;);


static uint32_t pp_frame_counter = 0;

static void pass_bloom(uint32_t current_image)
{
    VkCommandBuffer cmd        = renderer.frames[renderer.current_frame].cmdbuf;
    GpuProfiler*    frame_prof = &renderer.gpuprofiler[renderer.current_frame];

    GPU_SCOPE(frame_prof, cmd, "BLOOM", VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT)
    {
        rt_transition_all(cmd, &renderer.hdr_color[current_image], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                          VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        rt_transition_all(cmd, &renderer.bloom_chain[current_image][0], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                          VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);

        for(uint32_t mip = 1; mip < BLOOM_MIPS; ++mip)
        {
            rt_transition_all(cmd, &renderer.bloom_chain[current_image][mip], VK_IMAGE_LAYOUT_GENERAL,
                              VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                              VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
        }
        flush_barriers(cmd);

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g_render_pipelines.pipelines[pipelines.bloom_downsample]);

        const float bloom_threshold = 1.0f;
        const float bloom_knee      = 0.25f;

        for(uint32_t mip = 1; mip < BLOOM_MIPS; ++mip)
        {
            RenderTarget* dst = &renderer.bloom_chain[current_image][mip];

            BloomDownPush push  = {0};
            push.src_texture_id = renderer.bloom_chain[current_image][mip - 1].bindless_index;
            push.output_image_id = dst->bindless_index;
            push.sampler_id      = renderer.default_samplers.samplers[SAMPLER_LINEAR_CLAMP];
            push.first_pass      = 0u;
            push.width           = dst->width;
            push.height          = dst->height;
            push.threshold       = bloom_threshold;
            push.threshold_knee  = bloom_knee;

            vkCmdPushConstants(cmd, renderer.bindless_system.pipeline_layout, VK_SHADER_STAGE_ALL, 0, sizeof(BloomDownPush), &push);

            uint32_t gx = (push.width + 15) / 16;
            uint32_t gy = (push.height + 15) / 16;
            vkCmdDispatch(cmd, gx, gy, 1);

            rt_transition_all(cmd, dst, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                              VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
            flush_barriers(cmd);
        }

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g_render_pipelines.pipelines[pipelines.bloom_upsample]);
        for(uint32_t mip = BLOOM_MIPS - 1; mip > 0; --mip)
        {
            RenderTarget* src = &renderer.bloom_chain[current_image][mip];
            RenderTarget* dst = &renderer.bloom_chain[current_image][mip - 1];

            rt_transition_all(cmd, dst, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                              VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
            rt_transition_all(cmd, src, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                              VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
            flush_barriers(cmd);

            BloomUpPush push    = {0};
            push.src_texture_id = src->bindless_index;
            push.output_image_id = dst->bindless_index;
            push.sampler_id      = renderer.default_samplers.samplers[SAMPLER_LINEAR_CLAMP];
            push.blend_factor    = 0.85f;
            push.width           = dst->width;
            push.height          = dst->height;
            push.radius          = 1.0f;

            vkCmdPushConstants(cmd, renderer.bindless_system.pipeline_layout, VK_SHADER_STAGE_ALL, 0, sizeof(BloomUpPush), &push);

            uint32_t gx = (push.width + 15) / 16;
            uint32_t gy = (push.height + 15) / 16;
            vkCmdDispatch(cmd, gx, gy, 1);

            rt_transition_all(cmd, dst, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                              VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
            flush_barriers(cmd);
        }
    }
}

void            post_pass()
{

    VkCommandBuffer cmd        = renderer.frames[renderer.current_frame].cmdbuf;
    GpuProfiler*    frame_prof = &renderer.gpuprofiler[renderer.current_frame];
    uint32_t current_image = renderer.swapchain.current_image;

    pass_bloom(current_image);

    GPU_SCOPE(frame_prof, cmd, "POST", VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT)
    {
        rt_transition_all(cmd, &renderer.hdr_color[current_image], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                          VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        rt_transition_all(cmd, &renderer.bloom_chain[current_image][0], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                          VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        rt_transition_all(cmd, &renderer.depth[current_image], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                          VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        rt_transition_all(cmd, &renderer.ldr_color[current_image], VK_IMAGE_LAYOUT_GENERAL,
                          VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);

        flush_barriers(cmd);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g_render_pipelines.pipelines[pipelines.postprocess]);

        PostPush pp_push        = {0};
        pp_push.src_texture_id   = renderer.hdr_color[current_image].bindless_index;
        pp_push.output_image_id  = renderer.ldr_color[current_image].bindless_index;
        pp_push.depth_texture_id = renderer.depth[current_image].bindless_index;
        pp_push.sampler_id      = renderer.default_samplers.samplers[SAMPLER_LINEAR_CLAMP];
        pp_push.bloom_texture_id = renderer.bloom_chain[current_image][0].bindless_index;
        pp_push.bloom_intensity  = 0.66f;
        pp_push.width           = renderer.swapchain.extent.width;
        pp_push.height          = renderer.swapchain.extent.height;
        pp_push.frame           = pp_frame_counter++;

        pp_push.exposure          = 1.2f;
        pp_push.dof_enabled       = 1;
        pp_push.dof_focus_depth   = 0.020f;
        pp_push.dof_focus_range   = 0.015f;
        pp_push.dof_max_blur_radius = 6.0f;
        vkCmdPushConstants(cmd, renderer.bindless_system.pipeline_layout, VK_SHADER_STAGE_ALL, 0, sizeof(PostPush), &pp_push);

        uint32_t gx = (pp_push.width + 15) / 16;
        uint32_t gy = (pp_push.height + 15) / 16;


        vkCmdDispatch(cmd, gx, gy, 1);
    }
}

void pass_smaa()
{

    VkCommandBuffer cmd        = renderer.frames[renderer.current_frame].cmdbuf;
    GpuProfiler*    frame_prof = &renderer.gpuprofiler[renderer.current_frame];

    uint32_t current_image = renderer.swapchain.current_image;


    GPU_SCOPE(frame_prof, cmd, "SMAA", VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT)
    {
        rt_transition_all(cmd, &renderer.smaa_edges[current_image], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                          VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
        flush_barriers(cmd);
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
        flush_barriers(cmd);
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
    flush_barriers(cmd);

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
}


// image bliting
void pass_ldr_to_swapchain()
{

    VkCommandBuffer cmd        = renderer.frames[renderer.current_frame].cmdbuf;
    GpuProfiler*    frame_prof = &renderer.gpuprofiler[renderer.current_frame];

    uint32_t current_image = renderer.swapchain.current_image;

    GPU_SCOPE(frame_prof, cmd, "LDR_COPY", VK_PIPELINE_STAGE_2_TRANSFER_BIT)
    {
        rt_transition_all(cmd, &renderer.ldr_color[renderer.swapchain.current_image], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                          VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
        image_transition_swapchain(renderer.frames[renderer.current_frame].cmdbuf, &renderer.swapchain,
                                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_TRANSFER_BIT, 0);
        flush_barriers(cmd);
        VkImageBlit blit = {
            .srcSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .mipLevel = 0, .baseArrayLayer = 0, .layerCount = 1},
            .srcOffsets = {{0, 0, 0}, {renderer.swapchain.extent.width, renderer.swapchain.extent.height, 1}},

            .dstSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .mipLevel = 0, .baseArrayLayer = 0, .layerCount = 1},
            .dstOffsets = {{0, 0, 0}, {renderer.swapchain.extent.width, renderer.swapchain.extent.height, 1}}};

        vkCmdBlitImage(cmd, renderer.ldr_color[renderer.swapchain.current_image].image,
                       VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, renderer.swapchain.images[renderer.swapchain.current_image],
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_NEAREST);
    }
}

// imgui pass

void pass_imgui()
{

    VkCommandBuffer cmd        = renderer.frames[renderer.current_frame].cmdbuf;
    GpuProfiler*    frame_prof = &renderer.gpuprofiler[renderer.current_frame];

    uint32_t current_image = renderer.swapchain.current_image;


    GPU_SCOPE(frame_prof, cmd, "IMGUI", VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT)
    {
        image_transition_swapchain(cmd, &renderer.swapchain, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                   VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);

        flush_barriers(cmd);
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
}


    PUSH_CONSTANT(SkyPush, float inv_proj[4][4]; float basis_right[4]; float basis_up[4]; float basis_back[4];
                  float time; float cirrus; float cumulus; float pad0;);


// void pass_sky()
// {
//
//
//     // ── Sky pass ──────────────────────────────────────────
//     {
//         vec3 forward = {
//             cosf(cam.pitch) * sinf(cam.yaw),
//             sinf(cam.pitch),
//             -cosf(cam.pitch) * cosf(cam.yaw),
//         };
//         glm_vec3_normalize(forward);
//
//         vec3 world_up = {0.0f, 1.0f, 0.0f};
//         vec3 right    = {0.0f};
//         vec3 up       = {0.0f};
//         glm_vec3_cross(forward, world_up, right);
//         glm_vec3_normalize(right);
//         glm_vec3_cross(right, forward, up);
//
//         float aspect = (float)renderer.swapchain.extent.width / (float)renderer.swapchain.extent.height;
//         mat4  proj   = GLM_MAT4_IDENTITY_INIT;
//         mat4  inv_proj;
//         camera_build_proj_reverse_z_infinite(proj, &cam, aspect);
//         proj[1][1] *= -1.0f;
//         glm_mat4_inv(proj, inv_proj);
//
//         SkyPush sky_push = {0};
//         memcpy(sky_push.inv_proj, inv_proj, sizeof(sky_push.inv_proj));
//         sky_push.basis_right[0] = right[0];
//         sky_push.basis_right[1] = right[1];
//         sky_push.basis_right[2] = right[2];
//         sky_push.basis_up[0]    = up[0];
//         sky_push.basis_up[1]    = up[1];
//         sky_push.basis_up[2]    = up[2];
//         sky_push.basis_back[0]  = -forward[0];
//         sky_push.basis_back[1]  = -forward[1];
//         sky_push.basis_back[2]  = -forward[2];
//         sky_push.time           = (float)glfwGetTime() * 0.2f;
//         sky_push.cirrus         = 0.4f;
//         sky_push.cumulus        = 0.8f;
//
//         vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g_render_pipelines.pipelines[pipelines.sky]);
//         vk_cmd_set_viewport_scissor(cmd, renderer.swapchain.extent);
//         vkCmdPushConstants(cmd, renderer.bindless_system.pipeline_layout, VK_SHADER_STAGE_ALL, 0, sizeof(SkyPush), &sky_push);
//         vkCmdDraw(cmd, 4, 1, 0, 0);
//     }
// }
