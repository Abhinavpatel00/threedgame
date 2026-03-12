
/*
because we would already have allocated a lot of indexes before we can  allocate texture for meshes or voxels 
its not feasible to send texture index via voxel or mesh data in desired packed index so its better to pack material type from material id pool which contain material index either in enum form or just from flow id pool 
in this way material data can easily be packed in very compact form 

like this for 32 volume chunks


// 31   26   21   16   13   10   7        0
// +----+----+----+----+----+----+-------------------+
// | x  | y  | z  | n  | h  | w  |material id        |
// +----+----+----+----+----+----+-------------------+
//  5b   5b   5b   3b   4b   4b      6b

we can sacrifice some precision from h ,w because just rendering few triangle more might overthrow the cost of having to send extra 32 bytes uint just for material i am not sure we might do profiling later

but for starting above thing looks good

*/
#include <unistd.h>
typedef enum
{
    VOXEL_AIR = 0,
    VOXEL_STONE,
    VOXEL_GRASS,
    VOXEL_DIRT,
    VOXEL_SAND,
    VOXEL_CYAN_WOOL,
    VOXEL_GOLD_ORE,
    VOXEL_IRON_ORE,
    VOXEL_DIAMOND_ORE,
    VOXEL_COAL_BLOCK,
    VOXEL_IRON_BLOCK,
    VOXEL_GOLD_BLOCK,
    VOXEL_DIAMOND_BLOCK,
    VOXEL_EMERALD_BLOCK,

    VOXEL_COUNT

} VoxelType;

typedef enum
{
    FACE_POS_X = 0,
    FACE_NEG_X = 1,
    FACE_POS_Y = 2,
    FACE_NEG_Y = 3,
    FACE_POS_Z = 4,
    FACE_NEG_Z = 5
} FaceDir;


typedef struct
{
    VoxelType type;
} Voxel;

typedef struct
{
    const char* face_tex[3];
    const char* debug_name;
} VoxelMaterial;
/*
face_tex[0] = SIDE
face_tex[1] = TOP
face_tex[2] = BOTTOM
        TOP
         ▲
         │
 SIDE ─ voxel ─ SIDE
         │
         ▼
       BOTTOM
*/
enum
{
    TEX_SIDE   = 0,
    TEX_TOP    = 1,
    TEX_BOTTOM = 2
};
#include <stdint.h>


// well its fine becuase its not a lot of materials any way so no compression here
typedef struct
{
    uint32_t tex_side;
    uint32_t tex_top;
    uint32_t tex_bottom;
} GPUMaterial;


#define CUBE(tex) {tex, tex, tex}

#define TOP_BOTTOM(side, top, bottom) {side, top, bottom}
VoxelMaterial voxel_materials[VOXEL_COUNT] = {
    [VOXEL_AIR] = {.face_tex = {NULL, NULL, NULL}, .debug_name = "AIR"},

    [VOXEL_STONE] = {.face_tex = CUBE("data/PNG/Tiles/stone.png"), .debug_name = "STONE"},

    [VOXEL_GRASS] = {.face_tex = TOP_BOTTOM("data/block/grass_block_side.png", "data/block/grass_block_top.png", "data/block/dirt.png"),
                     .debug_name = "GRASS"},

    [VOXEL_DIRT] = {.face_tex = CUBE("data/PNG/Tiles/dirt.png"), .debug_name = "DIRT"},


    [VOXEL_IRON_ORE]      = {.face_tex = CUBE("data/block/iron_ore.png"), .debug_name = "IRON_ORE"},
    [VOXEL_GOLD_ORE]      = {.face_tex = CUBE("data/block/gold_ore.png"), .debug_name = "GOLD_ORE"},
    [VOXEL_DIAMOND_ORE]   = {.face_tex = CUBE("data/block/diamond_ore.png"), .debug_name = "DIAMOND_ORE"},
    [VOXEL_COAL_BLOCK]    = {.face_tex = CUBE("data/block/coal_block.png"), .debug_name = "COAL_BLOCK"},
    [VOXEL_IRON_BLOCK]    = {.face_tex = CUBE("data/block/iron_block.png"), .debug_name = "IRON_BLOCK"},
    [VOXEL_GOLD_BLOCK]    = {.face_tex = CUBE("data/block/gold_block.png"), .debug_name = "GOLD_BLOCK"},
    [VOXEL_DIAMOND_BLOCK] = {.face_tex = CUBE("data/block/diamond_block.png"), .debug_name = "DIAMOND_BLOCK"},
    [VOXEL_EMERALD_BLOCK] = {.face_tex = CUBE("data/block/emerald_block.png"), .debug_name = "EMERALD_BLOCK"},


};


