#include "external/debugbreak/debugbreak.h"
#include "tinytypes.h"
#include "flow/flow.h"
#include "vk_default.h"
#include "vk.h"
#include <GLFW/glfw3.h>
#include <dirent.h>
#include <stdalign.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <vulkan/vulkan_core.h>
#include "stb/stb_perlin.h"
#include "voxel.h"
#define DMON_IMPL
#include "external/dmon/dmon.h"
#include "external/tracy/public/tracy/TracyC.h"

static bool voxel_debug = true;

static bool take_screenshot = true;
#define VALIDATION true
#define KB(x) ((x) * 1024ULL)
#define MB(x) ((x) * 1024ULL * 1024ULL)
#define GB(x) ((x) * 1024ULL * 1024ULL * 1024ULL)
#define PAD(name, size) uint8_t name[(size)]
// imp gpu validation shows false positives may be bacause of data races
typedef struct
{
    vec3  position;
    float radius;

    vec3  direction;
    float angle;

    vec3  color;
    float intensity;
} SpotLight;
typedef struct LightBeam
{
    float pos_width[4];   // xyz = position, w = width
    float sun_height[4];  // xyz = sun_dir,  w = height
    float misc[4];        // x = opacity
} LightBeam;
/*
 if(editing mode of light)
 {
may be just cast a ray from mouse pointer to screen and if it matches then activate cimgui gizmo to capture stuff from it

and also there should be save position to file option for that light so that     we can them just load it from from when editing isnt enabled and dmon watches for file changes anyway
 }



   */
typedef struct
{
    uint32_t fullscreen;
    uint32_t postprocess;
    uint32_t triangle;
    uint32_t smaa_edge;
    uint32_t smaa_weight;
    uint32_t smaa_blend;
    uint32_t beam;
} EnginePipelines;

static EnginePipelines pipelines;


static const VoxelType terrain_voxels[] = {
    STONE, GRASS, DIRT, SAND, GRAVEL, CLAY,

};
typedef struct
{
    float pos[3];
    float uv[2];
} Vertex;

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


typedef enum PipelineType
{
    PIPELINE_TYPE_GRAPHICS,
    PIPELINE_TYPE_COMPUTE
} PipelineType;

typedef struct PipelineEntry
{
    PipelineType type;

    union
    {
        GraphicsPipelineConfig graphics;

        struct
        {
            const char* path;
        } compute;
    };

    bool dirty;

} PipelineEntry;

typedef struct RendererPipelines
{
    VkPipeline    pipelines[MAX_PIPELINES];
    PipelineEntry entries[MAX_PIPELINES];
    uint32_t      count;
} RendererPipelines;

static RendererPipelines render_pipelines;
static uint32_t          current_pipeline_build;

/* --------------------------------------------------------- */

PipelineID create_graphics_pipeline_cache(Renderer* r, GraphicsPipelineConfig* cfg)
{
    u32 id;
    flow_id_pool_create_id(&pipeline_id_pool, &id);

    if(id >= MAX_PIPELINES)
    {
        printf("Pipeline overflow\n");
        debug_break();
    }

    PipelineEntry* e = &render_pipelines.entries[id];

    e->type     = PIPELINE_TYPE_GRAPHICS;
    e->graphics = *cfg;
    e->dirty    = false;

    VkPipeline p = create_graphics_pipeline(r, cfg);

    render_pipelines.pipelines[id] = p;

    current_pipeline_build++;
    render_pipelines.count++;
    return id;
}

PipelineID create_compute_pipeline_cache(Renderer* r, const char* path)
{
    u32 id;
    flow_id_pool_create_id(&pipeline_id_pool, &id);

    if(id >= MAX_PIPELINES)
    {
        printf("Pipeline overflow\n");
        debug_break();
    }

    PipelineEntry* e = &render_pipelines.entries[id];

    e->type         = PIPELINE_TYPE_COMPUTE;
    e->compute.path = path;
    e->dirty        = false;

    VkPipeline p = create_compute_pipeline(r, path);

    render_pipelines.pipelines[id] = p;

    current_pipeline_build++;

    render_pipelines.count++;
    return id;
}

/* --------------------------------------------------------- */

