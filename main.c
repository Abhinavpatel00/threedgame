#include "renderer.h"
#include "passes.h"
#include "gltfloader.h"

#include <dirent.h>
#include <math.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GLTF_MODEL_DIR "assets/cubepets/Models/GLB format"
#define GRID_COLUMNS 5u
#define GRID_SPACING_X 2.6f
#define GRID_SPACING_Z 2.6f
static bool take_screenshot;
PUSH_CONSTANT(GltfUberPush, VkDeviceAddress draw_data_ptr; VkDeviceAddress skin_mats_ptr; VkDeviceAddress mat_ptr; uint64_t _pad0;
              float view_proj[4][4];);

typedef struct GltfIndirectDrawData
{
    VkDeviceAddress pos_ptr;
    VkDeviceAddress idx_ptr;
    VkDeviceAddress nrm_ptr;
    VkDeviceAddress uv_ptr;
    VkDeviceAddress joints_ptr;
    VkDeviceAddress weights_ptr;
    float           model[4][4];
    uint32_t        material_id;
    uint32_t        skin_offset;
    uint32_t        flags;
    uint32_t        _pad0;
} GltfIndirectDrawData;

typedef struct GltfGpuMesh
{
    BufferSlice position_slice;
    BufferSlice index_slice;
    BufferSlice normal_slice;
    BufferSlice uv_slice;
    BufferSlice joints_slice;
    BufferSlice weights_slice;
    bool        has_normal;
    bool        has_uv;
    bool        has_skin;
} GltfGpuMesh;

typedef struct GltfMaterialGPU
{
    vec4  base_color_factor;
    vec3  emissive_factor;
    float metallic_factor;
    float roughness_factor;

    uint32_t base_color_tex;
    uint32_t normal_tex;
    uint32_t mr_tex;
    uint32_t emissive_tex;
    uint32_t sampler_id;
    uint32_t flags;
    uint32_t _pad0;
    uint32_t _pad1;
} GltfMaterialGPU;

typedef struct GltfGpuModel
{
    GLTFContainer* cpu;
    GltfGpuMesh*   gpu_meshes;

    GltfMaterialGPU* material_data;
    uint32_t         material_count;
    BufferSlice      material_slice;

    SamplerID* sampler_map;
    TextureID* image_map;

    mat4* node_world_matrices;

    uint32_t*   skin_offsets;
    uint32_t    total_skin_mats;
    mat4*       skin_mats_cpu;
    BufferSlice skin_mats_slice;

    GltfIndirectDrawData*  draw_data_cpu;
    VkDrawIndirectCommand* indirect_cmds_cpu;
    BufferSlice            draw_data_slice;
    BufferSlice            indirect_cmd_slice;
    BufferSlice            indirect_count_slice;

    bool uploaded;
} GltfGpuModel;

typedef struct GltfSceneInstance
{
    GltfGpuModel model;
    vec3         position;
    uint32_t     animation_index;
    float        animation_time;
    float        animation_speed;
    bool         animation_paused;
} GltfSceneInstance;

static int cmp_string_ptrs(const void* a, const void* b)
{
    const char* const* lhs = (const char* const*)a;
    const char* const* rhs = (const char* const*)b;
    return strcmp(*lhs, *rhs);
}

static bool has_glb_extension(const char* name)
{
    if(!name)
        return false;

    size_t n = strlen(name);
    if(n < 4)
        return false;

    const char* ext = name + (n - 4);
    return strcmp(ext, ".glb") == 0 || strcmp(ext, ".GLB") == 0;
}

static bool collect_glb_paths(const char* dir_path, char*** out_paths, uint32_t* out_count)
{
    *out_paths = NULL;
    *out_count = 0;

    DIR* dir = opendir(dir_path);
    if(!dir)
        return false;

    uint32_t count = 0;
    uint32_t cap   = 16;
    char**   paths = (char**)calloc(cap, sizeof(char*));
    if(!paths)
    {
        closedir(dir);
        return false;
    }

    struct dirent* entry = NULL;
    while((entry = readdir(dir)) != NULL)
    {
        if(!has_glb_extension(entry->d_name))
            continue;

        if(count == cap)
        {
            cap *= 2;
            char** next = (char**)realloc(paths, cap * sizeof(char*));
            if(!next)
            {
                for(uint32_t i = 0; i < count; ++i)
                    free(paths[i]);
                free(paths);
                closedir(dir);
                return false;
            }
            paths = next;
        }

        size_t full_len = strlen(dir_path) + 1 + strlen(entry->d_name) + 1;
        paths[count]    = (char*)malloc(full_len);
        if(!paths[count])
        {
            for(uint32_t i = 0; i < count; ++i)
                free(paths[i]);
            free(paths);
            closedir(dir);
            return false;
        }

        snprintf(paths[count], full_len, "%s/%s", dir_path, entry->d_name);
        ++count;
    }

    closedir(dir);

    if(count == 0)
    {
        free(paths);
        return false;
    }

    qsort(paths, count, sizeof(char*), cmp_string_ptrs);
    *out_paths = paths;
    *out_count = count;
    return true;
}

static void cycle_animation_clip(GltfSceneInstance* instance, int delta)
{
    if(!instance || !instance->model.cpu || instance->model.cpu->animation_count == 0)
        return;

    uint32_t count = instance->model.cpu->animation_count;
    uint32_t index = instance->animation_index;
    if(index >= count)
        index = 0;

    int next_index = (int)index + delta;
    while(next_index < 0)
        next_index += (int)count;
    while(next_index >= (int)count)
        next_index -= (int)count;

    instance->animation_index = (uint32_t)next_index;
    instance->animation_time  = 0.0f;
}

