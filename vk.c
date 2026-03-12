#include "vk.h"
#include "flow/flow.h"
#include "tinytypes.h"
#include <assert.h>
#include <signal.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <vulkan/vulkan_core.h>
#include "external/SPIRV-Reflect/spirv_reflect.h"
//             https://vulkan.org/user/pages/09.events/vulkanised-2023/vulkanised_2023_using_vulkan_validation_effectively.pdf
#include "external/debugbreak/debugbreak.h"
#include "external/smaa/Textures/AreaTex.h"
#include "external/smaa/Textures/SearchTex.h"
// Forward declarations are only useful for pointers:
//
// typedef struct Texture Texture;
//
// Texture* create_texture();
//
// Because pointer size is known.
//
// But arrays need full struct size, so forward declarations don't work.


flow_id_pool      pipeline_id_pool                = {0};
flow_id_pool      texture_pool                    = {0};
flow_id_pool      sampler_pool                    = {0};
Texture           textures[MAX_BINDLESS_TEXTURES] = {0};  // reference by textureid
VkSampler         samplers[MAX_BINDLESS_SAMPLERS] = {0};  // reference by samplerid
RendererPipelines g_render_pipelines              = {0};


static void spv_to_slang(const char* spv, char* out)
{
    const char* name = strrchr(spv, '/');
    name             = name ? name + 1 : spv;

    char tmp[256];
    strcpy(tmp, name);

    char* stage = strstr(tmp, ".vert");
    if(!stage)
        stage = strstr(tmp, ".frag");
    if(!stage)
        stage = strstr(tmp, ".comp");

    if(stage)
        *stage = '\0';

    sprintf(out, "shaders/%s.slang", tmp);
}

