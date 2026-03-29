#include "gltfloader.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "external/cgltf/cgltf.h"

static GLTFAttributeType to_gltf_attr(cgltf_attribute_type type)
{
    switch(type)
    {
        case cgltf_attribute_type_position:
            return GLTF_ATTRIBUTE_TYPE_POSITION;
        case cgltf_attribute_type_normal:
            return GLTF_ATTRIBUTE_TYPE_NORMAL;
        case cgltf_attribute_type_tangent:
            return GLTF_ATTRIBUTE_TYPE_TANGENT;
        case cgltf_attribute_type_texcoord:
            return GLTF_ATTRIBUTE_TYPE_TEXCOORD;
        case cgltf_attribute_type_color:
            return GLTF_ATTRIBUTE_TYPE_COLOR;
        case cgltf_attribute_type_joints:
            return GLTF_ATTRIBUTE_TYPE_JOINTS;
        case cgltf_attribute_type_weights:
            return GLTF_ATTRIBUTE_TYPE_WEIGHTS;
        default:
            return GLTF_ATTRIBUTE_TYPE_INVALID;
    }
}

static void gltf_get_texture_view(const cgltf_data* scene, GLTFTextureView* out_view, const cgltf_texture_view* src)
{
    memset(out_view, 0, sizeof(*out_view));
    out_view->texture_index = SIZE_MAX;
    out_view->sample_index  = SIZE_MAX;
    out_view->scale         = src ? src->scale : 1.0f;
    out_view->transform.scale[0] = 1.0f;
    out_view->transform.scale[1] = 1.0f;

    if(!src || !src->texture)
        return;

    if(src->texture->name)
        strncpy(out_view->name, src->texture->name, GLTF_NAME_MAX_LENGTH - 1);

    if(src->texture->image)
        out_view->texture_index = (size_t)(src->texture->image - scene->images);

    if(src->texture->sampler)
        out_view->sample_index = (size_t)(src->texture->sampler - scene->samplers);

    out_view->tex_coord_set = src->texcoord;

    if(src->has_transform)
    {
        out_view->transform.offset[0] = src->transform.offset[0];
        out_view->transform.offset[1] = src->transform.offset[1];
        out_view->transform.scale[0]  = src->transform.scale[0];
        out_view->transform.scale[1]  = src->transform.scale[1];
        out_view->transform.rotation  = src->transform.rotation;
    }
}

static void add_image_usage(const cgltf_data* scene, const cgltf_texture_view* tex_view, GLTFImageUsage flag, GLTFImageUsage* usage)
{
    if(!scene || !tex_view || !tex_view->texture || !tex_view->texture->image || !usage)
        return;

    size_t image_id = (size_t)(tex_view->texture->image - scene->images);
    usage[image_id] |= flag;
}

static VkSamplerAddressMode gltf_convert_wrap_mode(cgltf_wrap_mode wrap_mode)
{
    switch(wrap_mode)
    {
        case cgltf_wrap_mode_clamp_to_edge:
            return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        case cgltf_wrap_mode_mirrored_repeat:
            return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
        case cgltf_wrap_mode_repeat:
        default:
            return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    }
}

static VkFilter gltf_convert_filter(cgltf_filter_type filter)
{
    switch(filter)
    {
        case cgltf_filter_type_nearest:
        case cgltf_filter_type_nearest_mipmap_nearest:
        case cgltf_filter_type_nearest_mipmap_linear:
            return VK_FILTER_NEAREST;

        case cgltf_filter_type_linear:
        case cgltf_filter_type_linear_mipmap_nearest:
        case cgltf_filter_type_linear_mipmap_linear:
        default:
            return VK_FILTER_LINEAR;
    }
}

static VkSamplerMipmapMode gltf_convert_mipmap_mode(cgltf_filter_type filter)
{
    switch(filter)
    {
        case cgltf_filter_type_nearest_mipmap_nearest:
        case cgltf_filter_type_linear_mipmap_nearest:
            return VK_SAMPLER_MIPMAP_MODE_NEAREST;

        case cgltf_filter_type_nearest_mipmap_linear:
        case cgltf_filter_type_linear_mipmap_linear:
            return VK_SAMPLER_MIPMAP_MODE_LINEAR;

        default:
            return VK_SAMPLER_MIPMAP_MODE_NEAREST;
    }
}