static void update_animation_time(GltfSceneInstance* instance, float delta_seconds)
{
    if(!instance || !instance->model.cpu || instance->model.cpu->animation_count == 0)
        return;

    if(instance->animation_paused)
        return;

    uint32_t anim_index = instance->animation_index;
    if(anim_index >= instance->model.cpu->animation_count)
        anim_index = 0;

    float duration = instance->model.cpu->animations[anim_index].duration;
    instance->animation_time += delta_seconds * instance->animation_speed;

    if(duration > 0.0f)
    {
        instance->animation_time = fmodf(instance->animation_time, duration);
        if(instance->animation_time < 0.0f)
            instance->animation_time += duration;
    }
}

static VkDeviceAddress slice_device_address(const Renderer* r, BufferSlice slice)
{
    VkBufferDeviceAddressInfo info = {
        .sType  = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
        .buffer = slice.buffer,
    };
    return vkGetBufferDeviceAddress(r->device, &info) + slice.offset;
}

static void gltf_dirname(const char* path, char* out, size_t out_size)
{
    if(!path || !out || out_size == 0)
        return;

    strncpy(out, path, out_size - 1);
    out[out_size - 1] = '\0';

    char* slash = strrchr(out, '/');
    if(slash)
        *slash = '\0';
    else
        out[0] = '\0';
}

static void gltf_join_path(const char* dir, const char* rel, char* out, size_t out_size)
{
    if(!out || out_size == 0)
        return;

    if(!dir || dir[0] == '\0')
    {
        strncpy(out, rel, out_size - 1);
        out[out_size - 1] = '\0';
        return;
    }

    if(rel && rel[0] == '/')
    {
        strncpy(out, rel, out_size - 1);
        out[out_size - 1] = '\0';
        return;
    }

    snprintf(out, out_size, "%s/%s", dir, rel ? rel : "");
}

static TextureID create_texture_from_memory(Renderer* r, const unsigned char* encoded, size_t encoded_size)
{
    int            w = 0, h = 0, channels = 0;
    unsigned char* pixels = stbi_load_from_memory(encoded, (int)encoded_size, &w, &h, &channels, 4);
    if(!pixels || w <= 0 || h <= 0)
        return renderer.dummy_texture;

    TextureCreateDesc desc = {
        .width     = (uint32_t)w,
        .height    = (uint32_t)h,
        .mip_count = 1,
        .format    = VK_FORMAT_R8G8B8A8_SRGB,
        .usage     = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
    };

    TextureID id = create_texture(r, &desc);
    if(id == UINT32_MAX)
    {
        stbi_image_free(pixels);
        return renderer.dummy_texture;
    }

    Texture*     tex        = &textures[id];
    VkDeviceSize image_size = (VkDeviceSize)w * (VkDeviceSize)h * 4u;

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

    if(!renderer_upload_texture_2d(r, cmd, tex, pixels, image_size, (uint32_t)w, (uint32_t)h, 0))
    {
        stbi_image_free(pixels);
        vk_end_one_time_cmd(r->device, r->graphics_queue, r->one_time_gfx_pool, cmd);
        destroy_texture(r, id);
        return renderer.dummy_texture;
    }

    barrier.oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL, 0, NULL, 1, &barrier);

    vk_end_one_time_cmd(r->device, r->graphics_queue, r->one_time_gfx_pool, cmd);
    stbi_image_free(pixels);

    return id;
}

static TextureID load_gltf_image_texture(const GLTFContainer* gltf, size_t image_index)
{
    if(!gltf || !gltf->handle || image_index >= gltf->handle->images_count)
        return renderer.dummy_texture;

    const cgltf_image* image = &gltf->handle->images[image_index];

    if(image->uri && strncmp(image->uri, "data:", 5) != 0)
    {
        char dir[512]   = {0};
        char full[1024] = {0};
        gltf_dirname(gltf->source_path, dir, sizeof(dir));
        gltf_join_path(dir, image->uri, full, sizeof(full));
        TextureID id = load_texture(&renderer, full);
        return id == UINT32_MAX ? renderer.dummy_texture : id;
    }

    if(image->buffer_view && image->buffer_view->buffer && image->buffer_view->buffer->data)
    {
        const uint8_t* bytes = (const uint8_t*)image->buffer_view->buffer->data + image->buffer_view->offset;
        size_t         size  = image->buffer_view->size;
        return create_texture_from_memory(&renderer, bytes, size);
    }

    return renderer.dummy_texture;
}

static SamplerID create_gltf_sampler(const SamplerDesc* src)
{
    SamplerCreateDesc desc = {
        .mag_filter  = src ? src->mag_filter : VK_FILTER_LINEAR,
        .min_filter  = src ? src->min_filter : VK_FILTER_LINEAR,
        .address_u   = src ? src->address_U : VK_SAMPLER_ADDRESS_MODE_REPEAT,
        .address_v   = src ? src->address_V : VK_SAMPLER_ADDRESS_MODE_REPEAT,
        .address_w   = src ? src->address_W : VK_SAMPLER_ADDRESS_MODE_REPEAT,
        .mipmap_mode = src ? src->mip_map_mode : VK_SAMPLER_MIPMAP_MODE_LINEAR,
        .max_lod     = src ? src->max_lod : 16.0f,
        .debug_name  = "gltf_sampler",
    };

    SamplerID id = create_sampler(&renderer, &desc);
    if(id == UINT32_MAX)
        return renderer.default_samplers.samplers[SAMPLER_LINEAR_WRAP];
    return id;
}