static const char* path_basename(const char* path)
{
    const char* slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

static bool shader_change_matches_spv(const char* changed, const char* spv)
{
    if(!changed || !spv)
        return false;

    char slang[256];
    spv_to_slang(spv, slang);

    if(strstr(changed, slang))
        return true;

    const char* changed_name = path_basename(changed);
    const char* slang_name   = path_basename(slang);

    return strcmp(changed_name, slang_name) == 0;
}
bool is_instance_extension_supported(const char* extension_name)
{
    uint32_t extensionCount = 0;
    vkEnumerateInstanceExtensionProperties(NULL, &extensionCount, NULL);
    VkExtensionProperties* extensions = malloc(extensionCount * sizeof(VkExtensionProperties));
    vkEnumerateInstanceExtensionProperties(NULL, &extensionCount, extensions);

    forEach(i, extensionCount)
    {
        if(strcmp(extension_name, extensions[i].extensionName) == 0)
        {
            free(extensions);
            return true;
        }
    }

    free(extensions);
    return false;
}

bool is_instance_layer_supported(const char* layer_name)
{
    uint32_t layer_count = 0;
    vkEnumerateInstanceLayerProperties(&layer_count, NULL);
    VkLayerProperties* layers = malloc(layer_count * sizeof(VkLayerProperties));
    vkEnumerateInstanceLayerProperties(&layer_count, layers);

    forEach(i, layer_count)
    {
        if(strcmp(layer_name, layers[i].layerName) == 0)
        {
            free(layers);
            return true;
        }
    }

    free(layers);
    return false;
}

#ifdef DEBUG
#if __GNUC__
#define ASSERT(c)                                                                                                      \
    if(!(c))                                                                                                           \
    __builtin_trap()
#elif _MSC_VER
#define ASSERT(c)                                                                                                      \
    if(!(c))                                                                                                           \
    __debugbreak()
#else
#define ASSERT(c)                                                                                                      \
    if(!(c))                                                                                                           \
    *(volatile int*)0 = 0
#endif
#else
#define ASSERT(c)
#endif

VKAPI_ATTR VkBool32 VKAPI_CALL debug_callback(VkDebugUtilsMessageSeverityFlagBitsEXT      severity,
                                              VkDebugUtilsMessageTypeFlagsEXT             type,
                                              const VkDebugUtilsMessengerCallbackDataEXT* data,
                                              void*                                       user_data)
{
    (void)type;
    (void)user_data;


    const char* tag = "MSG";

    if(severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
        tag = "ERROR";
    else if(severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
        tag = "WARN";
    else if(severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT)
        tag = "INFO";
    else if(severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT)
        tag = "VERBOSE";


    fprintf(stderr, "[VULKAN %s] %s\n", tag, data->pMessage);
    if(severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
    {
        debug_break();
    }

    // {
    //     raise(SIGTRAP);
    // }

    return VK_FALSE;
}


bool device_supports_extensions(VkPhysicalDevice gpu, const char** req, uint32_t req_count)
{
    uint32_t count = 0;
    vkEnumerateDeviceExtensionProperties(gpu, NULL, &count, NULL);

    VkExtensionProperties* props = malloc(sizeof(*props) * count);
    vkEnumerateDeviceExtensionProperties(gpu, NULL, &count, props);

    for(uint32_t i = 0; i < req_count; i++)
    {
        bool found = false;

        for(uint32_t j = 0; j < count; j++)
        {
            if(strcmp(req[i], props[j].extensionName) == 0)
            {
                found = true;
                break;
            }
        }

        if(!found)
        {
            log_error("[extensions] missing: %s", req[i]);
            free(props);
            return false;
        }

        log_info("[extensions] enabled: %s", req[i]);
    }

    free(props);
    return true;
}


typedef struct GpuScore
{
    VkPhysicalDevice device;
    uint32_t         score;
} GpuScore;

static uint32_t score_physical_device(VkPhysicalDevice gpu, VkSurfaceKHR surface, const char** required_exts, uint32_t required_ext_count)
{
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(gpu, &props);

    fprintf(stderr, "[GPU] Evaluating: %s\n", props.deviceName);

    if(!device_supports_extensions(gpu, required_exts, required_ext_count))
    {
        fprintf(stderr, "  -> rejected: missing required extensions\n");
        return 0;
    }

    uint32_t queue_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(gpu, &queue_count, NULL);

    VkQueueFamilyProperties qprops[32];
    vkGetPhysicalDeviceQueueFamilyProperties(gpu, &queue_count, qprops);

    VkBool32 can_present = VK_FALSE;
    forEach(i, queue_count)
    {
        VkBool32 present = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(gpu, i, surface, &present);
        if(present)
        {
            can_present = VK_TRUE;
            break;
        }
    }

    if(!can_present)
    {
        fprintf(stderr, "  -> rejected: cannot present to surface\n");
        return 0;
    }

    uint32_t score = 0;

    switch(props.deviceType)
    {
        case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:
            score += 1000;
            fprintf(stderr, "  + discrete bonus: 1000\n");
            break;
        case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU:
            score += 600;
            fprintf(stderr, "  + integrated bonus: 600\n");
            break;
        case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:
            score += 300;
            fprintf(stderr, "  + virtual bonus: 300\n");
            break;
        case VK_PHYSICAL_DEVICE_TYPE_CPU:
            score += 50;
            fprintf(stderr, "  + cpu fallback: 50\n");
            break;
        default:
            fprintf(stderr, "  + unknown type: 0\n");
            break;
    }

    VkPhysicalDeviceMemoryProperties mem;
    vkGetPhysicalDeviceMemoryProperties(gpu, &mem);

    for(uint32_t i = 0; i < mem.memoryHeapCount; i++)
    {
        if(mem.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
        {
            uint32_t add = (uint32_t)(mem.memoryHeaps[i].size / (1024 * 1024 * 64));
            score += add;
            fprintf(stderr, "  + VRAM factor: %u\n", add);
        }
    }

    if(score == 0)
        score = 1;

    fprintf(stderr, "  -> final score: %u\n\n", score);
    return score;
}

VkPhysicalDevice pick_physical_device(VkInstance instance, VkSurfaceKHR surface, RendererDesc* rcd)
{
    uint32_t count = 0;
    vkEnumeratePhysicalDevices(instance, &count, NULL);
    if(count == 0)
    {
        fprintf(stderr, "[GPU] No Vulkan devices found. Tragic.\n");
        return VK_NULL_HANDLE;
    }

    VkPhysicalDevice devices[16];
    vkEnumeratePhysicalDevices(instance, &count, devices);

    GpuScore best = {0};

    log_info("[GPU] Found %u device(s). Scoring...", count);

    for(uint32_t i = 0; i < count; i++)
    {
        uint32_t score = score_physical_device(devices[i], surface, rcd->device_extensions, rcd->device_extension_count);

        if(score > best.score)
        {
            best.device = devices[i];
            best.score  = score;
        }
    }

    if(best.device == VK_NULL_HANDLE)
    {
        fprintf(stderr, "[GPU] No suitable device found. Time to rethink life choices.\n");
    }
    else
    {
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(best.device, &props);
        log_info("[GPU] Selected device: %s (score %u)", props.deviceName, best.score);
    }

    return best.device;
}

void query_device_features(VkPhysicalDevice gpu, VkFeatureChain* out)
{
    memset(out, 0, sizeof(*out));

    out->core.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    out->v11.sType  = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;
    out->v12.sType  = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    out->v13.sType  = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;

    // maintenance5 feature struct
    out->maintenance5.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_FEATURES_KHR;

    // shader non-semantic info feature struct (for debug printf)
    out->shaderNonSemanticInfo.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_NON_SEMANTIC_INFO_FEATURES_KHR;

    // Chain: core -> v11 -> v12 -> v13 -> maintenance5 -> shaderNonSemanticInfo
    out->core.pNext         = &out->v11;
    out->v11.pNext          = &out->v12;
    out->v12.pNext          = &out->v13;
    out->v13.pNext          = &out->maintenance5;
    out->maintenance5.pNext = &out->shaderNonSemanticInfo;

    vkGetPhysicalDeviceFeatures2(gpu, &out->core);
}


typedef struct RendererCaps
{
    bool dynamic_rendering;
    bool sync2;
    bool descriptor_indexing;
    bool timeline_semaphores;
    bool multi_draw_indirect;
    bool multi_draw_indirect_count;
    bool buffer_device_address;
    bool maintenance4;
    bool bindless_textures;

    bool sampler_anisotropy;     // NEW
    bool atomic_int64;           // NEW
    bool scalar_block_layout;    // NEW
    bool robustness2;            // NEW
    bool index_type_uint8;       // NEW
    bool subgroup_size_control;  // NEW
    bool debug_printf;           // Debug Printf support (VK_KHR_shader_non_semantic_info)
    bool pipeline_statistics_query;
} RendererCaps;

RendererCaps default_caps(void)
{
    return (RendererCaps){
        .dynamic_rendering         = true,
        .sync2                     = true,
        .descriptor_indexing       = true,
        .timeline_semaphores       = true,
        .multi_draw_indirect       = true,
        .multi_draw_indirect_count = true,
        .buffer_device_address     = true,
        .maintenance4              = true,
        .bindless_textures         = true,

        .sampler_anisotropy        = true,
        .atomic_int64              = true,
        .scalar_block_layout       = true,
        .robustness2               = false,  // I’ll explain below
        .index_type_uint8          = true,
        .subgroup_size_control     = false,  // enable later if you need it
        .pipeline_statistics_query = false,
    };
}
static void enable_desc_indexing_feature(VkBool32* feature_field, const char* name)
{
    // feature_field currently contains "supported?" from vkGetPhysicalDeviceFeatures2
    if(*feature_field)
    {
        *feature_field = VK_TRUE;  // enable
        log_info("[bindless] enabled: %s", name);
    }
    else
    {
        log_info("[bindless] NOT available: %s", name);
        // leave it VK_FALSE
    }
}
static void apply_caps(VkFeatureChain* f, const RendererCaps* caps)
{
#define TRY_ENABLE(flag, supported, name)                                                                              \
    do                                                                                                                 \
    {                                                                                                                  \
        if((caps->flag) && (supported))                                                                                \
        {                                                                                                              \
            (supported) = VK_TRUE;                                                                                     \
            log_info("[features] enabled: %s", name);                                                                  \
        }                                                                                                              \
        else if(caps->flag)                                                                                            \
        {                                                                                                              \
            log_info("[features] unavailable: %s", name);                                                              \
        }                                                                                                              \
    } while(0)
    if(f->v11.shaderDrawParameters)
    {
        f->v11.shaderDrawParameters = VK_TRUE;
        log_info("[features] enabled: shaderDrawParameters (vulkan 1.1)");
    }
    else
    {
        log_info("[features] unavailable: shaderDrawParameters (vulkan 1.1)");
    }
    if(f->maintenance5.maintenance5)
    {
        f->maintenance5.maintenance5 = VK_TRUE;
        log_info("[features] enabled: maintenance5 (VK_KHR_maintenance5)");
    }
    else
    {
        log_info("[features] unavailable: maintenance5 (VK_KHR_maintenance5)");
    }

    TRY_ENABLE(debug_printf, f->shaderNonSemanticInfo.shaderNonSemanticInfo, "shaderNonSemanticInfo (Debug Printf)");

    TRY_ENABLE(sampler_anisotropy, f->core.features.samplerAnisotropy, "samplerAnisotropy");
    TRY_ENABLE(multi_draw_indirect, f->core.features.multiDrawIndirect, "multi-draw indirect");
    TRY_ENABLE(pipeline_statistics_query, f->core.features.pipelineStatisticsQuery, "pipeline statistics query");
    TRY_ENABLE(dynamic_rendering, f->v13.dynamicRendering, "dynamic rendering");
    TRY_ENABLE(sync2, f->v13.synchronization2, "synchronization2");
    TRY_ENABLE(descriptor_indexing, f->v12.descriptorIndexing, "descriptor indexing (vulkan 1.2)");
    TRY_ENABLE(timeline_semaphores, f->v12.timelineSemaphore, "timeline semaphores");
    TRY_ENABLE(multi_draw_indirect_count, f->v12.drawIndirectCount, "multi-draw indirect count (v1.2)");
    TRY_ENABLE(buffer_device_address, f->v12.bufferDeviceAddress, "buffer device address");
    TRY_ENABLE(maintenance4, f->v13.maintenance4, "maintenance4");

    if(caps->bindless_textures)
    {
        if(!f->v12.descriptorIndexing)
        {
            log_info("[bindless] descriptor indexing umbrella feature not supported -> bindless likely impossible");
        }


        enable_desc_indexing_feature(&f->v12.runtimeDescriptorArray, "runtimeDescriptorArray");

        enable_desc_indexing_feature(&f->v12.descriptorBindingVariableDescriptorCount, "descriptorBindingVariableDescriptorCount");

        enable_desc_indexing_feature(&f->v12.descriptorBindingPartiallyBound, "descriptorBindingPartiallyBound");

        enable_desc_indexing_feature(&f->v12.descriptorBindingSampledImageUpdateAfterBind, "descriptorBindingSampledImageUpdateAfterBind");

        enable_desc_indexing_feature(&f->v12.shaderSampledImageArrayNonUniformIndexing, "shaderSampledImageArrayNonUniformIndexing");
    }

#undef TRY_ENABLE
}

typedef struct queue_families
{
    VkQueue graphics_queue;
    VkQueue present_queue;
    VkQueue compute_queue;
    VkQueue transfer_queue;

    uint32_t graphics_family;
    uint32_t present_family;
    uint32_t compute_family;
    uint32_t transfer_family;

    int has_graphics;
    int has_present;
    int has_compute;
    int has_transfer;
} queue_families;
//  pick GPU → choose queue families → create VkDevice → get queues
// Fills `out` with available queue families.
// Must be called BEFORE logical device creation.
void find_queue_families(VkPhysicalDevice device, VkSurfaceKHR surface, queue_families* out);
// Call AFTER vkCreateDevice.
// Uses the family indices already stored in queue_families.
void init_device_queues(VkDevice device, queue_families* q);


void find_queue_families(VkPhysicalDevice device, VkSurfaceKHR surface, queue_families* out)
{
    // initialize everything
    *out = (queue_families){.graphics_queue = VK_NULL_HANDLE,
                            .present_queue  = VK_NULL_HANDLE,
                            .compute_queue  = VK_NULL_HANDLE,
                            .transfer_queue = VK_NULL_HANDLE,

                            .graphics_family = 0,
                            .present_family  = 0,
                            .compute_family  = 0,
                            .transfer_family = 0,

                            .has_graphics = 0,
                            .has_present  = 0,
                            .has_compute  = 0,
                            .has_transfer = 0};

    uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(device, &count, NULL);
    if(count == 0)
        return;

    VkQueueFamilyProperties* families = (VkQueueFamilyProperties*)malloc(sizeof(VkQueueFamilyProperties) * count);

    if(!families)
        return;

    vkGetPhysicalDeviceQueueFamilyProperties(device, &count, families);

    forEach(i, count)
    {
        const VkQueueFamilyProperties* f = &families[i];

        if(!out->has_graphics && (f->queueFlags & VK_QUEUE_GRAPHICS_BIT))
        {
            out->graphics_family = i;
            out->has_graphics    = 1;
        }

        if(!out->has_compute && (f->queueFlags & VK_QUEUE_COMPUTE_BIT))
        {
            out->compute_family = i;
            out->has_compute    = 1;
        }

        if(!out->has_transfer && (f->queueFlags & VK_QUEUE_TRANSFER_BIT))
        {
            out->transfer_family = i;
            out->has_transfer    = 1;
        }

        if(!out->has_present)
        {
            VkBool32 presentSupport = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(device, i, surface, &presentSupport);
            if(presentSupport)
            {
                out->present_family = i;
                out->has_present    = 1;
            }
        }

        if(out->has_graphics && out->has_present && out->has_compute && out->has_transfer)
        {
            break;
        }
    }

    free(families);
}

void init_device_queues(VkDevice device, queue_families* q)
{
    if(q->has_graphics)
        vkGetDeviceQueue(device, q->graphics_family, 0, &q->graphics_queue);

    if(q->has_present)
        vkGetDeviceQueue(device, q->present_family, 0, &q->present_queue);

    if(q->has_compute)
        vkGetDeviceQueue(device, q->compute_family, 0, &q->compute_queue);

    if(q->has_transfer)
        vkGetDeviceQueue(device, q->transfer_family, 0, &q->transfer_queue);
}
static bool device_has_extension(VkPhysicalDevice gpu, const char* ext)
{
    uint32_t count = 0;
    vkEnumerateDeviceExtensionProperties(gpu, NULL, &count, NULL);

    VkExtensionProperties* props = malloc(sizeof(*props) * count);
    vkEnumerateDeviceExtensionProperties(gpu, NULL, &count, props);

    bool found = false;
    forEach(i, count)
    {
        if(strcmp(props[i].extensionName, ext) == 0)
        {
            found = true;
            break;
        }
    }

    free(props);
    return found;
}


#ifndef PIPELINE_CACHE_MAGIC
#define PIPELINE_CACHE_MAGIC 0xCAFEBABE
#endif

typedef struct PipelineCachePrefixHeader
{
    uint32_t magic;
    uint32_t dataSize;
    uint64_t dataHash;

    uint32_t vendorID;
    uint32_t deviceID;
    uint32_t driverVersion;
    uint32_t driverABI;

    uint8_t uuid[VK_UUID_SIZE];
} PipelineCachePrefixHeader;

// very boring but reliable hash (FNV-1a)


static int write_all(FILE* f, const void* data, size_t size)
{
    return fwrite(data, 1, size, f) == size;
}

static int read_all(FILE* f, void* data, size_t size)
{
    return fread(data, 1, size, f) == size;
}

static void get_device_props(VkPhysicalDevice phys, VkPhysicalDeviceProperties* out)
{
    vkGetPhysicalDeviceProperties(phys, out);
}

static int validate_header(const PipelineCachePrefixHeader* h, const VkPhysicalDeviceProperties* props)
{
    if(h->magic != PIPELINE_CACHE_MAGIC)
        return 0;
    if(h->driverABI != sizeof(void*))
        return 0;
    if(h->vendorID != props->vendorID)
        return 0;
    if(h->deviceID != props->deviceID)
        return 0;
    if(h->driverVersion != props->driverVersion)
        return 0;
    if(memcmp(h->uuid, props->pipelineCacheUUID, VK_UUID_SIZE) != 0)
        return 0;
    return 1;
}

VkPipelineCache pipeline_cache_load_or_create(VkDevice device, VkPhysicalDevice phys, const char* path)
{
    VkPhysicalDeviceProperties props;
    get_device_props(phys, &props);

    VkPipelineCache cache = VK_NULL_HANDLE;

    FILE* f = fopen(path, "rb");
    if(!f)
    {
        // file missing, build empty cache
        VkPipelineCacheCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
        vkCreatePipelineCache(device, &ci, NULL, &cache);
        return cache;
    }

    PipelineCachePrefixHeader hdr;
    if(!read_all(f, &hdr, sizeof(hdr)))
    {
        fclose(f);
        goto fallback;
    }

    if(!validate_header(&hdr, &props))
    {
        fclose(f);
        goto fallback;
    }

    void* blob = malloc(hdr.dataSize);
    if(!blob)
    {
        fclose(f);
        goto fallback;
    }

    if(!read_all(f, blob, hdr.dataSize))
    {
        free(blob);
        fclose(f);
        goto fallback;
    }

    fclose(f);

    if(hash64_bytes(blob, hdr.dataSize) != hdr.dataHash)
    {
        free(blob);
        goto fallback;
    }

    VkPipelineCacheCreateInfo ci = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO, .initialDataSize = hdr.dataSize, .pInitialData = blob};

    VkResult res = vkCreatePipelineCache(device, &ci, NULL, &cache);
    free(blob);

    if(res != VK_SUCCESS)
    {


    fallback: {
        VkPipelineCacheCreateInfo empty = {.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};

        vkCreatePipelineCache(device, &empty, NULL, &cache);
    }
    }

    return cache;
}

void pipeline_cache_save(VkDevice device, VkPhysicalDevice phys, VkPipelineCache cache, const char* path)
{
    size_t size = 0;
    vkGetPipelineCacheData(device, cache, &size, NULL);
    if(size == 0)
        return;

    void* blob = malloc(size);
    if(!blob)
        return;

    vkGetPipelineCacheData(device, cache, &size, blob);

    VkPhysicalDeviceProperties props;
    get_device_props(phys, &props);

    PipelineCachePrefixHeader hdr = {.magic         = PIPELINE_CACHE_MAGIC,
                                     .dataSize      = (uint32_t)size,
                                     .dataHash      = hash64_bytes(blob, size),
                                     .vendorID      = props.vendorID,
                                     .deviceID      = props.deviceID,
                                     .driverVersion = props.driverVersion,
                                     .driverABI     = sizeof(void*)};
    memcpy(hdr.uuid, props.pipelineCacheUUID, VK_UUID_SIZE);

    char tmp[1024];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);

    FILE* f = fopen(tmp, "wb");
    if(!f)
    {
        free(blob);
        return;
    }

    write_all(f, &hdr, sizeof(hdr));
    write_all(f, blob, size);
    fclose(f);

    rename(tmp, path);
    free(blob);
}
static VkFormat pick_depth_format(VkPhysicalDevice gpu)
{
    VkFormat candidates[] = {
        VK_FORMAT_D32_SFLOAT,
        VK_FORMAT_D24_UNORM_S8_UINT,
        VK_FORMAT_D32_SFLOAT_S8_UINT,
    };

    for(uint32_t i = 0; i < ARRAY_COUNT(candidates); i++)
    {
        VkFormat           fmt = candidates[i];
        VkFormatProperties props;
        vkGetPhysicalDeviceFormatProperties(gpu, fmt, &props);

        if(props.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)
            return fmt;
    }

    return VK_FORMAT_UNDEFINED;
}
static PFN_vkVoidFunction imgui_vk_loader(const char* function_name, void* user_data)
{
    VkInstance instance = (VkInstance)user_data;
    return vkGetInstanceProcAddr(instance, function_name);
}

void imgui_init(GLFWwindow*       window,
                VkInstance        instance,
                VkPhysicalDevice  gpu,
                VkDevice          device,
                uint32_t          queue_family,
                VkQueue           queue,
                VkDescriptorPool  imgui_pool,
                uint32_t          min_image_count,
                uint32_t          image_count,
                VkFormat          swapchain_format,
                VkFormat          depth_format,
                VkImageUsageFlags swapchain_usage)
{

    igCreateContext(NULL);

    ImGuiIO* io     = igGetIO_Nil();
    io->IniFilename = NULL;
    io->LogFilename = NULL;

    igStyleColorsDark(NULL);

    ImGui_ImplGlfw_InitForVulkan(window, true);

    ImGui_ImplVulkan_LoadFunctions(VK_API_VERSION_1_3, imgui_vk_loader, instance);

    ImGui_ImplVulkan_InitInfo info    = {0};
    info.ApiVersion                   = VK_API_VERSION_1_3;
    info.Instance                     = instance;
    info.PhysicalDevice               = gpu;
    info.Device                       = device;
    info.QueueFamily                  = queue_family;
    info.Queue                        = queue;
    info.DescriptorPool               = imgui_pool;
    info.MinImageCount                = min_image_count;
    info.ImageCount                   = image_count;
    info.UseDynamicRendering          = true;
    info.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;

    info.PipelineInfoMain.PipelineRenderingCreateInfo = (VkPipelineRenderingCreateInfoKHR){
        .sType                   = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR,
        .colorAttachmentCount    = 1,
        .pColorAttachmentFormats = &swapchain_format,
        .depthAttachmentFormat   = depth_format,
    };

    info.PipelineInfoMain.SwapChainImageUsage = swapchain_usage;

    ImGui_ImplVulkan_Init(&info);
}

void renderer_create(Renderer* r, RendererDesc* desc)
{
    TracyCZoneN(ctx, "renderer_create", 1);
    // Instance
    // Debug messenger
    // Physical device
    // Device info
    // Logical device
    // Queues
    // Frame contexts

    {


        VkApplicationInfo app = {.sType              = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                                 .pApplicationName   = desc->app_name,
                                 .applicationVersion = VK_MAKE_VERSION(1, 0, 0),
                                 .pEngineName        = "FLOW",
                                 .engineVersion      = VK_MAKE_VERSION(1, 0, 0),
                                 .apiVersion         = VK_API_VERSION_1_3

        };

        const char* extensions[64];
        uint32_t    ext_count = 0;

        forEach(i, desc->instance_extension_count)
        {
            extensions[ext_count++] = desc->instance_extensions[i];
        }


        extensions[ext_count++] = VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME;

        if(desc->enable_validation)
        {
            if(!is_instance_extension_supported(VK_EXT_DEBUG_UTILS_EXTENSION_NAME))
            {
                log_warn("[instance] %s not supported by loader", VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
            }
            extensions[ext_count++] = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
        }

        const char* layers[8];
        uint32_t    layer_count = desc->instance_layer_count;

        if(layer_count)
        {
            memcpy(layers, desc->instance_layers, sizeof(char*) * layer_count);
        }
        if(desc->enable_validation)
        {
            if(!is_instance_layer_supported("VK_LAYER_KHRONOS_validation"))
            {
                log_warn("[instance] VK_LAYER_KHRONOS_validation not present");
            }
            layers[layer_count++] = "VK_LAYER_KHRONOS_validation";
        }

        VkValidationFeaturesEXT validation_features;
        memset(&validation_features, 0, sizeof(validation_features));
        validation_features.sType = VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT;
        //todo get this from rendererdesc may be
        static const VkValidationFeatureEnableEXT enabled_features[] = {
            VK_VALIDATION_FEATURE_ENABLE_GPU_ASSISTED_EXT, VK_VALIDATION_FEATURE_ENABLE_GPU_ASSISTED_RESERVE_BINDING_SLOT_EXT,
            VK_VALIDATION_FEATURE_ENABLE_BEST_PRACTICES_EXT, VK_VALIDATION_FEATURE_ENABLE_DEBUG_PRINTF_EXT,
            VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT};

        log_info("[instance] app=%s api=1.3 validation=%u gpu_validation=%u",
                 desc->app_name ? desc->app_name : "(null)", desc->enable_validation, desc->enable_gpu_based_validation);
        forEach(i, ext_count) log_info("[instance]  ext[%u]=%s", i, extensions[i]);

        if(layer_count > 0)
        {
            log_info("[instance] layers: %u", layer_count);
            forEach(i, layer_count) log_info("[instance]  layer[%u]=%s", i, layers[i]);
        }
        VkInstanceCreateInfo ci = {.sType                   = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
                                   .pApplicationInfo        = &app,
                                   .enabledExtensionCount   = ext_count,
                                   .ppEnabledExtensionNames = extensions,
                                   .enabledLayerCount       = layer_count,
                                   .ppEnabledLayerNames     = layers};
        if(desc->enable_validation && desc->enable_gpu_based_validation)
        {
            validation_features.enabledValidationFeatureCount =
                (uint32_t)(sizeof(enabled_features) / sizeof(enabled_features[0]));
            validation_features.pEnabledValidationFeatures = enabled_features;

            validation_features.pNext = ci.pNext;
            ci.pNext                  = &validation_features;
        }
        VK_CHECK(vkCreateInstance(&ci, r->instance.allocatorcallbacks, &r->instance.instance));
        volkLoadInstance(r->instance.instance);
        log_info("[renderer] instance created");
    }


    {


        //
        // 2. Debug Messenger
        //
        if(desc->enable_validation)
        {
            VkDebugUtilsMessengerCreateInfoEXT ci = {.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,

                                                     .messageSeverity = desc->validation_severity,

                                                     .messageType = desc->validation_types,

                                                     .pfnUserCallback = debug_callback};


            {
                vkCreateDebugUtilsMessengerEXT(r->instance.instance, &ci, r->instance.allocatorcallbacks, &r->instance.debug_messenger);

                log_info("[renderer] debug messenger created");


                {
                    VkDebugUtilsMessengerCallbackDataEXT data = {
                        .sType    = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CALLBACK_DATA_EXT,
                        .pMessage = "Validation is enabled and debug messenger is active",
                    };
                    vkSubmitDebugUtilsMessageEXT(r->instance.instance, VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT,
                                                 VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT, &data);
                }
            }
        }
    }

    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    r->window = glfwCreateWindow(desc->width, desc->height, "Vulkan", NULL, NULL);

    VK_CHECK(glfwCreateWindowSurface(r->instance.instance, r->window, NULL, &r->surface));
    //
    // 3. Pick Physical Device
    //
    {
        r->physical_device = pick_physical_device(r->instance.instance, r->surface, desc);


        if(r->physical_device == VK_NULL_HANDLE)
        {
            log_error("No GPU found");
            return;
        }

        vkGetPhysicalDeviceProperties(r->physical_device, &r->info.properties);

        vkGetPhysicalDeviceMemoryProperties(r->physical_device, &r->info.memory);

        log_info("[renderer] GPU: %s", r->info.properties.deviceName);
    }


    // 4. Query Features
    //
    if(desc->use_custom_features)
    {
        r->info.feature_chain = desc->custom_features;
    }
    else
    {
        query_device_features(r->physical_device, &r->info.feature_chain);

        RendererCaps caps = default_caps();

        // Enable debug printf if requested
        if(desc->enable_debug_printf)
        {
            caps.debug_printf = true;
        }

        if(desc->enable_pipeline_stats)
        {
            caps.pipeline_statistics_query = true;
        }

        apply_caps(&r->info.feature_chain, &caps);
    }

    if(!desc->use_custom_features)
    {
        r->info.feature_chain.core.pNext = &r->info.feature_chain.v11;
        r->info.feature_chain.v11.pNext  = &r->info.feature_chain.v12;
        r->info.feature_chain.v12.pNext  = &r->info.feature_chain.v13;
        r->info.feature_chain.v13.pNext  = NULL;

        VkBaseOutStructure* tail = (VkBaseOutStructure*)&r->info.feature_chain.v13;

        if(device_has_extension(r->physical_device, VK_KHR_MAINTENANCE_5_EXTENSION_NAME))
        {
            r->info.feature_chain.maintenance5.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_FEATURES_KHR;
            r->info.feature_chain.maintenance5.pNext = NULL;
            tail->pNext                              = (VkBaseOutStructure*)&r->info.feature_chain.maintenance5;
            tail                                     = (VkBaseOutStructure*)&r->info.feature_chain.maintenance5;
        }

        if(desc->enable_debug_printf && device_has_extension(r->physical_device, VK_KHR_SHADER_NON_SEMANTIC_INFO_EXTENSION_NAME))
        {
            r->info.feature_chain.shaderNonSemanticInfo.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_NON_SEMANTIC_INFO_FEATURES_KHR;
            r->info.feature_chain.shaderNonSemanticInfo.pNext = NULL;
            tail->pNext = (VkBaseOutStructure*)&r->info.feature_chain.shaderNonSemanticInfo;
            tail        = (VkBaseOutStructure*)&r->info.feature_chain.shaderNonSemanticInfo;
        }

        tail->pNext = NULL;
    }

    //
    // 5. Find Queue Families
    //
    queue_families q;
    find_queue_families(r->physical_device, r->surface, &q);

    r->graphics_queue_index = q.graphics_family;

    r->present_queue_index = q.present_family;

    r->compute_queue_index = q.has_compute ? q.compute_family : q.graphics_family;

    r->transfer_queue_index = q.has_transfer ? q.transfer_family : q.graphics_family;


    //
    // 6. Create Logical Device
    //
    {
        float priority = 1.0f;

        uint32_t unique[4];
        uint32_t count = 0;

        unique[count++] = r->graphics_queue_index;

        if(r->present_queue_index != r->graphics_queue_index)
            unique[count++] = r->present_queue_index;

        if(r->compute_queue_index != r->graphics_queue_index)
            unique[count++] = r->compute_queue_index;

        if(r->transfer_queue_index != r->graphics_queue_index)
            unique[count++] = r->transfer_queue_index;

        VkDeviceQueueCreateInfo queues[4];

        forEach(i, count)
        {
            queues[i] = (VkDeviceQueueCreateInfo){.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,

                                                  .queueFamilyIndex = unique[i],

                                                  .queueCount = 1,

                                                  .pQueuePriorities = &priority};
        }

        const char* extensions[32];
        uint32_t    ext_count = 0;

        forEach(i, desc->device_extension_count)
        {
            extensions[ext_count++] = desc->device_extensions[i];
        }

        if(device_has_extension(r->physical_device, VK_KHR_MAINTENANCE_5_EXTENSION_NAME))
        {
            extensions[ext_count++] = VK_KHR_MAINTENANCE_5_EXTENSION_NAME;
        }

        if(desc->enable_debug_printf && device_has_extension(r->physical_device, VK_KHR_SHADER_NON_SEMANTIC_INFO_EXTENSION_NAME))
        {
            extensions[ext_count++] = VK_KHR_SHADER_NON_SEMANTIC_INFO_EXTENSION_NAME;
            log_info("[renderer] VK_KHR_shader_non_semantic_info enabled for Debug Printf");
        }
        else if(desc->enable_debug_printf)
        {
            log_info("[renderer] VK_KHR_shader_non_semantic_info not available - Debug Printf disabled");
        }

        VkDeviceCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,

                                 .pNext = &r->info.feature_chain.core,

                                 .queueCreateInfoCount = count,

                                 .pQueueCreateInfos = queues,

                                 .enabledExtensionCount = ext_count,

                                 .ppEnabledExtensionNames = extensions};

        VK_CHECK(vkCreateDevice(r->physical_device, &ci, r->allocatorcallbacks, &r->device));

        volkLoadDevice(r->device);
        log_info("[renderer] logical device created");
    }


    //
    // 7. Get Queues
    //
    vkGetDeviceQueue(r->device, r->graphics_queue_index, 0, &r->graphics_queue);
    vkGetDeviceQueue(r->device, r->present_queue_index, 0, &r->present_queue);

    vkGetDeviceQueue(r->device, r->compute_queue_index, 0, &r->compute_queue);

    vkGetDeviceQueue(r->device, r->transfer_queue_index, 0, &r->transfer_queue);

    log_info("[renderer] queues acquired");

    //
    // 8. Create Frame Contexts
    //
    forEach(i, MAX_FRAMES_IN_FLIGHT)
    {
        FrameContext* f = &r->frames[i];
        vk_cmd_create_pool(r->device, r->graphics_queue_index, true, false, &f->cmdbufpool);
        vk_cmd_alloc(r->device, f->cmdbufpool, true, &f->cmdbuf);
        vk_create_semaphore(r->device, &f->image_available_semaphore);
        vk_create_fence(r->device, true, &f->in_flight_fence);
    }

    log_info("[renderer] frame contexts created");

    r->current_frame     = 0;
    r->cpu_prev_frame    = (double)time_now_ns();
    r->cpu_frame_ns      = 0.0;
    r->cpu_active_ns     = 0.0;
    r->cpu_wait_ns       = 0.0;
    r->cpu_wait_accum_ns = 0.0;

    log_info("[renderer] initialization complete");


    flow_id_pool_init(&texture_pool, MAX_BINDLESS_TEXTURES);

    flow_id_pool_init(&sampler_pool, MAX_BINDLESS_SAMPLERS);

    flow_id_pool_init(&pipeline_id_pool, MAX_PIPELINES);
    vk_cmd_create_pool(r->device, r->graphics_queue_index, true, false, &r->one_time_gfx_pool);

    int fb_w, fb_h;
    glfwGetFramebufferSize(r->window, &fb_w, &fb_h);
    //  descriptor_layout_cache_init(&r->descriptor_layout_cache);
    //pipeline_layout_cache_init(&r->pipeline_layout_cache);
    r->pipeline_cache = pipeline_cache_load_or_create(r->device, r->physical_device, "pipeline_cache.bin");
    VkDescriptorSetLayoutBinding bindings[] = {// textures
                                               {
                                                   .binding         = BINDLESS_TEXTURE_BINDING,
                                                   .descriptorType  = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
                                                   .descriptorCount = desc->bindless_sampled_image_count,
                                                   .stageFlags      = VK_SHADER_STAGE_ALL,
                                               },

                                               // samplers
                                               {
                                                   .binding         = BINDLESS_SAMPLER_BINDING,
                                                   .descriptorType  = VK_DESCRIPTOR_TYPE_SAMPLER,
                                                   .descriptorCount = desc->bindless_sampler_count,
                                                   .stageFlags      = VK_SHADER_STAGE_ALL,
                                               },

                                               // storage images
                                               {
                                                   .binding         = BINDLESS_STORAGE_IMAGE_BINDING,
                                                   .descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                                                   .descriptorCount = desc->bindless_storage_image_count,
                                                   .stageFlags      = VK_SHADER_STAGE_ALL,
                                               }};
    VkDescriptorBindingFlags     flags[ARRAY_COUNT(bindings)];

    forEach(i, ARRAY_COUNT(flags))
    {
        flags[i] = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT
                   | VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT;
    }

    VkDescriptorSetLayoutBindingFlagsCreateInfo ext = {
        .sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO,
        .bindingCount  = ARRAY_COUNT(flags),
        .pBindingFlags = flags,
    };

    VkDescriptorSetLayoutCreateInfo ci = {
        .sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .pNext        = &ext,
        .bindingCount = ARRAY_COUNT(bindings),
        .pBindings    = bindings,
        .flags        = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT,
    };

    vkCreateDescriptorSetLayout(r->device, &ci, NULL, &r->bindless_system.set_layout);


    VkDescriptorPoolSize sizes[] = {
        {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, desc->bindless_sampled_image_count},
        {VK_DESCRIPTOR_TYPE_SAMPLER, desc->bindless_sampler_count},
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, desc->bindless_storage_image_count},
    };
    VkDescriptorPoolCreateInfo cib = {
        .sType   = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .flags   = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT | VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT,
        .maxSets = 1,
        .poolSizeCount = ARRAY_COUNT(sizes),
        .pPoolSizes    = sizes,
    };

    vkCreateDescriptorPool(r->device, &cib, NULL, &r->bindless_system.pool);

    VkDescriptorSetAllocateInfo ai = {
        .sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool     = r->bindless_system.pool,
        .descriptorSetCount = 1,
        .pSetLayouts        = &r->bindless_system.set_layout,
    };

    vkAllocateDescriptorSets(r->device, &ai, &r->bindless_system.set);

    VkPushConstantRange push_range = {
        .stageFlags = VK_SHADER_STAGE_ALL,
        .offset     = 0,
        .size       = 256  // your device limit
    };

    VkPipelineLayoutCreateInfo playoutci = {
        .sType          = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1,
        .pSetLayouts    = &r->bindless_system.set_layout,

        .pushConstantRangeCount = 1,
        .pPushConstantRanges    = &push_range,
    };


    VK_CHECK(vkCreatePipelineLayout(r->device, &playoutci, NULL, &r->bindless_system.pipeline_layout));

    VmaAllocatorCreateInfo allocatorInfo = {};
    allocatorInfo.physicalDevice         = r->physical_device;
    allocatorInfo.device                 = r->device;
    allocatorInfo.instance               = r->instance.instance;

    allocatorInfo.flags |= VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT;
    allocatorInfo.flags |= VMA_ALLOCATOR_CREATE_KHR_MAINTENANCE4_BIT;
    allocatorInfo.flags |= VMA_ALLOCATOR_CREATE_KHR_MAINTENANCE5_BIT;

    //  use VMA_DYNAMIC_VULKAN_FUNCTIONS
    VmaVulkanFunctions vulkanFunctions = {
        .vkGetInstanceProcAddr                   = vkGetInstanceProcAddr,
        .vkGetDeviceProcAddr                     = vkGetDeviceProcAddr,
        .vkGetPhysicalDeviceProperties           = vkGetPhysicalDeviceProperties,
        .vkGetPhysicalDeviceMemoryProperties     = vkGetPhysicalDeviceMemoryProperties,
        .vkAllocateMemory                        = vkAllocateMemory,
        .vkFreeMemory                            = vkFreeMemory,
        .vkMapMemory                             = vkMapMemory,
        .vkUnmapMemory                           = vkUnmapMemory,
        .vkFlushMappedMemoryRanges               = vkFlushMappedMemoryRanges,
        .vkInvalidateMappedMemoryRanges          = vkInvalidateMappedMemoryRanges,
        .vkBindBufferMemory                      = vkBindBufferMemory,
        .vkBindImageMemory                       = vkBindImageMemory,
        .vkGetBufferMemoryRequirements           = vkGetBufferMemoryRequirements,
        .vkGetImageMemoryRequirements            = vkGetImageMemoryRequirements,
        .vkCreateBuffer                          = vkCreateBuffer,
        .vkDestroyBuffer                         = vkDestroyBuffer,
        .vkCreateImage                           = vkCreateImage,
        .vkDestroyImage                          = vkDestroyImage,
        .vkCmdCopyBuffer                         = vkCmdCopyBuffer,
        .vkGetBufferMemoryRequirements2KHR       = vkGetBufferMemoryRequirements2,
        .vkGetImageMemoryRequirements2KHR        = vkGetImageMemoryRequirements2,
        .vkBindBufferMemory2KHR                  = vkBindBufferMemory2,
        .vkBindImageMemory2KHR                   = vkBindImageMemory2,
        .vkGetPhysicalDeviceMemoryProperties2KHR = vkGetPhysicalDeviceMemoryProperties2,
        .vkGetDeviceBufferMemoryRequirements     = vkGetDeviceBufferMemoryRequirements,
        .vkGetDeviceImageMemoryRequirements      = vkGetDeviceImageMemoryRequirements,
    };
    allocatorInfo.pVulkanFunctions = &vulkanFunctions;


    vmaCreateAllocator(&allocatorInfo, &r->vmaallocator);

    VkDescriptorPoolSize pool_sizes[] = {
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1000},
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1000},
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1000},
        {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1000},
    };

    VkDescriptorPoolCreateInfo pool_info = {
        .sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .flags         = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT,
        .maxSets       = 1000,
        .poolSizeCount = 4,
        .pPoolSizes    = pool_sizes,
    };

    vkCreateDescriptorPool(r->device, &pool_info, NULL, &r->imgui_pool);
    VkFormat                depth_format = pick_depth_format(r->physical_device);
    VkFormat                hdr_format   = VK_FORMAT_R16G16B16A16_SFLOAT;
    FlowSwapchainCreateInfo sci          = {.surface         = r->surface,
                                            .width           = fb_w,
                                            .height          = fb_h,
                                            .min_image_count = 3,

                                            //.preferred_present_mode = VK_PRESENT_MODE_IMMEDIATE_KHR,
                                            .preferred_format      = desc->swapchain_preferred_format,
                                            .preferred_color_space = desc->swapchain_preferred_color_space,
                                            .extra_usage           = desc->swapchain_extra_usage_flags,
                                            .old_swapchain         = VK_NULL_HANDLE};

    if(desc->swapchain_preferred_present_mode)
    {

        sci.preferred_present_mode = desc->swapchain_preferred_present_mode;
    }
    else
    {
        sci.preferred_present_mode = vk_swapchain_select_present_mode(r->physical_device, r->surface, desc->vsync);
    }

    vk_create_swapchain(r->device, r->physical_device, &r->swapchain, &sci, r->graphics_queue, r->one_time_gfx_pool, r);


    RenderTargetSpec depth_spec = {.width  = r->swapchain.extent.width,
                                   .height = r->swapchain.extent.height,
                                   .layers = 1,

                                   .format = depth_format,

                                   .usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,

                                   .aspect = VK_IMAGE_ASPECT_DEPTH_BIT,

                                   .mip_count  = 1,
                                   .debug_name = "depth_buffer"};
    RenderTargetSpec hdr_spec   = {.width  = r->swapchain.extent.width,
                                   .height = r->swapchain.extent.height,
                                   .layers = 1,
                                   .format = hdr_format,
                                   .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                                   .mip_count  = 1,
                                   .debug_name = "hdr_color"};
    RenderTargetSpec ldr_spec   = {.width  = r->swapchain.extent.width,
                                   .height = r->swapchain.extent.height,
                                   .layers = 1,

                                   .format = VK_FORMAT_R8G8B8A8_UNORM,

                                   .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |

                                            VK_IMAGE_USAGE_STORAGE_BIT |       // compute writes tonemap result
                                            VK_IMAGE_USAGE_TRANSFER_SRC_BIT |  // copy → swapchain
                                            VK_IMAGE_USAGE_TRANSFER_DST_BIT |  // optional clears
                                            VK_IMAGE_USAGE_SAMPLED_BIT,        // future post effects

                                   .aspect = VK_IMAGE_ASPECT_COLOR_BIT,

                                   .mip_count  = 1,
                                   .debug_name = "ldr_color"};

    RenderTargetSpec smaa_edge_spec = {.width  = r->swapchain.extent.width,
                                       .height = r->swapchain.extent.height,
                                       .layers = 1,

                                       .format = VK_FORMAT_R8G8_UNORM,

                                       .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,

                                       .aspect = VK_IMAGE_ASPECT_COLOR_BIT,

                                       .mip_count  = 1,
                                       .debug_name = "smaa_edges"};

    RenderTargetSpec smaa_weight_spec = {.width  = r->swapchain.extent.width,
                                         .height = r->swapchain.extent.height,
                                         .layers = 1,

                                         .format = VK_FORMAT_R8G8B8A8_UNORM,

                                         .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,

                                         .aspect = VK_IMAGE_ASPECT_COLOR_BIT,

                                         .mip_count  = 1,
                                         .debug_name = "smaa_weights"};
    imgui_init(r->window, r->instance.instance, r->physical_device, r->device, r->graphics_queue_index,
               r->graphics_queue, r->imgui_pool, r->swapchain.image_count, r->swapchain.image_count,
               VK_FORMAT_B8G8R8A8_SRGB, depth_format, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);

    {


#include "external/smaa/Textures/AreaTex.h"

#include "external/smaa/Textures/SearchTex.h"


        TextureCreateDesc desc = {.width     = AREATEX_WIDTH,
                                  .height    = AREATEX_HEIGHT,
                                  .mip_count = 1,
                                  .format    = VK_FORMAT_R8G8_UNORM,
                                  .usage     = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT};

        TextureID id  = create_texture(r, &desc);
        Texture*  tex = &textures[id];

        VkDeviceSize size = AREATEX_SIZE;

        Buffer staging;
        create_buffer(r, size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_ONLY, &staging);

        memcpy(staging.mapping, areaTexBytes, size);

        VkCommandBuffer cmd = vk_begin_one_time_cmd(r->device, r->one_time_gfx_pool);

        VkImageMemoryBarrier barrier = {
            .sType                       = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .oldLayout                   = VK_IMAGE_LAYOUT_UNDEFINED,
            .newLayout                   = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .srcAccessMask               = 0,
            .dstAccessMask               = VK_ACCESS_TRANSFER_WRITE_BIT,
            .image                       = tex->image,
            .subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .subresourceRange.levelCount = 1,
            .subresourceRange.layerCount = 1,
        };

        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &barrier);

        VkBufferImageCopy region = {.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                    .imageSubresource.layerCount = 1,
                                    .imageExtent                 = {AREATEX_WIDTH, AREATEX_HEIGHT, 1}};

        vkCmdCopyBufferToImage(cmd, staging.buffer, tex->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

        barrier.oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.newLayout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL, 0,
                             NULL, 1, &barrier);

        vk_end_one_time_cmd(r->device, r->graphics_queue, r->one_time_gfx_pool, cmd);

        destroy_buffer(r, &staging);
    }
    {


        TextureID smaa_area;
        TextureID smaa_search;

        {
            TextureCreateDesc desc = {.width     = AREATEX_WIDTH,
                                      .height    = AREATEX_HEIGHT,
                                      .mip_count = 1,
                                      .format    = VK_FORMAT_R8G8_UNORM,
                                      .usage     = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT};

            smaa_area    = create_texture(r, &desc);
            Texture* tex = &textures[smaa_area];

            VkDeviceSize size = AREATEX_SIZE;

            Buffer staging;
            create_buffer(r, size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_ONLY, &staging);

            memcpy(staging.mapping, areaTexBytes, size);

            VkCommandBuffer cmd = vk_begin_one_time_cmd(r->device, r->one_time_gfx_pool);

            VkImageMemoryBarrier barrier = {.sType                       = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                                            .oldLayout                   = VK_IMAGE_LAYOUT_UNDEFINED,
                                            .newLayout                   = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                            .srcAccessMask               = 0,
                                            .dstAccessMask               = VK_ACCESS_TRANSFER_WRITE_BIT,
                                            .image                       = tex->image,
                                            .subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                            .subresourceRange.levelCount = 1,
                                            .subresourceRange.layerCount = 1};

            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0,
                                 NULL, 1, &barrier);

            VkBufferImageCopy region = {.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                        .imageSubresource.layerCount = 1,
                                        .imageExtent                 = {AREATEX_WIDTH, AREATEX_HEIGHT, 1}};

            vkCmdCopyBufferToImage(cmd, staging.buffer, tex->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

            barrier.oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            barrier.newLayout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL,
                                 0, NULL, 1, &barrier);

            vk_end_one_time_cmd(r->device, r->graphics_queue, r->one_time_gfx_pool, cmd);

            destroy_buffer(r, &staging);
        }

        {
            TextureCreateDesc desc = {.width     = SEARCHTEX_WIDTH,
                                      .height    = SEARCHTEX_HEIGHT,
                                      .mip_count = 1,
                                      .format    = VK_FORMAT_R8_UNORM,
                                      .usage     = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT};

            smaa_search  = create_texture(r, &desc);
            Texture* tex = &textures[smaa_search];

            VkDeviceSize size = SEARCHTEX_SIZE;

            Buffer staging;
            create_buffer(r, size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_ONLY, &staging);

            memcpy(staging.mapping, searchTexBytes, size);

            VkCommandBuffer cmd = vk_begin_one_time_cmd(r->device, r->one_time_gfx_pool);

            VkImageMemoryBarrier barrier = {.sType                       = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                                            .oldLayout                   = VK_IMAGE_LAYOUT_UNDEFINED,
                                            .newLayout                   = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                            .srcAccessMask               = 0,
                                            .dstAccessMask               = VK_ACCESS_TRANSFER_WRITE_BIT,
                                            .image                       = tex->image,
                                            .subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                            .subresourceRange.levelCount = 1,
                                            .subresourceRange.layerCount = 1};

            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0,
                                 NULL, 1, &barrier);

            VkBufferImageCopy region = {.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                        .imageSubresource.layerCount = 1,
                                        .imageExtent                 = {SEARCHTEX_WIDTH, SEARCHTEX_HEIGHT, 1}};

            vkCmdCopyBufferToImage(cmd, staging.buffer, tex->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

            barrier.oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            barrier.newLayout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL,
                                 0, NULL, 1, &barrier);

            vk_end_one_time_cmd(r->device, r->graphics_queue, r->one_time_gfx_pool, cmd);

            destroy_buffer(r, &staging);
        }

        r->smaa_area_tex   = smaa_area;
        r->smaa_search_tex = smaa_search;
    }
    forEach(i, r->swapchain.image_count)
    {
        rt_create(r, &r->depth[i], &depth_spec);
        rt_create(r, &r->hdr_color[i], &hdr_spec);
        rt_create(r, &r->ldr_color[i], &ldr_spec);

        rt_create(r, &r->smaa_edges[i], &smaa_edge_spec);
        rt_create(r, &r->smaa_weights[i], &smaa_weight_spec);
    }
    {
        const char* path = "data/dummy_texture.png";


        uint32_t       w, h, c;
        unsigned char* pixels = stbi_load(path, &w, &h, &c, 4);
        if(!pixels)
        {
            fprintf(stderr, "Failed to load %s\n", path);
        }
        TextureCreateDesc desc       = {.width     = w,
                                        .height    = h,
                                        .mip_count = 1,
                                        .format    = VK_FORMAT_R8G8B8A8_UNORM,
                                        .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_STORAGE_BIT};
        TextureID         id         = create_texture(r, &desc);
        Texture*          tex        = &textures[id];
        VkDeviceSize      image_size = w * h * 4;
        Buffer            staging;
        create_buffer(r, image_size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_AUTO_PREFER_HOST, &staging);

        memcpy(staging.mapping, pixels, image_size);
        stbi_image_free(pixels);
        VkCommandBuffer      cmd      = vk_begin_one_time_cmd(r->device, r->one_time_gfx_pool);
        VkImageMemoryBarrier barrier1 = {
            .sType                       = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .oldLayout                   = VK_IMAGE_LAYOUT_UNDEFINED,
            .newLayout                   = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .srcAccessMask               = 0,
            .dstAccessMask               = VK_ACCESS_TRANSFER_WRITE_BIT,
            .image                       = tex->image,
            .subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .subresourceRange.levelCount = tex->mip_count,
            .subresourceRange.layerCount = 1,
        };
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0,
                             NULL, 1, &barrier1);
        VkBufferImageCopy region = {.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                    .imageSubresource.layerCount = 1,
                                    .imageExtent                 = {w, h, 1}};
        vkCmdCopyBufferToImage(cmd, staging.buffer, tex->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        VkImageMemoryBarrier barrier2 = barrier1;
        barrier2.oldLayout            = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier2.newLayout            = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier2.srcAccessMask        = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier2.dstAccessMask        = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL, 0,
                             NULL, 1, &barrier2);
        vk_end_one_time_cmd(r->device, r->graphics_queue, r->one_time_gfx_pool, cmd);
        destroy_buffer(r, &staging);

        r->dummy_texture = id;
    };
    {
        forEach(i, MAX_FRAMES_IN_FLIGHT)
        {
            gpu_profiler_init(&r->gpuprofiler[i], r->device, r->info.properties.limits.timestampPeriod,
                              desc->enable_pipeline_stats && r->info.feature_chain.core.features.pipelineStatisticsQuery);
        }
    }


    memset(&r->default_samplers, 0, sizeof(r->default_samplers));

    {
        DefaultSamplerTable* table = &r->default_samplers;


        VkSamplerCreateInfo ci = {
            .sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
            .magFilter    = VK_FILTER_LINEAR,
            .minFilter    = VK_FILTER_LINEAR,
            .mipmapMode   = VK_SAMPLER_MIPMAP_MODE_LINEAR,
            .addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT,
            .addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT,
            .addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT,
            .minLod       = 0.0f,
            .maxLod       = VK_LOD_CLAMP_NONE,
        };

        // Linear wrap
        sampler_create(r, &ci, &table->samplers[SAMPLER_LINEAR_WRAP]);

        // Linear clamp
        ci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        ci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        ci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampler_create(r, &ci, &table->samplers[SAMPLER_LINEAR_CLAMP]);

        // Nearest wrap
        ci.magFilter    = VK_FILTER_NEAREST;
        ci.minFilter    = VK_FILTER_NEAREST;
        ci.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        ci.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        ci.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        sampler_create(r, &ci, &table->samplers[SAMPLER_NEAREST_WRAP]);

        // Nearest clamp
        ci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        ci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        ci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampler_create(r, &ci, &table->samplers[SAMPLER_NEAREST_CLAMP]);

        // Anisotropic wrap
        ci.magFilter        = VK_FILTER_LINEAR;
        ci.minFilter        = VK_FILTER_LINEAR;
        ci.addressModeU     = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        ci.addressModeV     = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        ci.addressModeW     = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        ci.anisotropyEnable = VK_TRUE;
        ci.maxAnisotropy    = 16.0f;
        sampler_create(r, &ci, &table->samplers[SAMPLER_LINEAR_WRAP_ANISO]);

        // Shadow sampler
        ci.anisotropyEnable = VK_FALSE;
        ci.compareEnable    = VK_TRUE;
        ci.compareOp        = VK_COMPARE_OP_LESS_OR_EQUAL;
        ci.addressModeU     = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        ci.addressModeV     = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        ci.addressModeW     = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        ci.borderColor      = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
        sampler_create(r, &ci, &table->samplers[SAMPLER_SHADOW]);
    }
    {
        VkDeviceSize size = r->swapchain.extent.width * r->swapchain.extent.height * 4;  // RGBA8

        create_buffer(r, size, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_AUTO_PREFER_HOST, &r->readback_buffer);
    }
    {
        forEach(i, MAX_FRAMES_IN_FLIGHT)
        {
            create_buffer(r, sizeof(GlobalData),
                          VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                          VMA_MEMORY_USAGE_CPU_TO_GPU,
                          &r->global_ubo[i]);
        }
    }
}

