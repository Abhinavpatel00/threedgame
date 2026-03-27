#include "renderer.h"


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


static uint32_t pp_frame_counter = 0;
void            post_pass()
{

    VkCommandBuffer cmd        = renderer.frames[renderer.current_frame].cmdbuf;
    GpuProfiler*    frame_prof = &renderer.gpuprofiler[renderer.current_frame];
    GPU_SCOPE(frame_prof, cmd, "POST", VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT)
    {
        rt_transition_all(cmd, &renderer.hdr_color[renderer.swapchain.current_image], VK_IMAGE_LAYOUT_GENERAL,
                          VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT);

        flush_barriers(cmd);
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

// imgui pass

void pass_imgui()
{

    VkCommandBuffer cmd        = renderer.frames[renderer.current_frame].cmdbuf;
    GpuProfiler*    frame_prof = &renderer.gpuprofiler[renderer.current_frame];

    uint32_t current_image = renderer.swapchain.current_image;


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