static SamplerDesc convert_sampler(const cgltf_sampler* src)
{
    SamplerDesc sampler = {0};

    if(!src)
    {
        sampler.min_filter   = VK_FILTER_LINEAR;
        sampler.mag_filter   = VK_FILTER_LINEAR;
        sampler.mip_map_mode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        sampler.address_U    = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        sampler.address_V    = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        sampler.address_W    = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        sampler.min_lod      = 0.0f;
        sampler.max_lod      = VK_LOD_CLAMP_NONE;
        sampler.max_anisotropy = 1.0f;
        sampler.compare_func = VK_COMPARE_OP_ALWAYS;
        sampler.model        = VK_NULL_HANDLE;
        sampler.ycbcr_range  = VK_SAMPLER_YCBCR_RANGE_ITU_FULL;
        sampler.chroma_offset_X = VK_CHROMA_LOCATION_COSITED_EVEN;
        sampler.chroma_offset_Y = VK_CHROMA_LOCATION_COSITED_EVEN;
        sampler.chroma_filter   = VK_FILTER_NEAREST;
        return sampler;
    }

    sampler.min_filter   = gltf_convert_filter(src->min_filter);
    sampler.mag_filter   = gltf_convert_filter(src->mag_filter);
    sampler.mip_map_mode = gltf_convert_mipmap_mode(src->min_filter);
    sampler.address_U    = gltf_convert_wrap_mode(src->wrap_s);
    sampler.address_V    = gltf_convert_wrap_mode(src->wrap_t);
    sampler.address_W    = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sampler.min_lod      = 0.0f;
    sampler.max_lod      = VK_LOD_CLAMP_NONE;
    sampler.max_anisotropy = 1.0f;
    sampler.compare_func = VK_COMPARE_OP_ALWAYS;
    sampler.model        = VK_NULL_HANDLE;
    sampler.ycbcr_range  = VK_SAMPLER_YCBCR_RANGE_ITU_FULL;
    sampler.chroma_offset_X = VK_CHROMA_LOCATION_COSITED_EVEN;
    sampler.chroma_offset_Y = VK_CHROMA_LOCATION_COSITED_EVEN;
    sampler.chroma_filter   = VK_FILTER_NEAREST;

    return sampler;
}

static bool unpack_vec(const cgltf_accessor* accessor, float* dst, uint32_t count, int components)
{
    for(uint32_t i = 0; i < count; ++i)
    {
        float values[4] = {0.0f, 0.0f, 0.0f, 1.0f};
        if(!cgltf_accessor_read_float(accessor, i, values, components))
            return false;

        for(int c = 0; c < components; ++c)
            dst[(size_t)i * (size_t)components + (size_t)c] = values[c];
    }
    return true;
}

