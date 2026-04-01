#include "renderer.h"
#include "passes.h"
#include "gltfloader.h"
#include "input.h"
#include "helpers.h"
#include "fs.h"
#include "audio.h"

#include <dirent.h>
#include <math.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define BLOCKY_DIR "assets/blocky/Models/GLB format"
#define PETS_DIR "assets/cubepets/Models/GLB format"
#define GRID_COLUMNS 5u
#define GRID_SPACING_X 2.6f
#define GRID_SPACING_Z 2.6f
static bool take_screenshot;


PUSH_CONSTANT(GltfUberPush, VkDeviceAddress draw_data_ptr; VkDeviceAddress skin_mats_ptr; VkDeviceAddress mat_ptr; uint64_t _pad0;);
PUSH_CONSTANT(DebugLinePush,
              VkDeviceAddress pos_ptr;
              VkDeviceAddress idx_ptr;
              mat4 model;
              vec4 color;
              uint32_t index_count;
              uint32_t _pad0[3];);

typedef struct GltfIndirectDrawData
{
    VkDeviceAddress vtx_ptr;
    VkDeviceAddress idx_ptr;
    VkDeviceAddress joints_ptr;
    VkDeviceAddress weights_ptr;
    float           model[4][4];
    uint32_t        material_id;
    uint32_t        skin_offset;
    uint32_t        flags;
    uint32_t        _pad0;
    float           bloom_color[3];
    uint32_t        _pad1;
} GltfIndirectDrawData;

typedef struct
{
    uint16_t vx, vy, vz;
    uint16_t tp;
    uint32_t np;
    uint16_t tu, tv;
} PackedVertex;

MU_INLINE PackedVertex mu_pack_vertex(float px, float py, float pz, float nx, float ny, float nz, float tx, float ty, float u, float v, int bitangent_sign)
{
    PackedVertex pv;

    pv.vx = mu_quantize_half(px);
    pv.vy = mu_quantize_half(py);
    pv.vz = mu_quantize_half(pz);

    int qnx = mu_quantize_snorm(nx, 10);
    int qny = mu_quantize_snorm(ny, 10);
    int qnz = mu_quantize_snorm(nz, 10);

    pv.np = ((uint32_t)(qnx & 1023)) | ((uint32_t)(qny & 1023) << 10) | ((uint32_t)(qnz & 1023) << 20)
            | ((uint32_t)(bitangent_sign & 3) << 30);

    int qtx = mu_quantize_snorm(tx, 8);
    int qty = mu_quantize_snorm(ty, 8);

    pv.tp = ((uint16_t)(qtx & 255) << 8) | ((uint16_t)(qty & 255));

    pv.tu = mu_quantize_half(u);
    pv.tv = mu_quantize_half(v);

    return pv;
}

typedef struct GltfGpuMesh
{
    BufferSlice packed_vertex_slice;
    BufferSlice index_slice;
    BufferSlice joints_slice;
    BufferSlice weights_slice;
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
    bool         use_transform;
    union
    {
        vec3 rotation;
        mat4 transform;
    } xform;
    bool     bloom_enabled;
    vec3     bloom_color;
    uint32_t animation_index;
    float    animation_time;
    float    animation_speed;
    bool     animation_paused;
} GltfSceneInstance;


typedef enum PrimitiveShape
{
    PRIM_NONE = 0,
    PRIM_CUBE,
    PRIM_CUBOID,
    PRIM_PLANE,
    PRIM_SPHERE,
    PRIM_CYLINDER,
    PRIM_CONE,
    PRIM_PYRAMID,
    PRIM_CAPSULE,
    PRIM_TORUS,
} PrimitiveShape;

typedef struct ObstacleInstance
{
    GltfSceneInstance instance;
    PrimitiveShape    shape;
    vec3              size;
    vec3              half_extents;
} ObstacleInstance;

typedef struct Draw3DDesc
{
    const char*    gltf_path;
    PrimitiveShape primitive;
    vec3           primitive_size;
    vec3           position;
    bool           use_transform;
    union
    {
        vec3 rotation;
        mat4 transform;
    } xform;
    bool     bloom_enabled;
    vec3     bloom_color;
    uint32_t animation_index;
    float    animation_time;
    float    animation_speed;
    bool     animation_paused;
} Draw3DDesc;

static bool draw_3d(const Draw3DDesc* desc, GltfSceneInstance* out_instance);
static int  cmp_string_ptrs(const void* a, const void* b)
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

static void free_glb_paths(char** paths, uint32_t count)
{
    if(!paths)
        return;

    for(uint32_t i = 0; i < count; ++i)
        free(paths[i]);
    free(paths);
}

static uint32_t find_animation_by_name(const GltfSceneInstance* instance, const char* name, uint32_t fallback)
{
    if(!instance || !instance->model.cpu || instance->model.cpu->animation_count == 0)
        return fallback;

    if(!name || name[0] == '\0')
        return fallback;

    for(uint32_t i = 0; i < instance->model.cpu->animation_count; ++i)
    {
        if(strcmp(instance->model.cpu->animations[i].name, name) == 0)
            return i;
    }

    return fallback;
}

