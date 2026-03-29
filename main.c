#include "renderer.h"
#include "passes.h"
#include "gltfloader.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>

#define GLTF_MODEL_PATH "assets/cubepets/Models/GLB format/animal-beaver.glb"

PUSH_CONSTANT(GltfUberPush,
              VkDeviceAddress pos_ptr;
              VkDeviceAddress idx_ptr;
              VkDeviceAddress nrm_ptr;
              VkDeviceAddress uv_ptr;

              float model[4][4];
              float view_proj[4][4];

              vec4 base_color_factor;
              vec3 emissive_factor;
              float metallic_factor;
              float roughness_factor;

              uint32_t base_color_tex;
              uint32_t normal_tex;
              uint32_t mr_tex;
              uint32_t emissive_tex;
              uint32_t sampler_id;
              uint32_t flags;
              uint32_t index_count;
);

typedef struct GltfGpuMesh
{
    BufferSlice position_slice;
    BufferSlice index_slice;
    BufferSlice normal_slice;
    BufferSlice uv_slice;
    bool has_normal;
    bool has_uv;
} GltfGpuMesh;

typedef struct GltfGpuMaterial
{
    vec4 base_color_factor;
    vec3 emissive_factor;
    float metallic_factor;
    float roughness_factor;

    TextureID base_color_tex;
    TextureID normal_tex;
    TextureID mr_tex;
    TextureID emissive_tex;
    SamplerID sampler_id;
} GltfGpuMaterial;

typedef struct GltfGpuModel
{
    GLTFContainer* cpu;
    GltfGpuMesh* gpu_meshes;
    GltfGpuMaterial* gpu_materials;
    SamplerID* sampler_map;
    TextureID* image_map;
    mat4* node_world_matrices;
    bool uploaded;
} GltfGpuModel;

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
    int w = 0, h = 0, channels = 0;
    unsigned char* pixels = stbi_load_from_memory(encoded, (int)encoded_size, &w, &h, &channels, 4);
    if(!pixels || w <= 0 || h <= 0)
        return renderer.dummy_texture;

    TextureCreateDesc desc = {
        .width = (uint32_t)w,
        .height = (uint32_t)h,
        .mip_count = 1,
        .format = VK_FORMAT_R8G8B8A8_SRGB,
        .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
    };

    TextureID id = create_texture(r, &desc);
    if(id == UINT32_MAX)
    {
        stbi_image_free(pixels);
        return renderer.dummy_texture;
    }

    Texture* tex = &textures[id];
    VkDeviceSize image_size = (VkDeviceSize)w * (VkDeviceSize)h * 4u;

    VkCommandBuffer cmd = vk_begin_one_time_cmd(r->device, r->one_time_gfx_pool);

    VkImageMemoryBarrier barrier = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .srcAccessMask = 0,
        .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .image = tex->image,
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

    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
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
        char dir[512] = {0};
        char full[1024] = {0};
        gltf_dirname(gltf->source_path, dir, sizeof(dir));
        gltf_join_path(dir, image->uri, full, sizeof(full));
        TextureID id = load_texture(&renderer, full);
        return id == UINT32_MAX ? renderer.dummy_texture : id;
    }

    if(image->buffer_view && image->buffer_view->buffer && image->buffer_view->buffer->data)
    {
        const uint8_t* bytes = (const uint8_t*)image->buffer_view->buffer->data + image->buffer_view->offset;
        size_t size = image->buffer_view->size;
        return create_texture_from_memory(&renderer, bytes, size);
    }

    return renderer.dummy_texture;
}