static TextureID texture_from_view(const GltfGpuModel* model, const GLTFTextureView* view)
{
    if(!model || !view)
        return renderer.dummy_texture;
    if(view->texture_index == SIZE_MAX)
        return renderer.dummy_texture;
    if(!model->image_map || !model->cpu || !model->cpu->handle)
        return renderer.dummy_texture;
    if(view->texture_index >= model->cpu->handle->images_count)
        return renderer.dummy_texture;
    return model->image_map[view->texture_index];
}

static SamplerID sampler_from_view(const GltfGpuModel* model, const GLTFTextureView* view)
{
    if(!model || !view)
        return renderer.default_samplers.samplers[SAMPLER_LINEAR_WRAP];
    if(view->sample_index == SIZE_MAX)
        return renderer.default_samplers.samplers[SAMPLER_LINEAR_WRAP];
    if(!model->sampler_map || view->sample_index >= model->cpu->sampler_count)
        return renderer.default_samplers.samplers[SAMPLER_LINEAR_WRAP];
    return model->sampler_map[view->sample_index];
}

static bool gltf_gpu_model_init(GltfGpuModel* model, const char* gltf_path)
{
    memset(model, 0, sizeof(*model));

    if(!loadGltf(gltf_path, GLTF_FLAG_LOAD_VERTICES | GLTF_FLAG_CALCULATE_BOUNDS, &model->cpu))
        return false;

    if(model->cpu->mesh_count == 0)
        return false;

    model->gpu_meshes          = (GltfGpuMesh*)calloc(model->cpu->mesh_count, sizeof(GltfGpuMesh));
    model->node_world_matrices = (mat4*)calloc(model->cpu->node_count > 0 ? model->cpu->node_count : 1u, sizeof(mat4));

    model->material_count = model->cpu->material_count + 1u;
    model->material_data  = (GltfMaterialGPU*)calloc(model->material_count, sizeof(GltfMaterialGPU));

    if(model->cpu->sampler_count > 0)
        model->sampler_map = (SamplerID*)calloc(model->cpu->sampler_count, sizeof(SamplerID));
    if(model->cpu->handle && model->cpu->handle->images_count > 0)
        model->image_map = (TextureID*)calloc(model->cpu->handle->images_count, sizeof(TextureID));

    if(model->cpu->skin_count > 0)
        model->skin_offsets = (uint32_t*)calloc(model->cpu->skin_count, sizeof(uint32_t));

    model->draw_data_cpu     = (GltfIndirectDrawData*)calloc(model->cpu->mesh_count, sizeof(GltfIndirectDrawData));
    model->indirect_cmds_cpu = (VkDrawIndirectCommand*)calloc(model->cpu->mesh_count, sizeof(VkDrawIndirectCommand));

    if(!model->gpu_meshes || !model->node_world_matrices || !model->material_data
       || (model->cpu->sampler_count > 0 && !model->sampler_map)
       || (model->cpu->handle && model->cpu->handle->images_count > 0 && !model->image_map)
       || (model->cpu->skin_count > 0 && !model->skin_offsets) || !model->draw_data_cpu || !model->indirect_cmds_cpu)
        return false;

    for(uint32_t i = 0; i < model->cpu->node_count; ++i)
        glm_mat4_identity(model->node_world_matrices[i]);

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

        if(src->attributes[GLTF_ATTRIBUTE_TYPE_NORMAL])
        {
            VkDeviceSize nrm_bytes            = (VkDeviceSize)src->vertex_count * 3u * sizeof(float);
            model->gpu_meshes[i].normal_slice = buffer_pool_alloc(&renderer.gpu_pool, nrm_bytes, 16);
            model->gpu_meshes[i].has_normal   = model->gpu_meshes[i].normal_slice.buffer != VK_NULL_HANDLE;
            if(!model->gpu_meshes[i].has_normal)
                return false;
        }

        if(src->attributes[GLTF_ATTRIBUTE_TYPE_TEXCOORD])
        {
            VkDeviceSize uv_bytes         = (VkDeviceSize)src->vertex_count * 2u * sizeof(float);
            model->gpu_meshes[i].uv_slice = buffer_pool_alloc(&renderer.gpu_pool, uv_bytes, 16);
            model->gpu_meshes[i].has_uv   = model->gpu_meshes[i].uv_slice.buffer != VK_NULL_HANDLE;
            if(!model->gpu_meshes[i].has_uv)
                return false;
        }

        if(src->attributes[GLTF_ATTRIBUTE_TYPE_JOINTS] && src->attributes[GLTF_ATTRIBUTE_TYPE_WEIGHTS])
        {
            VkDeviceSize joints_bytes          = (VkDeviceSize)src->vertex_count * 4u * sizeof(float);
            VkDeviceSize weights_bytes         = (VkDeviceSize)src->vertex_count * 4u * sizeof(float);
            model->gpu_meshes[i].joints_slice  = buffer_pool_alloc(&renderer.gpu_pool, joints_bytes, 16);
            model->gpu_meshes[i].weights_slice = buffer_pool_alloc(&renderer.gpu_pool, weights_bytes, 16);
            model->gpu_meshes[i].has_skin      = model->gpu_meshes[i].joints_slice.buffer != VK_NULL_HANDLE
                                            && model->gpu_meshes[i].weights_slice.buffer != VK_NULL_HANDLE;
            if(!model->gpu_meshes[i].has_skin)
                return false;
        }
    }

    for(uint32_t i = 0; i < model->cpu->sampler_count; ++i)
        model->sampler_map[i] = create_gltf_sampler(&model->cpu->samplers[i]);

    if(model->cpu->handle)
    {
        for(uint32_t i = 0; i < model->cpu->handle->images_count; ++i)
            model->image_map[i] = load_gltf_image_texture(model->cpu, i);
    }

    model->material_data[0] = (GltfMaterialGPU){
        .base_color_factor = {1.0f, 1.0f, 1.0f, 1.0f},
        .emissive_factor   = {0.0f, 0.0f, 0.0f},
        .metallic_factor   = 0.0f,
        .roughness_factor  = 1.0f,
        .base_color_tex    = renderer.dummy_texture,
        .normal_tex        = renderer.dummy_texture,
        .mr_tex            = renderer.dummy_texture,
        .emissive_tex      = renderer.dummy_texture,
        .sampler_id        = renderer.default_samplers.samplers[SAMPLER_LINEAR_WRAP],
        .flags             = 0,
    };

    for(uint32_t i = 0; i < model->cpu->material_count; ++i)
    {
        GLTFMaterial*    src = &model->cpu->materials[i];
        GltfMaterialGPU* dst = &model->material_data[i + 1u];

        *dst = model->material_data[0];

        if(src->material_type == GLTF_MATERIAL_TYPE_METALLIC_ROUGHNESS)
        {
            glm_vec4_copy(src->metallic_roughness.base_color_factor, dst->base_color_factor);
            dst->metallic_factor  = src->metallic_roughness.metallic_factor;
            dst->roughness_factor = src->metallic_roughness.roughness_factor;

            dst->base_color_tex = texture_from_view(model, &src->metallic_roughness.base_color_texture);
            dst->mr_tex         = texture_from_view(model, &src->metallic_roughness.metallic_roughness_texture);
            dst->sampler_id     = sampler_from_view(model, &src->metallic_roughness.base_color_texture);
        }

        dst->normal_tex   = texture_from_view(model, &src->normal_texture);
        dst->emissive_tex = texture_from_view(model, &src->emissive_texture);
        glm_vec3_copy(src->emissive_factor, dst->emissive_factor);
    }

    model->material_slice =
        buffer_pool_alloc(&renderer.gpu_pool, (VkDeviceSize)model->material_count * sizeof(GltfMaterialGPU), 16);
    if(model->material_slice.buffer == VK_NULL_HANDLE)
        return false;

    model->draw_data_slice =
        buffer_pool_alloc(&renderer.gpu_pool, (VkDeviceSize)model->cpu->mesh_count * sizeof(GltfIndirectDrawData), 16);
    if(model->draw_data_slice.buffer == VK_NULL_HANDLE)
        return false;

    model->indirect_cmd_slice =
        buffer_pool_alloc(&renderer.gpu_pool, (VkDeviceSize)model->cpu->mesh_count * sizeof(VkDrawIndirectCommand), 16);
    if(model->indirect_cmd_slice.buffer == VK_NULL_HANDLE)
        return false;

    model->indirect_count_slice = buffer_pool_alloc(&renderer.gpu_pool, sizeof(uint32_t), 4);
    if(model->indirect_count_slice.buffer == VK_NULL_HANDLE)
        return false;

    for(uint32_t i = 0; i < model->cpu->mesh_count; ++i)
    {
        model->indirect_cmds_cpu[i].vertexCount   = model->cpu->meshes[i].index_count;
        model->indirect_cmds_cpu[i].instanceCount = 1;
        model->indirect_cmds_cpu[i].firstVertex   = 0;
        model->indirect_cmds_cpu[i].firstInstance = i;
    }

    model->total_skin_mats = 0;
    for(uint32_t s = 0; s < model->cpu->skin_count; ++s)
    {
        model->skin_offsets[s] = model->total_skin_mats;
        model->total_skin_mats += model->cpu->skins[s].joint_count;
    }

    if(model->total_skin_mats > 0)
    {
        model->skin_mats_cpu = (mat4*)calloc(model->total_skin_mats, sizeof(mat4));
        if(!model->skin_mats_cpu)
            return false;

        model->skin_mats_slice = buffer_pool_alloc(&renderer.gpu_pool, (VkDeviceSize)model->total_skin_mats * sizeof(mat4), 16);
        if(model->skin_mats_slice.buffer == VK_NULL_HANDLE)
            return false;

        for(uint32_t i = 0; i < model->total_skin_mats; ++i)
            glm_mat4_identity(model->skin_mats_cpu[i]);
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
            if(model->gpu_meshes[i].normal_slice.buffer != VK_NULL_HANDLE)
                buffer_pool_free(model->gpu_meshes[i].normal_slice);
            if(model->gpu_meshes[i].uv_slice.buffer != VK_NULL_HANDLE)
                buffer_pool_free(model->gpu_meshes[i].uv_slice);
            if(model->gpu_meshes[i].joints_slice.buffer != VK_NULL_HANDLE)
                buffer_pool_free(model->gpu_meshes[i].joints_slice);
            if(model->gpu_meshes[i].weights_slice.buffer != VK_NULL_HANDLE)
                buffer_pool_free(model->gpu_meshes[i].weights_slice);
        }
    }

    if(model->material_slice.buffer != VK_NULL_HANDLE)
        buffer_pool_free(model->material_slice);

    if(model->draw_data_slice.buffer != VK_NULL_HANDLE)
        buffer_pool_free(model->draw_data_slice);

    if(model->indirect_cmd_slice.buffer != VK_NULL_HANDLE)
        buffer_pool_free(model->indirect_cmd_slice);

    if(model->indirect_count_slice.buffer != VK_NULL_HANDLE)
        buffer_pool_free(model->indirect_count_slice);

    if(model->skin_mats_slice.buffer != VK_NULL_HANDLE)
        buffer_pool_free(model->skin_mats_slice);

    if(model->sampler_map && model->cpu)
    {
        for(uint32_t i = 0; i < model->cpu->sampler_count; ++i)
        {
            SamplerID id = model->sampler_map[i];
            if(id != UINT32_MAX && id != renderer.default_samplers.samplers[SAMPLER_LINEAR_WRAP])
                destroy_sampler(&renderer, id);
        }
    }

    if(model->image_map && model->cpu && model->cpu->handle)
    {
        for(uint32_t i = 0; i < model->cpu->handle->images_count; ++i)
        {
            TextureID id = model->image_map[i];
            if(id != UINT32_MAX && id != renderer.dummy_texture)
                destroy_texture(&renderer, id);
        }
    }

    free(model->gpu_meshes);
    free(model->material_data);
    free(model->sampler_map);
    free(model->image_map);
    free(model->node_world_matrices);
    free(model->skin_offsets);
    free(model->skin_mats_cpu);
    free(model->draw_data_cpu);
    free(model->indirect_cmds_cpu);
    freeGltf(model->cpu);
    memset(model, 0, sizeof(*model));
}