static bool load_primitive_geometry(const cgltf_primitive* prim, GLTFMesh* out_mesh, GLTFFlags flags)
{
    memset(out_mesh, 0, sizeof(*out_mesh));

    const cgltf_accessor* pos_accessor = NULL;
    const cgltf_accessor* idx_accessor = prim->indices;

    for(size_t i = 0; i < prim->attributes_count; ++i)
    {
        const cgltf_attribute* attr = &prim->attributes[i];
        if(attr->type == cgltf_attribute_type_position)
        {
            pos_accessor = attr->data;
            break;
        }
    }

    if(!pos_accessor || pos_accessor->count == 0)
        return false;

    out_mesh->vertex_count = (u32)pos_accessor->count;
    out_mesh->index_count  = idx_accessor ? (u32)idx_accessor->count : out_mesh->vertex_count;

    if(!(flags & GLTF_FLAG_LOAD_VERTICES))
        return true;

    out_mesh->attributes[GLTF_ATTRIBUTE_TYPE_POSITION] = malloc((size_t)out_mesh->vertex_count * 3u * sizeof(float));
    out_mesh->index = (u32*)malloc((size_t)out_mesh->index_count * sizeof(u32));
    if(!out_mesh->attributes[GLTF_ATTRIBUTE_TYPE_POSITION] || !out_mesh->index)
        return false;

    if(!unpack_vec(pos_accessor, (float*)out_mesh->attributes[GLTF_ATTRIBUTE_TYPE_POSITION], out_mesh->vertex_count, 3))
        return false;

    if(idx_accessor)
    {
        for(uint32_t i = 0; i < out_mesh->index_count; ++i)
            out_mesh->index[i] = (u32)cgltf_accessor_read_index(idx_accessor, i);
    }
    else
    {
        for(uint32_t i = 0; i < out_mesh->index_count; ++i)
            out_mesh->index[i] = i;
    }

    for(size_t i = 0; i < prim->attributes_count; ++i)
    {
        const cgltf_attribute* attr = &prim->attributes[i];
        GLTFAttributeType dst_type  = to_gltf_attr(attr->type);
        if(dst_type <= GLTF_ATTRIBUTE_TYPE_POSITION || dst_type >= GLTF_ATTRIBUTE_TYPE_COUNT)
            continue;

        if(dst_type == GLTF_ATTRIBUTE_TYPE_TEXCOORD && attr->index != 0)
            continue;
        if((dst_type == GLTF_ATTRIBUTE_TYPE_COLOR || dst_type == GLTF_ATTRIBUTE_TYPE_JOINTS || dst_type == GLTF_ATTRIBUTE_TYPE_WEIGHTS) && attr->index != 0)
            continue;

        int components = 4;
        if(dst_type == GLTF_ATTRIBUTE_TYPE_NORMAL)
            components = 3;
        else if(dst_type == GLTF_ATTRIBUTE_TYPE_TEXCOORD)
            components = 2;

        size_t attr_bytes = (size_t)out_mesh->vertex_count * (size_t)components * sizeof(float);
        out_mesh->attributes[dst_type] = malloc(attr_bytes);
        if(!out_mesh->attributes[dst_type])
            return false;

        if(!unpack_vec(attr->data, (float*)out_mesh->attributes[dst_type], out_mesh->vertex_count, components))
            return false;
    }

    if(flags & GLTF_FLAG_CALCULATE_BOUNDS)
    {
        float* positions = (float*)out_mesh->attributes[GLTF_ATTRIBUTE_TYPE_POSITION];
        out_mesh->min[0] = out_mesh->max[0] = positions[0];
        out_mesh->min[1] = out_mesh->max[1] = positions[1];
        out_mesh->min[2] = out_mesh->max[2] = positions[2];

        for(uint32_t i = 1; i < out_mesh->vertex_count; ++i)
        {
            float x = positions[i * 3u + 0u];
            float y = positions[i * 3u + 1u];
            float z = positions[i * 3u + 2u];

            if(x < out_mesh->min[0]) out_mesh->min[0] = x;
            if(y < out_mesh->min[1]) out_mesh->min[1] = y;
            if(z < out_mesh->min[2]) out_mesh->min[2] = z;

            if(x > out_mesh->max[0]) out_mesh->max[0] = x;
            if(y > out_mesh->max[1]) out_mesh->max[1] = y;
            if(z > out_mesh->max[2]) out_mesh->max[2] = z;
        }
    }

    return true;
}

static void free_mesh(GLTFMesh* mesh)
{
    if(!mesh)
        return;

    free(mesh->index);
    mesh->index = NULL;

    for(int i = 0; i < GLTF_ATTRIBUTE_TYPE_COUNT; ++i)
    {
        free(mesh->attributes[i]);
        mesh->attributes[i] = NULL;
    }
}