static SamplerID create_gltf_sampler(const SamplerDesc* src)
{
    SamplerCreateDesc desc = {
        .mag_filter = src ? src->mag_filter : VK_FILTER_LINEAR,
        .min_filter = src ? src->min_filter : VK_FILTER_LINEAR,
        .address_u = src ? src->address_U : VK_SAMPLER_ADDRESS_MODE_REPEAT,
        .address_v = src ? src->address_V : VK_SAMPLER_ADDRESS_MODE_REPEAT,
        .address_w = src ? src->address_W : VK_SAMPLER_ADDRESS_MODE_REPEAT,
        .mipmap_mode = src ? src->mip_map_mode : VK_SAMPLER_MIPMAP_MODE_LINEAR,
        .max_lod = src ? src->max_lod : 16.0f,
        .debug_name = "gltf_sampler",
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

static bool gltf_gpu_model_init(GltfGpuModel* model)
{
    memset(model, 0, sizeof(*model));

    if(!loadGltf(GLTF_MODEL_PATH, GLTF_FLAG_LOAD_VERTICES | GLTF_FLAG_CALCULATE_BOUNDS, &model->cpu))
        return false;

    if(model->cpu->mesh_count == 0)
        return false;

    model->gpu_meshes = (GltfGpuMesh*)calloc(model->cpu->mesh_count, sizeof(GltfGpuMesh));
    model->node_world_matrices = (mat4*)calloc(model->cpu->node_count > 0 ? model->cpu->node_count : 1u, sizeof(mat4));

    if(model->cpu->material_count > 0)
        model->gpu_materials = (GltfGpuMaterial*)calloc(model->cpu->material_count, sizeof(GltfGpuMaterial));
    if(model->cpu->sampler_count > 0)
        model->sampler_map = (SamplerID*)calloc(model->cpu->sampler_count, sizeof(SamplerID));
    if(model->cpu->handle && model->cpu->handle->images_count > 0)
        model->image_map = (TextureID*)calloc(model->cpu->handle->images_count, sizeof(TextureID));

    if(!model->gpu_meshes || !model->node_world_matrices
       || (model->cpu->material_count > 0 && !model->gpu_materials)
       || (model->cpu->sampler_count > 0 && !model->sampler_map)
       || (model->cpu->handle && model->cpu->handle->images_count > 0 && !model->image_map))
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
        model->gpu_meshes[i].index_slice = buffer_pool_alloc(&renderer.gpu_pool, idx_bytes, 16);

        if(model->gpu_meshes[i].position_slice.buffer == VK_NULL_HANDLE || model->gpu_meshes[i].index_slice.buffer == VK_NULL_HANDLE)
            return false;

        if(src->attributes[GLTF_ATTRIBUTE_TYPE_NORMAL])
        {
            VkDeviceSize nrm_bytes = (VkDeviceSize)src->vertex_count * 3u * sizeof(float);
            model->gpu_meshes[i].normal_slice = buffer_pool_alloc(&renderer.gpu_pool, nrm_bytes, 16);
            model->gpu_meshes[i].has_normal = model->gpu_meshes[i].normal_slice.buffer != VK_NULL_HANDLE;
            if(!model->gpu_meshes[i].has_normal)
                return false;
        }

        if(src->attributes[GLTF_ATTRIBUTE_TYPE_TEXCOORD])
        {
            VkDeviceSize uv_bytes = (VkDeviceSize)src->vertex_count * 2u * sizeof(float);
            model->gpu_meshes[i].uv_slice = buffer_pool_alloc(&renderer.gpu_pool, uv_bytes, 16);
            model->gpu_meshes[i].has_uv = model->gpu_meshes[i].uv_slice.buffer != VK_NULL_HANDLE;
            if(!model->gpu_meshes[i].has_uv)
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

    for(uint32_t i = 0; i < model->cpu->material_count; ++i)
    {
        GLTFMaterial* src = &model->cpu->materials[i];
        GltfGpuMaterial* dst = &model->gpu_materials[i];

        glm_vec4_copy((vec4){1.0f, 1.0f, 1.0f, 1.0f}, dst->base_color_factor);
        glm_vec3_zero(dst->emissive_factor);
        dst->metallic_factor = 0.0f;
        dst->roughness_factor = 1.0f;

        dst->base_color_tex = renderer.dummy_texture;
        dst->normal_tex = renderer.dummy_texture;
        dst->mr_tex = renderer.dummy_texture;
        dst->emissive_tex = renderer.dummy_texture;
        dst->sampler_id = renderer.default_samplers.samplers[SAMPLER_LINEAR_WRAP];

        if(src->material_type == GLTF_MATERIAL_TYPE_METALLIC_ROUGHNESS)
        {
            glm_vec4_copy(src->metallic_roughness.base_color_factor, dst->base_color_factor);
            dst->metallic_factor = src->metallic_roughness.metallic_factor;
            dst->roughness_factor = src->metallic_roughness.roughness_factor;

            dst->base_color_tex = texture_from_view(model, &src->metallic_roughness.base_color_texture);
            dst->mr_tex = texture_from_view(model, &src->metallic_roughness.metallic_roughness_texture);
            dst->sampler_id = sampler_from_view(model, &src->metallic_roughness.base_color_texture);
        }

        dst->normal_tex = texture_from_view(model, &src->normal_texture);
        dst->emissive_tex = texture_from_view(model, &src->emissive_texture);
        glm_vec3_copy(src->emissive_factor, dst->emissive_factor);
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
        }
    }

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
    free(model->gpu_materials);
    free(model->sampler_map);
    free(model->image_map);
    free(model->node_world_matrices);
    freeGltf(model->cpu);
    memset(model, 0, sizeof(*model));
}

static void gltf_gpu_model_upload_once(GltfGpuModel* model, VkCommandBuffer cmd)
{
    if(model->uploaded)
        return;

    uint32_t barrier_capacity = model->cpu->mesh_count * 4u;
    VkBufferMemoryBarrier2* barriers = (VkBufferMemoryBarrier2*)calloc(barrier_capacity, sizeof(VkBufferMemoryBarrier2));
    if(!barriers)
        return;

    uint32_t b = 0;
    for(uint32_t i = 0; i < model->cpu->mesh_count; ++i)
    {
        GLTFMesh* src = &model->cpu->meshes[i];
        GltfGpuMesh* dst = &model->gpu_meshes[i];

        VkDeviceSize pos_bytes = (VkDeviceSize)src->vertex_count * 3u * sizeof(float);
        VkDeviceSize idx_bytes = (VkDeviceSize)src->index_count * sizeof(uint32_t);

        renderer_upload_buffer_to_slice(&renderer, cmd, dst->position_slice, src->attributes[GLTF_ATTRIBUTE_TYPE_POSITION], pos_bytes, 16);
        renderer_upload_buffer_to_slice(&renderer, cmd, dst->index_slice, src->index, idx_bytes, 16);

        barriers[b++] = (VkBufferMemoryBarrier2){
            .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
            .srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
            .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
            .dstStageMask = VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT,
            .dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
            .buffer = dst->position_slice.buffer,
            .offset = dst->position_slice.offset,
            .size = pos_bytes,
        };

        barriers[b++] = (VkBufferMemoryBarrier2){
            .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
            .srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
            .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
            .dstStageMask = VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT,
            .dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
            .buffer = dst->index_slice.buffer,
            .offset = dst->index_slice.offset,
            .size = idx_bytes,
        };

        if(dst->has_normal)
        {
            VkDeviceSize nrm_bytes = (VkDeviceSize)src->vertex_count * 3u * sizeof(float);
            renderer_upload_buffer_to_slice(&renderer, cmd, dst->normal_slice, src->attributes[GLTF_ATTRIBUTE_TYPE_NORMAL], nrm_bytes, 16);
            barriers[b++] = (VkBufferMemoryBarrier2){
                .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
                .srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
                .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                .dstStageMask = VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT,
                .dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
                .buffer = dst->normal_slice.buffer,
                .offset = dst->normal_slice.offset,
                .size = nrm_bytes,
            };
        }

        if(dst->has_uv)
        {
            VkDeviceSize uv_bytes = (VkDeviceSize)src->vertex_count * 2u * sizeof(float);
            renderer_upload_buffer_to_slice(&renderer, cmd, dst->uv_slice, src->attributes[GLTF_ATTRIBUTE_TYPE_TEXCOORD], uv_bytes, 16);
            barriers[b++] = (VkBufferMemoryBarrier2){
                .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
                .srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
                .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                .dstStageMask = VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT,
                .dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
                .buffer = dst->uv_slice.buffer,
                .offset = dst->uv_slice.offset,
                .size = uv_bytes,
            };
        }
    }

    VkDependencyInfo dep = {
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .bufferMemoryBarrierCount = b,
        .pBufferMemoryBarriers = barriers,
    };

    vkCmdPipelineBarrier2(cmd, &dep);
    free(barriers);

    model->uploaded = true;
}

static void draw_gltf_model(VkCommandBuffer cmd, const Camera* cam, const GltfGpuModel* model)
{
    float anim_time = (float)glfwGetTime();
    if(model->cpu->node_count > 0)
        gltf_apply_animation(model->cpu, 0, anim_time, model->node_world_matrices);

    vk_cmd_set_viewport_scissor(cmd, renderer.swapchain.extent);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g_render_pipelines.pipelines[pipelines.gltf_minimal]);

    for(uint32_t i = 0; i < model->cpu->mesh_count; ++i)
    {
        GltfUberPush push = {0};
        GLTFMesh* mesh = &model->cpu->meshes[i];
        GltfGpuMesh* gpu_mesh = &model->gpu_meshes[i];

        push.pos_ptr = slice_device_address(&renderer, gpu_mesh->position_slice);
        push.idx_ptr = slice_device_address(&renderer, gpu_mesh->index_slice);
        push.nrm_ptr = gpu_mesh->has_normal ? slice_device_address(&renderer, gpu_mesh->normal_slice) : 0;
        push.uv_ptr = gpu_mesh->has_uv ? slice_device_address(&renderer, gpu_mesh->uv_slice) : 0;
        push.index_count = mesh->index_count;

        push.flags = 0;
        if(gpu_mesh->has_uv) push.flags |= 0x1;
        if(gpu_mesh->has_normal) push.flags |= 0x2;

        u32 node_index = model->cpu->mesh_node_indices ? model->cpu->mesh_node_indices[i] : UINT_MAX;
        if(node_index != UINT_MAX && node_index < model->cpu->node_count)
            memcpy(push.model, model->node_world_matrices[node_index], sizeof(push.model));
        else
            glm_mat4_identity(push.model);

        memcpy(push.view_proj, cam->view_proj, sizeof(push.view_proj));

        u32 material_index = model->cpu->material_indices ? model->cpu->material_indices[i] : UINT_MAX;
        if(material_index != UINT_MAX && material_index < model->cpu->material_count && model->gpu_materials)
        {
            const GltfGpuMaterial* mat = &model->gpu_materials[material_index];
            memcpy(push.base_color_factor, mat->base_color_factor, sizeof(push.base_color_factor));
            memcpy(push.emissive_factor, mat->emissive_factor, sizeof(push.emissive_factor));
            push.metallic_factor = mat->metallic_factor;
            push.roughness_factor = mat->roughness_factor;
            push.base_color_tex = mat->base_color_tex;
            push.normal_tex = mat->normal_tex;
            push.mr_tex = mat->mr_tex;
            push.emissive_tex = mat->emissive_tex;
            push.sampler_id = mat->sampler_id;
        }
        else
        {
            glm_vec4_copy((vec4){1.0f, 1.0f, 1.0f, 1.0f}, push.base_color_factor);
            glm_vec3_zero(push.emissive_factor);
            push.metallic_factor = 0.0f;
            push.roughness_factor = 1.0f;
            push.base_color_tex = renderer.dummy_texture;
            push.normal_tex = renderer.dummy_texture;
            push.mr_tex = renderer.dummy_texture;
            push.emissive_tex = renderer.dummy_texture;
            push.sampler_id = renderer.default_samplers.samplers[SAMPLER_LINEAR_WRAP];
        }

        vkCmdPushConstants(cmd, renderer.bindless_system.pipeline_layout, VK_SHADER_STAGE_ALL, 0, sizeof(GltfUberPush), &push);
        vkCmdDraw(cmd, push.index_count, 1, 0, 0);
    }
}

int main(void)
{
    graphics_init();

    Camera cam = {0};
    camera_defaults_3d(&cam);
    camera3d_set_position(&cam, 0.0f, 0.6f, 4.0f);
    camera3d_set_rotation_yaw_pitch(&cam, 0.0f, 0.0f);

    GltfGpuModel beaver = {0};
    if(!gltf_gpu_model_init(&beaver))
    {
        gltf_gpu_model_destroy(&beaver);
        renderer_destroy(&renderer);
        return 1;
    }

    while(!glfwWindowShouldClose(renderer.window))
    {
        TracyCFrameMark;

        pipeline_rebuild(&renderer);
        frame_start(&renderer, &cam);

        VkCommandBuffer cmd = renderer.frames[renderer.current_frame].cmdbuf;
        GpuProfiler* frame_prof = &renderer.gpuprofiler[renderer.current_frame];

        vk_cmd_begin(cmd, false);
        gpu_profiler_begin_frame(frame_prof, cmd);
        {
            {
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, renderer.bindless_system.pipeline_layout, 0, 1,
                                        &renderer.bindless_system.set, 0, NULL);
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, renderer.bindless_system.pipeline_layout, 0, 1,
                                        &renderer.bindless_system.set, 0, NULL);

                rt_transition_all(cmd, &renderer.depth[renderer.swapchain.current_image], VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                                  VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                                  VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT);

                rt_transition_all(cmd, &renderer.hdr_color[renderer.swapchain.current_image], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                  VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);

                image_transition_swapchain(cmd, &renderer.swapchain, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                           VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
                flush_barriers(cmd);
            }

            gltf_gpu_model_upload_once(&beaver, cmd);

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

            vkCmdBeginRendering(cmd, &rendering);
            draw_gltf_model(cmd, &cam, &beaver);
            vkCmdEndRendering(cmd);

            post_pass();
            pass_smaa();
            pass_ldr_to_swapchain();

            image_transition_swapchain(cmd, &renderer.swapchain, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                                       VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, 0);
            flush_barriers(cmd);
        }

        vk_cmd_end(cmd);
        submit_frame(&renderer);
    }

    gltf_gpu_model_destroy(&beaver);
    renderer_destroy(&renderer);
    return 0;
}