void renderer_destroy(Renderer* r)
{
    vkDeviceWaitIdle(r->device);

    forEach(i, MAX_FRAMES_IN_FLIGHT)
    {
        gpu_profiler_destroy(&r->gpuprofiler[i], r->device);
    }

    pipeline_cache_save(r->device, r->physical_device, r->pipeline_cache, "pipeline_cache.bin");

    vkDestroyPipelineCache(r->device, r->pipeline_cache, NULL);

    forEach(i, MAX_FRAMES_IN_FLIGHT)
    {
        destroy_buffer(r, &r->global_ubo[i]);
    }

    vkDestroyDevice(r->device, NULL);
}

bool buffer_pool_init(Renderer*                r,
                      BufferPool*              pool,
                      VkDeviceSize             size_bytes,
                      VkBufferUsageFlags       usage,
                      VmaMemoryUsage           memory_usage,
                      VmaAllocationCreateFlags alloc_flags,
                      oa_uint32                max_allocs)
{
    if(!r || !pool || size_bytes == 0)
        return false;

    if(size_bytes > UINT32_MAX)
    {
        log_error("[buffer_pool] size exceeds 4GB: %llu", (unsigned long long)size_bytes);
        return false;
    }

    memset(pool, 0, sizeof(*pool));

    VkBufferCreateInfo buffer_info = {
        .sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size        = size_bytes,
        .usage       = usage,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };

    VmaAllocationCreateInfo alloc_info = {
        .usage = memory_usage,
        .flags = alloc_flags,
    };

    VmaAllocationInfo out_info = {0};
    VkResult res = vmaCreateBuffer(r->vmaallocator, &buffer_info, &alloc_info, &pool->buffer, &pool->allocation, &out_info);
    if(res != VK_SUCCESS)
    {
        log_error("[buffer_pool] vmaCreateBuffer failed: %d", res);
        return false;
    }

    pool->size_bytes   = size_bytes;
    pool->usage        = usage;
    pool->memory_usage = memory_usage;
    pool->alloc_flags  = alloc_flags;
    pool->mapped       = out_info.pMappedData;

    oa_init(&pool->allocator, (oa_uint32)size_bytes, max_allocs);

    return true;
}

