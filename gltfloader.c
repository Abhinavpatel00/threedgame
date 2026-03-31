#include "gltfloader.h"

#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "external/cglm/include/cglm/cglm.h"
#include "external/cgltf/cgltf.h"

static GLTFInterpolation to_interp(cgltf_interpolation_type interpolation)
{
    switch(interpolation)
    {
        case cgltf_interpolation_type_step:
            return GLTF_INTERPOLATION_STEP;
        case cgltf_interpolation_type_cubic_spline:
            return GLTF_INTERPOLATION_CUBICSPLINE;
        case cgltf_interpolation_type_linear:
        default:
            return GLTF_INTERPOLATION_LINEAR;
    }
}

static float clamp01(float x)
{
    if(x < 0.0f)
        return 0.0f;
    if(x > 1.0f)
        return 1.0f;
    return x;
}

static GLTFAnimationPath to_anim_path(cgltf_animation_path_type path)
{
    switch(path)
    {
        case cgltf_animation_path_type_rotation:
            return GLTF_ANIMATION_PATH_ROTATION;
        case cgltf_animation_path_type_scale:
            return GLTF_ANIMATION_PATH_SCALE;
        case cgltf_animation_path_type_weights:
            return GLTF_ANIMATION_PATH_WEIGHTS;
        case cgltf_animation_path_type_translation:
        default:
            return GLTF_ANIMATION_PATH_TRANSLATION;
    }
}

static void build_trs_matrix(const vec3 t, const versor r, const vec3 s, mat4 out)
{
    vec3 tt = {t[0], t[1], t[2]};
    versor rr = {r[0], r[1], r[2], r[3]};
    vec3 ss = {s[0], s[1], s[2]};
    mat4 tm;
    mat4 rm;
    mat4 sm;
    mat4 tr;

    glm_translate_make(tm, tt);
    glm_quat_mat4(rr, rm);
    glm_scale_make(sm, ss);
    glm_mat4_mul(tm, rm, tr);
    glm_mat4_mul(tr, sm, out);
}

static int animation_find_keyframe(const float* times, u32 count, float t)
{
    if(count < 2)
        return 0;

    if(t <= times[0])
        return 0;
    if(t >= times[count - 1])
        return (int)count - 2;

    for(u32 i = 0; i + 1 < count; ++i)
    {
        if(t >= times[i] && t <= times[i + 1])
            return (int)i;
    }

    return (int)count - 2;
}