// typedef enum
// {
//     VOXEL_AIR = 0,
//
//     // terrain
//     STONE,
//     GRASS,
//     DIRT,
//     SAND,
//     GRAVEL,
//     CLAY,
//
//     // stone variants
//     COBBLESTONE,
//     MOSSY_COBBLESTONE,
//     STONE_BRICKS,
//     CRACKED_STONE_BRICKS,
//
//     // ores
//     COAL_ORE,
//     IRON_ORE,
//     GOLD_ORE,
//     DIAMOND_ORE,
//     REDSTONE_ORE,
//     EMERALD_ORE,
//
//     // blocks
//     COAL_BLOCK,
//     IRON_BLOCK,
//     GOLD_BLOCK,
//     DIAMOND_BLOCK,
//     EMERALD_BLOCK,
//
//     // wood
//     OAK_LOG,
//     BIRCH_LOG,
//     SPRUCE_LOG,
//     JUNGLE_LOG,
//     ACACIA_LOG,
//     DARK_OAK_LOG,
//
//     OAK_PLANKS,
//     BIRCH_PLANKS,
//     SPRUCE_PLANKS,
//     JUNGLE_PLANKS,
//     ACACIA_PLANKS,
//     DARK_OAK_PLANKS,
//
//     OAK_LEAVES,
//     BIRCH_LEAVES,
//     SPRUCE_LEAVES,
//     JUNGLE_LEAVES,
//     ACACIA_LEAVES,
//     DARK_OAK_LEAVES,
//
//     // decorative
//     BRICKS,
//     BOOKSHELF,
//     CRAFTING_TABLE,
//     FURNACE,
//     TNT,
//
//     // minerals
//     OBSIDIAN,
//     BEDROCK,
//
//     // added from data.txt
//     ANDESITE,
//     DIORITE,
//     GRANITE,
//
//     ACACIA_TRAPDOOR,
//     BIRCH_TRAPDOOR,
//     DARK_OAK_TRAPDOOR,
//
//     BLACK_CONCRETE,
//     BLUE_CONCRETE,
//     BROWN_CONCRETE,
//     CYAN_CONCRETE,
//     GRAY_CONCRETE,
//     GREEN_CONCRETE,
//     LIGHT_BLUE_CONCRETE,
//     LIME_CONCRETE,
//     MAGENTA_CONCRETE,
//     ORANGE_CONCRETE,
//     PINK_CONCRETE,
//     PURPLE_CONCRETE,
//     RED_CONCRETE,
//     SILVER_CONCRETE,
//     WHITE_CONCRETE,
//     YELLOW_CONCRETE,
//
//     BLACK_WOOL,
//     BLUE_WOOL,
//     BROWN_WOOL,
//     CYAN_WOOL,
//     GRAY_WOOL,
//     GREEN_WOOL,
//     LIGHT_BLUE_WOOL,
//     LIME_WOOL,
//     MAGENTA_WOOL,
//     ORANGE_WOOL,
//     PINK_WOOL,
//     PURPLE_WOOL,
//     RED_WOOL,
//     SILVER_WOOL,
//     WHITE_WOOL,
//     YELLOW_WOOL,
//
//     BLACK_TERRACOTTA,
//     BLUE_TERRACOTTA,
//     BROWN_TERRACOTTA,
//     CYAN_TERRACOTTA,
//     GRAY_TERRACOTTA,
//     GREEN_TERRACOTTA,
//     LIGHT_BLUE_TERRACOTTA,
//     LIME_TERRACOTTA,
//     MAGENTA_TERRACOTTA,
//     ORANGE_TERRACOTTA,
//     PINK_TERRACOTTA,
//     PURPLE_TERRACOTTA,
//     RED_TERRACOTTA,
//     SILVER_TERRACOTTA,
//     WHITE_TERRACOTTA,
//     YELLOW_TERRACOTTA,
//
//     VOXEL_COUNT
//
// } VoxelType;
//
// +X → side
// -X → side
// +Z → side
// -Z → side
// +Y → top
// -Y → bottom
//