void buffer_pool_destroy(Renderer* r, BufferPool* pool)
{
    if(!r || !pool)
        return;

    oa_destroy(&pool->allocator);

    if(pool->buffer != VK_NULL_HANDLE)
        vmaDestroyBuffer(r->vmaallocator, pool->buffer, pool->allocation);

    memset(pool, 0, sizeof(*pool));
}

BufferSlice buffer_pool_alloc(BufferPool* pool, VkDeviceSize size_bytes, VkDeviceSize alignment)
{
    BufferSlice slice = {0};

    if(!pool || size_bytes == 0)
        return slice;

    if(alignment == 0)
        alignment = 1;

    if(size_bytes > UINT32_MAX || alignment > UINT32_MAX)
        return slice;

    OA_Allocation allocation = (alignment > 1) ?
                                   oa_allocate_aligned(&pool->allocator, (oa_uint32)size_bytes, (oa_uint32)alignment) :
                                   oa_allocate(&pool->allocator, (oa_uint32)size_bytes);

    if(allocation.offset == OA_NO_SPACE)
        return slice;

    slice.pool       = pool;
    slice.buffer     = pool->buffer;
    slice.offset     = allocation.offset;
    slice.size       = size_bytes;
    slice.allocation = allocation;

    if(pool->mapped)
        slice.mapped = (void*)((uint8_t*)pool->mapped + allocation.offset);

    return slice;
}

