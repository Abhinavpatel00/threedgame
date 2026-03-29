#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <cglm/types.h>

#include "tinytypes.h"

#define GLTF_NAME_MAX_LENGTH 64

typedef struct GLTFTextureTransform
{
    vec2  offset;
    vec2  scale;
    float rotation;
} GLTFTextureTransform;

typedef struct GLTFTextureView
{
    char    name[GLTF_NAME_MAX_LENGTH];
    size_t  texture_index;
    size_t  sample_index;
    int32_t tex_coord_set;
    float   scale;
    GLTFTextureTransform transform;
} GLTFTextureView;

typedef struct GLTFMetallicRoughnessMaterial
{
    GLTFTextureView base_color_texture;
    GLTFTextureView metallic_roughness_texture;

    vec4  base_color_factor;
    float metallic_factor;
    float roughness_factor;
} GLTFMetallicRoughnessMaterial;

typedef struct GLTFSpecularGlossinessMaterial
{
    GLTFTextureView diffuse_texture;
    GLTFTextureView specular_glossiness_texture;
    vec4  diffuse_factor;
    vec3  specular_factor;
    float glossiness_factor;
} GLTFSpecularGlossinessMaterial;

typedef enum GLTFMaterialType
{
    GLTF_MATERIAL_TYPE_METALLIC_ROUGHNESS,
    GLTF_MATERIAL_TYPE_SPECULAR_GLOSSINESS,
} GLTFMaterialType;

typedef enum GLTFMaterialAlphaMode
{
    GLTF_MATERIAL_ALPHA_MODE_OPAQUE,
    GLTF_MATERIAL_ALPHA_MODE_MASK,
    GLTF_MATERIAL_ALPHA_MODE_BLEND,
} GLTFMaterialAlphaMode;

typedef struct GLTFMaterial
{
    char name[GLTF_NAME_MAX_LENGTH];

    union
    {
        GLTFMetallicRoughnessMaterial metallic_roughness;
        GLTFSpecularGlossinessMaterial specular_glossiness;
    };

    GLTFMaterialType material_type;
    GLTFTextureView  normal_texture;
    GLTFTextureView  occlusion_texture;
    GLTFTextureView  emissive_texture;
    vec3             emissive_factor;
    GLTFMaterialAlphaMode alpha_mode;
    float            alpha_cutoff;
    bool             double_sided;
    bool             unlit;
} GLTFMaterial;

typedef struct GLTFNode
{
    char   pName[GLTF_NAME_MAX_LENGTH];
    u32    parent_index;
    u32*   child_indices;
    u32    child_count;
    u32    mesh_index;
    u32    mesh_count;
    float* weights;
    u32    weights_count;
    u32    skin_index;

    vec3   translation;
    versor rotation;
    vec3   scale;
    mat4   matrix;
} GLTFNode;

typedef struct GLTFSkin
{
    char name[GLTF_NAME_MAX_LENGTH];
    u32* joint_node_indices;
    u32  joint_count;
    u32  skeleton_node_index;
    mat4* inverse_bind_matrices;
} GLTFSkin;

typedef enum GLTFAttributeType
{
    GLTF_ATTRIBUTE_TYPE_INVALID,
    GLTF_ATTRIBUTE_TYPE_POSITION,
    GLTF_ATTRIBUTE_TYPE_NORMAL,
    GLTF_ATTRIBUTE_TYPE_TANGENT,
    GLTF_ATTRIBUTE_TYPE_TEXCOORD,
    GLTF_ATTRIBUTE_TYPE_COLOR,
    GLTF_ATTRIBUTE_TYPE_JOINTS,
    GLTF_ATTRIBUTE_TYPE_WEIGHTS,
    GLTF_ATTRIBUTE_TYPE_COUNT,
} GLTFAttributeType;

typedef struct GLTFMesh
{
    vec3 min;
    vec3 max;
    u32  index_count;
    u32  vertex_count;
    u32  start_index;
    u32* index;
    void* attributes[GLTF_ATTRIBUTE_TYPE_COUNT];
} GLTFMesh;

typedef enum GLTFImageUsage
{
    GLTF_IMAGE_USAGE_NONE                = 0u,
    GLTF_IMAGE_USAGE_BASE_COLOR          = 1u << 0u,
    GLTF_IMAGE_USAGE_METALLIC_ROUGHNESS  = 1u << 1u,
    GLTF_IMAGE_USAGE_DIFFUSE             = 1u << 2u,
    GLTF_IMAGE_USAGE_SPECULAR_GLOSSINESS = 1u << 3u,
    GLTF_IMAGE_USAGE_NORMAL              = 1u << 4u,
    GLTF_IMAGE_USAGE_OCCLUSION           = 1u << 5u,
    GLTF_IMAGE_USAGE_EMISSIVE            = 1u << 6u,
} GLTFImageUsage;

typedef struct SamplerDesc
{
    VkFilter                 min_filter;
    VkFilter                 mag_filter;
    VkSamplerMipmapMode      mip_map_mode;
    VkSamplerAddressMode     address_U;
    VkSamplerAddressMode     address_V;
    VkSamplerAddressMode     address_W;
    float                    mip_lod_bias;
    bool                     set_lod_bias;
    float                    min_lod;
    float                    max_lod;
    float                    max_anisotropy;
    VkCompareOp              compare_func;
    VkFormat                 format;
    VkSamplerYcbcrConversion model;
    VkSamplerYcbcrRange      ycbcr_range;
    VkChromaLocation         chroma_offset_X;
    VkChromaLocation         chroma_offset_Y;
    VkFilter                 chroma_filter;
    VkBool32                 force_explicit_reconstruction;
} SamplerDesc;

typedef struct GLTFContainer
{
    GLTFImageUsage* image_usage;
    GLTFMaterial*   materials;
    u32*            material_indices;
    SamplerDesc*    samplers;
    GLTFMesh*       meshes;
    GLTFNode*       nodes;
    GLTFSkin*       skins;

    u32 material_count;
    u32 sampler_count;
    u32 mesh_count;
    u32 node_count;
    u32 skin_count;
    u32 index_count;
    u32 vertex_count;
} GLTFContainer;

typedef enum GLTFFlags
{
    GLTF_FLAG_LOAD_VERTICES    = 0x1,
    GLTF_FLAG_CALCULATE_BOUNDS = 0x2,
} GLTFFlags;

bool loadGltf(const char* path, GLTFFlags flags, GLTFContainer** outGltf);
void freeGltf(GLTFContainer* gltf);