static void animation_sample_vec(const GLTFAnimationSampler* sampler, float t, float* out_values)
{
    if(!sampler || sampler->input_count == 0 || !sampler->input_times || !sampler->output_values)
        return;

    if(sampler->input_count == 1)
    {
        for(u32 c = 0; c < sampler->output_stride; ++c)
            out_values[c] = sampler->output_values[c];
        return;
    }

    int i0 = animation_find_keyframe(sampler->input_times, sampler->input_count, t);
    int i1 = i0 + 1;

    float t0 = sampler->input_times[i0];
    float t1 = sampler->input_times[i1];
    float alpha = (t1 > t0) ? (t - t0) / (t1 - t0) : 0.0f;
    alpha = clamp01(alpha);

    const float* v0 = &sampler->output_values[(size_t)i0 * sampler->output_stride];
    const float* v1 = &sampler->output_values[(size_t)i1 * sampler->output_stride];

    if(sampler->interpolation == GLTF_INTERPOLATION_STEP)
    {
        for(u32 c = 0; c < sampler->output_stride; ++c)
            out_values[c] = v0[c];
        return;
    }

    if(sampler->output_stride == 4)
    {
        versor q0 = {v0[0], v0[1], v0[2], v0[3]};
        versor q1 = {v1[0], v1[1], v1[2], v1[3]};
        versor q_out;
        glm_quat_slerp(q0, q1, alpha, q_out);
        out_values[0] = q_out[0];
        out_values[1] = q_out[1];
        out_values[2] = q_out[2];
        out_values[3] = q_out[3];
        return;
    }

    for(u32 c = 0; c < sampler->output_stride; ++c)
        out_values[c] = v0[c] + (v1[c] - v0[c]) * alpha;
}

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
    gltf->animation_count = (u32)data->animations_count;

    gltf->handle = data;
    strncpy(gltf->source_path, path, sizeof(gltf->source_path) - 1);

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
        gltf->mesh_node_indices = (u32*)malloc((size_t)gltf->mesh_count * sizeof(u32));
    }
    if(gltf->node_count > 0)
        gltf->nodes = (GLTFNode*)calloc(gltf->node_count, sizeof(GLTFNode));
    if(gltf->skin_count > 0)
        gltf->skins = (GLTFSkin*)calloc(gltf->skin_count, sizeof(GLTFSkin));
    if(gltf->animation_count > 0)
        gltf->animations = (GLTFAnimationClip*)calloc(gltf->animation_count, sizeof(GLTFAnimationClip));

    if((gltf->material_count > 0 && !gltf->materials) ||
       (gltf->sampler_count > 0 && !gltf->samplers) ||
       (gltf->mesh_count > 0 && (!gltf->meshes || !gltf->material_indices || !gltf->mesh_node_indices)) ||
       (gltf->node_count > 0 && !gltf->nodes) ||
       (gltf->skin_count > 0 && !gltf->skins) ||
       (gltf->animation_count > 0 && !gltf->animations))
    {
        freeGltf(gltf);
        return false;
    }

    for(u32 i = 0; i < gltf->mesh_count; ++i)
        gltf->mesh_node_indices[i] = UINT_MAX;

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

            for(u32 primitive = 0; primitive < dst->mesh_count; ++primitive)
            {
                u32 global_primitive = dst->mesh_index + primitive;
                if(global_primitive < gltf->mesh_count && gltf->mesh_node_indices[global_primitive] == UINT_MAX)
                    gltf->mesh_node_indices[global_primitive] = n;
            }
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
            memcpy(dst->matrix, src->matrix, sizeof(mat4));
            dst->has_matrix = true;
        }
        else
        {
            for(int r = 0; r < 4; ++r)
                for(int c = 0; c < 4; ++c)
                    dst->matrix[r][c] = (r == c) ? 1.0f : 0.0f;
            dst->has_matrix = false;
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

    for(u32 i = 0; i < gltf->animation_count; ++i)
    {
        const cgltf_animation* src_anim = &data->animations[i];
        GLTFAnimationClip* dst_anim     = &gltf->animations[i];

        if(src_anim->name)
            strncpy(dst_anim->name, src_anim->name, GLTF_NAME_MAX_LENGTH - 1);

        dst_anim->sampler_count = (u32)src_anim->samplers_count;
        dst_anim->channel_count = (u32)src_anim->channels_count;

        if(dst_anim->sampler_count > 0)
            dst_anim->samplers = (GLTFAnimationSampler*)calloc(dst_anim->sampler_count, sizeof(GLTFAnimationSampler));
        if(dst_anim->channel_count > 0)
            dst_anim->channels = (GLTFAnimationChannel*)calloc(dst_anim->channel_count, sizeof(GLTFAnimationChannel));

        if((dst_anim->sampler_count > 0 && !dst_anim->samplers) || (dst_anim->channel_count > 0 && !dst_anim->channels))
        {
            freeGltf(gltf);
            return false;
        }

        dst_anim->duration = 0.0f;

        for(u32 s = 0; s < dst_anim->sampler_count; ++s)
        {
            const cgltf_animation_sampler* src_sampler = &src_anim->samplers[s];
            GLTFAnimationSampler* dst_sampler          = &dst_anim->samplers[s];

            if(!src_sampler->input || !src_sampler->output)
                continue;

            dst_sampler->input_count   = (u32)src_sampler->input->count;
            dst_sampler->interpolation = to_interp(src_sampler->interpolation);

            if(src_sampler->output->type == cgltf_type_vec4)
                dst_sampler->output_stride = 4;
            else
                dst_sampler->output_stride = 3;

            if(dst_sampler->input_count == 0)
                continue;

            dst_sampler->input_times = (float*)malloc((size_t)dst_sampler->input_count * sizeof(float));
            dst_sampler->output_values = (float*)malloc((size_t)dst_sampler->input_count * dst_sampler->output_stride * sizeof(float));

            if(!dst_sampler->input_times || !dst_sampler->output_values)
            {
                freeGltf(gltf);
                return false;
            }

            for(u32 k = 0; k < dst_sampler->input_count; ++k)
            {
                float key_time = 0.0f;
                cgltf_accessor_read_float(src_sampler->input, k, &key_time, 1);
                dst_sampler->input_times[k] = key_time;
                if(key_time > dst_anim->duration)
                    dst_anim->duration = key_time;
            }

            for(u32 k = 0; k < dst_sampler->input_count; ++k)
            {
                float tmp[4] = {0.0f, 0.0f, 0.0f, 1.0f};
                cgltf_accessor_read_float(src_sampler->output, k, tmp, (int)dst_sampler->output_stride);
                for(u32 c = 0; c < dst_sampler->output_stride; ++c)
                    dst_sampler->output_values[(size_t)k * dst_sampler->output_stride + c] = tmp[c];
            }
        }

        for(u32 c = 0; c < dst_anim->channel_count; ++c)
        {
            const cgltf_animation_channel* src_channel = &src_anim->channels[c];
            GLTFAnimationChannel* dst_channel          = &dst_anim->channels[c];

            dst_channel->node_index = src_channel->target_node ? (u32)(src_channel->target_node - data->nodes) : UINT_MAX;
            dst_channel->sampler_index = src_channel->sampler ? (u32)(src_channel->sampler - src_anim->samplers) : UINT_MAX;
            dst_channel->path = to_anim_path(src_channel->target_path);
        }
    }

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

    if(gltf->animations)
    {
        for(u32 i = 0; i < gltf->animation_count; ++i)
        {
            GLTFAnimationClip* clip = &gltf->animations[i];
            if(clip->samplers)
            {
                for(u32 s = 0; s < clip->sampler_count; ++s)
                {
                    free(clip->samplers[s].input_times);
                    free(clip->samplers[s].output_values);
                }
            }
            free(clip->samplers);
            free(clip->channels);
        }
    }

    free(gltf->image_usage);
    free(gltf->materials);
    free(gltf->material_indices);
    free(gltf->samplers);
    free(gltf->meshes);
    free(gltf->mesh_node_indices);
    free(gltf->nodes);
    free(gltf->skins);
    free(gltf->animations);
    if(gltf->handle)
        cgltf_free(gltf->handle);
    free(gltf);
}