void buffer_pool_free(BufferSlice slice)
{
    if(!slice.pool)
        return;

    oa_free(&slice.pool->allocator, slice.allocation);
}

VkSurfaceCapabilities2KHR query_surface_capabilities(VkPhysicalDevice gpu, VkSurfaceKHR surface)
{
    VkPhysicalDeviceSurfaceInfo2KHR info = {
        .sType   = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SURFACE_INFO_2_KHR,
        .surface = surface,
    };

    VkSurfaceCapabilities2KHR caps = {
        .sType = VK_STRUCTURE_TYPE_SURFACE_CAPABILITIES_2_KHR,
    };

    VK_CHECK(vkGetPhysicalDeviceSurfaceCapabilities2KHR(gpu, &info, &caps));
    return caps;
}


VkExtent2D choose_extent(const VkSurfaceCapabilitiesKHR* caps, uint32_t desired_w, uint32_t desired_h)
{
    if(caps->currentExtent.width != 0xFFFFFFFF)
        return caps->currentExtent;
    VkExtent2D extent = {.width = desired_w, .height = desired_h};
    extent.width      = CLAMP(extent.width, caps->minImageExtent.width, caps->maxImageExtent.width);
    extent.height     = CLAMP(extent.height, caps->minImageExtent.height, caps->maxImageExtent.height);
    return extent;
}


VkSurfaceFormatKHR select_surface_format(VkPhysicalDevice gpu, VkSurfaceKHR surface, VkFormat preferred, VkColorSpaceKHR preferred_cs)
{
    uint32_t count = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(gpu, surface, &count, NULL);

    VkSurfaceFormatKHR formats[32];
    if(count > 32)
        count = 32;

    vkGetPhysicalDeviceSurfaceFormatsKHR(gpu, surface, &count, formats);

    forEach(i, count) if(formats[i].format == preferred && formats[i].colorSpace == preferred_cs) return formats[i];

    return formats[0];
}

// Choose minImageCount given a user hint, but always respect Vulkan caps.
static uint32_t choose_min_image_count(const VkSurfaceCapabilities2KHR* caps, uint32_t preferred_hint)
{
    const uint32_t min_cap = caps->surfaceCapabilities.minImageCount;

    // Never go below Vulkan's minimum, even if the hint is silly.
    uint32_t preferred = (preferred_hint > min_cap) ? preferred_hint : min_cap;

    // maxImageCount == 0 means "no upper bound"
    const uint32_t raw_max = caps->surfaceCapabilities.maxImageCount;
    const uint32_t max_cap = (raw_max == 0) ? preferred : raw_max;

    // Clamp to [min_cap, max_cap]
    if(preferred < min_cap)
        preferred = min_cap;
    if(preferred > max_cap)
        preferred = max_cap;

    return preferred;
}
void vk_create_swapchain(VkDevice                       device,
                         VkPhysicalDevice               gpu,
                         FlowSwapchain*                 out_swapchain,
                         const FlowSwapchainCreateInfo* info,
                         VkQueue                        graphics_queue,
                         VkCommandPool                  one_time_pool,
                         Renderer*                      r)
{
    VkSurfaceCapabilities2KHR caps = query_surface_capabilities(gpu, info->surface);

    // Query formats and present modes up-front to satisfy validation and pick supported values.
    VkSurfaceFormatKHR surface_format =
        select_surface_format(gpu, info->surface, info->preferred_format, info->preferred_color_space);

    VkExtent2D extent = choose_extent(&caps.surfaceCapabilities, info->width, info->height);

    if(extent.width == 0 || extent.height == 0)
        return;  // minimized, wait later


    VkImageUsageFlags usage = (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | info->extra_usage) & caps.surfaceCapabilities.supportedUsageFlags;
    VkSwapchainCreateInfoKHR ci = {.sType            = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
                                   .surface          = info->surface,
                                   .minImageCount    = choose_min_image_count(&caps, info->min_image_count),
                                   .imageFormat      = surface_format.format,
                                   .imageColorSpace  = surface_format.colorSpace,
                                   .imageExtent      = extent,
                                   .imageArrayLayers = 1,
                                   .imageUsage       = usage,
                                   .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
                                   .preTransform     = caps.surfaceCapabilities.currentTransform,
                                   .compositeAlpha   = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
                                   .presentMode      = info->preferred_present_mode,
                                   .clipped          = VK_TRUE,
                                   .oldSwapchain     = info->old_swapchain};

    log_info("[swapchain] create: extent=%ux%u minImageCount=%u format=%u colorSpace=%u presentMode=%u usage=0x%x",
             extent.width, extent.height, ci.minImageCount, ci.imageFormat, ci.imageColorSpace, ci.presentMode, usage);

    VK_CHECK(vkCreateSwapchainKHR(device, &ci, NULL, &out_swapchain->swapchain));

    out_swapchain->surface       = info->surface;
    out_swapchain->extent        = extent;
    out_swapchain->format        = surface_format.format;
    out_swapchain->color_space   = surface_format.colorSpace;
    out_swapchain->present_mode  = info->preferred_present_mode;
    out_swapchain->current_image = 0;
    out_swapchain->image_usage   = usage;
    // Query swapchain images
    VK_CHECK(vkGetSwapchainImagesKHR(device, out_swapchain->swapchain, &out_swapchain->image_count, NULL));
    log_info("[swapchain] images: %u", out_swapchain->image_count);

    if(out_swapchain->image_count > MAX_SWAPCHAIN_IMAGES)
        out_swapchain->image_count = MAX_SWAPCHAIN_IMAGES;  // don’t blow the stack

    VK_CHECK(vkGetSwapchainImagesKHR(device, out_swapchain->swapchain, &out_swapchain->image_count, out_swapchain->images));

    // Create image views
    forEach(i, out_swapchain->image_count)
    {
        VkImageViewCreateInfo view_ci = VK_IMAGE_VIEW_DEFAULT(out_swapchain->images[i], out_swapchain->format);
        VK_CHECK(vkCreateImageView(device, &view_ci, NULL, &out_swapchain->image_views[i]));
    }
    forEach(i, out_swapchain->image_count)
    {
        out_swapchain->states[i] =
            (ImageState){.layout = VK_IMAGE_LAYOUT_UNDEFINED, .stage = VK_PIPELINE_STAGE_2_NONE, .access = 0, .validity = IMAGE_STATE_UNDEFINED};

        flow_id_pool_create_id(&texture_pool, &out_swapchain->bindless_index[i]);

        if(info->extra_usage & VK_IMAGE_USAGE_SAMPLED_BIT)
        {
            VkDescriptorImageInfo img = {.imageView   = out_swapchain->image_views[i],
                                         .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};

            VkWriteDescriptorSet write = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,

                                          .dstSet          = r->bindless_system.set,
                                          .dstBinding      = BINDLESS_TEXTURE_BINDING,
                                          .dstArrayElement = out_swapchain->bindless_index[i],

                                          .descriptorCount = 1,
                                          .descriptorType  = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
                                          .pImageInfo      = &img};

            vkUpdateDescriptorSets(r->device, 1, &write, 0, NULL);
        }
        if(info->extra_usage & VK_IMAGE_USAGE_STORAGE_BIT)
        {
            VkDescriptorImageInfo img = {.imageView = out_swapchain->image_views[i], .imageLayout = VK_IMAGE_LAYOUT_GENERAL};

            VkWriteDescriptorSet write = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,

                                          .dstSet     = r->bindless_system.set,
                                          .dstBinding = BINDLESS_STORAGE_IMAGE_BINDING,

                                          .dstArrayElement = out_swapchain->bindless_index[i],
                                          .descriptorCount = 1,
                                          .descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                                          .pImageInfo      = &img};

            vkUpdateDescriptorSets(r->device, 1, &write, 0, NULL);
        }
    }
    vk_create_semaphores(device, out_swapchain->image_count, out_swapchain->render_finished);
}


void vk_swapchain_destroy(VkDevice device, FlowSwapchain* swapchain, flow_id_pool* id_pool)
{
    if(!swapchain)
        return;

    forEach(i, swapchain->image_count)
    {
        flow_id_pool_destroy_id(id_pool, swapchain->bindless_index[i]);
        if(swapchain->image_views[i] != VK_NULL_HANDLE)
        {
            vkDestroyImageView(device, swapchain->image_views[i], NULL);
        }
    }

    vk_destroy_semaphores(device, swapchain->image_count, swapchain->render_finished);
    if(swapchain->swapchain != VK_NULL_HANDLE)
    {
        vkDestroySwapchainKHR(device, swapchain->swapchain, NULL);
    }

    memset(swapchain, 0, sizeof(*swapchain));
}


void vk_swapchain_recreate(VkDevice         device,
                           VkPhysicalDevice gpu,
                           FlowSwapchain*   sc,
                           uint32_t         new_w,
                           uint32_t         new_h,
                           VkQueue          graphics_queue,
                           VkCommandPool    one_time_pool,
                           Renderer*        r)


{
    if(new_w == 0 || new_h == 0)
        return;
    vkDeviceWaitIdle(device);


    forEach(i, sc->image_count)
    {
        if(sc->image_views[i])
            vkDestroyImageView(device, sc->image_views[i], NULL);
    }

    vk_destroy_semaphores(device, sc->image_count, sc->render_finished);
    FlowSwapchainCreateInfo info = {0};
    info.surface                 = sc->surface;
    info.width                   = new_w;
    info.height                  = new_h;
    info.min_image_count         = MAX(3u, sc->image_count);
    info.preferred_format        = sc->format;
    info.preferred_color_space   = sc->color_space;
    info.preferred_present_mode  = sc->present_mode;
    info.extra_usage             = sc->image_usage & ~VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    info.old_swapchain           = sc->swapchain;

    VkSwapchainKHR old = sc->swapchain;

    vk_create_swapchain(device, gpu, sc, &info, graphics_queue, one_time_pool, r);

    if(old)
        vkDestroySwapchainKHR(device, old, NULL);
}


// ============================================================
// Vertex format helpers
// ============================================================

static uint32_t vertex_format_size(VertexFormat fmt)
{
    switch(fmt)
    {
        case FMT_FLOAT:
            return 4;
        case FMT_VEC2:
            return 8;
        case FMT_VEC3:
            return 12;
        case FMT_VEC4:
            return 16;
        default:
            return 0;
    }
}

static VkFormat vertex_format_to_vk(VertexFormat fmt)
{
    switch(fmt)
    {
        case FMT_FLOAT:
            return VK_FORMAT_R32_SFLOAT;
        case FMT_VEC2:
            return VK_FORMAT_R32G32_SFLOAT;
        case FMT_VEC3:
            return VK_FORMAT_R32G32B32_SFLOAT;
        case FMT_VEC4:
            return VK_FORMAT_R32G32B32A32_SFLOAT;
        default:
            return VK_FORMAT_UNDEFINED;
    }
}

// ============================================================
// Vertex input build result
// ============================================================

typedef struct VertexInputState
{
    VkVertexInputBindingDescription binding;

    VkVertexInputAttributeDescription attributes[MAX_VERTEX_ATTRS];

    VkPipelineVertexInputStateCreateInfo create_info;

} VertexInputState;

// static const VkFormat VK_FORMAT_TABLE[] =
// {
//     [FMT_FLOAT] = VK_FORMAT_R32_SFLOAT,
//     [FMT_VEC2]  = VK_FORMAT_R32G32_SFLOAT,
//     [FMT_VEC3]  = VK_FORMAT_R32G32B32_SFLOAT,
//     [FMT_VEC4]  = VK_FORMAT_R32G32B32A32_SFLOAT,
// };
//
// static const uint8_t VK_FORMAT_SIZE_TABLE[] =
// {
//     [FMT_FLOAT] = 4,
//     [FMT_VEC2]  = 8,
//     [FMT_VEC3]  = 12,
//     [FMT_VEC4]  = 16,
// };