// clang-format off

// #define CUBE(tex) {tex, tex, tex, tex, tex, tex}
// #define TOP_BOTTOM(side, top, bottom) \
// {side, side, top, bottom, side, side}
//
// #define SIDE_TOP_BOTTOM(side, top, bottom) \
// {side, side, side, side, top, bottom}
// VoxelMaterial voxel_materials[VOXEL_COUNT] =
// {
//
// [VOXEL_AIR] =
// {
//     .debug_name = "AIR"
// },
//
// // terrain
//
// [STONE] = { .face_tex = CUBE("data/PNG/Tiles/stone_diamond.png"), .debug_name = "STONE" },
// [GRASS] = { .face_tex = TOP_BOTTOM(
// 	//	"data/block/grass_block_side.png",
//
// 		"data/PNG/Tiles/stone_grass.png",
//
// 		//"data/block/grass_block_top.png",
//
// 		"data/PNG/Tiles/grass_top.png",
// 		"data/PNG/Tiles/dirt.png"
// 		), .debug_name = "GRASS" },
// [DIRT] = { .face_tex = CUBE("data/block/dirt.png"), .debug_name = "DIRT" },
// [SAND] = { .face_tex = CUBE("data/block/sand.png"), .debug_name = "SAND" },
// [GRAVEL] = { .face_tex = CUBE("data/block/gravel.png"), .debug_name = "GRAVEL" },
// [CLAY] = { .face_tex = CUBE("data/block/clay.png"), .debug_name = "CLAY" },
//
// // stone variants
// [COBBLESTONE] = { .face_tex = CUBE("data/block/cobblestone.png"), .debug_name = "COBBLESTONE" },
// [MOSSY_COBBLESTONE] = { .face_tex = CUBE("data/block/mossy_cobblestone.png"), .debug_name = "MOSSY_COBBLESTONE" },
// [STONE_BRICKS] = { .face_tex = CUBE("data/block/stone_bricks.png"), .debug_name = "STONE_BRICKS" },
// [CRACKED_STONE_BRICKS] = { .face_tex = CUBE("data/block/cracked_stone_bricks.png"), .debug_name = "CRACKED_STONE_BRICKS" },
//
// // ores
// [COAL_ORE] = { .face_tex = CUBE("data/block/coal_ore.png"), .debug_name = "COAL_ORE" },
// [IRON_ORE] = { .face_tex = CUBE("data/block/iron_ore.png"), .debug_name = "IRON_ORE" },
// [GOLD_ORE] = { .face_tex = CUBE("data/block/gold_ore.png"), .debug_name = "GOLD_ORE" },
// [DIAMOND_ORE] = { .face_tex = CUBE("data/block/diamond_ore.png"), .debug_name = "DIAMOND_ORE" },
// [REDSTONE_ORE] = { .face_tex = CUBE("data/block/redstone_ore.png"), .debug_name = "REDSTONE_ORE" },
// [EMERALD_ORE] = { .face_tex = CUBE("data/block/emerald_ore.png"), .debug_name = "EMERALD_ORE" },
//
// // blocks
// [COAL_BLOCK] = { .face_tex = CUBE("data/block/coal_block.png"), .debug_name = "COAL_BLOCK" },
// [IRON_BLOCK] = { .face_tex = CUBE("data/block/iron_block.png"), .debug_name = "IRON_BLOCK" },
// [GOLD_BLOCK] = { .face_tex = CUBE("data/block/gold_block.png"), .debug_name = "GOLD_BLOCK" },
// [DIAMOND_BLOCK] = { .face_tex = CUBE("data/block/diamond_block.png"), .debug_name = "DIAMOND_BLOCK" },
// [EMERALD_BLOCK] = { .face_tex = CUBE("data/block/emerald_block.png"), .debug_name = "EMERALD_BLOCK" },
//
// // wood
// [OAK_LOG] = { .face_tex = TOP_BOTTOM("data/block/oak_log.png", "data/block/oak_log_top.png", "data/block/oak_log_top.png"), .debug_name = "OAK_LOG" },
// [BIRCH_LOG] = { .face_tex = TOP_BOTTOM("data/block/birch_log.png", "data/block/birch_log_top.png", "data/block/birch_log_top.png"), .debug_name = "BIRCH_LOG" },
// [SPRUCE_LOG] = { .face_tex = TOP_BOTTOM("data/block/spruce_log.png", "data/block/spruce_log_top.png", "data/block/spruce_log_top.png"), .debug_name = "SPRUCE_LOG" },
// [JUNGLE_LOG] = { .face_tex = TOP_BOTTOM("data/block/jungle_log.png", "data/block/jungle_log_top.png", "data/block/jungle_log_top.png"), .debug_name = "JUNGLE_LOG" },
// [ACACIA_LOG] = { .face_tex = TOP_BOTTOM("data/block/acacia_log.png", "data/block/acacia_log_top.png", "data/block/acacia_log_top.png"), .debug_name = "ACACIA_LOG" },
// [DARK_OAK_LOG] = { .face_tex = TOP_BOTTOM("data/block/dark_oak_log.png", "data/block/dark_oak_log_top.png", "data/block/dark_oak_log_top.png"), .debug_name = "DARK_OAK_LOG" },
//
// [OAK_PLANKS] = { .face_tex = CUBE("data/block/oak_planks.png"), .debug_name = "OAK_PLANKS" },
// [BIRCH_PLANKS] = { .face_tex = CUBE("data/block/birch_planks.png"), .debug_name = "BIRCH_PLANKS" },
// [SPRUCE_PLANKS] = { .face_tex = CUBE("data/block/spruce_planks.png"), .debug_name = "SPRUCE_PLANKS" },
// [JUNGLE_PLANKS] = { .face_tex = CUBE("data/block/jungle_planks.png"), .debug_name = "JUNGLE_PLANKS" },
// [ACACIA_PLANKS] = { .face_tex = CUBE("data/block/acacia_planks.png"), .debug_name = "ACACIA_PLANKS" },
// [DARK_OAK_PLANKS] = { .face_tex = CUBE("data/block/dark_oak_planks.png"), .debug_name = "DARK_OAK_PLANKS" },
//
// [OAK_LEAVES] = { .face_tex = CUBE("data/block/oak_leaves.png"), .debug_name = "OAK_LEAVES" },
// [BIRCH_LEAVES] = { .face_tex = CUBE("data/block/birch_leaves.png"), .debug_name = "BIRCH_LEAVES" },
// [SPRUCE_LEAVES] = { .face_tex = CUBE("data/block/spruce_leaves.png"), .debug_name = "SPRUCE_LEAVES" },
// [JUNGLE_LEAVES] = { .face_tex = CUBE("data/block/jungle_leaves.png"), .debug_name = "JUNGLE_LEAVES" },
// [ACACIA_LEAVES] = { .face_tex = CUBE("data/block/acacia_leaves.png"), .debug_name = "ACACIA_LEAVES" },
// [DARK_OAK_LEAVES] = { .face_tex = CUBE("data/block/dark_oak_leaves.png"), .debug_name = "DARK_OAK_LEAVES" },
//
// // decorative
// [BRICKS] = { .face_tex = CUBE("data/block/bricks.png"), .debug_name = "BRICKS" },
// [BOOKSHELF] = { .face_tex = TOP_BOTTOM("data/block/bookshelf.png", "data/block/bookshelf_top.png", "data/block/bookshelf_top.png"), .debug_name = "BOOKSHELF" },
// [CRAFTING_TABLE] = {
//     .face_tex = {
//         "data/block/crafting_table_side.png",
//         "data/block/crafting_table_side.png",
//         "data/block/crafting_table_top.png",
//         "data/block/crafting_table_top.png", // using top for bottom as well
//         "data/block/crafting_table_side.png",
//         "data/block/crafting_table_side.png"
//     },
//     .debug_name = "CRAFTING_TABLE"
// },
// [FURNACE] = {
//     .face_tex = {
//         "data/block/furnace_front.png",
//         "data/block/furnace_side.png",
//         "data/block/furnace_top.png",
//         "data/block/furnace_top.png",
//         "data/block/furnace_side.png",
//         "data/block/furnace_side.png"
//     },
//     .debug_name = "FURNACE"
// },
// [TNT] = { .face_tex = TOP_BOTTOM("data/block/tnt_side.png", "data/block/tnt_top.png", "data/block/tnt_bottom.png"), .debug_name = "TNT" },
//
// // minerals
// [OBSIDIAN] = { .face_tex = CUBE("data/block/obsidian.png"), .debug_name = "OBSIDIAN" },
// [BEDROCK] = { .face_tex = CUBE("data/block/bedrock.png"), .debug_name = "BEDROCK" },
//
// // Added from data.txt
// [ANDESITE] = { .face_tex = CUBE("data/block/andesite.png"), .debug_name = "ANDESITE" },
// [DIORITE] = { .face_tex = CUBE("data/block/diorite.png"), .debug_name = "DIORITE" },
// [GRANITE] = { .face_tex = CUBE("data/block/granite.png"), .debug_name = "GRANITE" },
//
// [ACACIA_TRAPDOOR] = { .face_tex = CUBE("data/block/acacia_trapdoor.png"), .debug_name = "ACACIA_TRAPDOOR" },
// [BIRCH_TRAPDOOR] = { .face_tex = CUBE("data/block/birch_trapdoor.png"), .debug_name = "BIRCH_TRAPDOOR" },
// [DARK_OAK_TRAPDOOR] = { .face_tex = CUBE("data/block/dark_oak_trapdoor.png"), .debug_name = "DARK_OAK_TRAPDOOR" },
//
// [BLACK_CONCRETE] = { .face_tex = CUBE("data/block/black_concrete.png"), .debug_name = "BLACK_CONCRETE" },
// [BLUE_CONCRETE] = { .face_tex = CUBE("data/block/blue_concrete.png"), .debug_name = "BLUE_CONCRETE" },
// [BROWN_CONCRETE] = { .face_tex = CUBE("data/block/brown_concrete.png"), .debug_name = "BROWN_CONCRETE" },
// [CYAN_CONCRETE] = { .face_tex = CUBE("data/block/cyan_concrete.png"), .debug_name = "CYAN_CONCRETE" },
// [GRAY_CONCRETE] = { .face_tex = CUBE("data/block/gray_concrete.png"), .debug_name = "GRAY_CONCRETE" },
// [GREEN_CONCRETE] = { .face_tex = CUBE("data/block/green_concrete.png"), .debug_name = "GREEN_CONCRETE" },
// [LIGHT_BLUE_CONCRETE] = { .face_tex = CUBE("data/block/light_blue_concrete.png"), .debug_name = "LIGHT_BLUE_CONCRETE" },
// [LIME_CONCRETE] = { .face_tex = CUBE("data/block/lime_concrete.png"), .debug_name = "LIME_CONCRETE" },
// [MAGENTA_CONCRETE] = { .face_tex = CUBE("data/block/magenta_concrete.png"), .debug_name = "MAGENTA_CONCRETE" },
// [ORANGE_CONCRETE] = { .face_tex = CUBE("data/block/orange_concrete.png"), .debug_name = "ORANGE_CONCRETE" },
// [PINK_CONCRETE] = { .face_tex = CUBE("data/block/pink_concrete.png"), .debug_name = "PINK_CONCRETE" },
// [PURPLE_CONCRETE] = { .face_tex = CUBE("data/block/purple_concrete.png"), .debug_name = "PURPLE_CONCRETE" },
// [RED_CONCRETE] = { .face_tex = CUBE("data/block/red_concrete.png"), .debug_name = "RED_CONCRETE" },
// [SILVER_CONCRETE] = { .face_tex = CUBE("data/block/silver_concrete.png"), .debug_name = "SILVER_CONCRETE" },
// [WHITE_CONCRETE] = { .face_tex = CUBE("data/block/white_concrete.png"), .debug_name = "WHITE_CONCRETE" },
// [YELLOW_CONCRETE] = { .face_tex = CUBE("data/block/yellow_concrete.png"), .debug_name = "YELLOW_CONCRETE" },
//
// [BLACK_WOOL] = { .face_tex = CUBE("data/block/black_wool.png"), .debug_name = "BLACK_WOOL" },
// [BLUE_WOOL] = { .face_tex = CUBE("data/block/blue_wool.png"), .debug_name = "BLUE_WOOL" },
// [BROWN_WOOL] = { .face_tex = CUBE("data/block/brown_wool.png"), .debug_name = "BROWN_WOOL" },
// [CYAN_WOOL] = { .face_tex = CUBE("data/block/cyan_wool.png"), .debug_name = "CYAN_WOOL" },
// [GRAY_WOOL] = { .face_tex = CUBE("data/block/gray_wool.png"), .debug_name = "GRAY_WOOL" },
// [GREEN_WOOL] = { .face_tex = CUBE("data/block/green_wool.png"), .debug_name = "GREEN_WOOL" },
// [LIGHT_BLUE_WOOL] = { .face_tex = CUBE("data/block/light_blue_wool.png"), .debug_name = "LIGHT_BLUE_WOOL" },
// [LIME_WOOL] = { .face_tex = CUBE("data/block/lime_wool.png"), .debug_name = "LIME_WOOL" },
// [MAGENTA_WOOL] = { .face_tex = CUBE("data/block/magenta_wool.png"), .debug_name = "MAGENTA_WOOL" },
// [ORANGE_WOOL] = { .face_tex = CUBE("data/block/orange_wool.png"), .debug_name = "ORANGE_WOOL" },
// [PINK_WOOL] = { .face_tex = CUBE("data/block/pink_wool.png"), .debug_name = "PINK_WOOL" },
// [PURPLE_WOOL] = { .face_tex = CUBE("data/block/purple_wool.png"), .debug_name = "PURPLE_WOOL" },
// [RED_WOOL] = { .face_tex = CUBE("data/block/red_wool.png"), .debug_name = "RED_WOOL" },
// [SILVER_WOOL] = { .face_tex = CUBE("data/block/light_gray_wool.png"), .debug_name = "SILVER_WOOL" },
// [WHITE_WOOL] = { .face_tex = CUBE("data/block/white_wool.png"), .debug_name = "WHITE_WOOL" },
// [YELLOW_WOOL] = { .face_tex = CUBE("data/block/yellow_wool.png"), .debug_name = "YELLOW_WOOL" },
//
// [BLACK_TERRACOTTA] = { .face_tex = CUBE("data/block/black_terracotta.png"), .debug_name = "BLACK_TERRACOTTA" },
// [BLUE_TERRACOTTA] = { .face_tex = CUBE("data/block/blue_terracotta.png"), .debug_name = "BLUE_TERRACOTTA" },
// [BROWN_TERRACOTTA] = { .face_tex = CUBE("data/block/brown_terracotta.png"), .debug_name = "BROWN_TERRACOTTA" },
// [CYAN_TERRACOTTA] = { .face_tex = CUBE("data/block/cyan_terracotta.png"), .debug_name = "CYAN_TERRACOTTA" },
// [GRAY_TERRACOTTA] = { .face_tex = CUBE("data/block/gray_terracotta.png"), .debug_name = "GRAY_TERRACOTTA" },
// [GREEN_TERRACOTTA] = { .face_tex = CUBE("data/block/green_terracotta.png"), .debug_name = "GREEN_TERRACOTTA" },
// [LIGHT_BLUE_TERRACOTTA] = { .face_tex = CUBE("data/block/light_blue_terracotta.png"), .debug_name = "LIGHT_BLUE_TERRACOTTA" },
// [LIME_TERRACOTTA] = { .face_tex = CUBE("data/block/lime_terracotta.png"), .debug_name = "LIME_TERRACOTTA" },
// [MAGENTA_TERRACOTTA] = { .face_tex = CUBE("data/block/magenta_terracotta.png"), .debug_name = "MAGENTA_TERRACOTTA" },
// [ORANGE_TERRACOTTA] = { .face_tex = CUBE("data/block/orange_terracotta.png"), .debug_name = "ORANGE_TERRACOTTA" },
// [PINK_TERRACOTTA] = { .face_tex = CUBE("data/block/pink_terracotta.png"), .debug_name = "PINK_TERRACOTTA" },
// [PURPLE_TERRACOTTA] = { .face_tex = CUBE("data/block/purple_terracotta.png"), .debug_name = "PURPLE_TERRACOTTA" },
// [RED_TERRACOTTA] = { .face_tex = CUBE("data/block/red_terracotta.png"), .debug_name = "RED_TERRACOTTA" },
// [SILVER_TERRACOTTA] = { .face_tex = CUBE("data/block/light_gray_terracotta.png"), .debug_name = "SILVER_TERRACOTTA" },
// [WHITE_TERRACOTTA] = { .face_tex = CUBE("data/block/white_terracotta.png"), .debug_name = "WHITE_TERRACOTTA" },
// [YELLOW_TERRACOTTA] = { .face_tex = CUBE("data/block/yellow_terracotta.png"), .debug_name = "YELLOW_TERRACOTTA" },
//
// };
//

// clang-format on