void mark_pipelines_dirty(const char* changed)
{
    for(uint32_t i = 0; i < render_pipelines.count; i++)
    {
        PipelineEntry* e = &render_pipelines.entries[i];

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

/* --------------------------------------------------------- */

void rebuild_dirty_pipelines(Renderer* r)
{
    bool any_dirty = false;

    for(int i = 0; i < render_pipelines.count; i++)
        if(render_pipelines.entries[i].dirty)
            any_dirty = true;

    if(!any_dirty)
        return;

    vkDeviceWaitIdle(r->device);

    for(int i = 0; i < render_pipelines.count; i++)
    {
        PipelineEntry* e = &render_pipelines.entries[i];

        if(!e->dirty)
            continue;

        e->dirty = false;

        vkDestroyPipeline(r->device, render_pipelines.pipelines[i], NULL);

        if(e->type == PIPELINE_TYPE_GRAPHICS)
        {
            render_pipelines.pipelines[i] = create_graphics_pipeline(r, &e->graphics);
        }
        else
        {
            render_pipelines.pipelines[i] = create_compute_pipeline(r, e->compute.path);
        }

        printf("Pipeline %d hot reloaded\n", i);
    }
}

static volatile bool shader_changed = false;
static char          changed_shader[256];
static void watch_cb(dmon_watch_id id, dmon_action action, const char* root, const char* filepath, const char* oldfilepath, void* user)
{
    if(action == DMON_ACTION_MODIFY || action == DMON_ACTION_CREATE)
    {
        if(strstr(filepath, ".slang"))
        {
            snprintf(changed_shader, sizeof(changed_shader), "%s", filepath);
            shader_changed = true;
        }
    }
}


typedef struct
{
    uint32_t data0;
    uint32_t data1;
} PackedFace;

typedef struct
{
    VoxelType type;
} Voxel;

typedef struct
{
    const char* path;
    TextureID   id;
} TextureCacheEntry;


#define CHUNK_SIZE 128
#define STREAM_CHUNK_RADIUS 2
#define STREAM_CHUNK_DIAMETER (STREAM_CHUNK_RADIUS * 2 + 1)
#define STREAM_CHUNK_COUNT (STREAM_CHUNK_DIAMETER * STREAM_CHUNK_DIAMETER)
#define MAX_STREAM_FACES 2500000u

typedef struct
{
    Voxel voxels[CHUNK_SIZE][CHUNK_SIZE][CHUNK_SIZE];
} Chunk;
typedef struct
{
    PackedFace* faces;
    uint32_t    face_count;
    uint32_t    capacity;
} ChunkMesh;

#define MAX_TEXTURES 2222

static TextureCacheEntry voxel_texture_cache[MAX_TEXTURES];
uint32_t                 voxel_texture_cache_count = 0;

static TextureID block_textures[VOXEL_COUNT][6];
static bool      block_texture_resolved[VOXEL_COUNT][6];
static TextureID block_texture_fallback = UINT32_MAX;

typedef struct
{
    bool      active;
    int       center_chunk_x;
    int       center_chunk_z;
    uint32_t  next_chunk_index;
    ChunkMesh mesh;
} StreamBuildState;

static void generate_chunk_wfc(Chunk* chunk, int chunk_x, int chunk_z);
void        append_chunk_mesh(Renderer* r, Chunk* chunk, ChunkMesh* mesh, int rel_chunk_x, int rel_chunk_z);

TextureID get_texture(Renderer* r, const char* path)
{
    for(uint32_t i = 0; i < voxel_texture_cache_count; i++)
    {
        if(strcmp(voxel_texture_cache[i].path, path) == 0)
            return voxel_texture_cache[i].id;
    }

    TextureID id = load_texture(r, path);

    voxel_texture_cache[voxel_texture_cache_count++] = (TextureCacheEntry){.path = path, .id = id};

    return id;
}

void init_block_textures(Renderer* r)
{
    block_texture_fallback = get_texture(r, "data/dummy_texture.png");

    for(int b = 0; b < VOXEL_COUNT; b++)
    {
        for(int f = 0; f < 6; f++)
        {
            block_textures[b][f]         = block_texture_fallback;
            block_texture_resolved[b][f] = false;
        }
    }
}

static inline TextureID resolve_block_texture(Renderer* r, VoxelType type, FaceDir face)
{
    if(block_texture_resolved[type][face])
        return block_textures[type][face];

    block_texture_resolved[type][face] = true;

    const char* path = voxel_materials[type].face_tex[face];
    if(path)
    {
        TextureID id = get_texture(r, path);
        if(id != UINT32_MAX)
        {
            block_textures[type][face] = id;
            return id;
        }
    }

    block_textures[type][face] = block_texture_fallback;
    return block_texture_fallback;
}

static void stream_build_begin(StreamBuildState* state, int center_chunk_x, int center_chunk_z)
{
    state->active           = true;
    state->center_chunk_x   = center_chunk_x;
    state->center_chunk_z   = center_chunk_z;
    state->next_chunk_index = 0;
    state->mesh.face_count  = 0;
}

static bool stream_build_step(Renderer* r, StreamBuildState* state, Chunk* scratch_chunk, uint32_t chunk_budget)
{
    if(!state->active)
        return true;

    const int diameter = STREAM_CHUNK_DIAMETER;

    for(uint32_t i = 0; i < chunk_budget; i++)
    {
        if(state->next_chunk_index >= STREAM_CHUNK_COUNT)
        {
            state->active = false;
            return true;
        }

        uint32_t index = state->next_chunk_index++;

        int dx = (int)(index % (uint32_t)diameter) - STREAM_CHUNK_RADIUS;
        int dz = (int)(index / (uint32_t)diameter) - STREAM_CHUNK_RADIUS;

        int world_cx = state->center_chunk_x + dx;
        int world_cz = state->center_chunk_z + dz;

        generate_chunk_wfc(scratch_chunk, world_cx, world_cz);
        append_chunk_mesh(r, scratch_chunk, &state->mesh, dx, dz);
    }

    return false;
}

void build_debug_voxel_palette(Chunk* chunk)
{
    memset(chunk, 0, sizeof(*chunk));

    const int step            = 2;
    const int cells_per_axis  = CHUNK_SIZE / step;
    const int cells_per_layer = cells_per_axis * cells_per_axis;

    for(int t = 1; t < VOXEL_COUNT; t++)
    {
        int slot = t - 1;

        int x = (slot % cells_per_axis) * step;
        int z = ((slot / cells_per_axis) % cells_per_axis) * step;
        int y = (slot / cells_per_layer) * step;

        if(y >= CHUNK_SIZE)
            break;

        chunk->voxels[x][y][z].type = (VoxelType)t;
    }
}

/* Multi-layer terrain height:
   - Continental layer  (very low freq)  gives broad hills / valleys
   - Detail layer       (medium freq)    adds local variation
   Heights are combined and remapped to [0,1]                            */
static float terrain(float x, float z)
{
    float continental = stb_perlin_fbm_noise3(x * 0.002f, 0.0f, z * 0.002f, 2.0f, 0.45f, 4);
    float detail      = stb_perlin_fbm_noise3(x * 0.008f, 0.0f, z * 0.008f, 2.0f, 0.5f, 5);
    /* Blend: continental dominates, detail adds texture */
    float combined = continental * 0.7f + detail * 0.3f;
    return combined;
}

/* Hermite smoothstep for blending biome heights at boundaries */
static inline float smoothstep(float edge0, float edge1, float x)
{
    float t = (x - edge0) / (edge1 - edge0);
    if(t < 0.0f)
        t = 0.0f;
    if(t > 1.0f)
        t = 1.0f;
    return t * t * (3.0f - 2.0f * t);
}

typedef enum
{
    BIOME_GRASSLAND,
    BIOME_FOREST,
    BIOME_ROCKY,
    BIOME_MOUNTAIN,
    BIOME_SANDY,
    BIOME_DESERT,
    BIOME_CLAYEY,
    BIOME_SNOW,
    BIOME_COUNT
} BiomeTile;

/* Adjacency bitmasks -- each biome lists which biomes may sit next to it.
   Adding more allowed neighbours makes the WFC converge more easily;
   restricting them creates distinct biome regions.                        */
static const uint16_t biome_neighbors[BIOME_COUNT] = {
    /* GRASSLAND */ (1u << BIOME_GRASSLAND) | (1u << BIOME_FOREST) | (1u << BIOME_ROCKY) | (1u << BIOME_CLAYEY) | (1u << BIOME_SANDY),
    /* FOREST    */ (1u << BIOME_FOREST) | (1u << BIOME_GRASSLAND) | (1u << BIOME_ROCKY) | (1u << BIOME_SNOW),
    /* ROCKY     */ (1u << BIOME_ROCKY) | (1u << BIOME_MOUNTAIN) | (1u << BIOME_GRASSLAND) | (1u << BIOME_FOREST),
    /* MOUNTAIN  */ (1u << BIOME_MOUNTAIN) | (1u << BIOME_ROCKY) | (1u << BIOME_SNOW),
    /* SANDY     */ (1u << BIOME_SANDY) | (1u << BIOME_DESERT) | (1u << BIOME_GRASSLAND) | (1u << BIOME_CLAYEY),
    /* DESERT    */ (1u << BIOME_DESERT) | (1u << BIOME_SANDY),
    /* CLAYEY    */ (1u << BIOME_CLAYEY) | (1u << BIOME_GRASSLAND) | (1u << BIOME_SANDY),
    /* SNOW      */ (1u << BIOME_SNOW) | (1u << BIOME_MOUNTAIN) | (1u << BIOME_FOREST),
};

uint32_t squirrel_noise5(int position, uint32_t seed)
{
    const uint32_t BIT_NOISE1 = 0xd2a80a3f;
    const uint32_t BIT_NOISE2 = 0xa884f197;
    const uint32_t BIT_NOISE3 = 0x6c736f4b;

    uint32_t mangled = position;
    mangled *= BIT_NOISE1;
    mangled += seed;
    mangled ^= (mangled >> 8);
    mangled += BIT_NOISE2;
    mangled ^= (mangled << 8);
    mangled *= BIT_NOISE3;
    mangled ^= (mangled >> 8);

    return mangled;
}

static inline int world_to_chunk_coord(float value)
{
    return (int)floorf(value / (float)CHUNK_SIZE);
}

static inline uint32_t hash2i(int x, int z, uint32_t seed)
{
    uint32_t h = squirrel_noise5(x, seed);
    h          = squirrel_noise5(z ^ (int)h, h ^ 0x9e3779b9u);
    return h;
}

static inline BiomeTile choose_biome_from_mask(uint16_t mask, uint32_t rng)
{
    if(mask == 0)
        return BIOME_GRASSLAND;

    BiomeTile candidates[BIOME_COUNT];
    int       count = 0;
    for(int i = 0; i < BIOME_COUNT; i++)
    {
        if(mask & (1u << i))
            candidates[count++] = (BiomeTile)i;
    }

    if(count == 0)
        return BIOME_GRASSLAND;

    return candidates[rng % (uint32_t)count];
}

static inline VoxelType biome_surface_voxel(BiomeTile biome, uint32_t rng)
{
    switch(biome)
    {
        case BIOME_GRASSLAND:
            return (rng & 1u) ? GRASS : DIRT;
        case BIOME_FOREST:
            return GRASS;
        case BIOME_ROCKY:
            return (rng & 3u) ? STONE : GRAVEL;
        case BIOME_MOUNTAIN:
            return STONE;
        case BIOME_SANDY:
            return SAND;
        case BIOME_DESERT:
            return SAND;
        case BIOME_CLAYEY:
            return (rng & 1u) ? CLAY : DIRT;
        case BIOME_SNOW:
            return GRAVEL; /* closest to snow-like */
        default:
            return DIRT;
    }
}

static inline float biome_height_bias(BiomeTile biome)
{
    switch(biome)
    {
        case BIOME_GRASSLAND:
            return 4.0f;
        case BIOME_FOREST:
            return 8.0f;
        case BIOME_ROCKY:
            return 16.0f;
        case BIOME_MOUNTAIN:
            return 30.0f;
        case BIOME_SANDY:
            return -2.0f;
        case BIOME_DESERT:
            return -4.0f;
        case BIOME_CLAYEY:
            return 2.0f;
        case BIOME_SNOW:
            return 22.0f;
        default:
            return 0.0f;
    }
}

/* WFC runs on a coarse grid (COARSE_STEP voxels per tile) then biome
   assignments are bilinearly interpolated to every column for smooth
   transitions.  Adjacency constraints propagate left, up, and diagonally
   so chunk-edge biomes stay deterministic (seeded from world coords).     */
#define COARSE_STEP 8
#define COARSE_DIM (CHUNK_SIZE / COARSE_STEP + 1)

static BiomeTile wfc_collapse_coarse_cell(int gx, int gz, BiomeTile left, BiomeTile up, BiomeTile diag, bool has_left, bool has_up)
{
    uint16_t possible = (1u << BIOME_COUNT) - 1u;

    if(has_left)
        possible &= biome_neighbors[left];
    if(has_up)
        possible &= biome_neighbors[up];
    if(has_left && has_up)
        possible &= biome_neighbors[diag];

    uint32_t rng = hash2i(gx, gz, 0xBEEF1337u);
    return choose_biome_from_mask(possible, rng);
}

static void generate_chunk_wfc(Chunk* chunk, int chunk_x, int chunk_z)
{
    memset(chunk, 0, sizeof(*chunk));

    /* --- Phase 1: collapse biomes on coarse grid --- */
    BiomeTile coarse[COARSE_DIM][COARSE_DIM];

    for(int cz = 0; cz < COARSE_DIM; cz++)
        for(int cx = 0; cx < COARSE_DIM; cx++)
        {
            int gx = chunk_x * CHUNK_SIZE + cx * COARSE_STEP;
            int gz = chunk_z * CHUNK_SIZE + cz * COARSE_STEP;

            BiomeTile left = (cx > 0) ? coarse[cz][cx - 1] : BIOME_GRASSLAND;
            BiomeTile up   = (cz > 0) ? coarse[cz - 1][cx] : BIOME_GRASSLAND;
            BiomeTile diag = (cx > 0 && cz > 0) ? coarse[cz - 1][cx - 1] : BIOME_GRASSLAND;

            coarse[cz][cx] = wfc_collapse_coarse_cell(gx, gz, left, up, diag, cx > 0, cz > 0);
        }

    /* --- Phase 2: fill voxel columns with bilinear height blending --- */
    for(int z = 0; z < CHUNK_SIZE; z++)
        for(int x = 0; x < CHUNK_SIZE; x++)
        {
            int gx = chunk_x * CHUNK_SIZE + x;
            int gz = chunk_z * CHUNK_SIZE + z;

            /* Coarse-grid cell coords + fractional t */
            int   cx0 = x / COARSE_STEP;
            int   cz0 = z / COARSE_STEP;
            int   cx1 = (cx0 + 1 < COARSE_DIM) ? cx0 + 1 : cx0;
            int   cz1 = (cz0 + 1 < COARSE_DIM) ? cz0 + 1 : cz0;
            float tx  = smoothstep(0.0f, 1.0f, (float)(x - cx0 * COARSE_STEP) / (float)COARSE_STEP);
            float tz  = smoothstep(0.0f, 1.0f, (float)(z - cz0 * COARSE_STEP) / (float)COARSE_STEP);

            /* Four corner biome height biases -- bilinear blend */
            float b00          = biome_height_bias(coarse[cz0][cx0]);
            float b10          = biome_height_bias(coarse[cz0][cx1]);
            float b01          = biome_height_bias(coarse[cz1][cx0]);
            float b11          = biome_height_bias(coarse[cz1][cx1]);
            float blended_bias = b00 * (1 - tx) * (1 - tz) + b10 * tx * (1 - tz) + b01 * (1 - tx) * tz + b11 * tx * tz;

            /* Nearest coarse cell decides surface block type */
            BiomeTile biome = coarse[(int)((tz > 0.5f) ? cz1 : cz0)][(int)((tx > 0.5f) ? cx1 : cx0)];

            float n         = terrain((float)gx, (float)gz);
            n               = n * 0.5f + 0.5f;
            int base_height = (int)(n * (float)(CHUNK_SIZE - 1));
            int height      = (int)glm_clamp((float)base_height + blended_bias, 1.0f, (float)(CHUNK_SIZE - 1));

            VoxelType top_type = biome_surface_voxel(biome, hash2i(gx, gz, 1337u) >> 8);

            for(int y = 0; y < height; y++)
            {
                if(y < height - 4)
                    chunk->voxels[x][y][z].type = STONE;
                else if(y < height - 1)
                    chunk->voxels[x][y][z].type = DIRT;
                else
                    chunk->voxels[x][y][z].type = top_type;
            }
        }
}

static inline bool is_air(Chunk* c, int x, int y, int z)
{
    if(x < 0 || y < 0 || z < 0)
        return true;
    if(x >= CHUNK_SIZE)
        return true;
    if(y >= CHUNK_SIZE)
        return true;
    if(z >= CHUNK_SIZE)
        return true;

    return c->voxels[x][y][z].type == VOXEL_AIR;
}


static inline PackedFace pack_face(uint32_t x, uint32_t y, uint32_t z, uint32_t face, uint32_t tex, int chunk_x, int chunk_z)
{
    PackedFace f;

    f.data0 = (x & 255) | ((y & 255) << 8) | ((z & 4095) << 16) | ((face & 7) << 28);

    uint32_t packed_tex = (tex & 0xFFFFu);
    uint32_t packed_cx  = ((uint32_t)(chunk_x + 128) & 0xFFu) << 16;
    uint32_t packed_cz  = ((uint32_t)(chunk_z + 128) & 0xFFu) << 24;

    f.data1 = packed_tex | packed_cx | packed_cz;

    return f;
}

static inline void emit_face(ChunkMesh* mesh, PackedFace face)
{
    if(mesh->face_count < mesh->capacity)
        mesh->faces[mesh->face_count++] = face;
}

void append_chunk_mesh(Renderer* r, Chunk* chunk, ChunkMesh* mesh, int rel_chunk_x, int rel_chunk_z)
{
    for(int x = 0; x < CHUNK_SIZE; x++)
        for(int y = 0; y < CHUNK_SIZE; y++)
            for(int z = 0; z < CHUNK_SIZE; z++)
            {
                Voxel v = chunk->voxels[x][y][z];

                if(v.type == VOXEL_AIR)
                    continue;

                if(is_air(chunk, x + 1, y, z))
                    emit_face(mesh, pack_face(x, y, z, FACE_POS_X, resolve_block_texture(r, v.type, FACE_POS_X),
                                              rel_chunk_x, rel_chunk_z));

                if(is_air(chunk, x - 1, y, z))
                    emit_face(mesh, pack_face(x, y, z, FACE_NEG_X, resolve_block_texture(r, v.type, FACE_NEG_X),
                                              rel_chunk_x, rel_chunk_z));

                if(is_air(chunk, x, y + 1, z))
                    emit_face(mesh, pack_face(x, y, z, FACE_POS_Y, resolve_block_texture(r, v.type, FACE_POS_Y),
                                              rel_chunk_x, rel_chunk_z));

                if(is_air(chunk, x, y - 1, z))
                    emit_face(mesh, pack_face(x, y, z, FACE_NEG_Y, resolve_block_texture(r, v.type, FACE_NEG_Y),
                                              rel_chunk_x, rel_chunk_z));

                if(is_air(chunk, x, y, z + 1))
                    emit_face(mesh, pack_face(x, y, z, FACE_POS_Z, resolve_block_texture(r, v.type, FACE_POS_Z),
                                              rel_chunk_x, rel_chunk_z));

                if(is_air(chunk, x, y, z - 1))
                    emit_face(mesh, pack_face(x, y, z, FACE_NEG_Z, resolve_block_texture(r, v.type, FACE_NEG_Z),
                                              rel_chunk_x, rel_chunk_z));
            }
}

void rebuild_streamed_world_mesh(Renderer* r, Chunk* scratch_chunk, ChunkMesh* mesh, int center_chunk_x, int center_chunk_z)
{
    mesh->face_count = 0;

    for(int dz = -STREAM_CHUNK_RADIUS; dz <= STREAM_CHUNK_RADIUS; dz++)
        for(int dx = -STREAM_CHUNK_RADIUS; dx <= STREAM_CHUNK_RADIUS; dx++)
        {
            int world_cx = center_chunk_x + dx;
            int world_cz = center_chunk_z + dz;

            generate_chunk_wfc(scratch_chunk, world_cx, world_cz);
            append_chunk_mesh(r, scratch_chunk, mesh, dx, dz);
        }

    if(mesh->face_count >= mesh->capacity)
        printf("warning: mesh face capacity reached (%u)\n", mesh->capacity);
}

int main()
{
    current_pipeline_build = 0;
    VK_CHECK(volkInitialize());
    if(!is_instance_extension_supported("VK_KHR_wayland_surface"))
        glfwInitHint(GLFW_PLATFORM, GLFW_PLATFORM_X11);
    else
        glfwInitHint(GLFW_PLATFORM, GLFW_PLATFORM_WAYLAND);
    glfwInit();
    const char* dev_exts[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME, VK_EXT_ROBUSTNESS_2_EXTENSION_NAME};

    u32          glfw_ext_count = 0;
    const char** glfw_exts      = glfwGetRequiredInstanceExtensions(&glfw_ext_count);
    Renderer     renderer       = {0};

    {
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

        };

        renderer_create(&renderer, &desc);
        init_block_textures(&renderer);
        {
            GraphicsPipelineConfig cfg = pipeline_config_default();
            cfg.vert_path              = "compiledshaders/minimal_proc.vert.spv";
            cfg.frag_path              = "compiledshaders/minimal_proc.frag.spv";
            cfg.color_attachment_count = 1;
            cfg.color_formats          = &renderer.hdr_color[1].format;
            cfg.depth_test_enable      = false;
            cfg.depth_write_enable     = false;
            pipelines.fullscreen       = create_graphics_pipeline_cache(&renderer, &cfg);
        }
        pipelines.postprocess = create_compute_pipeline_cache(&renderer, "compiledshaders/postprocess.comp.spv");
        {
            GraphicsPipelineConfig cfg = pipeline_config_default();
            cfg.vert_path              = "compiledshaders/triangle.vert.spv";
            cfg.frag_path              = "compiledshaders/triangle.frag.spv";
            cfg.color_attachment_count = 1;
            cfg.color_formats          = &renderer.hdr_color[1].format;
            cfg.depth_format           = renderer.depth[1].format;
            cfg.polygon_mode           = VK_POLYGON_MODE_FILL;

            pipelines.triangle = create_graphics_pipeline_cache(&renderer, &cfg);
        }
        {
            GraphicsPipelineConfig cfg = pipeline_config_default();
            cfg.vert_path              = "compiledshaders/smaa_edge.vert.spv";
            cfg.frag_path              = "compiledshaders/smaa_edge.frag.spv";
            cfg.color_attachment_count = 1;
            cfg.color_formats          = &renderer.smaa_edges[1].format;

            pipelines.smaa_edge = create_graphics_pipeline_cache(&renderer, &cfg);
        }

        {
            GraphicsPipelineConfig cfg = pipeline_config_default();
            cfg.vert_path              = "compiledshaders/smaa_weight.vert.spv";
            cfg.frag_path              = "compiledshaders/smaa_weight.frag.spv";
            cfg.color_attachment_count = 1;
            cfg.color_formats          = &renderer.smaa_weights[1].format;

            pipelines.smaa_weight = create_graphics_pipeline_cache(&renderer, &cfg);
        }

        {
            GraphicsPipelineConfig cfg = pipeline_config_default();
            cfg.vert_path              = "compiledshaders/smaa_blend.vert.spv";
            cfg.frag_path              = "compiledshaders/smaa_blend.frag.spv";
            cfg.color_attachment_count = 1;
            cfg.color_formats          = &renderer.ldr_color[0].format;

            pipelines.smaa_blend = create_graphics_pipeline_cache(&renderer, &cfg);
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

            pipelines.beam = create_graphics_pipeline_cache(&renderer, &beam);

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


        {
        }
    }

    BufferPool pool = {0};
    buffer_pool_init(&renderer, &pool, MB(256),
                     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT,
                     VMA_MEMORY_USAGE_AUTO,
                     VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT, 2048);

    VkBufferDeviceAddressInfo addrInfo = {.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, .buffer = pool.buffer};
    VkDeviceAddress           base_addr = vkGetBufferDeviceAddress(renderer.device, &addrInfo);

    TextureID   tex_id         = load_texture(&renderer, "/home/lk/myprojects/flowgame/data/PNG/Tiles/greystone.png");
    BufferSlice indirect_slice = buffer_pool_alloc(&pool, sizeof(VkDrawIndirectCommand), 16);
    BufferSlice count_slice    = buffer_pool_alloc(&pool, sizeof(uint32_t), 4);
    VkDrawIndirectCommand* cpu_indirect = (VkDrawIndirectCommand*)indirect_slice.mapped;
    uint32_t*              cpu_count    = (uint32_t*)count_slice.mapped;

    // describe ONE draw call
    cpu_indirect[0].vertexCount   = 3;
    cpu_indirect[0].instanceCount = 1;
    cpu_indirect[0].firstVertex   = 0;
    cpu_indirect[0].firstInstance = 0;

    // number of draws
    *cpu_count = 1;
#define MAX_LIGHT_BEAM 64
    BufferSlice     light_beam = buffer_pool_alloc(&pool, MAX_LIGHT_BEAM * sizeof(LightBeam), 16);
    LightBeam*      cpu_beams  = (LightBeam*)light_beam.mapped;
    VkDeviceAddress beam_addr  = base_addr + light_beam.offset;


    u32 beam_count = 64;
    {
        const float beam_width  = 1.75f;
        const float beam_height = 14.0f;
        const float beam_alpha  = 1.25f;

        for(u32 i = 0; i < beam_count; ++i)
        {
            float x = 4.0f + (float)(i % 4) * 6.0f;
            float z = 2.0f + (float)(i / 4) * 6.0f;

            cpu_beams[i] = (LightBeam){
                .pos_width  = {x, 9.0f, z, beam_width},
                .sun_height = {0.12f, -1.0f, 0.08f, beam_height},
                .misc       = {beam_alpha, 0.0f, 0.0f, 0.0f},
            };
        }
    }

    /* device address */

    Camera cam = {

        .position   = {33.0f, 55.3f, 53.6f},
        .yaw        = glm_rad(5.7f),
        .pitch      = glm_rad(0.0f),
        .move_speed = 3.0f,
        .look_speed = 0.0025f,
        .fov_y      = glm_rad(75.0f),
        .near_z     = 0.05f,
        .far_z      = 2000.0f,

        .view_proj = GLM_MAT4_IDENTITY_INIT,
    };
    if(voxel_debug)
    {
        glm_vec3_copy((vec3){11.0f, 3.3f, 8.6f}, cam.position);
    }
    else
    {
        cam.position[0] = CHUNK_SIZE * 0.5f;
        cam.position[2] = CHUNK_SIZE * 0.5f;
    }
    glfwSetInputMode(renderer.window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);


    /* Push layout shared with older geometry shaders
         face_ptr=0, face_count=8, aspect=12, pad0=16, pad1=20, pad2=24, pad3=28,
         view_proj=32, texture_id=96, sampler_id=100 */
    // may be all push constant can be defined in one header that both gpu and cpu share
    PUSH_CONSTANT(Push, VkDeviceAddress face_ptr;  //8
                  uint  face_count;                //5
                  float aspect;                    //4
                  vec3  cam_pos;                   // camera world position
                  uint  pad1;                      // alignment
                  vec3  cam_dir;                   // camera forward (normalized)
                  uint  pad2;

                  float view_proj[4][4]; uint texture_id; uint sampler_id;

    );

    PUSH_CONSTANT(PostPush, uint32_t src_texture_id; uint32_t output_image_id; uint32_t sampler_id;

                  uint32_t width; uint32_t height;

                  uint frame;

    );

    PUSH_CONSTANT(EdgePush, uint32_t texture_id; uint32_t sampler_id;);


    PUSH_CONSTANT(BlendPush, uint32_t color_tex; uint32_t weight_tex; uint32_t sampler_id; uint32_t pad;);

    PUSH_CONSTANT(WeightPush, uint32_t edge_tex; uint32_t area_tex; uint32_t search_tex; uint32_t sampler_id;);
    PUSH_CONSTANT(Lightbeampush, VkDeviceAddress beam_ptr; uint64_t pad; float view_proj[4][4]; uint texture_id; uint sampler_id;

    );


    //  dmon_init();
    //    dmon_watch("shaders", watch_cb, DMON_WATCHFLAGS_RECURSIVE, NULL);

    uint32_t pp_frame_counter = 0;
    while(!glfwWindowShouldClose(renderer.window))
    {
        TracyCFrameMark;
        TracyCZoneN(frame_loop_zone, "Frame Loop", 1);

        TracyCZoneN(hot_reload_zone, "Hot Reload + Pipeline Rebuild", 1);
        if(shader_changed)
        {
            shader_changed = false;

            system("./compileslang.sh");

            mark_pipelines_dirty(changed_shader);
        }
        rebuild_dirty_pipelines(&renderer);
        TracyCZoneEnd(hot_reload_zone);

        TracyCZoneN(frame_start_zone, "frame_start", 1);
        frame_start(&renderer, &cam);
        TracyCZoneEnd(frame_start_zone);

        TracyCZoneN(streaming_zone, "Frame Loop", 1);
        if(!voxel_debug)
        {
        }
        TracyCZoneEnd(streaming_zone);

        VkCommandBuffer cmd        = renderer.frames[renderer.current_frame].cmdbuf;
        GpuProfiler*    frame_prof = &renderer.gpuprofiler[renderer.current_frame];

        uint32_t current_image = renderer.swapchain.current_image;
        TracyCZoneN(record_cmd_zone, "Record Command Buffer", 1);
        vk_cmd_begin(cmd, false);
        {
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, renderer.bindless_system.pipeline_layout, 0,
                                    1, &renderer.bindless_system.set, 0, NULL);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, renderer.bindless_system.pipeline_layout, 0, 1,
                                    &renderer.bindless_system.set, 0, NULL);
            rt_transition_all(cmd, &renderer.depth[renderer.swapchain.current_image], VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                              VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                              VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT);
            rt_transition_all(cmd, &renderer.hdr_color[renderer.swapchain.current_image], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                              VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);

            image_transition_swapchain(cmd, &renderer.swapchain, VK_IMAGE_LAYOUT_GENERAL,
                                       VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
        }

        VkRenderingAttachmentInfo color = {.sType     = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                                           .imageView = renderer.hdr_color[renderer.swapchain.current_image].view,
                                           .imageLayout =
                                               renderer.hdr_color[renderer.swapchain.current_image].mip_states[0].layout,
                                           .loadOp           = VK_ATTACHMENT_LOAD_OP_CLEAR,
                                           .storeOp          = VK_ATTACHMENT_STORE_OP_STORE,
                                           .clearValue.color = {{0.1f, 0.1f, 0.1f, 1.0f}}};
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
        gpu_profiler_begin_frame(frame_prof, cmd);
        {
            vkCmdBeginRendering(cmd, &rendering);
            GPU_SCOPE(frame_prof, cmd, "Main Pass", VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT)
            {
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, render_pipelines.pipelines[pipelines.triangle]);
                vk_cmd_set_viewport_scissor(cmd, renderer.swapchain.extent);

                Push push = {0};

                push.aspect     = (float)renderer.swapchain.extent.width / (float)renderer.swapchain.extent.height;
                push.texture_id = tex_id;
                push.sampler_id = renderer.default_samplers.samplers[SAMPLER_LINEAR_CLAMP];


                glm_vec3_copy(cam.cam_dir, push.cam_dir);
                glm_vec3_copy(cam.position, push.cam_pos);
                glm_mat4_copy(cam.view_proj, push.view_proj);  // this one was already correct

                vkCmdPushConstants(cmd, renderer.bindless_system.pipeline_layout, VK_SHADER_STAGE_ALL, 0, sizeof(Push), &push);

                vkCmdDrawIndirectCount(cmd, indirect_slice.buffer, indirect_slice.offset, count_slice.buffer,
                                       count_slice.offset, 1024, sizeof(VkDrawIndirectCommand));
            }

            //
            // {
            //     Lightbeampush push = {0};
            //     push.beam_ptr      = beam_addr;
            //     glm_mat4_copy(cam.view_proj, push.view_proj);
            //
            //     vkCmdPushConstants(cmd, renderer.bindless_system.pipeline_layout, VK_SHADER_STAGE_ALL, 0,
            //                        sizeof(Lightbeampush), &push);
            //     vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, render_pipelines.pipelines[pipelines.beam]);
            //     //6 vertices × beam_count instances
            //     vkCmdDraw(cmd,
            //               6,           // vertices per quad
            //               beam_count,  // instances
            //               0, 0);
            // }
            //
            // //
            vkCmdEndRendering(cmd);
        }


        GPU_SCOPE(frame_prof, cmd, "POST", VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT)
        {
            rt_transition_all(cmd, &renderer.hdr_color[renderer.swapchain.current_image], VK_IMAGE_LAYOUT_GENERAL,
                              VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT);


            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, render_pipelines.pipelines[pipelines.postprocess]);

            PostPush pp_push        = {0};
            pp_push.src_texture_id  = renderer.hdr_color[renderer.swapchain.current_image].bindless_index;
            pp_push.output_image_id = renderer.ldr_color[renderer.swapchain.current_image].bindless_index;
            pp_push.sampler_id      = renderer.default_samplers.samplers[SAMPLER_LINEAR_CLAMP];
            pp_push.width           = renderer.swapchain.extent.width;
            pp_push.height          = renderer.swapchain.extent.height;
            pp_push.frame           = pp_frame_counter++;
            vkCmdPushConstants(cmd, renderer.bindless_system.pipeline_layout, VK_SHADER_STAGE_ALL, 0, sizeof(PostPush), &pp_push);

            uint32_t gx = (pp_push.width + 15) / 16;
            uint32_t gy = (pp_push.height + 15) / 16;


            vkCmdDispatch(cmd, gx, gy, 1);
        }

        {
            rt_transition_all(cmd, &renderer.smaa_edges[current_image], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                              VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);

            VkRenderingAttachmentInfo color = {.sType            = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                                               .imageView        = renderer.smaa_edges[current_image].view,
                                               .imageLayout      = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                               .loadOp           = VK_ATTACHMENT_LOAD_OP_CLEAR,
                                               .storeOp          = VK_ATTACHMENT_STORE_OP_STORE,
                                               .clearValue.color = {{0, 0, 0, 0}}};

            VkRenderingInfo rendering = {.sType                = VK_STRUCTURE_TYPE_RENDERING_INFO,
                                         .renderArea.extent    = renderer.swapchain.extent,
                                         .layerCount           = 1,
                                         .colorAttachmentCount = 1,
                                         .pColorAttachments    = &color};

            vkCmdBeginRendering(cmd, &rendering);

            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, render_pipelines.pipelines[pipelines.smaa_edge]);

            vk_cmd_set_viewport_scissor(cmd, renderer.swapchain.extent);

            EdgePush push   = {0};
            push.texture_id = renderer.ldr_color[current_image].bindless_index;
            push.sampler_id = renderer.default_samplers.samplers[SAMPLER_LINEAR_CLAMP];

            vkCmdPushConstants(cmd, renderer.bindless_system.pipeline_layout, VK_SHADER_STAGE_ALL, 0, sizeof(EdgePush), &push);

            vkCmdDraw(cmd, 3, 1, 0, 0);

            vkCmdEndRendering(cmd);
        }
        {
            rt_transition_all(cmd, &renderer.smaa_weights[current_image], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                              VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
            rt_transition_all(cmd, &renderer.smaa_edges[current_image], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                              VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
            VkRenderingAttachmentInfo color = {.sType            = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                                               .imageView        = renderer.smaa_weights[current_image].view,
                                               .imageLayout      = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                               .loadOp           = VK_ATTACHMENT_LOAD_OP_CLEAR,
                                               .storeOp          = VK_ATTACHMENT_STORE_OP_STORE,
                                               .clearValue.color = {{0, 0, 0, 0}}};

            VkRenderingInfo rendering = {.sType                = VK_STRUCTURE_TYPE_RENDERING_INFO,
                                         .renderArea.extent    = renderer.swapchain.extent,
                                         .layerCount           = 1,
                                         .colorAttachmentCount = 1,
                                         .pColorAttachments    = &color};

            vkCmdBeginRendering(cmd, &rendering);

            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, render_pipelines.pipelines[pipelines.smaa_weight]);


            vk_cmd_set_viewport_scissor(cmd, renderer.swapchain.extent);
            WeightPush push = {0};

            push.edge_tex   = renderer.smaa_edges[current_image].bindless_index;
            push.area_tex   = renderer.smaa_area_tex;
            push.search_tex = renderer.smaa_search_tex;
            push.sampler_id = renderer.default_samplers.samplers[SAMPLER_LINEAR_CLAMP];

            vkCmdPushConstants(cmd, renderer.bindless_system.pipeline_layout, VK_SHADER_STAGE_ALL, 0, sizeof(WeightPush), &push);

            vkCmdDraw(cmd, 3, 1, 0, 0);

            vkCmdEndRendering(cmd);
        }

        rt_transition_all(cmd, &renderer.ldr_color[current_image], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                          VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);


        {
            VkRenderingAttachmentInfo color = {.sType       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                                               .imageView   = renderer.ldr_color[current_image].view,
                                               .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                               .loadOp      = VK_ATTACHMENT_LOAD_OP_LOAD,
                                               .storeOp     = VK_ATTACHMENT_STORE_OP_STORE};

            VkRenderingInfo rendering = {.sType                = VK_STRUCTURE_TYPE_RENDERING_INFO,
                                         .renderArea.extent    = renderer.swapchain.extent,
                                         .layerCount           = 1,
                                         .colorAttachmentCount = 1,
                                         .pColorAttachments    = &color};

            vkCmdBeginRendering(cmd, &rendering);

            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, render_pipelines.pipelines[pipelines.smaa_blend]);


            vk_cmd_set_viewport_scissor(cmd, renderer.swapchain.extent);

            BlendPush push  = {0};
            push.color_tex  = renderer.ldr_color[current_image].bindless_index;
            push.weight_tex = renderer.smaa_weights[current_image].bindless_index;
            push.sampler_id = renderer.default_samplers.samplers[SAMPLER_LINEAR_CLAMP];

            vkCmdPushConstants(cmd, renderer.bindless_system.pipeline_layout, VK_SHADER_STAGE_ALL, 0, sizeof(BlendPush), &push);

            vkCmdDraw(cmd, 3, 1, 0, 0);


            vkCmdEndRendering(cmd);
        }

        rt_transition_all(cmd, &renderer.ldr_color[renderer.swapchain.current_image], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                          VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
        image_transition_swapchain(renderer.frames[renderer.current_frame].cmdbuf, &renderer.swapchain,
                                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_TRANSFER_BIT, 0);

        VkImageBlit blit = {
            .srcSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .mipLevel = 0, .baseArrayLayer = 0, .layerCount = 1},
            .srcOffsets = {{0, 0, 0}, {renderer.swapchain.extent.width, renderer.swapchain.extent.height, 1}},

            .dstSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .mipLevel = 0, .baseArrayLayer = 0, .layerCount = 1},
            .dstOffsets = {{0, 0, 0}, {renderer.swapchain.extent.width, renderer.swapchain.extent.height, 1}}};

        vkCmdBlitImage(cmd, renderer.ldr_color[renderer.swapchain.current_image].image,
                       VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, renderer.swapchain.images[renderer.swapchain.current_image],
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_NEAREST);


        {
            image_transition_swapchain(cmd, &renderer.swapchain, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                       VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);


            VkRenderingAttachmentInfo color = {.sType       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                                               .imageView   = renderer.swapchain.image_views[current_image],
                                               .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                               .loadOp      = VK_ATTACHMENT_LOAD_OP_LOAD,
                                               .storeOp     = VK_ATTACHMENT_STORE_OP_STORE};

            VkRenderingInfo rendering = {.sType                = VK_STRUCTURE_TYPE_RENDERING_INFO,
                                         .renderArea.extent    = renderer.swapchain.extent,
                                         .layerCount           = 1,
                                         .colorAttachmentCount = 1,
                                         .pColorAttachments    = &color};

            vkCmdBeginRendering(cmd, &rendering);
            {
                ImDrawData* draw_data = igGetDrawData();
                ImGui_ImplVulkan_RenderDrawData(draw_data, cmd, VK_NULL_HANDLE);
            }
            vkCmdEndRendering(cmd);
        }


        if(take_screenshot)
        {
            renderer_record_screenshot(&renderer, cmd);
            take_screenshot = false;
        }
        image_transition_swapchain(renderer.frames[renderer.current_frame].cmdbuf, &renderer.swapchain,
                                   VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, 0);


        vk_cmd_end(renderer.frames[renderer.current_frame].cmdbuf);
        TracyCZoneEnd(record_cmd_zone);


        TracyCZoneN(submit_zone, "Submit + Present", 1);
        submit_frame(&renderer);
        TracyCZoneEnd(submit_zone);

        if(take_screenshot)
        {
            renderer_save_screenshot(&renderer);
        }

        TracyCZoneEnd(frame_loop_zone);
    }


    printf(" renderer size is %zu", sizeof(Renderer));
    printf("Push size = %zu\n", alignof(Camera));
    printf("Push size = %zu\n", alignof(Push));
    printf("view_proj offset = %zu\n", offsetof(Push, view_proj));
    printf(" pushis %zu    ", alignof(Push));
    //    ANALYZE_STRUCT(ImageState);
    //renderer_destroy(&renderer);
    return 0;
}
