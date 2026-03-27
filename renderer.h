
#pragma once

#include "vk.h"

 Renderer renderer ;

 extern Renderer renderer;
typedef struct
{
    uint32_t fullscreen;
    uint32_t postprocess;
    uint32_t triangle;
    uint32_t triangle_wireframe;

    uint32_t beam;
    uint32_t sky;
} EnginePipelines;

 EnginePipelines pipelines;
extern EnginePipelines pipelines;

#define VALIDATION false

void graphics_init()
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
    MU_SCOPE_TIMER("Renderer Creation")
    {
        renderer_create(&renderer, &desc);
    }
}
