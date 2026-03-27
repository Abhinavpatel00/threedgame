#include "renderer.h"

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