static void gltf_gpu_model_upload_once(GltfGpuModel* model, VkCommandBuffer cmd)
{
    if(model->uploaded)
        return;

    uint32_t barrier_capacity = model->cpu->mesh_count * 8u + 5u;
    VkBufferMemoryBarrier2* barriers = (VkBufferMemoryBarrier2*)calloc(barrier_capacity, sizeof(VkBufferMemoryBarrier2));
    if(!barriers)
        return;

    uint32_t b = 0;
    for(uint32_t i = 0; i < model->cpu->mesh_count; ++i)
    {
        GLTFMesh*    src = &model->cpu->meshes[i];
        GltfGpuMesh* dst = &model->gpu_meshes[i];

        VkDeviceSize pos_bytes = (VkDeviceSize)src->vertex_count * 3u * sizeof(float);
        VkDeviceSize idx_bytes = (VkDeviceSize)src->index_count * sizeof(uint32_t);

        renderer_upload_buffer_to_slice(&renderer, cmd, dst->position_slice,
                                        src->attributes[GLTF_ATTRIBUTE_TYPE_POSITION], pos_bytes, 16);
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

        if(dst->has_normal)
        {
            VkDeviceSize nrm_bytes = (VkDeviceSize)src->vertex_count * 3u * sizeof(float);
            renderer_upload_buffer_to_slice(&renderer, cmd, dst->normal_slice,
                                            src->attributes[GLTF_ATTRIBUTE_TYPE_NORMAL], nrm_bytes, 16);
            barriers[b++] = (VkBufferMemoryBarrier2){
                .sType         = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
                .srcStageMask  = VK_PIPELINE_STAGE_2_COPY_BIT,
                .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                .dstStageMask  = VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT,
                .dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
                .buffer        = dst->normal_slice.buffer,
                .offset        = dst->normal_slice.offset,
                .size          = nrm_bytes,
            };
        }

        if(dst->has_uv)
        {
            VkDeviceSize uv_bytes = (VkDeviceSize)src->vertex_count * 2u * sizeof(float);
            renderer_upload_buffer_to_slice(&renderer, cmd, dst->uv_slice,
                                            src->attributes[GLTF_ATTRIBUTE_TYPE_TEXCOORD], uv_bytes, 16);
            barriers[b++] = (VkBufferMemoryBarrier2){
                .sType         = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
                .srcStageMask  = VK_PIPELINE_STAGE_2_COPY_BIT,
                .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                .dstStageMask  = VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT,
                .dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
                .buffer        = dst->uv_slice.buffer,
                .offset        = dst->uv_slice.offset,
                .size          = uv_bytes,
            };
        }

        if(dst->has_skin)
        {
            VkDeviceSize joints_bytes  = (VkDeviceSize)src->vertex_count * 4u * sizeof(float);
            VkDeviceSize weights_bytes = (VkDeviceSize)src->vertex_count * 4u * sizeof(float);
            renderer_upload_buffer_to_slice(&renderer, cmd, dst->joints_slice,
                                            src->attributes[GLTF_ATTRIBUTE_TYPE_JOINTS], joints_bytes, 16);
            renderer_upload_buffer_to_slice(&renderer, cmd, dst->weights_slice,
                                            src->attributes[GLTF_ATTRIBUTE_TYPE_WEIGHTS], weights_bytes, 16);

            barriers[b++] = (VkBufferMemoryBarrier2){
                .sType         = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
                .srcStageMask  = VK_PIPELINE_STAGE_2_COPY_BIT,
                .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                .dstStageMask  = VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT,
                .dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
                .buffer        = dst->joints_slice.buffer,
                .offset        = dst->joints_slice.offset,
                .size          = joints_bytes,
            };

            barriers[b++] = (VkBufferMemoryBarrier2){
                .sType         = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
                .srcStageMask  = VK_PIPELINE_STAGE_2_COPY_BIT,
                .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                .dstStageMask  = VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT,
                .dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
                .buffer        = dst->weights_slice.buffer,
                .offset        = dst->weights_slice.offset,
                .size          = weights_bytes,
            };
        }
    }

    if(model->material_slice.buffer != VK_NULL_HANDLE)
    {
        VkDeviceSize mat_bytes = (VkDeviceSize)model->material_count * sizeof(GltfMaterialGPU);
        renderer_upload_buffer_to_slice(&renderer, cmd, model->material_slice, model->material_data, mat_bytes, 16);

        barriers[b++] = (VkBufferMemoryBarrier2){
            .sType         = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
            .srcStageMask  = VK_PIPELINE_STAGE_2_COPY_BIT,
            .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
            .dstStageMask  = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
            .dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
            .buffer        = model->material_slice.buffer,
            .offset        = model->material_slice.offset,
            .size          = mat_bytes,
        };
    }

    if(model->indirect_cmd_slice.buffer != VK_NULL_HANDLE)
    {
        VkDeviceSize cmd_bytes = (VkDeviceSize)model->cpu->mesh_count * sizeof(VkDrawIndirectCommand);
        renderer_upload_buffer_to_slice(&renderer, cmd, model->indirect_cmd_slice, model->indirect_cmds_cpu, cmd_bytes, 16);

        barriers[b++] = (VkBufferMemoryBarrier2){
            .sType         = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
            .srcStageMask  = VK_PIPELINE_STAGE_2_COPY_BIT,
            .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
            .dstStageMask  = VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT,
            .dstAccessMask = VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT,
            .buffer        = model->indirect_cmd_slice.buffer,
            .offset        = model->indirect_cmd_slice.offset,
            .size          = cmd_bytes,
        };
    }

    if(model->indirect_count_slice.buffer != VK_NULL_HANDLE)
    {
        uint32_t draw_count = model->cpu->mesh_count;
        renderer_upload_buffer_to_slice(&renderer, cmd, model->indirect_count_slice, &draw_count, sizeof(draw_count), 4);

        barriers[b++] = (VkBufferMemoryBarrier2){
            .sType         = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
            .srcStageMask  = VK_PIPELINE_STAGE_2_COPY_BIT,
            .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
            .dstStageMask  = VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT,
            .dstAccessMask = VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT,
            .buffer        = model->indirect_count_slice.buffer,
            .offset        = model->indirect_count_slice.offset,
            .size          = sizeof(uint32_t),
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

static void gltf_gpu_model_update_skinning(GltfSceneInstance* instance, VkCommandBuffer cmd)
{
    GltfGpuModel* model = &instance->model;

    if(model->cpu->node_count > 0 && model->cpu->animation_count > 0)
    {
        uint32_t anim_index = instance->animation_index;
        if(anim_index >= model->cpu->animation_count)
            anim_index = 0;
        gltf_apply_animation(model->cpu, anim_index, instance->animation_time, model->node_world_matrices);
    }

    if(model->total_skin_mats == 0 || model->skin_mats_slice.buffer == VK_NULL_HANDLE)
        return;

    for(uint32_t s = 0; s < model->cpu->skin_count; ++s)
    {
        GLTFSkin* skin = &model->cpu->skins[s];
        uint32_t  base = model->skin_offsets[s];

        for(uint32_t j = 0; j < skin->joint_count; ++j)
        {
            uint32_t node_index = skin->joint_node_indices[j];
            if(node_index < model->cpu->node_count)
                glm_mat4_mul(model->node_world_matrices[node_index], skin->inverse_bind_matrices[j],
                             model->skin_mats_cpu[base + j]);
            else
                glm_mat4_identity(model->skin_mats_cpu[base + j]);
        }
    }

    VkDeviceSize skin_bytes = (VkDeviceSize)model->total_skin_mats * sizeof(mat4);
    renderer_upload_buffer_to_slice(&renderer, cmd, model->skin_mats_slice, model->skin_mats_cpu, skin_bytes, 16);

    VkBufferMemoryBarrier2 barrier = {
        .sType         = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
        .srcStageMask  = VK_PIPELINE_STAGE_2_COPY_BIT,
        .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
        .dstStageMask  = VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT,
        .dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
        .buffer        = model->skin_mats_slice.buffer,
        .offset        = model->skin_mats_slice.offset,
        .size          = skin_bytes,
    };

    VkDependencyInfo dep = {
        .sType                    = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .bufferMemoryBarrierCount = 1,
        .pBufferMemoryBarriers    = &barrier,
    };

    vkCmdPipelineBarrier2(cmd, &dep);
}

static void gltf_gpu_model_update_draw_data(GltfSceneInstance* instance, VkCommandBuffer cmd)
{
    GltfGpuModel* model = &instance->model;

    if(model->cpu->mesh_count == 0 || model->draw_data_slice.buffer == VK_NULL_HANDLE || !model->draw_data_cpu)
        return;

    mat4 instance_transform;
    glm_mat4_identity(instance_transform);
    glm_translate(instance_transform, instance->position);

    for(uint32_t i = 0; i < model->cpu->mesh_count; ++i)
    {
        GltfIndirectDrawData* draw     = &model->draw_data_cpu[i];
        GLTFMesh*             mesh     = &model->cpu->meshes[i];
        GltfGpuMesh*          gpu_mesh = &model->gpu_meshes[i];

        draw->pos_ptr     = slice_device_address(&renderer, gpu_mesh->position_slice);
        draw->idx_ptr     = slice_device_address(&renderer, gpu_mesh->index_slice);
        draw->nrm_ptr     = gpu_mesh->has_normal ? slice_device_address(&renderer, gpu_mesh->normal_slice) : 0;
        draw->uv_ptr      = gpu_mesh->has_uv ? slice_device_address(&renderer, gpu_mesh->uv_slice) : 0;
        draw->joints_ptr  = 0;
        draw->weights_ptr = 0;
        draw->material_id = 0;
        draw->skin_offset = 0;

        draw->flags = 0;
        if(gpu_mesh->has_uv)
            draw->flags |= 0x1;
        if(gpu_mesh->has_normal)
            draw->flags |= 0x2;

        u32 node_index = model->cpu->mesh_node_indices ? model->cpu->mesh_node_indices[i] : UINT_MAX;
        if(node_index != UINT_MAX && node_index < model->cpu->node_count)
            glm_mat4_mul(instance_transform, model->node_world_matrices[node_index], draw->model);
        else
            memcpy(draw->model, instance_transform, sizeof(draw->model));

        u32 material_index = model->cpu->material_indices ? model->cpu->material_indices[i] : UINT_MAX;
        if(material_index != UINT_MAX && material_index < model->cpu->material_count)
            draw->material_id = material_index + 1u;

        u32 skin_index = UINT_MAX;
        if(node_index != UINT_MAX && node_index < model->cpu->node_count)
            skin_index = model->cpu->nodes[node_index].skin_index;

        if(gpu_mesh->has_skin && skin_index != UINT_MAX && skin_index < model->cpu->skin_count && model->skin_offsets)
        {
            draw->flags |= 0x4;
            draw->joints_ptr  = slice_device_address(&renderer, gpu_mesh->joints_slice);
            draw->weights_ptr = slice_device_address(&renderer, gpu_mesh->weights_slice);
            draw->skin_offset = model->skin_offsets[skin_index];
        }

        model->indirect_cmds_cpu[i].vertexCount = mesh->index_count;
    }

    VkDeviceSize draw_data_bytes = (VkDeviceSize)model->cpu->mesh_count * sizeof(GltfIndirectDrawData);
    renderer_upload_buffer_to_slice(&renderer, cmd, model->draw_data_slice, model->draw_data_cpu, draw_data_bytes, 16);

    VkBufferMemoryBarrier2 barrier = {
        .sType         = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
        .srcStageMask  = VK_PIPELINE_STAGE_2_COPY_BIT,
        .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
        .dstStageMask  = VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT,
        .dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
        .buffer        = model->draw_data_slice.buffer,
        .offset        = model->draw_data_slice.offset,
        .size          = draw_data_bytes,
    };

    VkDependencyInfo dep = {
        .sType                    = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .bufferMemoryBarrierCount = 1,
        .pBufferMemoryBarriers    = &barrier,
    };

    vkCmdPipelineBarrier2(cmd, &dep);
}

static void draw_gltf_model(VkCommandBuffer cmd, const Camera* cam, const GltfGpuModel* model)
{
    vk_cmd_set_viewport_scissor(cmd, renderer.swapchain.extent);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g_render_pipelines.pipelines[pipelines.gltf_minimal]);

    if(model->draw_data_slice.buffer == VK_NULL_HANDLE || model->indirect_cmd_slice.buffer == VK_NULL_HANDLE
       || model->indirect_count_slice.buffer == VK_NULL_HANDLE)
        return;

    GltfUberPush push  = {0};
    push.draw_data_ptr = slice_device_address(&renderer, model->draw_data_slice);
    push.skin_mats_ptr =
        model->skin_mats_slice.buffer != VK_NULL_HANDLE ? slice_device_address(&renderer, model->skin_mats_slice) : 0;
    push.mat_ptr = slice_device_address(&renderer, model->material_slice);
    memcpy(push.view_proj, cam->view_proj, sizeof(push.view_proj));

    vkCmdPushConstants(cmd, renderer.bindless_system.pipeline_layout, VK_SHADER_STAGE_ALL, 0, sizeof(GltfUberPush), &push);

    vkCmdDrawIndirectCount(cmd, model->indirect_cmd_slice.buffer, model->indirect_cmd_slice.offset,
                           model->indirect_count_slice.buffer, model->indirect_count_slice.offset,
                           model->cpu->mesh_count, sizeof(VkDrawIndirectCommand));
}

int main(void)
{
    graphics_init();

    Camera cam = {0};
    camera_defaults_3d(&cam);
    camera3d_set_position(&cam, 0.0f, 0.6f, 4.0f);
    camera3d_set_rotation_yaw_pitch(&cam, 0.0f, 0.0f);

    char**   glb_paths = NULL;
    uint32_t glb_count = 0;
    if(!collect_glb_paths(GLTF_MODEL_DIR, &glb_paths, &glb_count))
    {
        renderer_destroy(&renderer);
        return 1;
    }

    GltfSceneInstance* instances = (GltfSceneInstance*)calloc(glb_count, sizeof(GltfSceneInstance));
    if(!instances)
    {
        for(uint32_t i = 0; i < glb_count; ++i)
            free(glb_paths[i]);
        free(glb_paths);
        renderer_destroy(&renderer);
        return 1;
    }

    uint32_t loaded_count = 0;
    for(uint32_t i = 0; i < glb_count; ++i)
    {
        if(!gltf_gpu_model_init(&instances[loaded_count].model, glb_paths[i]))
            continue;

        uint32_t col    = loaded_count % GRID_COLUMNS;
        uint32_t row    = loaded_count / GRID_COLUMNS;
        float    grid_w = (float)(GRID_COLUMNS - 1u) * GRID_SPACING_X;

        instances[loaded_count].position[0] = (float)col * GRID_SPACING_X - 0.5f * grid_w;
        instances[loaded_count].position[1] = 0.0f;
        instances[loaded_count].position[2] = -(float)row * GRID_SPACING_Z;
        ++loaded_count;
    }

    for(uint32_t i = 0; i < glb_count; ++i)
        free(glb_paths[i]);
    free(glb_paths);

    if(loaded_count == 0)
    {
        free(instances);
        renderer_destroy(&renderer);
        return 1;
    }

    for(uint32_t i = 0; i < loaded_count; ++i)
    {
        instances[i].animation_index  = 0;
        instances[i].animation_time   = 0.0f;
        instances[i].animation_speed  = 1.0f;
        instances[i].animation_paused = false;
    }

    bool   prev_space        = false;
    bool   prev_left         = false;
    bool   prev_right        = false;
    bool   prev_r            = false;
    bool   prev_up           = false;
    bool   prev_down         = false;
    double prev_time_seconds = glfwGetTime();

    while(!glfwWindowShouldClose(renderer.window))
    {
        TracyCFrameMark;

        double now_seconds         = glfwGetTime();
        float  frame_delta_seconds = (float)(now_seconds - prev_time_seconds);
        prev_time_seconds          = now_seconds;

        if(frame_delta_seconds < 0.0f)
            frame_delta_seconds = 0.0f;

        bool key_space = glfwGetKey(renderer.window, GLFW_KEY_SPACE) == GLFW_PRESS;
        bool key_left  = glfwGetKey(renderer.window, GLFW_KEY_LEFT) == GLFW_PRESS;
        bool key_right = glfwGetKey(renderer.window, GLFW_KEY_RIGHT) == GLFW_PRESS;
        bool key_r     = glfwGetKey(renderer.window, GLFW_KEY_R) == GLFW_PRESS;
        bool key_up    = glfwGetKey(renderer.window, GLFW_KEY_UP) == GLFW_PRESS;
        bool key_down  = glfwGetKey(renderer.window, GLFW_KEY_DOWN) == GLFW_PRESS;

        if(key_space && !prev_space)
        {
            bool pause_state = !instances[0].animation_paused;
            for(uint32_t i = 0; i < loaded_count; ++i)
                instances[i].animation_paused = pause_state;
        }

        if(key_right && !prev_right)
        {
            for(uint32_t i = 0; i < loaded_count; ++i)
                cycle_animation_clip(&instances[i], 1);
        }

        if(key_left && !prev_left)
        {
            for(uint32_t i = 0; i < loaded_count; ++i)
                cycle_animation_clip(&instances[i], -1);
        }

        if(key_r && !prev_r)
        {
            for(uint32_t i = 0; i < loaded_count; ++i)
                instances[i].animation_time = 0.0f;
        }

        if(key_up && !prev_up)
        {
            for(uint32_t i = 0; i < loaded_count; ++i)
                instances[i].animation_speed = glm_min(instances[i].animation_speed + 0.25f, 4.0f);
        }

        if(key_down && !prev_down)
        {
            for(uint32_t i = 0; i < loaded_count; ++i)
                instances[i].animation_speed = glm_max(instances[i].animation_speed - 0.25f, 0.25f);
        }

        prev_space = key_space;
        prev_left  = key_left;
        prev_right = key_right;
        prev_r     = key_r;
        prev_up    = key_up;
        prev_down  = key_down;

        for(uint32_t i = 0; i < loaded_count; ++i)
            update_animation_time(&instances[i], frame_delta_seconds);

        pipeline_rebuild(&renderer);
        frame_start(&renderer, &cam);

        VkCommandBuffer cmd        = renderer.frames[renderer.current_frame].cmdbuf;
        GpuProfiler*    frame_prof = &renderer.gpuprofiler[renderer.current_frame];

        vk_cmd_begin(cmd, false);
        gpu_profiler_begin_frame(frame_prof, cmd);
        {
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
            }

            for(uint32_t i = 0; i < loaded_count; ++i)
            {
                gltf_gpu_model_upload_once(&instances[i].model, cmd);
                gltf_gpu_model_update_skinning(&instances[i], cmd);
                gltf_gpu_model_update_draw_data(&instances[i], cmd);
            }

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

    GPU_SCOPE(frame_prof, cmd, "MAIN", VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT)
    {
	    vkCmdBeginRendering(cmd, &rendering);
            for(uint32_t i = 0; i < loaded_count; ++i)
                draw_gltf_model(cmd, &cam, &instances[i].model);
            vkCmdEndRendering(cmd);
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

            post_pass();
            pass_smaa();
            pass_ldr_to_swapchain();
            pass_imgui();


            if(take_screenshot)
            {
                renderer_record_screenshot(&renderer, cmd);
            }

            image_transition_swapchain(cmd, &renderer.swapchain, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                                       VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, 0);
            flush_barriers(cmd);
        }

        vk_cmd_end(cmd);
        submit_frame(&renderer);


        if(take_screenshot)
        {
            renderer_save_screenshot(&renderer);

            take_screenshot = false;
        }
    }

    for(uint32_t i = 0; i < loaded_count; ++i)
        gltf_gpu_model_destroy(&instances[i].model);
    free(instances);

    renderer_destroy(&renderer);
    return 0;
}