static double bytes_to_mib(VkDeviceSize bytes)
{
    return (double)bytes / (1024.0 * 1024.0);
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

static void free_primitive_mesh(GLTFMesh* mesh)
{
    if(!mesh)
        return;

    free(mesh->index);
    mesh->index = NULL;

    for(uint32_t i = 0; i < GLTF_ATTRIBUTE_TYPE_COUNT; ++i)
    {
        free(mesh->attributes[i]);
        mesh->attributes[i] = NULL;
    }
}

static void primitive_mesh_compute_bounds(GLTFMesh* mesh)
{
    if(!mesh || !mesh->attributes[GLTF_ATTRIBUTE_TYPE_POSITION] || mesh->vertex_count == 0)
        return;

    float* positions = (float*)mesh->attributes[GLTF_ATTRIBUTE_TYPE_POSITION];
    mesh->min[0] = mesh->max[0] = positions[0];
    mesh->min[1] = mesh->max[1] = positions[1];
    mesh->min[2] = mesh->max[2] = positions[2];

    for(uint32_t i = 1; i < mesh->vertex_count; ++i)
    {
        float x = positions[i * 3u + 0u];
        float y = positions[i * 3u + 1u];
        float z = positions[i * 3u + 2u];
        if(x < mesh->min[0])
            mesh->min[0] = x;
        if(y < mesh->min[1])
            mesh->min[1] = y;
        if(z < mesh->min[2])
            mesh->min[2] = z;
        if(x > mesh->max[0])
            mesh->max[0] = x;
        if(y > mesh->max[1])
            mesh->max[1] = y;
        if(z > mesh->max[2])
            mesh->max[2] = z;
    }
}

static void triangle_normal(float ax, float ay, float az, float bx, float by, float bz, float cx, float cy, float cz, float* out_nx, float* out_ny, float* out_nz)
{
    float ux = bx - ax;
    float uy = by - ay;
    float uz = bz - az;
    float vx = cx - ax;
    float vy = cy - ay;
    float vz = cz - az;

    float nx  = uy * vz - uz * vy;
    float ny  = uz * vx - ux * vz;
    float nz  = ux * vy - uy * vx;
    float len = sqrtf(nx * nx + ny * ny + nz * nz);
    if(len > 0.0f)
    {
        nx /= len;
        ny /= len;
        nz /= len;
    }

    *out_nx = nx;
    *out_ny = ny;
    *out_nz = nz;
}

static bool build_primitive_mesh_box(const vec3 size, GLTFMesh* out_mesh)
{
    if(!out_mesh)
        return false;

    memset(out_mesh, 0, sizeof(*out_mesh));

    const float hx = size[0] * 0.5f;
    const float hy = size[1] * 0.5f;
    const float hz = size[2] * 0.5f;

    const uint32_t vertex_count = 24;
    const uint32_t index_count  = 36;

    float*    positions = (float*)malloc((size_t)vertex_count * 3u * sizeof(float));
    float*    normals   = (float*)malloc((size_t)vertex_count * 3u * sizeof(float));
    float*    uvs       = (float*)malloc((size_t)vertex_count * 2u * sizeof(float));
    uint32_t* indices   = (uint32_t*)malloc((size_t)index_count * sizeof(uint32_t));

    if(!positions || !normals || !uvs || !indices)
    {
        free(positions);
        free(normals);
        free(uvs);
        free(indices);
        return false;
    }

    const float face_positions[6][12] = {
        {hx, -hy, -hz, hx, -hy, hz, hx, hy, hz, hx, hy, -hz}, {-hx, -hy, hz, -hx, -hy, -hz, -hx, hy, -hz, -hx, hy, hz},
        {-hx, hy, -hz, hx, hy, -hz, hx, hy, hz, -hx, hy, hz}, {-hx, -hy, hz, hx, -hy, hz, hx, -hy, -hz, -hx, -hy, -hz},
        {-hx, -hy, hz, -hx, hy, hz, hx, hy, hz, hx, -hy, hz}, {hx, -hy, -hz, hx, hy, -hz, -hx, hy, -hz, -hx, -hy, -hz},
    };

    const float face_normals[6][3] = {
        {1.0f, 0.0f, 0.0f},  {-1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f},
        {0.0f, -1.0f, 0.0f}, {0.0f, 0.0f, 1.0f},  {0.0f, 0.0f, -1.0f},
    };

    const float face_uvs[8] = {0.0f, 0.0f, 1.0f, 0.0f, 1.0f, 1.0f, 0.0f, 1.0f};

    uint32_t v = 0;
    uint32_t i = 0;
    for(uint32_t face = 0; face < 6; ++face)
    {
        for(uint32_t k = 0; k < 4; ++k)
        {
            positions[v * 3u + 0u] = face_positions[face][k * 3u + 0u];
            positions[v * 3u + 1u] = face_positions[face][k * 3u + 1u];
            positions[v * 3u + 2u] = face_positions[face][k * 3u + 2u];

            normals[v * 3u + 0u] = face_normals[face][0];
            normals[v * 3u + 1u] = face_normals[face][1];
            normals[v * 3u + 2u] = face_normals[face][2];

            uvs[v * 2u + 0u] = face_uvs[k * 2u + 0u];
            uvs[v * 2u + 1u] = face_uvs[k * 2u + 1u];
            ++v;
        }

        uint32_t base = face * 4u;
        indices[i++]  = base + 0u;
        indices[i++]  = base + 1u;
        indices[i++]  = base + 2u;
        indices[i++]  = base + 2u;
        indices[i++]  = base + 3u;
        indices[i++]  = base + 0u;
    }

    out_mesh->vertex_count                             = vertex_count;
    out_mesh->index_count                              = index_count;
    out_mesh->index                                    = indices;
    out_mesh->attributes[GLTF_ATTRIBUTE_TYPE_POSITION] = positions;
    out_mesh->attributes[GLTF_ATTRIBUTE_TYPE_NORMAL]   = normals;
    out_mesh->attributes[GLTF_ATTRIBUTE_TYPE_TEXCOORD] = uvs;

    primitive_mesh_compute_bounds(out_mesh);
    return true;
}

static bool build_primitive_mesh_plane(const vec3 size, GLTFMesh* out_mesh)
{
    if(!out_mesh)
        return false;

    memset(out_mesh, 0, sizeof(*out_mesh));

    const float hx = size[0] * 0.5f;
    const float hz = size[2] * 0.5f;

    const uint32_t vertex_count = 4;
    const uint32_t index_count  = 6;

    float*    positions = (float*)malloc((size_t)vertex_count * 3u * sizeof(float));
    float*    normals   = (float*)malloc((size_t)vertex_count * 3u * sizeof(float));
    float*    uvs       = (float*)malloc((size_t)vertex_count * 2u * sizeof(float));
    uint32_t* indices   = (uint32_t*)malloc((size_t)index_count * sizeof(uint32_t));

    if(!positions || !normals || !uvs || !indices)
    {
        free(positions);
        free(normals);
        free(uvs);
        free(indices);
        return false;
    }

    positions[0]  = -hx;
    positions[1]  = 0.0f;
    positions[2]  = -hz;
    positions[3]  = hx;
    positions[4]  = 0.0f;
    positions[5]  = -hz;
    positions[6]  = hx;
    positions[7]  = 0.0f;
    positions[8]  = hz;
    positions[9]  = -hx;
    positions[10] = 0.0f;
    positions[11] = hz;

    for(uint32_t v = 0; v < vertex_count; ++v)
    {
        normals[v * 3u + 0u] = 0.0f;
        normals[v * 3u + 1u] = 1.0f;
        normals[v * 3u + 2u] = 0.0f;
    }

    uvs[0] = 0.0f;
    uvs[1] = 0.0f;
    uvs[2] = 1.0f;
    uvs[3] = 0.0f;
    uvs[4] = 1.0f;
    uvs[5] = 1.0f;
    uvs[6] = 0.0f;
    uvs[7] = 1.0f;

    indices[0] = 0u;
    indices[1] = 1u;
    indices[2] = 2u;
    indices[3] = 2u;
    indices[4] = 3u;
    indices[5] = 0u;

    out_mesh->vertex_count                             = vertex_count;
    out_mesh->index_count                              = index_count;
    out_mesh->index                                    = indices;
    out_mesh->attributes[GLTF_ATTRIBUTE_TYPE_POSITION] = positions;
    out_mesh->attributes[GLTF_ATTRIBUTE_TYPE_NORMAL]   = normals;
    out_mesh->attributes[GLTF_ATTRIBUTE_TYPE_TEXCOORD] = uvs;

    primitive_mesh_compute_bounds(out_mesh);
    return true;
}

static bool build_primitive_mesh_sphere(const vec3 size, GLTFMesh* out_mesh)
{
    if(!out_mesh)
        return false;

    memset(out_mesh, 0, sizeof(*out_mesh));

    const float    radius       = size[0] * 0.5f;
    const uint32_t segments_u   = 24;
    const uint32_t segments_v   = 16;
    const uint32_t vertex_count = (segments_u + 1u) * (segments_v + 1u);
    const uint32_t index_count  = segments_u * segments_v * 6u;

    float*    positions = (float*)malloc((size_t)vertex_count * 3u * sizeof(float));
    float*    normals   = (float*)malloc((size_t)vertex_count * 3u * sizeof(float));
    float*    uvs       = (float*)malloc((size_t)vertex_count * 2u * sizeof(float));
    uint32_t* indices   = (uint32_t*)malloc((size_t)index_count * sizeof(uint32_t));

    if(!positions || !normals || !uvs || !indices)
    {
        free(positions);
        free(normals);
        free(uvs);
        free(indices);
        return false;
    }

    const float pi     = 3.14159265358979323846f;
    const float two_pi = 6.28318530717958647692f;

    uint32_t v = 0;
    for(uint32_t y = 0; y <= segments_v; ++y)
    {
        float vy    = (float)y / (float)segments_v;
        float theta = vy * pi;
        float sin_t = sinf(theta);
        float cos_t = cosf(theta);

        for(uint32_t x = 0; x <= segments_u; ++x)
        {
            float vx    = (float)x / (float)segments_u;
            float phi   = vx * two_pi;
            float sin_p = sinf(phi);
            float cos_p = cosf(phi);

            float nx = cos_p * sin_t;
            float ny = cos_t;
            float nz = sin_p * sin_t;

            positions[v * 3u + 0u] = nx * radius;
            positions[v * 3u + 1u] = ny * radius;
            positions[v * 3u + 2u] = nz * radius;

            normals[v * 3u + 0u] = nx;
            normals[v * 3u + 1u] = ny;
            normals[v * 3u + 2u] = nz;

            uvs[v * 2u + 0u] = vx;
            uvs[v * 2u + 1u] = 1.0f - vy;
            ++v;
        }
    }

    uint32_t i = 0;
    for(uint32_t y = 0; y < segments_v; ++y)
    {
        uint32_t row  = y * (segments_u + 1u);
        uint32_t next = row + segments_u + 1u;
        for(uint32_t x = 0; x < segments_u; ++x)
        {
            uint32_t a = row + x;
            uint32_t b = next + x;
            uint32_t c = next + x + 1u;
            uint32_t d = row + x + 1u;

            indices[i++] = a;
            indices[i++] = b;
            indices[i++] = c;
            indices[i++] = c;
            indices[i++] = d;
            indices[i++] = a;
        }
    }

    out_mesh->vertex_count                             = vertex_count;
    out_mesh->index_count                              = index_count;
    out_mesh->index                                    = indices;
    out_mesh->attributes[GLTF_ATTRIBUTE_TYPE_POSITION] = positions;
    out_mesh->attributes[GLTF_ATTRIBUTE_TYPE_NORMAL]   = normals;
    out_mesh->attributes[GLTF_ATTRIBUTE_TYPE_TEXCOORD] = uvs;

    primitive_mesh_compute_bounds(out_mesh);
    return true;
}

static bool build_primitive_mesh_cylinder(const vec3 size, bool cone, GLTFMesh* out_mesh)
{
    if(!out_mesh)
        return false;

    memset(out_mesh, 0, sizeof(*out_mesh));

    const float    radius   = size[0] * 0.5f;
    const float    height   = size[1];
    const uint32_t segments = 24;

    const uint32_t side_verts       = (segments + 1u) * 2u;
    const uint32_t cap_verts        = segments + 1u;
    const uint32_t vertex_count     = side_verts + cap_verts + (cone ? 0u : cap_verts);
    const uint32_t side_index_count = segments * 6u;
    const uint32_t cap_index_count  = segments * 3u;
    const uint32_t index_count      = side_index_count + cap_index_count + (cone ? 0u : cap_index_count);

    float*    positions = (float*)malloc((size_t)vertex_count * 3u * sizeof(float));
    float*    normals   = (float*)malloc((size_t)vertex_count * 3u * sizeof(float));
    float*    uvs       = (float*)malloc((size_t)vertex_count * 2u * sizeof(float));
    uint32_t* indices   = (uint32_t*)malloc((size_t)index_count * sizeof(uint32_t));

    if(!positions || !normals || !uvs || !indices)
    {
        free(positions);
        free(normals);
        free(uvs);
        free(indices);
        return false;
    }

    const float half_h = height * 0.5f;
    const float two_pi = 6.28318530717958647692f;

    uint32_t v = 0;
    for(uint32_t s = 0; s <= segments; ++s)
    {
        float t   = (float)s / (float)segments;
        float ang = t * two_pi;
        float cs  = cosf(ang);
        float sn  = sinf(ang);

        float nx    = cs;
        float nz    = sn;
        float top_r = cone ? 0.0f : radius;

        positions[v * 3u + 0u] = cs * radius;
        positions[v * 3u + 1u] = -half_h;
        positions[v * 3u + 2u] = sn * radius;
        normals[v * 3u + 0u]   = nx;
        normals[v * 3u + 1u]   = cone ? radius / height : 0.0f;
        normals[v * 3u + 2u]   = nz;
        uvs[v * 2u + 0u]       = t;
        uvs[v * 2u + 1u]       = 0.0f;
        ++v;

        positions[v * 3u + 0u] = cs * top_r;
        positions[v * 3u + 1u] = half_h;
        positions[v * 3u + 2u] = sn * top_r;
        normals[v * 3u + 0u]   = nx;
        normals[v * 3u + 1u]   = cone ? radius / height : 0.0f;
        normals[v * 3u + 2u]   = nz;
        uvs[v * 2u + 0u]       = t;
        uvs[v * 2u + 1u]       = 1.0f;
        ++v;
    }

    uint32_t bottom_center = v;
    positions[v * 3u + 0u] = 0.0f;
    positions[v * 3u + 1u] = -half_h;
    positions[v * 3u + 2u] = 0.0f;
    normals[v * 3u + 0u]   = 0.0f;
    normals[v * 3u + 1u]   = -1.0f;
    normals[v * 3u + 2u]   = 0.0f;
    uvs[v * 2u + 0u]       = 0.5f;
    uvs[v * 2u + 1u]       = 0.5f;
    ++v;

    for(uint32_t s = 0; s < segments; ++s)
    {
        float t                = (float)s / (float)segments;
        float ang              = t * two_pi;
        float cs               = cosf(ang);
        float sn               = sinf(ang);
        positions[v * 3u + 0u] = cs * radius;
        positions[v * 3u + 1u] = -half_h;
        positions[v * 3u + 2u] = sn * radius;
        normals[v * 3u + 0u]   = 0.0f;
        normals[v * 3u + 1u]   = -1.0f;
        normals[v * 3u + 2u]   = 0.0f;
        uvs[v * 2u + 0u]       = (cs + 1.0f) * 0.5f;
        uvs[v * 2u + 1u]       = (sn + 1.0f) * 0.5f;
        ++v;
    }

    uint32_t top_center = v;
    if(!cone)
    {
        positions[v * 3u + 0u] = 0.0f;
        positions[v * 3u + 1u] = half_h;
        positions[v * 3u + 2u] = 0.0f;
        normals[v * 3u + 0u]   = 0.0f;
        normals[v * 3u + 1u]   = 1.0f;
        normals[v * 3u + 2u]   = 0.0f;
        uvs[v * 2u + 0u]       = 0.5f;
        uvs[v * 2u + 1u]       = 0.5f;
        ++v;

        for(uint32_t s = 0; s < segments; ++s)
        {
            float t                = (float)s / (float)segments;
            float ang              = t * two_pi;
            float cs               = cosf(ang);
            float sn               = sinf(ang);
            positions[v * 3u + 0u] = cs * radius;
            positions[v * 3u + 1u] = half_h;
            positions[v * 3u + 2u] = sn * radius;
            normals[v * 3u + 0u]   = 0.0f;
            normals[v * 3u + 1u]   = 1.0f;
            normals[v * 3u + 2u]   = 0.0f;
            uvs[v * 2u + 0u]       = (cs + 1.0f) * 0.5f;
            uvs[v * 2u + 1u]       = (sn + 1.0f) * 0.5f;
            ++v;
        }
    }

    uint32_t i = 0;
    for(uint32_t s = 0; s < segments; ++s)
    {
        uint32_t base = s * 2u;
        uint32_t a    = base + 0u;
        uint32_t b    = base + 1u;
        uint32_t c    = base + 3u;
        uint32_t d    = base + 2u;
        indices[i++]  = a;
        indices[i++]  = b;
        indices[i++]  = c;
        indices[i++]  = c;
        indices[i++]  = d;
        indices[i++]  = a;
    }

    uint32_t bottom_ring = bottom_center + 1u;
    for(uint32_t s = 0; s < segments; ++s)
    {
        uint32_t a   = bottom_center;
        uint32_t b   = bottom_ring + s;
        uint32_t c   = bottom_ring + ((s + 1u) % segments);
        indices[i++] = a;
        indices[i++] = c;
        indices[i++] = b;
    }

    if(!cone)
    {
        uint32_t top_ring = top_center + 1u;
        for(uint32_t s = 0; s < segments; ++s)
        {
            uint32_t a   = top_center;
            uint32_t b   = top_ring + s;
            uint32_t c   = top_ring + ((s + 1u) % segments);
            indices[i++] = a;
            indices[i++] = b;
            indices[i++] = c;
        }
    }

    out_mesh->vertex_count                             = vertex_count;
    out_mesh->index_count                              = index_count;
    out_mesh->index                                    = indices;
    out_mesh->attributes[GLTF_ATTRIBUTE_TYPE_POSITION] = positions;
    out_mesh->attributes[GLTF_ATTRIBUTE_TYPE_NORMAL]   = normals;
    out_mesh->attributes[GLTF_ATTRIBUTE_TYPE_TEXCOORD] = uvs;

    primitive_mesh_compute_bounds(out_mesh);
    return true;
}

static bool build_primitive_mesh_pyramid(const vec3 size, GLTFMesh* out_mesh)
{
    if(!out_mesh)
        return false;

    memset(out_mesh, 0, sizeof(*out_mesh));

    const float hx = size[0] * 0.5f;
    const float hz = size[2] * 0.5f;
    const float hy = size[1] * 0.5f;

    const uint32_t vertex_count = 18;
    const uint32_t index_count  = 18;

    float*    positions = (float*)malloc((size_t)vertex_count * 3u * sizeof(float));
    float*    normals   = (float*)malloc((size_t)vertex_count * 3u * sizeof(float));
    float*    uvs       = (float*)malloc((size_t)vertex_count * 2u * sizeof(float));
    uint32_t* indices   = (uint32_t*)malloc((size_t)index_count * sizeof(uint32_t));

    if(!positions || !normals || !uvs || !indices)
    {
        free(positions);
        free(normals);
        free(uvs);
        free(indices);
        return false;
    }

    float bx0 = -hx, by0 = -hy, bz0 = -hz;
    float bx1 = hx, by1 = -hy, bz1 = -hz;
    float bx2 = hx, by2 = -hy, bz2 = hz;
    float bx3 = -hx, by3 = -hy, bz3 = hz;
    float ax = 0.0f, ay = hy, az = 0.0f;

    uint32_t v = 0;

    float nbase_x = 0.0f, nbase_y = -1.0f, nbase_z = 0.0f;

    float base_positions[6][3] = {
        {bx0, by0, bz0}, {bx1, by1, bz1}, {bx2, by2, bz2}, {bx0, by0, bz0}, {bx2, by2, bz2}, {bx3, by3, bz3},
    };

    float base_uvs[6][2] = {
        {0.0f, 0.0f}, {1.0f, 0.0f}, {1.0f, 1.0f}, {0.0f, 0.0f}, {1.0f, 1.0f}, {0.0f, 1.0f},
    };

    for(uint32_t i = 0; i < 6; ++i)
    {
        positions[v * 3u + 0u] = base_positions[i][0];
        positions[v * 3u + 1u] = base_positions[i][1];
        positions[v * 3u + 2u] = base_positions[i][2];
        normals[v * 3u + 0u]   = nbase_x;
        normals[v * 3u + 1u]   = nbase_y;
        normals[v * 3u + 2u]   = nbase_z;
        uvs[v * 2u + 0u]       = base_uvs[i][0];
        uvs[v * 2u + 1u]       = base_uvs[i][1];
        indices[v]             = v;
        ++v;
    }

    float side_tris[4][3][3] = {
        {{ax, ay, az}, {bx1, by1, bz1}, {bx0, by0, bz0}},
        {{ax, ay, az}, {bx2, by2, bz2}, {bx1, by1, bz1}},
        {{ax, ay, az}, {bx3, by3, bz3}, {bx2, by2, bz2}},
        {{ax, ay, az}, {bx0, by0, bz0}, {bx3, by3, bz3}},
    };

    float side_uvs[3][2] = {{0.5f, 1.0f}, {1.0f, 0.0f}, {0.0f, 0.0f}};

    for(uint32_t s = 0; s < 4; ++s)
    {
        float nx = 0.0f, ny = 0.0f, nz = 0.0f;
        triangle_normal(side_tris[s][0][0], side_tris[s][0][1], side_tris[s][0][2], side_tris[s][1][0], side_tris[s][1][1],
                        side_tris[s][1][2], side_tris[s][2][0], side_tris[s][2][1], side_tris[s][2][2], &nx, &ny, &nz);

        for(uint32_t k = 0; k < 3; ++k)
        {
            positions[v * 3u + 0u] = side_tris[s][k][0];
            positions[v * 3u + 1u] = side_tris[s][k][1];
            positions[v * 3u + 2u] = side_tris[s][k][2];
            normals[v * 3u + 0u]   = nx;
            normals[v * 3u + 1u]   = ny;
            normals[v * 3u + 2u]   = nz;
            uvs[v * 2u + 0u]       = side_uvs[k][0];
            uvs[v * 2u + 1u]       = side_uvs[k][1];
            indices[v]             = v;
            ++v;
        }
    }

    out_mesh->vertex_count                             = vertex_count;
    out_mesh->index_count                              = index_count;
    out_mesh->index                                    = indices;
    out_mesh->attributes[GLTF_ATTRIBUTE_TYPE_POSITION] = positions;
    out_mesh->attributes[GLTF_ATTRIBUTE_TYPE_NORMAL]   = normals;
    out_mesh->attributes[GLTF_ATTRIBUTE_TYPE_TEXCOORD] = uvs;

    primitive_mesh_compute_bounds(out_mesh);
    return true;
}

static bool build_primitive_mesh_capsule(const vec3 size, GLTFMesh* out_mesh)
{
    if(!out_mesh)
        return false;

    memset(out_mesh, 0, sizeof(*out_mesh));

    float radius = size[0] * 0.5f;
    float height = size[1];
    if(radius <= 0.0f)
        radius = 0.5f;
    if(height <= 0.0f)
        height = radius * 2.0f;

    float cylinder_height = height - 2.0f * radius;
    if(cylinder_height < 0.0f)
        cylinder_height = 0.0f;
    float half_cyl = cylinder_height * 0.5f;

    const uint32_t segments_u    = 24;
    const uint32_t hemi_segments = 8;
    const uint32_t cyl_segments  = cylinder_height > 0.0f ? 4u : 0u;
    const uint32_t ring_count    = (hemi_segments + 1u) + cyl_segments + hemi_segments;
    const uint32_t vertex_count  = ring_count * (segments_u + 1u);
    const uint32_t index_count   = (ring_count - 1u) * segments_u * 6u;

    float*    positions = (float*)malloc((size_t)vertex_count * 3u * sizeof(float));
    float*    normals   = (float*)malloc((size_t)vertex_count * 3u * sizeof(float));
    float*    uvs       = (float*)malloc((size_t)vertex_count * 2u * sizeof(float));
    uint32_t* indices   = (uint32_t*)malloc((size_t)index_count * sizeof(uint32_t));

    if(!positions || !normals || !uvs || !indices)
    {
        free(positions);
        free(normals);
        free(uvs);
        free(indices);
        return false;
    }

    const float pi     = 3.14159265358979323846f;
    const float two_pi = 6.28318530717958647692f;

    uint32_t v = 0;
    for(uint32_t ring = 0; ring < ring_count; ++ring)
    {
        float ring_y = 0.0f;
        float ring_r = radius;
        float ny     = 0.0f;
        float nr     = 1.0f;

        if(ring <= hemi_segments)
        {
            float t     = (float)ring / (float)hemi_segments;
            float theta = t * (pi * 0.5f);
            ring_y      = cosf(theta) * radius + half_cyl;
            ring_r      = sinf(theta) * radius;
            ny          = cosf(theta);
            nr          = sinf(theta);
        }
        else if(ring < hemi_segments + 1u + cyl_segments)
        {
            if(cyl_segments > 0u)
            {
                uint32_t cyl_index = ring - (hemi_segments + 1u);
                float    t         = (float)(cyl_index + 1u) / (float)(cyl_segments + 1u);
                ring_y             = half_cyl - t * cylinder_height;
            }
            else
            {
                ring_y = -half_cyl;
            }
            ring_r = radius;
            ny     = 0.0f;
            nr     = 1.0f;
        }
        else
        {
            uint32_t bottom_ring = ring - (hemi_segments + 1u + cyl_segments);
            float    t           = (float)bottom_ring / (float)hemi_segments;
            float    theta       = t * (pi * 0.5f);
            ring_y               = -cosf(theta) * radius - half_cyl;
            ring_r               = sinf(theta) * radius;
            ny                   = -cosf(theta);
            nr                   = sinf(theta);
        }

        for(uint32_t s = 0; s <= segments_u; ++s)
        {
            float phi = (float)s / (float)segments_u * two_pi;
            float cs  = cosf(phi);
            float sn  = sinf(phi);

            positions[v * 3u + 0u] = cs * ring_r;
            positions[v * 3u + 1u] = ring_y;
            positions[v * 3u + 2u] = sn * ring_r;

            normals[v * 3u + 0u] = cs * nr;
            normals[v * 3u + 1u] = ny;
            normals[v * 3u + 2u] = sn * nr;

            uvs[v * 2u + 0u] = (float)s / (float)segments_u;
            uvs[v * 2u + 1u] = (float)ring / (float)(ring_count - 1u);
            ++v;
        }
    }

    uint32_t i = 0;
    for(uint32_t ring = 0; ring + 1u < ring_count; ++ring)
    {
        uint32_t row  = ring * (segments_u + 1u);
        uint32_t next = row + segments_u + 1u;
        for(uint32_t s = 0; s < segments_u; ++s)
        {
            uint32_t a   = row + s;
            uint32_t b   = next + s;
            uint32_t c   = next + s + 1u;
            uint32_t d   = row + s + 1u;
            indices[i++] = a;
            indices[i++] = b;
            indices[i++] = c;
            indices[i++] = c;
            indices[i++] = d;
            indices[i++] = a;
        }
    }

    out_mesh->vertex_count                             = vertex_count;
    out_mesh->index_count                              = index_count;
    out_mesh->index                                    = indices;
    out_mesh->attributes[GLTF_ATTRIBUTE_TYPE_POSITION] = positions;
    out_mesh->attributes[GLTF_ATTRIBUTE_TYPE_NORMAL]   = normals;
    out_mesh->attributes[GLTF_ATTRIBUTE_TYPE_TEXCOORD] = uvs;

    primitive_mesh_compute_bounds(out_mesh);
    return true;
}

static bool build_primitive_mesh_torus(const vec3 size, GLTFMesh* out_mesh)
{
    if(!out_mesh)
        return false;

    memset(out_mesh, 0, sizeof(*out_mesh));

    float major_radius = size[0] * 0.5f;
    float tube_radius  = size[1] * 0.5f;
    if(major_radius <= 0.0f)
        major_radius = 0.75f;
    if(tube_radius <= 0.0f)
        tube_radius = major_radius * 0.35f;

    const uint32_t segments_major = 32;
    const uint32_t segments_tube  = 16;
    const uint32_t vertex_count   = (segments_major + 1u) * (segments_tube + 1u);
    const uint32_t index_count    = segments_major * segments_tube * 6u;

    float*    positions = (float*)malloc((size_t)vertex_count * 3u * sizeof(float));
    float*    normals   = (float*)malloc((size_t)vertex_count * 3u * sizeof(float));
    float*    uvs       = (float*)malloc((size_t)vertex_count * 2u * sizeof(float));
    uint32_t* indices   = (uint32_t*)malloc((size_t)index_count * sizeof(uint32_t));

    if(!positions || !normals || !uvs || !indices)
    {
        free(positions);
        free(normals);
        free(uvs);
        free(indices);
        return false;
    }

    const float two_pi = 6.28318530717958647692f;

    uint32_t v = 0;
    for(uint32_t u = 0; u <= segments_major; ++u)
    {
        float tu  = (float)u / (float)segments_major;
        float phi = tu * two_pi;
        float cp  = cosf(phi);
        float sp  = sinf(phi);

        for(uint32_t t = 0; t <= segments_tube; ++t)
        {
            float tv    = (float)t / (float)segments_tube;
            float theta = tv * two_pi;
            float ct    = cosf(theta);
            float st    = sinf(theta);

            float r                = major_radius + tube_radius * ct;
            positions[v * 3u + 0u] = r * cp;
            positions[v * 3u + 1u] = tube_radius * st;
            positions[v * 3u + 2u] = r * sp;

            normals[v * 3u + 0u] = ct * cp;
            normals[v * 3u + 1u] = st;
            normals[v * 3u + 2u] = ct * sp;

            uvs[v * 2u + 0u] = tu;
            uvs[v * 2u + 1u] = tv;
            ++v;
        }
    }

    uint32_t i = 0;
    for(uint32_t u = 0; u < segments_major; ++u)
    {
        uint32_t row  = u * (segments_tube + 1u);
        uint32_t next = row + segments_tube + 1u;
        for(uint32_t t = 0; t < segments_tube; ++t)
        {
            uint32_t a   = row + t;
            uint32_t b   = next + t;
            uint32_t c   = next + t + 1u;
            uint32_t d   = row + t + 1u;
            indices[i++] = a;
            indices[i++] = b;
            indices[i++] = c;
            indices[i++] = c;
            indices[i++] = d;
            indices[i++] = a;
        }
    }

    out_mesh->vertex_count                             = vertex_count;
    out_mesh->index_count                              = index_count;
    out_mesh->index                                    = indices;
    out_mesh->attributes[GLTF_ATTRIBUTE_TYPE_POSITION] = positions;
    out_mesh->attributes[GLTF_ATTRIBUTE_TYPE_NORMAL]   = normals;
    out_mesh->attributes[GLTF_ATTRIBUTE_TYPE_TEXCOORD] = uvs;

    primitive_mesh_compute_bounds(out_mesh);
    return true;
}

static bool build_primitive_mesh(PrimitiveShape shape, const vec3 size, GLTFMesh* out_mesh)
{
    switch(shape)
    {
        case PRIM_CUBE:
        case PRIM_CUBOID:
            return build_primitive_mesh_box(size, out_mesh);
        case PRIM_PLANE:
            return build_primitive_mesh_plane(size, out_mesh);
        case PRIM_SPHERE:
            return build_primitive_mesh_sphere(size, out_mesh);
        case PRIM_CYLINDER:
            return build_primitive_mesh_cylinder(size, false, out_mesh);
        case PRIM_CONE:
            return build_primitive_mesh_cylinder(size, true, out_mesh);
        case PRIM_PYRAMID:
            return build_primitive_mesh_pyramid(size, out_mesh);
        case PRIM_CAPSULE:
            return build_primitive_mesh_capsule(size, out_mesh);
        case PRIM_TORUS:
            return build_primitive_mesh_torus(size, out_mesh);
        default:
            return false;
    }
}

static GLTFContainer* create_primitive_container(PrimitiveShape shape, const vec3 size)
{
    GLTFContainer* gltf = (GLTFContainer*)calloc(1, sizeof(GLTFContainer));
    if(!gltf)
        return NULL;

    gltf->mesh_count = 1;
    gltf->meshes     = (GLTFMesh*)calloc(1, sizeof(GLTFMesh));
    if(!gltf->meshes)
    {
        free(gltf);
        return NULL;
    }

    if(!build_primitive_mesh(shape, size, &gltf->meshes[0]))
    {
        free_primitive_mesh(&gltf->meshes[0]);
        free(gltf->meshes);
        free(gltf);
        return NULL;
    }

    gltf->vertex_count = gltf->meshes[0].vertex_count;
    gltf->index_count  = gltf->meshes[0].index_count;
    strncpy(gltf->source_path, "primitive", sizeof(gltf->source_path) - 1);

    return gltf;
}

static bool gltf_gpu_model_init_from_cpu(GltfGpuModel* model, GLTFContainer* cpu)
{
    if(!model || !cpu)
        return false;

    memset(model, 0, sizeof(*model));
    model->cpu = cpu;

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

        VkDeviceSize vtx_bytes = (VkDeviceSize)src->vertex_count * sizeof(PackedVertex);
        VkDeviceSize idx_bytes = (VkDeviceSize)src->index_count * sizeof(uint32_t);

        model->gpu_meshes[i].packed_vertex_slice = buffer_pool_alloc(&renderer.gpu_pool, vtx_bytes, 16);
        model->gpu_meshes[i].index_slice         = buffer_pool_alloc(&renderer.gpu_pool, idx_bytes, 16);

        if(model->gpu_meshes[i].packed_vertex_slice.buffer == VK_NULL_HANDLE || model->gpu_meshes[i].index_slice.buffer == VK_NULL_HANDLE)
            return false;

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

static bool gltf_gpu_model_init(GltfGpuModel* model, const char* gltf_path)
{
    if(!model)
        return false;

    GLTFContainer* cpu = NULL;
    if(!loadGltf(gltf_path, GLTF_FLAG_LOAD_VERTICES | GLTF_FLAG_CALCULATE_BOUNDS, &cpu))
        return false;

    if(!gltf_gpu_model_init_from_cpu(model, cpu))
    {
        freeGltf(cpu);
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
            if(model->gpu_meshes[i].packed_vertex_slice.buffer != VK_NULL_HANDLE)
                buffer_pool_free(model->gpu_meshes[i].packed_vertex_slice);
            if(model->gpu_meshes[i].index_slice.buffer != VK_NULL_HANDLE)
                buffer_pool_free(model->gpu_meshes[i].index_slice);
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

static void destroy_obstacles(ObstacleInstance* obstacles, uint32_t count)
{
    if(!obstacles)
        return;

    for(uint32_t i = 0; i < count; ++i)
        gltf_gpu_model_destroy(&obstacles[i].instance.model);
    free(obstacles);
}

static void obstacle_set_half_extents(ObstacleInstance* obstacle)
{
    if(!obstacle)
        return;

    obstacle->half_extents[0] = obstacle->size[0] * 0.5f;
    obstacle->half_extents[1] = obstacle->size[1] * 0.5f;
    obstacle->half_extents[2] = obstacle->size[2] * 0.5f;

    if(obstacle->shape == PRIM_TORUS)
    {
        float major               = obstacle->size[0] * 0.5f;
        float tube                = obstacle->size[1] * 0.5f;
        obstacle->half_extents[0] = major + tube;
        obstacle->half_extents[2] = major + tube;
        obstacle->half_extents[1] = tube;
    }
}

static PrimitiveShape random_obstacle_shape(void)
{
    PrimitiveShape shapes[] = {
        PRIM_CUBE, PRIM_CUBOID, PRIM_SPHERE, PRIM_CYLINDER, PRIM_CONE, PRIM_PYRAMID, PRIM_CAPSULE, PRIM_TORUS,
    };

    int index = rand_int_range(0, (int)(sizeof(shapes) / sizeof(shapes[0]) - 1u));
    return shapes[index];
}

static void random_obstacle_size(PrimitiveShape shape, vec3 out_size)
{
    if(!out_size)
        return;

    switch(shape)
    {
        case PRIM_CUBE:
            out_size[0] = rand_float_range(0.7f, 1.6f);
            out_size[1] = out_size[0];
            out_size[2] = out_size[0];
            break;
        case PRIM_CUBOID:
            out_size[0] = rand_float_range(0.6f, 1.6f);
            out_size[1] = rand_float_range(0.6f, 2.0f);
            out_size[2] = rand_float_range(0.6f, 1.6f);
            break;
        case PRIM_SPHERE:
            out_size[0] = rand_float_range(0.8f, 1.6f);
            out_size[1] = out_size[0];
            out_size[2] = out_size[0];
            break;
        case PRIM_CYLINDER:
        case PRIM_CONE:
            out_size[0] = rand_float_range(0.6f, 1.2f);
            out_size[1] = rand_float_range(0.8f, 2.2f);
            out_size[2] = out_size[0];
            break;
        case PRIM_PYRAMID:
            out_size[0] = rand_float_range(0.7f, 1.6f);
            out_size[1] = rand_float_range(0.8f, 2.0f);
            out_size[2] = rand_float_range(0.7f, 1.6f);
            break;
        case PRIM_CAPSULE:
            out_size[0] = rand_float_range(0.6f, 1.1f);
            out_size[1] = rand_float_range(1.6f, 2.8f);
            out_size[2] = out_size[0];
            break;
        case PRIM_TORUS:
            out_size[0] = rand_float_range(1.4f, 2.4f);
            out_size[1] = rand_float_range(0.4f, 0.8f);
            out_size[2] = out_size[0];
            break;
        default:
            out_size[0] = 1.0f;
            out_size[1] = 1.0f;
            out_size[2] = 1.0f;
            break;
    }
}

static void respawn_obstacle(ObstacleInstance* obstacle, float player_z, float min_ahead, float max_ahead, float lane_min_x, float lane_max_x)
{
    if(!obstacle)
        return;

    float ahead                    = rand_float_range(min_ahead, max_ahead);
    obstacle->instance.position[0] = rand_float_range(lane_min_x, lane_max_x);
    obstacle->instance.position[1] = 0.0f;
    obstacle->instance.position[2] = player_z - ahead;
}

static bool init_obstacles(uint32_t           count,
                           float              player_z,
                           float              min_ahead,
                           float              max_ahead,
                           float              lane_min_x,
                           float              lane_max_x,
                           ObstacleInstance** out_obstacles,
                           uint32_t*          out_count)
{
    if(!out_obstacles || !out_count || count == 0)
        return false;

    ObstacleInstance* obstacles = (ObstacleInstance*)calloc(count, sizeof(ObstacleInstance));
    if(!obstacles)
        return false;

    uint32_t created = 0;
    float    spacing = (max_ahead - min_ahead) / (float)count;
    for(uint32_t i = 0; i < count; ++i)
    {
        PrimitiveShape shape = random_obstacle_shape();
        vec3           size  = {0.0f, 0.0f, 0.0f};
        random_obstacle_size(shape, size);

        Draw3DDesc desc        = {0};
        desc.primitive         = shape;
        desc.primitive_size[0] = size[0];
        desc.primitive_size[1] = size[1];
        desc.primitive_size[2] = size[2];
        desc.position[0]       = rand_float_range(lane_min_x, lane_max_x);
        desc.position[1]       = 0.0f;
        desc.position[2]       = player_z - (min_ahead + spacing * (float)i + rand_float_range(0.0f, spacing));
        desc.bloom_enabled     = false;
        desc.bloom_color[0]    = 0.2f;
        desc.bloom_color[1]    = 0.6f;
        desc.bloom_color[2]    = 0.2f;
        desc.animation_speed   = 1.0f;
        desc.animation_paused  = true;

        if(!draw_3d(&desc, &obstacles[created].instance))
            continue;

        obstacles[created].shape   = shape;
        obstacles[created].size[0] = size[0];
        obstacles[created].size[1] = size[1];
        obstacles[created].size[2] = size[2];
        obstacle_set_half_extents(&obstacles[created]);
        ++created;
    }

    if(created == 0)
    {
        free(obstacles);
        return false;
    }

    *out_obstacles = obstacles;
    *out_count     = created;
    return true;
}

static bool aabb_overlap(const vec3 a_pos, const vec3 a_half, const vec3 b_pos, const vec3 b_half)
{
    if(fabsf(a_pos[0] - b_pos[0]) > (a_half[0] + b_half[0]))
        return false;
    if(fabsf(a_pos[1] - b_pos[1]) > (a_half[1] + b_half[1]))
        return false;
    if(fabsf(a_pos[2] - b_pos[2]) > (a_half[2] + b_half[2]))
        return false;
    return true;
}

static void debug_draw_line_list(VkCommandBuffer cmd, const vec3* positions, uint32_t pos_count,
                                 const uint32_t* indices, uint32_t index_count, const vec4 color)
{
    if(!positions || !indices || pos_count == 0 || index_count == 0)
        return;

    BufferSlice pos_slice = buffer_pool_alloc(&renderer.cpu_pool, sizeof(vec3) * pos_count, 16);
    BufferSlice idx_slice = buffer_pool_alloc(&renderer.cpu_pool, sizeof(uint32_t) * index_count, 4);
    if(pos_slice.buffer == VK_NULL_HANDLE || idx_slice.buffer == VK_NULL_HANDLE)
        return;

    memcpy(pos_slice.mapped, positions, sizeof(vec3) * pos_count);
    memcpy(idx_slice.mapped, indices, sizeof(uint32_t) * index_count);

    DebugLinePush push = {0};
    push.pos_ptr = slice_device_address(&renderer, pos_slice);
    push.idx_ptr = slice_device_address(&renderer, idx_slice);
    glm_mat4_identity(push.model);
    glm_vec4_copy(color, push.color);
    push.index_count = index_count;

    vkCmdPushConstants(cmd, renderer.bindless_system.pipeline_layout, VK_SHADER_STAGE_ALL, 0, sizeof(DebugLinePush), &push);
    vkCmdDraw(cmd, index_count, 1, 0, 0);
}

static void debug_draw_aabb(VkCommandBuffer cmd, const vec3 center, const vec3 half, const vec4 color)
{
    vec3     positions[8];
    uint32_t indices[24];

    build_aabb_wireframe(center, half, positions, indices);
    debug_draw_line_list(cmd, positions, 8, indices, 24, color);
}

static bool draw_3d(const Draw3DDesc* desc, GltfSceneInstance* out_instance)
{
    if(!desc || !out_instance)
        return false;

    bool use_primitive = desc->gltf_path == NULL && desc->primitive != PRIM_NONE;
    if(!desc->gltf_path && !use_primitive)
        return false;

    memset(out_instance, 0, sizeof(*out_instance));

    if(use_primitive)
    {
        vec3 size = {desc->primitive_size[0], desc->primitive_size[1], desc->primitive_size[2]};
        if(size[0] <= 0.0f)
            size[0] = 1.0f;
        if(size[2] <= 0.0f)
            size[2] = 1.0f;
        if(desc->primitive == PRIM_CUBE || desc->primitive == PRIM_CUBOID)
        {
            if(size[1] <= 0.0f)
                size[1] = 1.0f;
        }
        if(desc->primitive == PRIM_SPHERE)
        {
            size[1] = size[0];
            size[2] = size[0];
        }
        if(desc->primitive == PRIM_CYLINDER || desc->primitive == PRIM_CONE)
        {
            if(size[1] <= 0.0f)
                size[1] = 1.0f;
            size[2] = size[0];
        }
        if(desc->primitive == PRIM_PYRAMID)
        {
            if(size[1] <= 0.0f)
                size[1] = 1.0f;
            if(size[2] <= 0.0f)
                size[2] = size[0];
        }
        if(desc->primitive == PRIM_CAPSULE)
        {
            if(size[1] <= 0.0f)
                size[1] = size[0] * 2.0f;
            size[2] = size[0];
        }
        if(desc->primitive == PRIM_TORUS)
        {
            if(size[1] <= 0.0f)
                size[1] = size[0] * 0.5f;
            size[2] = size[0];
        }

        GLTFContainer* primitive = create_primitive_container(desc->primitive, size);
        if(!primitive)
            return false;
        if(!gltf_gpu_model_init_from_cpu(&out_instance->model, primitive))
        {
            freeGltf(primitive);
            return false;
        }
    }
    else
    {
        if(!gltf_gpu_model_init(&out_instance->model, desc->gltf_path))
            return false;
    }

    out_instance->position[0]   = desc->position[0];
    out_instance->position[1]   = desc->position[1];
    out_instance->position[2]   = desc->position[2];
    out_instance->use_transform = desc->use_transform;
    if(desc->use_transform)
        memcpy(out_instance->xform.transform, desc->xform.transform, sizeof(out_instance->xform.transform));
    else
        glm_vec3_copy(desc->xform.rotation, out_instance->xform.rotation);
    out_instance->bloom_enabled    = desc->bloom_enabled;
    out_instance->bloom_color[0]   = desc->bloom_color[0];
    out_instance->bloom_color[1]   = desc->bloom_color[1];
    out_instance->bloom_color[2]   = desc->bloom_color[2];
    out_instance->animation_index  = desc->animation_index;
    out_instance->animation_time   = desc->animation_time;
    out_instance->animation_speed  = desc->animation_speed;
    out_instance->animation_paused = desc->animation_paused;

    if(out_instance->model.cpu && out_instance->model.cpu->animation_count > 0)
    {
        if(out_instance->animation_index >= out_instance->model.cpu->animation_count)
            out_instance->animation_index = 0;
    }
    else
    {
        out_instance->animation_index  = 0;
        out_instance->animation_time   = 0.0f;
        out_instance->animation_speed  = 1.0f;
        out_instance->animation_paused = true;
    }

    return true;
}

static void gltf_gpu_model_upload_once(GltfGpuModel* model, VkCommandBuffer cmd)
{
    if(model->uploaded)
        return;

    uint32_t barrier_capacity = model->cpu->mesh_count * 6u + 5u;
    VkBufferMemoryBarrier2* barriers = (VkBufferMemoryBarrier2*)calloc(barrier_capacity, sizeof(VkBufferMemoryBarrier2));
    if(!barriers)
        return;

    uint32_t b = 0;
    for(uint32_t i = 0; i < model->cpu->mesh_count; ++i)
    {
        GLTFMesh*    src = &model->cpu->meshes[i];
        GltfGpuMesh* dst = &model->gpu_meshes[i];

        VkDeviceSize vtx_bytes = (VkDeviceSize)src->vertex_count * sizeof(PackedVertex);
        VkDeviceSize idx_bytes = (VkDeviceSize)src->index_count * sizeof(uint32_t);

        float* positions = (float*)src->attributes[GLTF_ATTRIBUTE_TYPE_POSITION];
        float* normals   = (float*)src->attributes[GLTF_ATTRIBUTE_TYPE_NORMAL];
        float* uvs       = (float*)src->attributes[GLTF_ATTRIBUTE_TYPE_TEXCOORD];
        float* tangents  = (float*)src->attributes[GLTF_ATTRIBUTE_TYPE_TANGENT];

        PackedVertex* packed_vertices = (PackedVertex*)malloc(vtx_bytes);
        if(!packed_vertices)
        {
            free(barriers);
            return;
        }

        for(uint32_t v = 0; v < src->vertex_count; ++v)
        {
            float px = positions[v * 3u + 0u];
            float py = positions[v * 3u + 1u];
            float pz = positions[v * 3u + 2u];

            float nx = 0.0f;
            float ny = 1.0f;
            float nz = 0.0f;
            if(normals)
            {
                nx = normals[v * 3u + 0u];
                ny = normals[v * 3u + 1u];
                nz = normals[v * 3u + 2u];
            }

            float tx             = 1.0f;
            float ty             = 0.0f;
            int   bitangent_sign = 1;
            if(tangents)
            {
                tx             = tangents[v * 4u + 0u];
                ty             = tangents[v * 4u + 1u];
                bitangent_sign = tangents[v * 4u + 3u] >= 0.0f ? 1 : -1;
            }

            float u = 0.0f;
            float w = 0.0f;
            if(uvs)
            {
                u = uvs[v * 2u + 0u];
                w = uvs[v * 2u + 1u];
            }

            packed_vertices[v] = mu_pack_vertex(px, py, pz, nx, ny, nz, tx, ty, u, w, bitangent_sign);
        }

        renderer_upload_buffer_to_slice(&renderer, cmd, dst->packed_vertex_slice, packed_vertices, vtx_bytes, 16);
        renderer_upload_buffer_to_slice(&renderer, cmd, dst->index_slice, src->index, idx_bytes, 16);
        free(packed_vertices);

        barriers[b++] = (VkBufferMemoryBarrier2){
            .sType         = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
            .srcStageMask  = VK_PIPELINE_STAGE_2_COPY_BIT,
            .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
            .dstStageMask  = VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT,
            .dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
            .buffer        = dst->packed_vertex_slice.buffer,
            .offset        = dst->packed_vertex_slice.offset,
            .size          = vtx_bytes,
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

    if(model->cpu->node_count > 0)
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
    if(instance->use_transform)
    {
        glm_mat4_mul(instance_transform, instance->xform.transform, instance_transform);
    }
    else
    {
        mat4 rot;
        glm_mat4_identity(rot);
        glm_rotate_x(rot, instance->xform.rotation[0], rot);
        glm_rotate_y(rot, instance->xform.rotation[1], rot);
        glm_rotate_z(rot, instance->xform.rotation[2], rot);
        glm_mat4_mul(instance_transform, rot, instance_transform);
    }

    for(uint32_t i = 0; i < model->cpu->mesh_count; ++i)
    {
        GltfIndirectDrawData* draw     = &model->draw_data_cpu[i];
        GLTFMesh*             mesh     = &model->cpu->meshes[i];
        GltfGpuMesh*          gpu_mesh = &model->gpu_meshes[i];

        draw->vtx_ptr     = slice_device_address(&renderer, gpu_mesh->packed_vertex_slice);
        draw->idx_ptr     = slice_device_address(&renderer, gpu_mesh->index_slice);
        draw->joints_ptr  = 0;
        draw->weights_ptr = 0;
        draw->material_id = 0;
        draw->skin_offset = 0;

        draw->flags = 0;
        if(instance->bloom_enabled)
            draw->flags |= 0x8u;
        draw->bloom_color[0] = instance->bloom_color[0];
        draw->bloom_color[1] = instance->bloom_color[1];
        draw->bloom_color[2] = instance->bloom_color[2];
        u32 node_index       = model->cpu->mesh_node_indices ? model->cpu->mesh_node_indices[i] : UINT_MAX;
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

static void draw_gltf_model(VkCommandBuffer cmd, const GltfGpuModel* model)
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

    vkCmdPushConstants(cmd, renderer.bindless_system.pipeline_layout, VK_SHADER_STAGE_ALL, 0, sizeof(GltfUberPush), &push);

    vkCmdDrawIndirectCount(cmd, model->indirect_cmd_slice.buffer, model->indirect_cmd_slice.offset,
                           model->indirect_count_slice.buffer, model->indirect_count_slice.offset,
                           model->cpu->mesh_count, sizeof(VkDrawIndirectCommand));
}

int main(void)
{
    if(!fs_init(NULL, "save", ".."))
        return 1;
    atexit(fs_shutdown);

    graphics_init();
    rand_seed((uint64_t)time(NULL));

    AudioConfig audio_cfg = audio_default_config();
    AudioSystem* audio = audio_create(&audio_cfg);
    if(!audio)
    {
        renderer_destroy(&renderer);
        return 1;
    }

    Camera cam = {0};
    camera_defaults_3d(&cam);
    camera3d_set_position(&cam, 0.0f, 0.6f, 4.0f);
    camera3d_set_rotation_yaw_pitch(&cam, 0.0f, 0.0f);

    vec3 prev_listener_pos = {0.0f, 0.0f, 0.0f};
    glm_vec3_copy(cam.position, prev_listener_pos);

    char**   glb_paths = NULL;
    uint32_t glb_count = 0;

    if(!collect_glb_paths(BLOCKY_DIR, &glb_paths, &glb_count))
    {
        audio_destroy(audio);
        renderer_destroy(&renderer);
        return 1;
    }

    GltfSceneInstance player      = {0};
    Draw3DDesc        player_desc = {0};
    player_desc.gltf_path         = glb_paths[0];
    player_desc.xform.rotation[0] = 0.0f;
    player_desc.position[0]       = 0.0f;
    player_desc.position[1]       = 0.0f;
    player_desc.position[2]       = 0.0f;
    player_desc.bloom_enabled     = false;
    player_desc.bloom_color[0]    = 1.0f;
    player_desc.bloom_color[1]    = 1.0f;
    player_desc.bloom_color[2]    = 1.0f;
    player_desc.animation_index   = 1;
    player_desc.animation_time    = 1.0f;
    player_desc.animation_speed   = 2.0f;

    player_desc.animation_paused = false;
    player_desc.use_transform    = true;
    compose_transform_pos_rot(player_desc.position, (vec3){0.0f, glm_rad(180.0f), 0.0f}, player_desc.xform.transform);
    if(!draw_3d(&player_desc, &player))
    {
        free_glb_paths(glb_paths, glb_count);
        audio_destroy(audio);
        renderer_destroy(&renderer);
        return 1;
    }

    const uint32_t obstacle_capacity   = 48;
    const float    obstacle_spawn_min  = 12.0f;
    const float    obstacle_spawn_max  = 70.0f;
    const float    obstacle_lane_min_x = -6.0f;
    const float    obstacle_lane_max_x = 6.0f;

    ObstacleInstance* obstacles      = NULL;
    uint32_t          obstacle_count = 0;
    if(!init_obstacles(obstacle_capacity, player_desc.position[2], obstacle_spawn_min, obstacle_spawn_max,
                       obstacle_lane_min_x, obstacle_lane_max_x, &obstacles, &obstacle_count))
    {
        gltf_gpu_model_destroy(&player.model);
        free_glb_paths(glb_paths, glb_count);
        audio_destroy(audio);
        renderer_destroy(&renderer);
        return 1;
    }

    Input       input             = {0};
    ActionMap   actions           = {0};
    double      prev_time_seconds = glfwGetTime();
    float       player_vel_x      = 0.0f;
    float       player_vel_z      = 0.0f;
    float       player_speed      = 3.5f;
    float       player_max_speed  = 7.0f;
    float       player_min_speed  = 1.5f;
    float       player_strafe     = 5.0f;
    const char* player_idle_anim  = "idle";
    const char* player_move_anim  = "run";
    bool        game_paused       = false;
    bool        collision_debug   = false;
    vec3        player_pos        = {0.0f, 0.0f, 0.0f};
    vec3        prev_player_pos   = {0.0f, 0.0f, 0.0f};
    vec3        player_half       = {0.35f, 0.8f, 0.35f};
    float       score_distance    = 0.0f;

    VmaBudget memory_budgets[VK_MAX_MEMORY_HEAPS] = {0};
    uint32_t  memory_heap_count                   = renderer.info.memory.memoryHeapCount;
    double    memory_budget_last_update           = -1.0;
    double    memory_budget_interval_s            = 2.0;
    bool      memory_window_open                  = true;

    input_init(&input);
    input_attach(&input, renderer.window);
    input_actions_default(&actions);
    player.position[0]            = player_pos[0];
    player.position[1]            = player_pos[1];
    player.position[2]            = player_pos[2];
    g_postfx_settings.dof_enabled = false;
    while(!glfwWindowShouldClose(renderer.window))
    {
        TracyCFrameMark;

        double now_seconds         = glfwGetTime();
        float  frame_delta_seconds = (float)(now_seconds - prev_time_seconds);
        prev_time_seconds          = now_seconds;

        if(frame_delta_seconds < 0.0f)
            frame_delta_seconds = 0.0f;

        input_update(&input, renderer.window);

        bool load_model_1 = input_action_pressed(&input, &actions, ACTION_LOAD_MODEL_1);
        bool load_model_2 = input_action_pressed(&input, &actions, ACTION_LOAD_MODEL_2);
        if(load_model_1 || load_model_2)
        {
            const char* model_dir  = load_model_1 ? PETS_DIR : BLOCKY_DIR;
            char**      next_paths = NULL;
            uint32_t    next_count = 0;
            if(collect_glb_paths(model_dir, &next_paths, &next_count))
            {
                GltfSceneInstance next_player = {0};
                Draw3DDesc        next_desc   = {0};
                next_desc.gltf_path           = next_paths[0];
                next_desc.position[0]         = player_pos[0];
                next_desc.position[1]         = player_pos[1];
                next_desc.position[2]         = player_pos[2];
                next_desc.bloom_enabled       = false;
                next_desc.bloom_color[0]      = 0.0f;
                next_desc.bloom_color[1]      = 0.0f;
                next_desc.bloom_color[2]      = 0.0f;
                next_desc.animation_index     = 0;
                next_desc.animation_time      = 0.0f;
                next_desc.animation_speed     = 1.0f;
                next_desc.animation_paused    = game_paused;

                next_desc.use_transform = true;
                compose_transform_pos_rot(next_desc.position, (vec3){0.0f, glm_rad(180.0f), 0.0f}, next_desc.xform.transform);

                if(draw_3d(&next_desc, &next_player))
                {
                    gltf_gpu_model_destroy(&player.model);
                    player = next_player;
                    free_glb_paths(glb_paths, glb_count);
                    glb_paths = next_paths;
                    glb_count = next_count;
                }
                else
                {
                    free_glb_paths(next_paths, next_count);
                }
            }
        }

        if(input_action_pressed(&input, &actions, ACTION_TOGGLE_PAUSE))
        {
            game_paused             = !game_paused;
            player.animation_paused = game_paused;
        }

        if(input_action_pressed(&input, &actions, ACTION_RESET_ANIM))
        {
            player.animation_time = 0.0f;
            player_pos[0]         = 0.0f;
            player_pos[2]         = 0.0f;
            score_distance        = 0.0f;
            for(uint32_t i = 0; i < obstacle_count; ++i)
                respawn_obstacle(&obstacles[i], player_pos[2], obstacle_spawn_min, obstacle_spawn_max,
                                 obstacle_lane_min_x, obstacle_lane_max_x);
        }

        float move_x = 0.0f;
        float move_z = 0.0f;
        input_get_move_vector(&input, KEY_LEFT, KEY_RIGHT, KEY_UP, KEY_DOWN, &move_x, &move_z);

        prev_player_pos[0] = player_pos[0];
        prev_player_pos[1] = player_pos[1];
        prev_player_pos[2] = player_pos[2];

        if(!game_paused)
        {
            float target_speed = player_speed + move_z * 2.5f;
            if(target_speed < player_min_speed)
                target_speed = player_min_speed;
            if(target_speed > player_max_speed)
                target_speed = player_max_speed;

            player_vel_x = move_x * player_strafe;
            player_vel_z = -target_speed;

            player_pos[0] += player_vel_x * frame_delta_seconds;
            player_pos[2] += player_vel_z * frame_delta_seconds;

            if(player_pos[0] < obstacle_lane_min_x)
                player_pos[0] = obstacle_lane_min_x;
            else if(player_pos[0] > obstacle_lane_max_x)
                player_pos[0] = obstacle_lane_max_x;

            score_distance = -player_pos[2];
        }

        if(player.model.cpu && player.model.cpu->animation_count > 0 && !player.animation_paused)
        {
            bool        moving     = fabsf(player_vel_x) > 0.01f || fabsf(player_vel_z) > 0.01f;
            const char* anim       = moving ? player_move_anim : player_idle_anim;
            uint32_t    anim_index = find_animation_by_name(&player, anim, 0);
            if(player.animation_index != anim_index)
                player.animation_time = 0.0f;
            player.animation_index = anim_index;
            player.animation_speed = 1.0f;
        }

        bool        hit_obstacle            = false;
        int         hit_obstacle_index      = -1;
        const float obstacle_despawn_behind = 6.0f;

        for(uint32_t i = 0; i < obstacle_count; ++i)
        {
            ObstacleInstance* obs = &obstacles[i];
            if(!game_paused)
            {
                if(obs->instance.position[2] - player_pos[2] > obstacle_despawn_behind)
                    respawn_obstacle(obs, player_pos[2], obstacle_spawn_min, obstacle_spawn_max, obstacle_lane_min_x, obstacle_lane_max_x);
            }

            vec3 expanded_half = {
                obs->half_extents[0] + player_half[0],
                obs->half_extents[1] + player_half[1],
                obs->half_extents[2] + player_half[2],
            };

            if(segment_aabb_intersect(prev_player_pos, player_pos, obs->instance.position, expanded_half))
            {
                hit_obstacle = true;
                hit_obstacle_index = (int)i;
                break;
            }
        }

        if(hit_obstacle)
        {
            player_pos[0]         = 0.0f;
            player_pos[2]         = 0.0f;
            score_distance        = 0.0f;
            player.animation_time = 0.0f;
            for(uint32_t i = 0; i < obstacle_count; ++i)
                respawn_obstacle(&obstacles[i], player_pos[2], obstacle_spawn_min, obstacle_spawn_max,
                                 obstacle_lane_min_x, obstacle_lane_max_x);
        }

        player.position[0] = player_pos[0];
        player.position[1] = player_pos[1];
        player.position[2] = player_pos[2];

        update_animation_time(&player, frame_delta_seconds);

        camera3d_set_position(&cam, player_pos[0], player_pos[1] + 1.2f, player_pos[2] + 4.5f);
        camera3d_set_rotation_yaw_pitch(&cam, 0.0f, -0.08f);

        vec3 listener_forward = {
            cosf(cam.pitch) * sinf(cam.yaw),
            sinf(cam.pitch),
            -cosf(cam.pitch) * cosf(cam.yaw),
        };
        glm_vec3_normalize(listener_forward);

        vec3 listener_vel = {0.0f, 0.0f, 0.0f};
        if(frame_delta_seconds > 0.0f)
        {
            vec3 delta;
            glm_vec3_sub(cam.position, prev_listener_pos, delta);
            glm_vec3_scale(delta, 1.0f / frame_delta_seconds, listener_vel);
        }
        glm_vec3_copy(cam.position, prev_listener_pos);

        AudioListener listener = {0};
        listener.position[0] = cam.position[0];
        listener.position[1] = cam.position[1];
        listener.position[2] = cam.position[2];
        listener.velocity[0] = listener_vel[0];
        listener.velocity[1] = listener_vel[1];
        listener.velocity[2] = listener_vel[2];
        listener.at[0] = listener_forward[0];
        listener.at[1] = listener_forward[1];
        listener.at[2] = listener_forward[2];
        listener.up[0] = 0.0f;
        listener.up[1] = 1.0f;
        listener.up[2] = 0.0f;

        audio_update(audio, &listener);

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

                rt_transition_mip(cmd, &renderer.bloom_chain[renderer.swapchain.current_image], 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                  VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);

                image_transition_swapchain(cmd, &renderer.swapchain, VK_IMAGE_LAYOUT_GENERAL,
                                           VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
                flush_barriers(cmd);
            }

            gltf_gpu_model_upload_once(&player.model, cmd);
            gltf_gpu_model_update_skinning(&player, cmd);
            gltf_gpu_model_update_draw_data(&player, cmd);

            for(uint32_t i = 0; i < obstacle_count; ++i)
            {
                gltf_gpu_model_upload_once(&obstacles[i].instance.model, cmd);
                gltf_gpu_model_update_skinning(&obstacles[i].instance, cmd);
                gltf_gpu_model_update_draw_data(&obstacles[i].instance, cmd);
            }

            VkRenderingAttachmentInfo color = {
                .sType            = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                .imageView        = renderer.hdr_color[renderer.swapchain.current_image].view,
                .imageLayout      = renderer.hdr_color[renderer.swapchain.current_image].mip_states[0].layout,
                .loadOp           = VK_ATTACHMENT_LOAD_OP_CLEAR,
                .storeOp          = VK_ATTACHMENT_STORE_OP_STORE,
                .clearValue.color = {{0.10f, 0.12f, 0.15f, 1.0f}},
            };

            VkRenderingAttachmentInfo bloom_src = {
                .sType            = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                .imageView        = renderer.bloom_chain[renderer.swapchain.current_image].mip_views[0],
                .imageLayout      = renderer.bloom_chain[renderer.swapchain.current_image].mip_states[0].layout,
                .loadOp           = VK_ATTACHMENT_LOAD_OP_CLEAR,
                .storeOp          = VK_ATTACHMENT_STORE_OP_STORE,
                .clearValue.color = {{0.0f, 0.0f, 0.0f, 1.0f}},
            };

            VkRenderingAttachmentInfo colors[2] = {color, bloom_src};

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
                .colorAttachmentCount = 2,
                .pColorAttachments    = colors,
                .pDepthAttachment     = &depth,
            };

            GPU_SCOPE(frame_prof, cmd, "MAIN", VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT)
            {
                vkCmdBeginRendering(cmd, &rendering);
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g_render_pipelines.pipelines[pipelines.ground]);
                vk_cmd_set_viewport_scissor(cmd, renderer.swapchain.extent);
                vkCmdDraw(cmd, 3, 1, 0, 0);
                draw_gltf_model(cmd, &player.model);
                for(uint32_t i = 0; i < obstacle_count; ++i)
                    draw_gltf_model(cmd, &obstacles[i].instance.model);

                if(collision_debug)
                {
                    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g_render_pipelines.pipelines[pipelines.triangle_wireframe]);
                    vk_cmd_set_viewport_scissor(cmd, renderer.swapchain.extent);

                    vec4 player_color = {0.2f, 0.7f, 1.0f, 1.0f};
                    vec4 sweep_color  = {1.0f, 1.0f, 1.0f, 1.0f};
                    vec4 hit_color    = {1.0f, 0.2f, 0.2f, 1.0f};
                    vec4 obs_color    = {0.2f, 1.0f, 0.4f, 1.0f};
                    vec4 expand_color = {1.0f, 0.85f, 0.2f, 1.0f};

                    debug_draw_aabb(cmd, player_pos, player_half, player_color);

                    for(uint32_t i = 0; i < obstacle_count; ++i)
                    {
                        ObstacleInstance* obs = &obstacles[i];
                        vec4 color;
                        if(hit_obstacle && (int)i == hit_obstacle_index)
                            glm_vec4_copy(hit_color, color);
                        else
                            glm_vec4_copy(obs_color, color);
                        debug_draw_aabb(cmd, obs->instance.position, obs->half_extents, color);

                        vec3 expanded_half = {
                            obs->half_extents[0] + player_half[0],
                            obs->half_extents[1] + player_half[1],
                            obs->half_extents[2] + player_half[2],
                        };
                        debug_draw_aabb(cmd, obs->instance.position, expanded_half, expand_color);
                    }

                    vec3 sweep_positions[2];
                    uint32_t sweep_indices[2] = {0, 1};
                    glm_vec3_copy(prev_player_pos, sweep_positions[0]);
                    glm_vec3_copy(player_pos, sweep_positions[1]);
                    debug_draw_line_list(cmd, sweep_positions, 2, sweep_indices, 2, sweep_color);
                }
                vkCmdEndRendering(cmd);
            }

            pass_toon_outline();
            pass_analytic_fog();
            post_pass();
            pass_smaa();
            pass_ldr_to_swapchain();

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
                igText("Score (distance): %.1f", score_distance);
                igText("Speed: %.2f", fabsf(player_vel_z));

                igSeparator();
                igText("Collision Debug");
                igCheckbox("Show collision volumes", &collision_debug);

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

                igSeparator();
                igText("Post FX");

                bool dof_enabled = g_postfx_settings.dof_enabled != 0;
                if(igCheckbox("DoF Enabled", &dof_enabled))
                    g_postfx_settings.dof_enabled = dof_enabled ? 1u : 0u;

                bool fog_enabled = g_postfx_settings.fog_enabled != 0;
                if(igCheckbox("Analytic Fog Enabled", &fog_enabled))
                    g_postfx_settings.fog_enabled = fog_enabled ? 1u : 0u;

                igSliderFloat("Exposure", &g_postfx_settings.exposure, -4.0f, 4.0f, "%.2f", 0);
                igSliderFloat("Bloom Intensity", &g_postfx_settings.bloom_intensity, 0.0f, 2.5f, "%.2f", 0);

                igSliderFloat("DoF Focus Point", &g_postfx_settings.dof_focus_point, 0.01f, 200.0f, "%.2f", 0);
                igSliderFloat("DoF Focus Scale", &g_postfx_settings.dof_focus_scale, 0.1f, 32.0f, "%.2f", 0);
                igSliderFloat("DoF Far Plane", &g_postfx_settings.dof_far_plane, 10.0f, 2000.0f, "%.1f", 0);
                igSliderFloat("DoF Max Blur", &g_postfx_settings.dof_max_blur_size, 1.0f, 40.0f, "%.2f", 0);
                igSliderFloat("DoF Radius Scale", &g_postfx_settings.dof_rad_scale, 0.05f, 2.0f, "%.3f", 0);

                igSliderFloat("Fog Density", &g_postfx_settings.fog_density, 0.0f, 4.0f, "%.2f", 0);
                igSliderFloat("Fog Radius", &g_postfx_settings.fog_radius, 0.5f, 64.0f, "%.2f", 0);
                igSliderFloat("Fog Noise", &g_postfx_settings.fog_noise, 0.0f, 0.5f, "%.3f", 0);
                igSliderFloat3("Fog Color", g_postfx_settings.fog_color, 0.0f, 1.0f, "%.3f", 0);
                igSliderFloat3("Fog Center", g_postfx_settings.fog_center, -64.0f, 64.0f, "%.2f", 0);

                igEnd();

                if(igBegin("Memory Budget", &memory_window_open, 0))
                {
                    if(memory_window_open)
                    {
                        if(memory_budget_last_update < 0.0 || now_seconds - memory_budget_last_update >= memory_budget_interval_s)
                        {
                            vmaGetHeapBudgets(renderer.vmaallocator, memory_budgets);
                            memory_budget_last_update = now_seconds;
                        }

                        VkDeviceSize total_usage       = 0;
                        VkDeviceSize total_budget      = 0;
                        VkDeviceSize total_alloc_bytes = 0;
                        uint32_t     total_alloc_count = 0;

                        for(uint32_t i = 0; i < memory_heap_count; ++i)
                        {
                            total_usage += memory_budgets[i].usage;
                            total_budget += memory_budgets[i].budget;
                            total_alloc_bytes += memory_budgets[i].statistics.allocationBytes;
                            total_alloc_count += memory_budgets[i].statistics.allocationCount;
                        }

                        igText("Total: usage %.2f MiB / budget %.2f MiB", bytes_to_mib(total_usage), bytes_to_mib(total_budget));
                        igText("Total: alloc %.2f MiB, allocs %u", bytes_to_mib(total_alloc_bytes), total_alloc_count);
                        igSeparator();

                        for(uint32_t i = 0; i < memory_heap_count; ++i)
                        {
                            VmaBudget* b            = &memory_budgets[i];
                            double     percent_used = 0.0;
                            if(b->budget > 0)
                                percent_used = (double)b->usage * 100.0 / (double)b->budget;
                            igText("Heap %u", i);
                            igText("  usage %.2f MiB / budget %.2f MiB (%.1f%%)", bytes_to_mib(b->usage),
                                   bytes_to_mib(b->budget), percent_used);
                            igText("  alloc %.2f MiB, allocs %u", bytes_to_mib(b->statistics.allocationBytes),
                                   b->statistics.allocationCount);
                            igText("  blocks %.2f MiB, blocks %u", bytes_to_mib(b->statistics.blockBytes), b->statistics.blockCount);
                        }
                    }
                }
                igEnd();

                igRender();
            }
            TracyCZoneEnd(imgui_zone);

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

    destroy_obstacles(obstacles, obstacle_count);
    gltf_gpu_model_destroy(&player.model);
    free_glb_paths(glb_paths, glb_count);
    audio_destroy(audio);
    renderer_destroy(&renderer);
    return 0;
}
