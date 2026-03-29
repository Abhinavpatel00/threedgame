#include "renderer.h"

Renderer renderer;
EnginePipelines pipelines;

void graphics_init(void)
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
                                           | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        .vsync               = false,
        .enable_debug_printf = false,

        .bindless_sampled_image_count     = 65536,
        .bindless_sampler_count           = 256,
        .bindless_storage_image_count     = 16384,
        .enable_pipeline_stats            = false,
        .swapchain_preferred_present_mode = VK_PRESENT_MODE_IMMEDIATE_KHR,

        .size_of_cpu_pool     = MB(32),
        .size_of_gpu_pool     = MB(512),
        .size_of_staging_pool = MB(128),

    };
    MU_SCOPE_TIMER("Renderer Creation")
    {
        renderer_create(&renderer, &desc);
    }

gfx_pipelines();
}









void gfx_pipelines()
{
    MU_SCOPE_TIMER("Pipeline construction")
    {
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
            cfg.vert_path              = "compiledshaders/gltf_uber.vert.spv";
            cfg.frag_path              = "compiledshaders/gltf_uber.frag.spv";
            cfg.color_attachment_count = 1;
            cfg.color_formats          = &renderer.hdr_color[1].format;
            cfg.depth_format           = renderer.depth[1].format;
            cfg.depth_test_enable      = true;
            cfg.depth_write_enable     = true;
            cfg.topology               = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
            pipelines.gltf_minimal     = pipeline_create_graphics(&renderer, &cfg);
        }
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
            GraphicsPipelineConfig cfg = pipeline_config_default();
            cfg.vert_path              = "compiledshaders/sprite.vert.spv";
            cfg.frag_path              = "compiledshaders/sprite.frag.spv";
            cfg.color_attachment_count = 1;
            cfg.color_formats          = &renderer.hdr_color[1].format;
            cfg.depth_format           = renderer.depth[1].format;
            cfg.depth_test_enable      = false;
            cfg.depth_write_enable     = false;
            cfg.cull_mode              = VK_CULL_MODE_NONE;
            cfg.topology               = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
            cfg.blends[0]              = (ColorAttachmentBlend){.blend_enable = true,
                                                                 .src_color    = VK_BLEND_FACTOR_SRC_ALPHA,
                                                                 .dst_color    = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
                                                                 .color_op     = VK_BLEND_OP_ADD,
                                                                 .src_alpha    = VK_BLEND_FACTOR_ONE,
                                                                 .dst_alpha    = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
                                                                 .alpha_op     = VK_BLEND_OP_ADD,
                                                                 .write_mask   = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT
                                                                               | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT};
            pipelines.sprite           = pipeline_create_graphics(&renderer, &cfg);
        }
        {
            GraphicsPipelineConfig cfg = pipeline_config_default();
            cfg.vert_path              = "compiledshaders/slug_text.vert.spv";
            cfg.frag_path              = "compiledshaders/slug_text.frag.spv";
            cfg.color_attachment_count = 1;
            cfg.color_formats          = &renderer.hdr_color[1].format;
            cfg.depth_format           = renderer.depth[1].format;
            cfg.depth_test_enable      = false;
            cfg.depth_write_enable     = false;
            cfg.cull_mode              = VK_CULL_MODE_NONE;
            cfg.topology               = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
            cfg.blends[0]              = (ColorAttachmentBlend){.blend_enable = true,
                                                                 .src_color    = VK_BLEND_FACTOR_SRC_ALPHA,
                                                                 .dst_color    = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
                                                                 .color_op     = VK_BLEND_OP_ADD,
                                                                 .src_alpha    = VK_BLEND_FACTOR_ONE,
                                                                 .dst_alpha    = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
                                                                 .alpha_op     = VK_BLEND_OP_ADD,
                                                                 .write_mask   = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT
                                                                               | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT};

            pipelines.slug_text = pipeline_create_graphics(&renderer, &cfg);
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
    }
}