bool loadGltf(const char* path, GLTFFlags flags, GLTFContainer** outGltf)
{
    if(!path || !outGltf)
        return false;

    *outGltf = NULL;

    cgltf_options options = {0};
    cgltf_data* data      = NULL;

    if(cgltf_parse_file(&options, path, &data) != cgltf_result_success)
        return false;

    if(cgltf_load_buffers(&options, data, path) != cgltf_result_success)
    {
        cgltf_free(data);
        return false;
    }

    if(cgltf_validate(data) != cgltf_result_success)
    {
        cgltf_free(data);
        return false;
    }

    GLTFContainer* gltf = (GLTFContainer*)calloc(1, sizeof(GLTFContainer));
    if(!gltf)
    {
        cgltf_free(data);
        return false;
    }

    gltf->material_count = (u32)data->materials_count;
    gltf->sampler_count  = (u32)data->samplers_count;
    gltf->node_count     = (u32)data->nodes_count;
    gltf->skin_count     = (u32)data->skins_count;

    size_t primitive_count = 0;
    for(size_t m = 0; m < data->meshes_count; ++m)
        primitive_count += data->meshes[m].primitives_count;
    gltf->mesh_count = (u32)primitive_count;

    if(data->images_count > 0)
        gltf->image_usage = (GLTFImageUsage*)calloc(data->images_count, sizeof(GLTFImageUsage));

    if(gltf->material_count > 0)
        gltf->materials = (GLTFMaterial*)calloc(gltf->material_count, sizeof(GLTFMaterial));
    if(gltf->sampler_count > 0)
        gltf->samplers = (SamplerDesc*)calloc(gltf->sampler_count, sizeof(SamplerDesc));
    if(gltf->mesh_count > 0)
    {
        gltf->meshes = (GLTFMesh*)calloc(gltf->mesh_count, sizeof(GLTFMesh));
        gltf->material_indices = (u32*)malloc((size_t)gltf->mesh_count * sizeof(u32));
    }
    if(gltf->node_count > 0)
        gltf->nodes = (GLTFNode*)calloc(gltf->node_count, sizeof(GLTFNode));
    if(gltf->skin_count > 0)
        gltf->skins = (GLTFSkin*)calloc(gltf->skin_count, sizeof(GLTFSkin));

    if((gltf->material_count > 0 && !gltf->materials) ||
       (gltf->sampler_count > 0 && !gltf->samplers) ||
       (gltf->mesh_count > 0 && (!gltf->meshes || !gltf->material_indices)) ||
       (gltf->node_count > 0 && !gltf->nodes) ||
       (gltf->skin_count > 0 && !gltf->skins))
    {
        freeGltf(gltf);
        cgltf_free(data);
        return false;
    }

    u32 draw_index = 0;
    for(size_t m = 0; m < data->meshes_count; ++m)
    {
        const cgltf_mesh* mesh = &data->meshes[m];

        for(size_t p = 0; p < mesh->primitives_count; ++p)
        {
            const cgltf_primitive* prim = &mesh->primitives[p];
            if(!load_primitive_geometry(prim, &gltf->meshes[draw_index], flags))
            {
                freeGltf(gltf);
                cgltf_free(data);
                return false;
            }

            gltf->meshes[draw_index].start_index = gltf->index_count;
            gltf->index_count += gltf->meshes[draw_index].index_count;
            gltf->vertex_count += gltf->meshes[draw_index].vertex_count;

            gltf->material_indices[draw_index] = prim->material ? (u32)(prim->material - data->materials) : UINT_MAX;
            ++draw_index;
        }
    }

    for(u32 n = 0; n < gltf->node_count; ++n)
    {
        const cgltf_node* src = &data->nodes[n];
        GLTFNode* dst         = &gltf->nodes[n];

        if(src->name)
            strncpy(dst->pName, src->name, GLTF_NAME_MAX_LENGTH - 1);

        dst->parent_index = src->parent ? (u32)(src->parent - data->nodes) : UINT_MAX;
        dst->child_count  = (u32)src->children_count;
        dst->mesh_index   = UINT_MAX;
        dst->mesh_count   = 0;
        dst->skin_index   = src->skin ? (u32)(src->skin - data->skins) : UINT_MAX;

        if(src->children_count > 0)
        {
            dst->child_indices = (u32*)malloc(src->children_count * sizeof(u32));
            if(!dst->child_indices)
            {
                freeGltf(gltf);
                cgltf_free(data);
                return false;
            }
            for(size_t i = 0; i < src->children_count; ++i)
                dst->child_indices[i] = (u32)(src->children[i] - data->nodes);
        }

        if(src->weights_count > 0 && src->weights)
        {
            dst->weights_count = (u32)src->weights_count;
            dst->weights = (float*)malloc(src->weights_count * sizeof(float));
            if(!dst->weights)
            {
                freeGltf(gltf);
                cgltf_free(data);
                return false;
            }
            for(size_t i = 0; i < src->weights_count; ++i)
                dst->weights[i] = src->weights[i];
        }

        if(src->mesh)
        {
            u32 mesh_base = 0;
            for(size_t m = 0; m < data->meshes_count; ++m)
            {
                if(&data->meshes[m] == src->mesh)
                    break;
                mesh_base += (u32)data->meshes[m].primitives_count;
            }
            dst->mesh_index = mesh_base;
            dst->mesh_count = (u32)src->mesh->primitives_count;
        }

        dst->translation[0] = src->has_translation ? src->translation[0] : 0.0f;
        dst->translation[1] = src->has_translation ? src->translation[1] : 0.0f;
        dst->translation[2] = src->has_translation ? src->translation[2] : 0.0f;

        dst->rotation[0] = src->has_rotation ? src->rotation[0] : 0.0f;
        dst->rotation[1] = src->has_rotation ? src->rotation[1] : 0.0f;
        dst->rotation[2] = src->has_rotation ? src->rotation[2] : 0.0f;
        dst->rotation[3] = src->has_rotation ? src->rotation[3] : 1.0f;

        dst->scale[0] = src->has_scale ? src->scale[0] : 1.0f;
        dst->scale[1] = src->has_scale ? src->scale[1] : 1.0f;
        dst->scale[2] = src->has_scale ? src->scale[2] : 1.0f;

        if(src->has_matrix)
        {
            for(int r = 0; r < 4; ++r)
                for(int c = 0; c < 4; ++c)
                    dst->matrix[r][c] = src->matrix[r * 4 + c];
        }
        else
        {
            for(int r = 0; r < 4; ++r)
                for(int c = 0; c < 4; ++c)
                    dst->matrix[r][c] = (r == c) ? 1.0f : 0.0f;
        }
    }

    for(u32 s = 0; s < gltf->skin_count; ++s)
    {
        const cgltf_skin* src = &data->skins[s];
        GLTFSkin* dst         = &gltf->skins[s];

        if(src->name)
            strncpy(dst->name, src->name, GLTF_NAME_MAX_LENGTH - 1);

        dst->skeleton_node_index = src->skeleton ? (u32)(src->skeleton - data->nodes) : UINT_MAX;
        dst->joint_count = (u32)src->joints_count;

        if(dst->joint_count == 0)
            continue;

        dst->joint_node_indices = (u32*)malloc((size_t)dst->joint_count * sizeof(u32));
        dst->inverse_bind_matrices = (mat4*)malloc((size_t)dst->joint_count * sizeof(mat4));
        if(!dst->joint_node_indices || !dst->inverse_bind_matrices)
        {
            freeGltf(gltf);
            cgltf_free(data);
            return false;
        }

        for(u32 i = 0; i < dst->joint_count; ++i)
            dst->joint_node_indices[i] = (u32)(src->joints[i] - data->nodes);

        if(src->inverse_bind_matrices)
        {
            cgltf_accessor_unpack_floats(src->inverse_bind_matrices, (float*)dst->inverse_bind_matrices, (size_t)dst->joint_count * 16u);
        }
        else
        {
            for(u32 i = 0; i < dst->joint_count; ++i)
            {
                for(int r = 0; r < 4; ++r)
                    for(int c = 0; c < 4; ++c)
                        dst->inverse_bind_matrices[i][r][c] = (r == c) ? 1.0f : 0.0f;
            }
        }
    }

    for(u32 i = 0; i < gltf->sampler_count; ++i)
        gltf->samplers[i] = convert_sampler(&data->samplers[i]);

    for(u32 i = 0; i < gltf->material_count; ++i)
    {
        const cgltf_material* src = &data->materials[i];
        GLTFMaterial* dst         = &gltf->materials[i];

        if(src->name)
            strncpy(dst->name, src->name, GLTF_NAME_MAX_LENGTH - 1);

        if(src->has_pbr_specular_glossiness)
        {
            dst->material_type = GLTF_MATERIAL_TYPE_SPECULAR_GLOSSINESS;
            gltf_get_texture_view(data, &dst->specular_glossiness.diffuse_texture, &src->pbr_specular_glossiness.diffuse_texture);
            gltf_get_texture_view(data, &dst->specular_glossiness.specular_glossiness_texture, &src->pbr_specular_glossiness.specular_glossiness_texture);
            add_image_usage(data, &src->pbr_specular_glossiness.diffuse_texture, GLTF_IMAGE_USAGE_DIFFUSE, gltf->image_usage);
            add_image_usage(data, &src->pbr_specular_glossiness.specular_glossiness_texture, GLTF_IMAGE_USAGE_SPECULAR_GLOSSINESS, gltf->image_usage);

            memcpy(dst->specular_glossiness.diffuse_factor, src->pbr_specular_glossiness.diffuse_factor, sizeof(vec4));
            memcpy(dst->specular_glossiness.specular_factor, src->pbr_specular_glossiness.specular_factor, sizeof(vec3));
            dst->specular_glossiness.glossiness_factor = src->pbr_specular_glossiness.glossiness_factor;
        }
        else
        {
            dst->material_type = GLTF_MATERIAL_TYPE_METALLIC_ROUGHNESS;
            gltf_get_texture_view(data, &dst->metallic_roughness.base_color_texture, &src->pbr_metallic_roughness.base_color_texture);
            gltf_get_texture_view(data, &dst->metallic_roughness.metallic_roughness_texture, &src->pbr_metallic_roughness.metallic_roughness_texture);
            add_image_usage(data, &src->pbr_metallic_roughness.base_color_texture, GLTF_IMAGE_USAGE_BASE_COLOR, gltf->image_usage);
            add_image_usage(data, &src->pbr_metallic_roughness.metallic_roughness_texture, GLTF_IMAGE_USAGE_METALLIC_ROUGHNESS, gltf->image_usage);

            memcpy(dst->metallic_roughness.base_color_factor, src->pbr_metallic_roughness.base_color_factor, sizeof(vec4));
            dst->metallic_roughness.metallic_factor  = src->pbr_metallic_roughness.metallic_factor;
            dst->metallic_roughness.roughness_factor = src->pbr_metallic_roughness.roughness_factor;
        }

        gltf_get_texture_view(data, &dst->normal_texture, &src->normal_texture);
        gltf_get_texture_view(data, &dst->occlusion_texture, &src->occlusion_texture);
        gltf_get_texture_view(data, &dst->emissive_texture, &src->emissive_texture);

        add_image_usage(data, &src->normal_texture, GLTF_IMAGE_USAGE_NORMAL, gltf->image_usage);
        add_image_usage(data, &src->occlusion_texture, GLTF_IMAGE_USAGE_OCCLUSION, gltf->image_usage);
        add_image_usage(data, &src->emissive_texture, GLTF_IMAGE_USAGE_EMISSIVE, gltf->image_usage);

        memcpy(dst->emissive_factor, src->emissive_factor, sizeof(vec3));

        switch(src->alpha_mode)
        {
            case cgltf_alpha_mode_mask:
                dst->alpha_mode = GLTF_MATERIAL_ALPHA_MODE_MASK;
                break;
            case cgltf_alpha_mode_blend:
                dst->alpha_mode = GLTF_MATERIAL_ALPHA_MODE_BLEND;
                break;
            case cgltf_alpha_mode_opaque:
            default:
                dst->alpha_mode = GLTF_MATERIAL_ALPHA_MODE_OPAQUE;
                break;
        }

        dst->alpha_cutoff = src->alpha_cutoff;
        dst->double_sided = src->double_sided;
        dst->unlit        = src->unlit;
    }

    cgltf_free(data);
    *outGltf = gltf;
    return true;
}

void freeGltf(GLTFContainer* gltf)
{
    if(!gltf)
        return;

    if(gltf->meshes)
    {
        for(u32 i = 0; i < gltf->mesh_count; ++i)
            free_mesh(&gltf->meshes[i]);
    }

    if(gltf->nodes)
    {
        for(u32 i = 0; i < gltf->node_count; ++i)
        {
            free(gltf->nodes[i].child_indices);
            free(gltf->nodes[i].weights);
        }
    }

    if(gltf->skins)
    {
        for(u32 i = 0; i < gltf->skin_count; ++i)
        {
            free(gltf->skins[i].joint_node_indices);
            free(gltf->skins[i].inverse_bind_matrices);
        }
    }

    free(gltf->image_usage);
    free(gltf->materials);
    free(gltf->material_indices);
    free(gltf->samplers);
    free(gltf->meshes);
    free(gltf->nodes);
    free(gltf->skins);
    free(gltf);
}