void gltf_apply_animation(const GLTFContainer* gltf, u32 animation_index, float time_seconds, mat4* out_node_world_matrices)
{
    if(!gltf || !out_node_world_matrices || gltf->node_count == 0)
        return;

    vec3* node_t = (vec3*)malloc((size_t)gltf->node_count * sizeof(vec3));
    versor* node_r = (versor*)malloc((size_t)gltf->node_count * sizeof(versor));
    vec3* node_s = (vec3*)malloc((size_t)gltf->node_count * sizeof(vec3));
    bool* computed = (bool*)calloc((size_t)gltf->node_count, sizeof(bool));
    if(!node_t || !node_r || !node_s || !computed)
    {
        free(node_t);
        free(node_r);
        free(node_s);
        free(computed);
        return;
    }

    for(u32 i = 0; i < gltf->node_count; ++i)
    {
        glm_vec3_copy(gltf->nodes[i].translation, node_t[i]);
        glm_vec4_copy(gltf->nodes[i].rotation, node_r[i]);
        glm_vec3_copy(gltf->nodes[i].scale, node_s[i]);
    }

    if(animation_index < gltf->animation_count)
    {
        const GLTFAnimationClip* clip = &gltf->animations[animation_index];
        float t = time_seconds;
        if(clip->duration > 0.0f)
            t = fmodf(time_seconds, clip->duration);

        for(u32 c = 0; c < clip->channel_count; ++c)
        {
            const GLTFAnimationChannel* channel = &clip->channels[c];
            if(channel->node_index == UINT_MAX || channel->node_index >= gltf->node_count)
                continue;
            if(channel->sampler_index == UINT_MAX || channel->sampler_index >= clip->sampler_count)
                continue;

            const GLTFAnimationSampler* sampler = &clip->samplers[channel->sampler_index];
            float sampled[4] = {0.0f, 0.0f, 0.0f, 1.0f};
            animation_sample_vec(sampler, t, sampled);

            switch(channel->path)
            {
                case GLTF_ANIMATION_PATH_TRANSLATION:
                    node_t[channel->node_index][0] = sampled[0];
                    node_t[channel->node_index][1] = sampled[1];
                    node_t[channel->node_index][2] = sampled[2];
                    break;
                case GLTF_ANIMATION_PATH_ROTATION:
                    node_r[channel->node_index][0] = sampled[0];
                    node_r[channel->node_index][1] = sampled[1];
                    node_r[channel->node_index][2] = sampled[2];
                    node_r[channel->node_index][3] = sampled[3];
                    glm_quat_normalize(node_r[channel->node_index]);
                    break;
                case GLTF_ANIMATION_PATH_SCALE:
                    node_s[channel->node_index][0] = sampled[0];
                    node_s[channel->node_index][1] = sampled[1];
                    node_s[channel->node_index][2] = sampled[2];
                    break;
                case GLTF_ANIMATION_PATH_WEIGHTS:
                default:
                    break;
            }
        }
    }

    for(u32 i = 0; i < gltf->node_count; ++i)
    {
        bool progress = false;
        for(u32 n = 0; n < gltf->node_count; ++n)
        {
            if(computed[n])
                continue;

            u32 parent = gltf->nodes[n].parent_index;
            if(parent != UINT_MAX && parent < gltf->node_count && !computed[parent])
                continue;

            mat4 local;
            if(gltf->nodes[n].has_matrix)
                glm_mat4_copy(gltf->nodes[n].matrix, local);
            else
                build_trs_matrix(node_t[n], node_r[n], node_s[n], local);

            if(parent != UINT_MAX && parent < gltf->node_count)
                glm_mat4_mul(out_node_world_matrices[parent], local, out_node_world_matrices[n]);
            else
                glm_mat4_copy(local, out_node_world_matrices[n]);

            computed[n] = true;
            progress = true;
        }

        if(!progress)
            break;
    }

    for(u32 i = 0; i < gltf->node_count; ++i)
    {
        if(computed[i])
            continue;

        if(gltf->nodes[i].has_matrix)
            glm_mat4_copy(gltf->nodes[i].matrix, out_node_world_matrices[i]);
        else
            build_trs_matrix(node_t[i], node_r[i], node_s[i], out_node_world_matrices[i]);
    }

    free(node_t);
    free(node_r);
    free(node_s);
    free(computed);
}