// ============================================================
// Build vertex input state from GraphicsPipelineConfig
// ============================================================


static void vertex_input_build(const GraphicsPipelineConfig* config, VertexInputState* out)
{
    memset(out, 0, sizeof(*out));


    if(!config->use_vertex_input)
    {
        out->create_info.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

        out->create_info.vertexBindingDescriptionCount   = 0;
        out->create_info.vertexAttributeDescriptionCount = 0;
        out->create_info.pVertexBindingDescriptions      = NULL;
        out->create_info.pVertexAttributeDescriptions    = NULL;

        return;
    }

    const VertexBinding* binding = &config->vertex_binding;

    // --------------------------------------------------------
    // Create binding description
    // --------------------------------------------------------

    out->binding.binding   = 0;
    out->binding.stride    = binding->stride;
    out->binding.inputRate = binding->input_rate;

    // --------------------------------------------------------
    // Create attribute descriptions
    // --------------------------------------------------------

    uint32_t offset = 0;


    uint32_t attr_count = binding->attr_count;

    if(attr_count > MAX_VERTEX_ATTRS)
    {
        fprintf(stderr, "[VertexInput] attr_count exceeds MAX_VERTEX_ATTRS (%u)\n", MAX_VERTEX_ATTRS);

        attr_count = MAX_VERTEX_ATTRS;
    }
    forEach(i, attr_count)
    {
        const VertexAttr* attr = &binding->attrs[i];

        VkVertexInputAttributeDescription* vk_attr = &out->attributes[i];

        vk_attr->location = attr->location;
        vk_attr->binding  = 0;
        vk_attr->format   = vertex_format_to_vk((VertexFormat)attr->format);
        if(vk_attr->format == VK_FORMAT_UNDEFINED)
        {
            fprintf(stderr, "[VertexInput] Invalid format at location %u\n", attr->location);
        }

        vk_attr->offset = offset;

        offset += vertex_format_size((VertexFormat)attr->format);
    }


    if(offset > binding->stride)
    {
        fprintf(stderr, "[VertexInput] ERROR: attribute size exceeds stride\n");
    }


    out->create_info.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

    out->create_info.vertexBindingDescriptionCount   = 1;
    out->create_info.vertexAttributeDescriptionCount = attr_count;

    out->create_info.pVertexBindingDescriptions = &out->binding;

    out->create_info.pVertexAttributeDescriptions = out->attributes;
}


static void pipeline_config_validate(const GraphicsPipelineConfig* cfg)
{

    if(cfg->color_attachment_count > MAX_COLOR_ATTACHMENTS)
    {
        fprintf(stderr, "color_attachment_count exceeds MAX_COLOR_ATTACHMENTS\n");
        abort();
    }

    if(cfg->color_attachment_count > 0 && cfg->color_formats == NULL)
    {
        fprintf(stderr, "color_formats is NULL but color_attachment_count > 0\n");
        abort();
    }
}

static bool read_file(const char* path, void** out_data, size_t* out_size)
{
    *out_data = NULL;
    *out_size = 0;

    FILE* f = fopen(path, "rb");
    if(!f)
    {
        log_error("Failed to open '%s' (errno=%d)", path, errno);
        return false;
    }

    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    rewind(f);

    if(len <= 0)
    {
        log_error("Invalid size for '%s'", path);
        fclose(f);
        return false;
    }

    void* data = malloc((size_t)len);
    if(!data)
    {
        log_error("Out of memory reading '%s'", path);
        fclose(f);
        return false;
    }

    if(fread(data, 1, (size_t)len, f) != (size_t)len)
    {
        log_error("Short read for '%s'", path);
        free(data);
        fclose(f);
        return false;
    }

    fclose(f);
    *out_data = data;
    *out_size = (size_t)len;
    return true;
}

static VkShaderModule create_shader_module(VkDevice device, const void* code, size_t size)
{
    VkShaderModuleCreateInfo ci = {
        .sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = size,
        .pCode    = (const uint32_t*)code,
    };

    VkShaderModule mod = VK_NULL_HANDLE;
    VK_CHECK(vkCreateShaderModule(device, &ci, NULL, &mod));
    return mod;
}


VkPipeline create_graphics_pipeline(Renderer* renderer, const GraphicsPipelineConfig* cfg)
{


    void*  vs_code = NULL;
    size_t vs_size = 0;

    void*  fs_code = NULL;
    size_t fs_size = 0;

    if(!read_file(cfg->vert_path, &vs_code, &vs_size))
        abort();

    if(!read_file(cfg->frag_path, &fs_code, &fs_size))
        abort();

    VkShaderModule vs = create_shader_module(renderer->device, vs_code, vs_size);

    VkShaderModule fs = create_shader_module(renderer->device, fs_code, fs_size);


    VkPipelineShaderStageCreateInfo stages[2] = {{
                                                     .sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                     .stage  = VK_SHADER_STAGE_VERTEX_BIT,
                                                     .module = vs,
                                                     .pName  = "main",
                                                 },
                                                 {
                                                     .sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                     .stage  = VK_SHADER_STAGE_FRAGMENT_BIT,
                                                     .module = fs,
                                                     .pName  = "main",
                                                 }};

    VertexInputState vertex_input;
    vertex_input_build(cfg, &vertex_input);


    VkPipelineInputAssemblyStateCreateInfo input_asm = {
        .sType                  = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology               = cfg->topology,
        .primitiveRestartEnable = VK_FALSE,
    };


    VkPipelineViewportStateCreateInfo viewport = {
        .sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1,
        .scissorCount  = 1,
    };


    VkPipelineRasterizationStateCreateInfo raster = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,

        .polygonMode = cfg->polygon_mode,
        .cullMode    = cfg->cull_mode,
        .frontFace   = cfg->front_face,

        .lineWidth = 1.0f,

        .depthClampEnable        = VK_FALSE,
        .rasterizerDiscardEnable = VK_FALSE,
        .depthBiasEnable         = VK_FALSE,
    };


    VkPipelineMultisampleStateCreateInfo msaa = {
        .sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
    };


    VkPipelineDepthStencilStateCreateInfo depth = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,

        .depthTestEnable  = cfg->depth_test_enable,
        .depthWriteEnable = cfg->depth_write_enable,
        .depthCompareOp   = cfg->depth_compare_op,

        .depthBoundsTestEnable = VK_FALSE,
        .stencilTestEnable     = VK_FALSE,
    };


    VkPipelineColorBlendAttachmentState blends[MAX_COLOR_ATTACHMENTS];

    forEach(i, cfg->color_attachment_count)
    {
        const ColorAttachmentBlend*          src = &cfg->blends[i];
        VkPipelineColorBlendAttachmentState* dst = &blends[i];

        dst->blendEnable = src->blend_enable;

        dst->srcColorBlendFactor = src->src_color;
        dst->dstColorBlendFactor = src->dst_color;
        dst->colorBlendOp        = src->color_op;

        dst->srcAlphaBlendFactor = src->src_alpha;
        dst->dstAlphaBlendFactor = src->dst_alpha;
        dst->alphaBlendOp        = src->alpha_op;

        dst->colorWriteMask = src->write_mask;
    }

    VkPipelineColorBlendStateCreateInfo blend_state = {
        .sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = cfg->color_attachment_count,
        .pAttachments    = blends,
    };

    // ----------------------------
    // Dynamic states
    // ----------------------------

    VkDynamicState dyn_states[] = {
        VK_DYNAMIC_STATE_VIEWPORT,
        VK_DYNAMIC_STATE_SCISSOR,
    };

    VkPipelineDynamicStateCreateInfo dyn = {
        .sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = 2,
        .pDynamicStates    = dyn_states,
    };


    VkPipelineRenderingCreateInfo rendering = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,

        .colorAttachmentCount    = cfg->color_attachment_count,
        .pColorAttachmentFormats = cfg->color_formats,

        .depthAttachmentFormat   = cfg->depth_format,
        .stencilAttachmentFormat = VK_FORMAT_UNDEFINED,
    };


    VkGraphicsPipelineCreateInfo pipe = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,

        .pNext = &rendering,

        .stageCount = 2,
        .pStages    = stages,

        .pVertexInputState   = &vertex_input.create_info,
        .pInputAssemblyState = &input_asm,
        .pViewportState      = &viewport,
        .pRasterizationState = &raster,
        .pMultisampleState   = &msaa,
        .pDepthStencilState  = &depth,
        .pColorBlendState    = &blend_state,
        .pDynamicState       = &dyn,
        //
        // .layout = pipeline_layout_cache_build(renderer->device, &renderer->descriptor_layout_cache, &renderer->pipeline_layout_cache,
        //                                       set_bindings, refl.binding_counts, refl.set_create_flags, set_flags,
        //                                       refl.set_count, refl.push_ranges, refl.push_count),

        .layout     = renderer->bindless_system.pipeline_layout,
        .renderPass = VK_NULL_HANDLE,
        .subpass    = 0,
    };

    VkPipeline pipeline;

    VkResult res = vkCreateGraphicsPipelines(renderer->device, renderer->pipeline_cache, 1, &pipe, NULL, &pipeline);

    if(res != VK_SUCCESS)
    {
        fprintf(stderr, "Failed to create graphics pipeline\n");
        abort();
    }
    free(vs_code);
    free(fs_code);


    return pipeline;
}

VkPipeline create_compute_pipeline(Renderer* renderer, const char* compute_path)
{

    void*  code = NULL;
    size_t size = 0;

    if(!read_file(compute_path, &code, &size))
        abort();

    VkShaderModule                  module = create_shader_module(renderer->device, code, size);
    VkPipelineLayout                layout = renderer->bindless_system.pipeline_layout;
    VkPipelineShaderStageCreateInfo stage  = {
         .sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
         .stage  = VK_SHADER_STAGE_COMPUTE_BIT,
         .module = module,
         .pName  = "main",
    };
    VkComputePipelineCreateInfo ci = {
        .sType  = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage  = stage,
        .layout = layout,
    };

    VkPipeline pipeline;

    VK_CHECK(vkCreateComputePipelines(renderer->device, renderer->pipeline_cache, 1, &ci, NULL, &pipeline));

    vkDestroyShaderModule(renderer->device, module, NULL);

    return pipeline;
}

void vk_cmd_set_viewport_scissor(VkCommandBuffer cmd, VkExtent2D extent)
{
    VkViewport vp = {
        .x        = 0.0f,
        .y        = 0.0f,
        .width    = (float)extent.width,
        .height   = (float)extent.height,
        .minDepth = 0.0f,
        .maxDepth = 1.0f,
    };
    VkRect2D sc = {
        .offset = {0, 0},
        .extent = extent,
    };
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &sc);
}


void image_transition_swapchain(VkCommandBuffer cmd, FlowSwapchain* sc, VkImageLayout new_layout, VkPipelineStageFlags2 dst_stage, VkAccessFlags2 dst_access)
{
    uint32_t index = sc->current_image;

    ImageState* state = &sc->states[index];

    VkPipelineStageFlags2 src_stage;
    VkAccessFlags2        src_access;
    if(state->validity == IMAGE_STATE_UNDEFINED)
    {
        src_stage  = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        src_access = 0;
    }
    else
    {
        src_stage  = state->stage;
        src_access = state->access;
    }
    VkImageMemoryBarrier2 barrier = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,

        .srcStageMask  = src_stage,
        .srcAccessMask = src_access,

        .dstStageMask  = dst_stage,
        .dstAccessMask = dst_access,

        .oldLayout = state->layout,
        .newLayout = new_layout,

        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,

        .image = sc->images[index],

        .subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .baseMipLevel = 0, .levelCount = 1, .baseArrayLayer = 0, .layerCount = 1}};

    VkDependencyInfo dep = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &barrier};

    vkCmdPipelineBarrier2(cmd, &dep);

    state->layout   = new_layout;
    state->stage    = dst_stage;
    state->access   = dst_access;
    state->validity = IMAGE_STATE_VALID;
}


inline void cmd_transition_all_mips(VkCommandBuffer       cmd,
                                    VkImage               image,
                                    ImageState*           state,
                                    VkImageAspectFlags    aspect,
                                    uint32_t              mipCount,
                                    VkPipelineStageFlags2 newStage,
                                    VkAccessFlags2        newAccess,
                                    VkImageLayout         newLayout,
                                    uint32_t              newQueueFamily)
{
    if(state->validity == IMAGE_STATE_VALID)
    {
        if(state->stage == newStage && state->access == newAccess && state->layout == newLayout && state->queue_family == newQueueFamily)
        {
            return;
        }
    }

    VkImageMemoryBarrier2 barrier = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,

        .srcStageMask = state->validity == IMAGE_STATE_VALID ? state->stage : VK_PIPELINE_STAGE_2_NONE,

        .srcAccessMask = state->validity == IMAGE_STATE_VALID ? state->access : VK_ACCESS_2_NONE,

        .dstStageMask  = newStage,
        .dstAccessMask = newAccess,

        .oldLayout = state->validity == IMAGE_STATE_VALID ? state->layout : VK_IMAGE_LAYOUT_UNDEFINED,

        .newLayout = newLayout,

        .srcQueueFamilyIndex = state->validity == IMAGE_STATE_EXTERNAL ? state->queue_family : VK_QUEUE_FAMILY_IGNORED,

        .dstQueueFamilyIndex = newQueueFamily,

        .image = image,

        .subresourceRange = image_subresource_range(aspect, 0, mipCount)};

    VkDependencyInfo dep = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &barrier};

    vkCmdPipelineBarrier2(cmd, &dep);

    state->stage        = newStage;
    state->access       = newAccess;
    state->layout       = newLayout;
    state->queue_family = newQueueFamily;
    state->validity     = IMAGE_STATE_VALID;
    state->dirty_mips   = 0;
}


inline void cmd_transition_mip(VkCommandBuffer       cmd,
                               VkImage               image,
                               ImageState*           state,
                               VkImageAspectFlags    aspect,
                               uint32_t              mip,
                               VkPipelineStageFlags2 newStage,
                               VkAccessFlags2        newAccess,
                               VkImageLayout         newLayout,
                               uint32_t              newQueueFamily)
{
    uint32_t bit = 1u << mip;

    if(state->validity == IMAGE_STATE_VALID)
    {
        if((state->dirty_mips & bit) == 0 && state->stage == newStage && state->access == newAccess
           && state->layout == newLayout && state->queue_family == newQueueFamily)
        {
            return;
        }
    }

    VkImageMemoryBarrier2 barrier = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,

        .srcStageMask = state->validity == IMAGE_STATE_VALID ? state->stage : VK_PIPELINE_STAGE_2_NONE,

        .srcAccessMask = state->validity == IMAGE_STATE_VALID ? state->access : VK_ACCESS_2_NONE,

        .dstStageMask  = newStage,
        .dstAccessMask = newAccess,

        .oldLayout = state->validity == IMAGE_STATE_VALID ? state->layout : VK_IMAGE_LAYOUT_UNDEFINED,

        .newLayout = newLayout,

        .srcQueueFamilyIndex = state->validity == IMAGE_STATE_EXTERNAL ? state->queue_family : VK_QUEUE_FAMILY_IGNORED,

        .dstQueueFamilyIndex = newQueueFamily,

        .image = image,

        .subresourceRange = image_subresource_range(aspect, mip, 1)};

    VkDependencyInfo dep = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &barrier};

    vkCmdPipelineBarrier2(cmd, &dep);

    state->stage        = newStage;
    state->access       = newAccess;
    state->layout       = newLayout;
    state->queue_family = newQueueFamily;
    state->validity     = IMAGE_STATE_VALID;

    state->dirty_mips &= ~bit;
}


VkPresentModeKHR vk_swapchain_select_present_mode(VkPhysicalDevice physical_device, VkSurfaceKHR surface, bool vsync)
{
    uint32_t count = 0;

    VK_CHECK(vkGetPhysicalDeviceSurfacePresentModesKHR(physical_device, surface, &count, NULL));

    VkPresentModeKHR modes[16];

    if(count > 16)
        count = 16;  // sanity clamp

    VK_CHECK(vkGetPhysicalDeviceSurfacePresentModesKHR(physical_device, surface, &count, modes));


    // ============================================================
    // VSYNC ON → must use FIFO (guaranteed by Vulkan spec)
    // ============================================================

    if(vsync)
    {
        for(uint32_t i = 0; i < count; i++)
        {
            if(modes[i] == VK_PRESENT_MODE_FIFO_KHR)
                return VK_PRESENT_MODE_FIFO_KHR;
        }

        // Spec guarantees FIFO exists, but fallback anyway
        return VK_PRESENT_MODE_FIFO_KHR;
    }


    // ============================================================
    // VSYNC OFF → prefer MAILBOX, then IMMEDIATE, fallback FIFO
    // ============================================================

    VkPresentModeKHR best = VK_PRESENT_MODE_FIFO_KHR;

    for(uint32_t i = 0; i < count; i++)
    {
        if(modes[i] == VK_PRESENT_MODE_MAILBOX_KHR)
            return VK_PRESENT_MODE_MAILBOX_KHR;

        if(modes[i] == VK_PRESENT_MODE_IMMEDIATE_KHR)
            best = VK_PRESENT_MODE_IMMEDIATE_KHR;
    }

    return best;
}
bool create_buffer(Renderer* r, VkDeviceSize size, VkBufferUsageFlags usage, VmaMemoryUsage memory_usage, Buffer* out)
{
    VkBufferCreateInfo buffer_info = {
        .sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size        = size,
        .usage       = usage,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    VmaAllocationCreateInfo alloc_info = {.usage = memory_usage,
                                          .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT
                                                   | VMA_ALLOCATION_CREATE_MAPPED_BIT};


    if(vmaCreateBuffer(r->vmaallocator, &buffer_info, &alloc_info, &out->buffer, &out->allocation, NULL) != VK_SUCCESS)
    {
        return false;
    }
    out->buffer_size = size;
    out->mapping     = NULL;

    VmaAllocationInfo info;
    vmaGetAllocationInfo(r->vmaallocator, out->allocation, &info);

    out->mapping = info.pMappedData;

    if(usage & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT)
    {
        VkBufferDeviceAddressInfo addr_info = {.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, .buffer = out->buffer};

        out->address = vkGetBufferDeviceAddress(r->device, &addr_info);
    }
    else
    {
        out->address = 0;
    }

    return true;
}


void destroy_buffer(Renderer* r, Buffer* b)
{
    if(!b->buffer)
        return;

    vmaDestroyBuffer(r->vmaallocator, b->buffer, b->allocation);

    memset(b, 0, sizeof(*b));
}
static FLOW_INLINE VkImageAspectFlags get_image_aspect(VkFormat format)
{
    switch(format)
    {
        case VK_FORMAT_D16_UNORM:
        case VK_FORMAT_D32_SFLOAT:
        case VK_FORMAT_D24_UNORM_S8_UINT:
        case VK_FORMAT_D32_SFLOAT_S8_UINT:
            return VK_IMAGE_ASPECT_DEPTH_BIT;

        case VK_FORMAT_S8_UINT:
            return VK_IMAGE_ASPECT_STENCIL_BIT;

        default:
            return VK_IMAGE_ASPECT_COLOR_BIT;
    }
}
TextureID create_texture(Renderer* r, const TextureCreateDesc* desc)
{
    TextureID id;

    if(!flow_id_pool_create_id(&texture_pool, &id))
    {
        fprintf(stderr, "Texture pool exhausted\n");
        return UINT32_MAX;
    }

    Texture* tex = &textures[id];

    tex->width     = desc->width;
    tex->height    = desc->height;
    tex->mip_count = desc->mip_count;
    tex->format    = desc->format;

    VkImageCreateInfo image_info = {
        .sType     = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,

        .extent.width  = desc->width,
        .extent.height = desc->height,
        .extent.depth  = 1,

        .mipLevels   = desc->mip_count,
        .arrayLayers = 1,

        .format = desc->format,
        .tiling = VK_IMAGE_TILING_OPTIMAL,

        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .samples       = VK_SAMPLE_COUNT_1_BIT,
        .sharingMode   = VK_SHARING_MODE_EXCLUSIVE,

        .usage = desc->usage,
    };

    VmaAllocationCreateInfo alloc_info = {.usage = VMA_MEMORY_USAGE_GPU_ONLY};

    vmaCreateImage(r->vmaallocator, &image_info, &alloc_info, &tex->image, &tex->allocation, NULL);

    VkImageViewCreateInfo view_info = {.sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                                       .image    = tex->image,
                                       .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                       .format   = desc->format,

                                       .subresourceRange.aspectMask     = get_image_aspect(desc->format),
                                       .subresourceRange.baseMipLevel   = 0,
                                       .subresourceRange.levelCount     = desc->mip_count,
                                       .subresourceRange.baseArrayLayer = 0,
                                       .subresourceRange.layerCount     = 1};

    vkCreateImageView(r->device, &view_info, r->allocatorcallbacks, &tex->view);

    tex->valid = true;

    VkDescriptorImageInfo img = {.imageView = tex->view, .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};

    if(desc->usage & VK_IMAGE_USAGE_SAMPLED_BIT)
    {
        VkDescriptorImageInfo img = {.imageView = tex->view, .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};

        VkWriteDescriptorSet write = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,

                                      .dstSet          = r->bindless_system.set,
                                      .dstBinding      = BINDLESS_TEXTURE_BINDING,
                                      .dstArrayElement = id,

                                      .descriptorCount = 1,
                                      .descriptorType  = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
                                      .pImageInfo      = &img};

        vkUpdateDescriptorSets(r->device, 1, &write, 0, NULL);
    }


    if(desc->usage & VK_IMAGE_USAGE_STORAGE_BIT)
    {
        VkDescriptorImageInfo img = {.imageView = tex->view, .imageLayout = VK_IMAGE_LAYOUT_GENERAL};

        VkWriteDescriptorSet write = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,

                                      .dstSet          = r->bindless_system.set,
                                      .dstBinding      = BINDLESS_STORAGE_IMAGE_BINDING,
                                      .dstArrayElement = id,

                                      .descriptorCount = 1,
                                      .descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                                      .pImageInfo      = &img};

        vkUpdateDescriptorSets(r->device, 1, &write, 0, NULL);
    }


    return id;
}

#define DDSKTX_IMPLEMENT

#include "external/dds-ktx/dds-ktx.h"
static VkFormat dds_to_vk(ddsktx_format fmt)
{
    switch(fmt)
    {
        case DDSKTX_FORMAT_BC1:
            return VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
        case DDSKTX_FORMAT_BC3:
            return VK_FORMAT_BC3_UNORM_BLOCK;
        case DDSKTX_FORMAT_BC4:
            return VK_FORMAT_BC4_UNORM_BLOCK;
        case DDSKTX_FORMAT_BC5:
            return VK_FORMAT_BC5_UNORM_BLOCK;

        case DDSKTX_FORMAT_R8:
            return VK_FORMAT_R8_UNORM;
        case DDSKTX_FORMAT_RG8:
            return VK_FORMAT_R8G8_UNORM;
        case DDSKTX_FORMAT_RGBA8:
            return VK_FORMAT_R8G8B8A8_UNORM;

        default:
            printf("Unsupported DDS format %s\n", ddsktx_format_str(fmt));
            return VK_FORMAT_UNDEFINED;
    }
}

static void* load_file(const char* path, int* size)
{
    FILE* f = fopen(path, "rb");
    fseek(f, 0, SEEK_END);
    *size = ftell(f);
    fseek(f, 0, SEEK_SET);

    void* data = malloc(*size);
    fread(data, 1, *size, f);
    fclose(f);

    return data;
}
TextureID load_texture(Renderer* r, const char* path)
{
    /* read file */
    FILE* f = fopen(path, "rb");
    if(!f)
    {
        printf("Failed to open %s\n", path);
        return UINT32_MAX;
    }

    fseek(f, 0, SEEK_END);
    int size = ftell(f);
    fseek(f, 0, SEEK_SET);

    void* file_data = malloc(size);
    fread(file_data, 1, size, f);
    fclose(f);

    /* try DDS/KTX first */
    ddsktx_texture_info info = {0};

    if(ddsktx_parse(&info, file_data, size, NULL))
    {
        VkFormat format = VK_FORMAT_UNDEFINED;

        switch(info.format)
        {
            case DDSKTX_FORMAT_BC1:
                format = VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
                break;
            case DDSKTX_FORMAT_BC3:
                format = VK_FORMAT_BC3_UNORM_BLOCK;
                break;
            case DDSKTX_FORMAT_BC4:
                format = VK_FORMAT_BC4_UNORM_BLOCK;
                break;
            case DDSKTX_FORMAT_BC5:
                format = VK_FORMAT_BC5_UNORM_BLOCK;
                break;

            case DDSKTX_FORMAT_R8:
                format = VK_FORMAT_R8_UNORM;
                break;
            case DDSKTX_FORMAT_RG8:
                format = VK_FORMAT_R8G8_UNORM;
                break;
            case DDSKTX_FORMAT_RGBA8:
                format = VK_FORMAT_R8G8B8A8_UNORM;
                break;

            default:
                printf("Unsupported DDS/KTX format\n");
                free(file_data);
                return UINT32_MAX;
        }

        TextureCreateDesc desc = {.width     = info.width,
                                  .height    = info.height,
                                  .mip_count = info.num_mips,
                                  .format    = format,
                                  .usage     = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT};

        TextureID id  = create_texture(r, &desc);
        Texture*  tex = &textures[id];

        VkCommandBuffer cmd = vk_begin_one_time_cmd(r->device, r->one_time_gfx_pool);

        VkImageMemoryBarrier barrier = {.sType                       = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                                        .oldLayout                   = VK_IMAGE_LAYOUT_UNDEFINED,
                                        .newLayout                   = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                        .srcAccessMask               = 0,
                                        .dstAccessMask               = VK_ACCESS_TRANSFER_WRITE_BIT,
                                        .image                       = tex->image,
                                        .subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                        .subresourceRange.levelCount = tex->mip_count,
                                        .subresourceRange.layerCount = 1};

        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &barrier);

        for(int mip = 0; mip < info.num_mips; mip++)
        {
            ddsktx_sub_data sub;
            ddsktx_get_sub(&info, &sub, file_data, size, 0, 0, mip);

            Buffer staging;
            create_buffer(r, sub.size_bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_ONLY, &staging);

            memcpy(staging.mapping, sub.buff, sub.size_bytes);

            VkBufferImageCopy region = {.bufferOffset                = 0,
                                        .imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                        .imageSubresource.mipLevel   = mip,
                                        .imageSubresource.layerCount = 1,
                                        .imageExtent                 = {sub.width, sub.height, 1}};

            vkCmdCopyBufferToImage(cmd, staging.buffer, tex->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

            destroy_buffer(r, &staging);
        }

        barrier.oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.newLayout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL, 0,
                             NULL, 1, &barrier);

        vk_end_one_time_cmd(r->device, r->graphics_queue, r->one_time_gfx_pool, cmd);

        free(file_data);
        return id;
    }

    /* fallback → PNG/JPG via stb */

    int            w, h, c;
    unsigned char* pixels = stbi_load_from_memory(file_data, size, &w, &h, &c, 4);

    if(!pixels)
    {
        printf("Failed to load image %s\n", path);
        free(file_data);
        return UINT32_MAX;
    }

    TextureCreateDesc desc = {.width     = w,
                              .height    = h,
                              .mip_count = 1,
                              .format    = VK_FORMAT_R8G8B8A8_SRGB,
                              .usage     = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT};

    TextureID id  = create_texture(r, &desc);
    Texture*  tex = &textures[id];

    VkDeviceSize image_size = w * h * 4;

    Buffer staging;
    create_buffer(r, image_size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_ONLY, &staging);

    memcpy(staging.mapping, pixels, image_size);

    stbi_image_free(pixels);
    free(file_data);

    VkCommandBuffer cmd = vk_begin_one_time_cmd(r->device, r->one_time_gfx_pool);

    VkImageMemoryBarrier barrier = {.sType                       = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                                    .oldLayout                   = VK_IMAGE_LAYOUT_UNDEFINED,
                                    .newLayout                   = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                    .srcAccessMask               = 0,
                                    .dstAccessMask               = VK_ACCESS_TRANSFER_WRITE_BIT,
                                    .image                       = tex->image,
                                    .subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                    .subresourceRange.levelCount = 1,
                                    .subresourceRange.layerCount = 1};

    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &barrier);

    VkBufferImageCopy region = {.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                .imageSubresource.layerCount = 1,
                                .imageExtent                 = {w, h, 1}};

    vkCmdCopyBufferToImage(cmd, staging.buffer, tex->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    barrier.oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL, 0, NULL, 1, &barrier);

    vk_end_one_time_cmd(r->device, r->graphics_queue, r->one_time_gfx_pool, cmd);

    destroy_buffer(r, &staging);

    return id;
}

SamplerID create_sampler(Renderer* r, const SamplerCreateDesc* desc)
{
    SamplerID id;

    if(!flow_id_pool_create_id(&sampler_pool, &id))
    {
        fprintf(stderr, "Sampler pool exhausted\n");
        return UINT32_MAX;
    }

    VkSamplerCreateInfo info = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,

                                .magFilter = desc->mag_filter,
                                .minFilter = desc->min_filter,

                                .addressModeU = desc->address_u,
                                .addressModeV = desc->address_v,
                                .addressModeW = desc->address_w,

                                .mipmapMode = desc->mipmap_mode,

                                .minLod = 0.0f,
                                .maxLod = desc->max_lod};

    VK_CHECK(vkCreateSampler(r->device, &info, r->allocatorcallbacks, &samplers[id]));

    VkDescriptorImageInfo sampler_info = {.sampler = samplers[id]};

    VkWriteDescriptorSet write = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,

                                  .dstSet          = r->bindless_system.set,
                                  .dstBinding      = BINDLESS_SAMPLER_BINDING,
                                  .dstArrayElement = id,

                                  .descriptorCount = 1,
                                  .descriptorType  = VK_DESCRIPTOR_TYPE_SAMPLER,
                                  .pImageInfo      = &sampler_info};

    vkUpdateDescriptorSets(r->device, 1, &write, 0, NULL);

    return id;
}

void destroy_sampler(Renderer* r, SamplerID id)
{
    VkSampler sampler = samplers[id];

    if(!sampler)
        return;

    vkDestroySampler(r->device, sampler, r->allocatorcallbacks);

    samplers[id] = VK_NULL_HANDLE;

    flow_id_pool_destroy_id(&sampler_pool, id);
}


// create_texture()        -> allocate GPU image
// upload_texture_data()   -> copy pixels
// load_texture_file()     -> disk I/O
//
//
//
//
//
//
//
//
//
//


internal uint32_t rt_compute_mip_count(uint32_t w, uint32_t h)
{
    uint32_t max_dim = w > h ? w : h;
    uint32_t mips    = 1;
    while(max_dim > 1)
    {
        max_dim >>= 1;
        mips++;
    }
    return mips;
}


bool rt_create(Renderer* r, RenderTarget* rt, const RenderTargetSpec* spec)
{

    if(!r || !rt || !spec || spec->width == 0 || spec->height == 0)
        return false;

    memset(rt, 0, sizeof(*rt));

    rt->format = spec->format;
    rt->width  = spec->width;
    rt->height = spec->height;
    rt->usage  = spec->usage;
    rt->aspect = spec->aspect ? spec->aspect : get_image_aspect(spec->format);
    rt->layers = spec->layers;
    // Mip count
    uint32_t mips = spec->mip_count;
    if(mips == 0)
        mips = rt_compute_mip_count(spec->width, spec->height);
    if(mips > RT_MAX_MIPS)
        mips = RT_MAX_MIPS;
    rt->mip_count = mips;

    // // Bindless slots unused until registered
    // rt->sampled_id = UINT32_MAX;
    // rt->storage_id = UINT32_MAX;
    //
    // Create image
    VkImageCreateInfo image_info = {
        .sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType     = VK_IMAGE_TYPE_2D,
        .format        = rt->format,
        .extent        = {rt->width, rt->height, 1},
        .mipLevels     = rt->mip_count,
        .arrayLayers   = spec->layers,
        .samples       = VK_SAMPLE_COUNT_1_BIT,
        .tiling        = VK_IMAGE_TILING_OPTIMAL,
        .usage         = rt->usage,
        .sharingMode   = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };

    VmaAllocationCreateInfo alloc_info = {
        .usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
        .flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT,
    };

    VkResult res = vmaCreateImage(r->vmaallocator, &image_info, &alloc_info, &rt->image, &rt->allocation, NULL);
    if(res != VK_SUCCESS)
    {
        log_error("[rt_create] vmaCreateImage failed: %d", res);
        return false;
    }

    // Full mip chain view (for sampling)
    VkImageViewCreateInfo view_info = {
        .sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image    = rt->image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format   = rt->format,
        .subresourceRange =
            {
                .aspectMask     = rt->aspect,
                .baseMipLevel   = 0,
                .levelCount     = rt->mip_count,
                .baseArrayLayer = 0,
                .layerCount     = 1,
            },
    };

    VK_CHECK(vkCreateImageView(r->device, &view_info, NULL, &rt->view));

    // Per-mip views (for attachment use)
    for(uint32_t mip = 0; mip < rt->mip_count; mip++)
    {
        VkImageViewCreateInfo mip_view_info = {
            .sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image    = rt->image,
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format   = rt->format,
            .subresourceRange =
                {
                    .aspectMask     = rt->aspect,
                    .baseMipLevel   = mip,
                    .levelCount     = 1,
                    .baseArrayLayer = 0,
                    .layerCount     = 1,
                },
        };
        VK_CHECK(vkCreateImageView(r->device, &mip_view_info, NULL, &rt->mip_views[mip]));
    }

    // Init per-mip states to undefined
    for(uint32_t mip = 0; mip < rt->mip_count; mip++)
    {
        rt->mip_states[mip] = (ImageState){
            .stage        = VK_PIPELINE_STAGE_2_NONE,
            .access       = VK_ACCESS_2_NONE,
            .layout       = VK_IMAGE_LAYOUT_UNDEFINED,
            .queue_family = VK_QUEUE_FAMILY_IGNORED,
            .validity     = IMAGE_STATE_UNDEFINED,
            .dirty_mips   = 0,
        };
    }

    if(spec->debug_name)
    {
        (void)spec->debug_name;  // for future VK_EXT_debug_utils
    }

    uint32_t id;

    if(!flow_id_pool_create_id(&texture_pool, &id))
    {
        fprintf(stderr, "Texture pool exhausted\n");
        return UINT32_MAX;
    }
    rt->bindless_index = id;

    if(spec->usage & VK_IMAGE_USAGE_SAMPLED_BIT)
    {
        VkDescriptorImageInfo img = {.imageView = rt->view, .imageLayout = VK_IMAGE_LAYOUT_GENERAL};

        VkWriteDescriptorSet write = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,

                                      .dstSet          = r->bindless_system.set,
                                      .dstBinding      = BINDLESS_TEXTURE_BINDING,
                                      .dstArrayElement = id,

                                      .descriptorCount = 1,
                                      .descriptorType  = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
                                      .pImageInfo      = &img};

        vkUpdateDescriptorSets(r->device, 1, &write, 0, NULL);
    }
    if(spec->usage & VK_IMAGE_USAGE_STORAGE_BIT)
    {


        VkDescriptorImageInfo img = {.imageView = rt->view, .imageLayout = VK_IMAGE_LAYOUT_GENERAL};

        VkWriteDescriptorSet write = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,

                                      .dstSet          = r->bindless_system.set,
                                      .dstBinding      = BINDLESS_STORAGE_IMAGE_BINDING,
                                      .dstArrayElement = id,

                                      .descriptorCount = 1,
                                      .descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                                      .pImageInfo      = &img};

        vkUpdateDescriptorSets(r->device, 1, &write, 0, NULL);
    }

    log_info("[rt_create] %ux%u fmt=%d mips=%u ", rt->width, rt->height, rt->format, rt->mip_count);
    return true;
}

void rt_destroy(Renderer* r, RenderTarget* rt)
{
    if(!r || !rt || !rt->image)
        return;
    vkDeviceWaitIdle(r->device);
    uint32_t id = rt->bindless_index;

    if(id != UINT32_MAX)
    {
        // Clear sampled descriptor if used
        if(rt->usage & VK_IMAGE_USAGE_SAMPLED_BIT)
        {
            VkDescriptorImageInfo img = {.imageView = textures[r->dummy_texture].view, .imageLayout = VK_IMAGE_LAYOUT_GENERAL};

            VkWriteDescriptorSet write = {.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                          .dstSet          = r->bindless_system.set,
                                          .dstBinding      = BINDLESS_TEXTURE_BINDING,
                                          .dstArrayElement = id,
                                          .descriptorCount = 1,
                                          .descriptorType  = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
                                          .pImageInfo      = &img};

            vkUpdateDescriptorSets(r->device, 1, &write, 0, NULL);
        }

        // Clear storage descriptor if used
        if(rt->usage & VK_IMAGE_USAGE_STORAGE_BIT)
        {
            VkDescriptorImageInfo img = {.imageView = textures[r->dummy_texture].view, .imageLayout = VK_IMAGE_LAYOUT_GENERAL};

            VkWriteDescriptorSet write = {.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                          .dstSet          = r->bindless_system.set,
                                          .dstBinding      = BINDLESS_STORAGE_IMAGE_BINDING,
                                          .dstArrayElement = id,
                                          .descriptorCount = 1,
                                          .descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                                          .pImageInfo      = &img};

            vkUpdateDescriptorSets(r->device, 1, &write, 0, NULL);
        }

        flow_id_pool_destroy_id(&texture_pool, id);
    }

    if(rt->view)
        vkDestroyImageView(r->device, rt->view, NULL);

    for(uint32_t i = 0; i < rt->mip_count; i++)
    {
        if(rt->mip_views[i])
            vkDestroyImageView(r->device, rt->mip_views[i], NULL);
    }

    if(rt->image)
        vmaDestroyImage(r->vmaallocator, rt->image, rt->allocation);

    memset(rt, 0, sizeof(*rt));
}


bool rt_resize(Renderer* r, RenderTarget* rt, uint32_t width, uint32_t height)


{
    if(!r || !rt)
        return false;

    if(width == rt->width && height == rt->height)
        return true;

    RenderTargetSpec spec = {.width      = width,
                             .height     = height,
                             .layers     = rt->layers,
                             .format     = rt->format,
                             .usage      = rt->usage,
                             .aspect     = rt->aspect,
                             .mip_count  = rt->mip_count,
                             .debug_name = rt->debug_name};

    rt_destroy(r, rt);

    return rt_create(r, rt, &spec);
}


void renderer_save_screenshot(Renderer* r)

{
    vkDeviceWaitIdle(r->device);

    uint32_t width  = r->swapchain.extent.width;
    uint32_t height = r->swapchain.extent.height;

    uint8_t* pixels = r->readback_buffer.mapping;

    stbi_write_png("screenshot.png", width, height, 4, pixels, width * 4);

    printf("Screenshot saved\n");
}


void renderer_record_screenshot(Renderer* r, VkCommandBuffer cmd)
{
    uint32_t width  = r->swapchain.extent.width;
    uint32_t height = r->swapchain.extent.height;


    image_transition_swapchain(cmd, &r->swapchain, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);

    VkBufferImageCopy region = {
        .bufferOffset      = 0,
        .bufferRowLength   = 0,
        .bufferImageHeight = 0,

        .imageSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .mipLevel = 0, .baseArrayLayer = 0, .layerCount = 1},

        .imageOffset = {0, 0, 0},
        .imageExtent = {width, height, 1}};

    vkCmdCopyImageToBuffer(cmd, r->swapchain.images[r->swapchain.current_image], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           r->readback_buffer.buffer, 1, &region);
}


PipelineID pipeline_create_compute(Renderer* r, const char* path)
{


    u32 id;
    flow_id_pool_create_id(&pipeline_id_pool, &id);

    if(id >= MAX_PIPELINES)
    {
        printf("Pipeline overflow\n");
        debug_break();
    }

    PipelineEntry* e = &g_render_pipelines.entries[id];

    e->type         = PIPELINE_TYPE_COMPUTE;
    e->compute.path = path;
    e->dirty        = false;

    VkPipeline p = create_compute_pipeline(r, path);

    g_render_pipelines.pipelines[id] = p;


    g_render_pipelines.count++;
    return id;
}
PipelineID pipeline_create_graphics(Renderer* r, GraphicsPipelineConfig* cfg)
{
    u32 id;
    flow_id_pool_create_id(&pipeline_id_pool, &id);

    PipelineEntry* e = &g_render_pipelines.entries[id];

    e->type     = PIPELINE_TYPE_GRAPHICS;
    e->graphics = *cfg;
    e->dirty    = false;

    g_render_pipelines.pipelines[id] = create_graphics_pipeline(r, cfg);

    g_render_pipelines.count++;

    return id;
}

VkPipeline pipeline_get(PipelineID id)
{
    return g_render_pipelines.pipelines[id];
}
void pipeline_rebuild(Renderer* r)
{
    bool any_dirty = false;

    for(int i = 0; i < g_render_pipelines.count; i++)
        if(g_render_pipelines.entries[i].dirty)
            any_dirty = true;

    if(!any_dirty)
        return;

    vkDeviceWaitIdle(r->device);

    for(int i = 0; i < g_render_pipelines.count; i++)
    {
        PipelineEntry* e = &g_render_pipelines.entries[i];

        if(!e->dirty)
            continue;

        e->dirty = false;

        vkDestroyPipeline(r->device, g_render_pipelines.pipelines[i], NULL);

        if(e->type == PIPELINE_TYPE_GRAPHICS)
            g_render_pipelines.pipelines[i] = create_graphics_pipeline(r, &e->graphics);
        else
            g_render_pipelines.pipelines[i] = create_compute_pipeline(r, e->compute.path);

        printf("Pipeline %d hot reloaded\n", i);
    }
}

void pipeline_mark_dirty(const char* changed)
{
    for(uint32_t i = 0; i < g_render_pipelines.count; i++)
    {
        PipelineEntry* e = &g_render_pipelines.entries[i];

        if(e->type != PIPELINE_TYPE_GRAPHICS && e->type != PIPELINE_TYPE_COMPUTE)
            continue;

        bool matches = false;

        if(e->type == PIPELINE_TYPE_GRAPHICS)
        {
            matches = shader_change_matches_spv(changed, e->graphics.vert_path)
                      || shader_change_matches_spv(changed, e->graphics.frag_path);
        }
        else
        {
            matches = shader_change_matches_spv(changed, e->compute.path);
        }

        if(matches)
            e->dirty = true;
    }
}
bool sampler_create(Renderer* r, const VkSamplerCreateInfo* ci, uint32_t* out_sampler_id)
{
    if(!r || !ci || !out_sampler_id)
        return false;

    VkSampler sampler = VK_NULL_HANDLE;

    VkResult res = vkCreateSampler(r->device, ci, NULL, &sampler);
    if(res != VK_SUCCESS)
        return false;

    uint32_t id;
    if(!flow_id_pool_create_id(&sampler_pool, &id))
    {
        vkDestroySampler(r->device, sampler, NULL);
        return false;
    }

    samplers[id] = sampler;

    *out_sampler_id = id;

    VkDescriptorImageInfo sampler_info = {.sampler = samplers[id]};

    VkWriteDescriptorSet write = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,

                                  .dstSet          = r->bindless_system.set,
                                  .dstBinding      = BINDLESS_SAMPLER_BINDING,
                                  .dstArrayElement = id,

                                  .descriptorCount = 1,
                                  .descriptorType  = VK_DESCRIPTOR_TYPE_SAMPLER,
                                  .pImageInfo      = &sampler_info};

    vkUpdateDescriptorSets(r->device, 1, &write, 0, NULL);
    return true;
}
