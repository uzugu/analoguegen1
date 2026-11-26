#include "gb_brain.h"
#include "gb_audio.h"
#include "pokered_ram.h"

#ifndef OF_PC
#include "of_file.h"
#endif

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ============================================================================
 * Verified Pokemon Red (US) 1 MB ROM table offsets
 * ========================================================================== */

#define ROM_MAP_HEADER_PTRS   0x001AEu  /* 248 * uint16_t LE in bank 0 */
#define ROM_MAP_HEADER_BANKS  0x0C23Du  /* 248 * uint8_t */
#define ROM_TILESETS_TABLE    0x0C7BEu  /* 12 bytes per tileset */
#define ROM_WILD_DATA_PTRS    0x0CEEBu  /* 248 * uint16_t LE in bank 0x03 */
#define ROM_SPRITE_SHEETS     0x17B27u  /* 4 bytes per sprite 1..72 */

#define MAX_MAPS              248u
#define MAX_WARPS             32u
#define MAX_TILESET_COLL      64u
#define MAX_VRAM_SPRITE_TILES 124u

#define MAP_OVERWORLD_W_LIMIT 64u
#define MAP_OVERWORLD_BYTES   (GB_ADDR_WALK_COUNTER - GB_ADDR_OVERWORLD_MAP)

#define RGB565_C(r8, g8, b8) \
    ((uint16_t)((((r8) & 0xF8u) << 8) | (((g8) & 0xFCu) << 3) | (((b8) & 0xF8u) >> 3)))

/* ============================================================================
 * Synthetic GB memory + ROM state
 * ========================================================================== */

static uint8_t   s_wram[0x2000];
static uint8_t   s_vram[0x2000];
static uint8_t   s_io[0x100];
static uint16_t  s_lcd_fb[GB_LCD_W * GB_LCD_H];

static uint8_t  *s_rom_data = NULL;
static size_t    s_rom_size = 0u;
static uint8_t   s_rom_bank = 1u;
static bool      s_initialized = false;

/* ============================================================================
 * Input state
 * ========================================================================== */

static gb_input_t s_in_cur;
static gb_input_t s_in_prev;

/* ============================================================================
 * Overworld map / warp / connection state
 * ========================================================================== */

typedef struct {
    bool     active;
    uint8_t  dst_map_id;
    uint8_t  dst_warp_id;
    uint8_t  dst_map_w;
    int8_t   y_align;
    int8_t   x_align;
} map_conn_t;

typedef struct {
    uint8_t y;
    uint8_t x;
    uint8_t dst_warp_id;
    uint8_t dst_map_id;
} map_warp_t;

enum {
    CONN_N = 0,
    CONN_S = 1,
    CONN_W = 2,
    CONN_E = 3
};

static uint8_t    s_cur_map = 0u;
static uint8_t    s_tileset_id = 0u;
static uint8_t    s_map_w = 0u;
static uint8_t    s_map_h = 0u;
static uint8_t    s_ts_bank = 0u;
static uint16_t   s_ts_blocks_ptr = 0u;
static uint16_t   s_ts_gfx_ptr = 0u;
static uint16_t   s_ts_coll_ptr = 0u;
static uint8_t    s_grass_tile = 0xFFu;
static bool       s_walkable[256];

static uint8_t    s_player_x = 5u;
static uint8_t    s_player_y = 6u;
static uint8_t    s_prev_x = 5u;
static uint8_t    s_prev_y = 6u;
static uint8_t    s_facing_dir = 0u; /* 0=S, 4=N, 8=W, 12=E */
static uint8_t    s_walk_counter = 0u;
static int8_t     s_walk_dx = 0;
static int8_t     s_walk_dy = 0;

static uint8_t    s_last_outdoor_map = 0u;
static uint8_t    s_last_outdoor_x = 5u;
static uint8_t    s_last_outdoor_y = 6u;

static map_conn_t s_conns[4];
static map_warp_t s_warps[MAX_WARPS];
static uint8_t    s_num_warps = 0u;

static uint32_t   s_rng = 0x504F4B45u;

/* ============================================================================
 * Party & native battle state
 * ========================================================================== */

typedef struct {
    uint8_t species;
    uint8_t level;
    uint16_t hp;
    uint16_t max_hp;
    uint8_t moves[4];
} party_mon_t;

typedef enum {
    BTL_MAIN = 0,
    BTL_FIGHT,
    BTL_MSG
} battle_menu_mode_t;

static party_mon_t s_party[6];
static uint8_t     s_active_party_idx = 0u;

static uint8_t            s_in_battle = 0u;
static battle_menu_mode_t s_btl_mode = BTL_MAIN;
static uint8_t            s_btl_main_cursor = 0u;  /* 0=FIGHT, 1=CATCH, 2=RUN */
static uint8_t            s_btl_move_cursor = 0u;  /* 0..3 */
static uint8_t            s_enemy_species = 0u;
static uint8_t            s_enemy_level = 1u;
static uint8_t            s_enemy_type1 = GB_TYPE_NORMAL;
static uint8_t            s_enemy_type2 = GB_TYPE_NORMAL;
static uint8_t            s_enemy_dv_ad = 0u;
static uint8_t            s_enemy_dv_ss = 0u;
static uint16_t           s_enemy_hp = 1u;
static uint16_t           s_enemy_max_hp = 1u;
static bool               s_enemy_shiny = false;
static char               s_btl_msg_line1[28];
static char               s_btl_msg_line2[28];
static uint16_t           s_btl_msg_timer = 0u;
static bool               s_btl_exit_after_msg = false;

/* ============================================================================
 * Gen 1 species & move metadata tables (internal IDs match mod_gen3box/moveinfo)
 * ========================================================================== */

typedef struct {
    uint8_t     species;
    const char *name;
    uint8_t     type1;
    uint8_t     type2;
    uint8_t     base_hp;
    uint8_t     base_atk;
    uint16_t    col_primary;
    uint16_t    col_accent;
} species_info_t;

static const species_info_t s_species_db[] = {
    { 0x01u, "RHYDON",     GB_TYPE_GROUND,   GB_TYPE_ROCK,     105, 130, RGB565_C(132, 138, 148), RGB565_C(200, 206, 214) },
    { 0x04u, "CLEFAIRY",   GB_TYPE_NORMAL,   GB_TYPE_NORMAL,    70,  45, RGB565_C(242, 178, 206), RGB565_C(252, 224, 236) },
    { 0x05u, "SPEAROW",    GB_TYPE_NORMAL,   GB_TYPE_FLYING,    40,  60, RGB565_C(178, 118,  72), RGB565_C(232,  96,  80) },
    { 0x07u, "NIDOKING",   GB_TYPE_POISON,   GB_TYPE_GROUND,    81,  92, RGB565_C(146,  92, 188), RGB565_C(216, 198, 232) },
    { 0x09u, "IVYSAUR",    GB_TYPE_GRASS,    GB_TYPE_POISON,    60,  62, RGB565_C( 64, 178, 136), RGB565_C(228,  96, 128) },
    { 0x0Eu, "GENGAR",     GB_TYPE_GHOST,    GB_TYPE_POISON,    60,  65, RGB565_C(112,  76, 158), RGB565_C(232,  72,  86) },
    { 0x10u, "NIDOQUEEN",  GB_TYPE_POISON,   GB_TYPE_GROUND,    90,  82, RGB565_C( 82, 144, 204), RGB565_C(234, 214, 168) },
    { 0x13u, "LAPRAS",     GB_TYPE_WATER,    GB_TYPE_ICE,      130,  85, RGB565_C( 78, 152, 222), RGB565_C(224, 226, 196) },
    { 0x14u, "ARCANINE",   GB_TYPE_FIRE,     GB_TYPE_FIRE,      90, 110, RGB565_C(228, 112,  52), RGB565_C(248, 226, 172) },
    { 0x15u, "MEW",        GB_TYPE_PSYCHIC,  GB_TYPE_PSYCHIC,  100, 100, RGB565_C(244, 172, 208), RGB565_C(140, 214, 250) },
    { 0x16u, "GYARADOS",   GB_TYPE_WATER,    GB_TYPE_FLYING,    95, 125, RGB565_C( 56, 132, 222), RGB565_C(236, 208, 128) },
    { 0x19u, "GASTLY",     GB_TYPE_GHOST,    GB_TYPE_POISON,    30,  35, RGB565_C( 96,  64, 132), RGB565_C(212, 198, 234) },
    { 0x1Au, "SCYTHER",    GB_TYPE_BUG,      GB_TYPE_FLYING,    70, 110, RGB565_C( 96, 194,  92), RGB565_C(224, 236, 192) },
    { 0x1Cu, "BLASTOISE",  GB_TYPE_WATER,    GB_TYPE_WATER,     79,  83, RGB565_C( 64, 138, 216), RGB565_C(188, 140,  86) },
    { 0x21u, "GROWLITHE",  GB_TYPE_FIRE,     GB_TYPE_FIRE,      55,  70, RGB565_C(228, 118,  56), RGB565_C(246, 224, 172) },
    { 0x22u, "ONIX",       GB_TYPE_ROCK,     GB_TYPE_GROUND,    35,  45, RGB565_C(142, 144, 152), RGB565_C(198, 202, 210) },
    { 0x24u, "PIDGEY",     GB_TYPE_NORMAL,   GB_TYPE_FLYING,    40,  45, RGB565_C(198, 154,  96), RGB565_C(242, 226, 186) },
    { 0x26u, "KADABRA",    GB_TYPE_PSYCHIC,  GB_TYPE_PSYCHIC,   40,  35, RGB565_C(232, 192,  68), RGB565_C(172, 116,  68) },
    { 0x2Fu, "PSYDUCK",    GB_TYPE_WATER,    GB_TYPE_WATER,     50,  52, RGB565_C(236, 202,  76), RGB565_C(238, 232, 214) },
    { 0x3Bu, "DIGLETT",    GB_TYPE_GROUND,   GB_TYPE_GROUND,    10,  55, RGB565_C(168, 118,  78), RGB565_C(232, 132, 144) },
    { 0x42u, "DRAGONITE",  GB_TYPE_DRAGON,   GB_TYPE_FLYING,    91, 134, RGB565_C(236, 156,  68), RGB565_C(116, 186, 142) },
    { 0x49u, "MOLTRES",    GB_TYPE_FIRE,     GB_TYPE_FLYING,    90, 100, RGB565_C(244, 132,  48), RGB565_C(252, 224,  76) },
    { 0x4Au, "ARTICUNO",   GB_TYPE_ICE,      GB_TYPE_FLYING,    90,  85, RGB565_C( 98, 182, 244), RGB565_C(224, 244, 255) },
    { 0x4Bu, "ZAPDOS",     GB_TYPE_ELECTRIC, GB_TYPE_FLYING,    90,  90, RGB565_C(248, 212,  52), RGB565_C( 48,  52,  64) },
    { 0x4Cu, "DITTO",      GB_TYPE_NORMAL,   GB_TYPE_NORMAL,    48,  48, RGB565_C(196, 128, 214), RGB565_C(234, 188, 242) },
    { 0x4Du, "MEOWTH",     GB_TYPE_NORMAL,   GB_TYPE_NORMAL,    40,  45, RGB565_C(236, 218, 174), RGB565_C(236, 198,  68) },
    { 0x52u, "VULPIX",     GB_TYPE_FIRE,     GB_TYPE_FIRE,      38,  41, RGB565_C(212, 102,  62), RGB565_C(236, 168,  96) },
    { 0x54u, "PIKACHU",    GB_TYPE_ELECTRIC, GB_TYPE_ELECTRIC,  35,  55, RGB565_C(248, 208,  48), RGB565_C(224,  64,  48) },
    { 0x55u, "RAICHU",     GB_TYPE_ELECTRIC, GB_TYPE_ELECTRIC,  60,  90, RGB565_C(236, 152,  48), RGB565_C(246, 226, 146) },
    { 0x58u, "DRATINI",    GB_TYPE_DRAGON,   GB_TYPE_DRAGON,    41,  64, RGB565_C(108, 146, 224), RGB565_C(232, 240, 252) },
    { 0x64u, "JIGGLYPUFF", GB_TYPE_NORMAL,   GB_TYPE_NORMAL,   115,  45, RGB565_C(246, 178, 206), RGB565_C( 88, 198, 176) },
    { 0x66u, "EEVEE",      GB_TYPE_NORMAL,   GB_TYPE_NORMAL,    55,  55, RGB565_C(184, 132,  82), RGB565_C(238, 218, 176) },
    { 0x67u, "FLAREON",    GB_TYPE_FIRE,     GB_TYPE_FIRE,      65, 130, RGB565_C(232,  96,  48), RGB565_C(246, 216, 128) },
    { 0x68u, "JOLTEON",    GB_TYPE_ELECTRIC, GB_TYPE_ELECTRIC,  65,  65, RGB565_C(246, 212,  56), RGB565_C(244, 244, 232) },
    { 0x69u, "VAPOREON",   GB_TYPE_WATER,    GB_TYPE_WATER,    130,  65, RGB565_C( 82, 168, 234), RGB565_C(212, 238, 252) },
    { 0x6Bu, "ZUBAT",      GB_TYPE_POISON,   GB_TYPE_FLYING,    40,  45, RGB565_C(102, 112, 206), RGB565_C(198, 118, 198) },
    { 0x70u, "WEEDLE",     GB_TYPE_BUG,      GB_TYPE_POISON,    40,  35, RGB565_C(196, 152,  88), RGB565_C(232, 142, 162) },
    { 0x7Bu, "CATERPIE",   GB_TYPE_BUG,      GB_TYPE_BUG,       45,  30, RGB565_C( 88, 198,  84), RGB565_C(240, 180,  56) },
    { 0x7Du, "BUTTERFREE", GB_TYPE_BUG,      GB_TYPE_FLYING,    60,  45, RGB565_C(142, 118, 214), RGB565_C(238, 242, 250) },
    { 0x7Eu, "MACHAMP",    GB_TYPE_FIGHTING, GB_TYPE_FIGHTING,  90, 130, RGB565_C(132, 156, 182), RGB565_C(226, 202,  86) },
    { 0x83u, "MEWTWO",     GB_TYPE_PSYCHIC,  GB_TYPE_PSYCHIC,  106, 110, RGB565_C(196, 182, 214), RGB565_C(166,  92, 204) },
    { 0x84u, "SNORLAX",    GB_TYPE_NORMAL,   GB_TYPE_NORMAL,   160, 110, RGB565_C( 64, 124, 144), RGB565_C(236, 222, 186) },
    { 0x85u, "MAGIKARP",   GB_TYPE_WATER,    GB_TYPE_WATER,     20,  10, RGB565_C(224,  88,  64), RGB565_C(244, 212,  88) },
    { 0x94u, "ABRA",       GB_TYPE_PSYCHIC,  GB_TYPE_PSYCHIC,   25,  20, RGB565_C(238, 204,  76), RGB565_C(162, 122,  86) },
    { 0x95u, "ALAKAZAM",   GB_TYPE_PSYCHIC,  GB_TYPE_PSYCHIC,   55,  50, RGB565_C(238, 198,  68), RGB565_C(156, 112,  74) },
    { 0x96u, "PIDGEOTTO",  GB_TYPE_NORMAL,   GB_TYPE_FLYING,    63,  60, RGB565_C(204, 148,  92), RGB565_C(234, 112,  92) },
    { 0x97u, "PIDGEOT",    GB_TYPE_NORMAL,   GB_TYPE_FLYING,    83,  80, RGB565_C(212, 156,  96), RGB565_C(244, 206,  86) },
    { 0x99u, "BULBASAUR",  GB_TYPE_GRASS,    GB_TYPE_POISON,    45,  49, RGB565_C( 82, 194, 132), RGB565_C( 64, 152,  76) },
    { 0x9Au, "VENUSAUR",   GB_TYPE_GRASS,    GB_TYPE_POISON,    80,  82, RGB565_C( 68, 172, 122), RGB565_C(228,  86, 122) },
    { 0xA5u, "RATTATA",    GB_TYPE_NORMAL,   GB_TYPE_NORMAL,    30,  56, RGB565_C(162,  96, 202), RGB565_C(236, 222, 180) },
    { 0xA6u, "RATICATE",   GB_TYPE_NORMAL,   GB_TYPE_NORMAL,    55,  81, RGB565_C(186, 138,  84), RGB565_C(238, 222, 184) },
    { 0xA9u, "GEODUDE",    GB_TYPE_ROCK,     GB_TYPE_GROUND,    40,  80, RGB565_C(148, 138, 126), RGB565_C(198, 190, 178) },
    { 0xADu, "MAGNEMITE",  GB_TYPE_ELECTRIC, GB_TYPE_ELECTRIC,  25,  35, RGB565_C(172, 188, 204), RGB565_C(224,  78,  78) },
    { 0xB0u, "CHARMANDER", GB_TYPE_FIRE,     GB_TYPE_FIRE,      39,  52, RGB565_C(236, 116,  52), RGB565_C(248, 214,  84) },
    { 0xB1u, "SQUIRTLE",   GB_TYPE_WATER,    GB_TYPE_WATER,     44,  48, RGB565_C( 84, 174, 224), RGB565_C(186, 134,  82) },
    { 0xB2u, "CHARMELEON", GB_TYPE_FIRE,     GB_TYPE_FIRE,      58,  64, RGB565_C(222,  72,  52), RGB565_C(246, 208,  78) },
    { 0xB3u, "WARTORTLE",  GB_TYPE_WATER,    GB_TYPE_WATER,     59,  63, RGB565_C( 74, 152, 224), RGB565_C(222, 232, 244) },
    { 0xB4u, "CHARIZARD",  GB_TYPE_FIRE,     GB_TYPE_FLYING,    78,  84, RGB565_C(234, 122,  48), RGB565_C( 72, 168, 176) },
    { 0xB9u, "ODDISH",     GB_TYPE_GRASS,    GB_TYPE_POISON,    45,  50, RGB565_C( 72, 108, 212), RGB565_C( 72, 204,  88) }
};

typedef struct {
    uint8_t     move_id;
    const char *name;
    uint8_t     type_id;
    uint8_t     power;
} move_info_t;

static const move_info_t s_move_db[] = {
    { 10u, "SCRATCH",     GB_TYPE_NORMAL,   40u },
    { 22u, "VINE WHIP",   GB_TYPE_GRASS,    35u },
    { 33u, "TACKLE",      GB_TYPE_NORMAL,   35u },
    { 44u, "BITE",        GB_TYPE_NORMAL,   60u },
    { 45u, "GROWL",       GB_TYPE_NORMAL,    0u },
    { 52u, "EMBER",       GB_TYPE_FIRE,     40u },
    { 55u, "WATER GUN",   GB_TYPE_WATER,    40u },
    { 75u, "RAZOR LEAF",  GB_TYPE_GRASS,    55u },
    { 84u, "THUNDERSHK",  GB_TYPE_ELECTRIC, 40u },
    { 93u, "CONFUSION",   GB_TYPE_PSYCHIC,  50u },
    { 98u, "QUICK ATK",   GB_TYPE_NORMAL,   40u },
    {122u, "LICK",        GB_TYPE_GHOST,    20u }
};

/* Compact 5x7 uppercase/ASCII bitmap font for battle screen (column-major). */
static const uint8_t s_mini_font[95][5] = {
    {0x00,0x00,0x00,0x00,0x00},{0x00,0x00,0x5F,0x00,0x00},{0x00,0x07,0x00,0x07,0x00},{0x14,0x7F,0x14,0x7F,0x14},
    {0x24,0x2A,0x7F,0x2A,0x12},{0x23,0x13,0x08,0x64,0x62},{0x36,0x49,0x55,0x22,0x50},{0x00,0x05,0x03,0x00,0x00},
    {0x00,0x1C,0x22,0x41,0x00},{0x00,0x41,0x22,0x1C,0x00},{0x14,0x08,0x3E,0x08,0x14},{0x08,0x08,0x3E,0x08,0x08},
    {0x00,0x50,0x30,0x00,0x00},{0x08,0x08,0x08,0x08,0x08},{0x00,0x60,0x60,0x00,0x00},{0x20,0x10,0x08,0x04,0x02},
    {0x3E,0x51,0x49,0x45,0x3E},{0x00,0x42,0x7F,0x40,0x00},{0x42,0x61,0x51,0x49,0x46},{0x21,0x41,0x45,0x4B,0x31},
    {0x18,0x14,0x12,0x7F,0x10},{0x27,0x45,0x45,0x45,0x39},{0x3C,0x4A,0x49,0x49,0x30},{0x01,0x71,0x09,0x05,0x03},
    {0x36,0x49,0x49,0x49,0x36},{0x06,0x49,0x49,0x29,0x1E},{0x00,0x36,0x36,0x00,0x00},{0x00,0x56,0x36,0x00,0x00},
    {0x08,0x14,0x22,0x41,0x00},{0x14,0x14,0x14,0x14,0x14},{0x00,0x41,0x22,0x14,0x08},{0x02,0x01,0x51,0x09,0x06},
    {0x32,0x49,0x79,0x41,0x3E},{0x7E,0x11,0x11,0x11,0x7E},{0x7F,0x49,0x49,0x49,0x36},{0x3E,0x41,0x41,0x41,0x22},
    {0x7F,0x41,0x41,0x22,0x1C},{0x7F,0x49,0x49,0x49,0x41},{0x7F,0x09,0x09,0x09,0x01},{0x3E,0x41,0x49,0x49,0x7A},
    {0x7F,0x08,0x08,0x08,0x7F},{0x00,0x41,0x7F,0x41,0x00},{0x20,0x40,0x41,0x3F,0x01},{0x7F,0x08,0x14,0x22,0x41},
    {0x7F,0x40,0x40,0x40,0x40},{0x7F,0x02,0x0C,0x02,0x7F},{0x7F,0x04,0x08,0x10,0x7F},{0x3E,0x41,0x41,0x41,0x3E},
    {0x7F,0x09,0x09,0x09,0x06},{0x3E,0x41,0x51,0x21,0x5E},{0x7F,0x09,0x19,0x29,0x46},{0x46,0x49,0x49,0x49,0x31},
    {0x01,0x01,0x7F,0x01,0x01},{0x3F,0x40,0x40,0x40,0x3F},{0x1F,0x20,0x40,0x20,0x1F},{0x3F,0x40,0x38,0x40,0x3F},
    {0x63,0x14,0x08,0x14,0x63},{0x07,0x08,0x70,0x08,0x07},{0x61,0x51,0x49,0x45,0x43},{0x00,0x7F,0x41,0x41,0x00},
    {0x02,0x04,0x08,0x10,0x20},{0x00,0x41,0x41,0x7F,0x00},{0x04,0x02,0x01,0x02,0x04},{0x40,0x40,0x40,0x40,0x40},
    {0x00,0x01,0x02,0x04,0x00},{0x20,0x54,0x54,0x54,0x78},{0x7F,0x48,0x44,0x44,0x38},{0x38,0x44,0x44,0x44,0x20},
    {0x38,0x44,0x44,0x48,0x7F},{0x38,0x54,0x54,0x54,0x18},{0x08,0x7E,0x09,0x01,0x02},{0x0C,0x52,0x52,0x52,0x3E},
    {0x7F,0x08,0x04,0x04,0x78},{0x00,0x44,0x7D,0x40,0x00},{0x20,0x40,0x44,0x3D,0x00},{0x7F,0x10,0x28,0x44,0x00},
    {0x00,0x41,0x7F,0x40,0x00},{0x7C,0x04,0x18,0x04,0x78},{0x7C,0x08,0x04,0x04,0x78},{0x38,0x44,0x44,0x44,0x38},
    {0x7C,0x14,0x14,0x14,0x08},{0x08,0x14,0x14,0x18,0x7C},{0x7C,0x08,0x04,0x04,0x08},{0x48,0x54,0x54,0x54,0x20},
    {0x04,0x3F,0x44,0x40,0x20},{0x3C,0x40,0x40,0x20,0x7C},{0x1C,0x20,0x40,0x20,0x1C},{0x3C,0x40,0x30,0x40,0x3C},
    {0x44,0x28,0x10,0x28,0x44},{0x0C,0x50,0x50,0x50,0x3C},{0x44,0x64,0x54,0x4C,0x44},{0x00,0x08,0x36,0x41,0x00},
    {0x00,0x00,0x7F,0x00,0x00},{0x00,0x41,0x36,0x08,0x00},{0x10,0x08,0x08,0x10,0x08}
};

/* ============================================================================
 * Low-level ROM / WRAM helpers
 * ========================================================================== */

static uint32_t next_rng(void)
{
    uint32_t x = s_rng;
    if (x == 0u) {
        x = 0x504F4B45u;
    }
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    s_rng = x;
    return x;
}

static size_t rom_bank_off(uint8_t bank, uint16_t addr)
{
    if (addr >= 0x4000u) {
        return (size_t)bank * 0x4000u + (size_t)(addr - 0x4000u);
    }
    return (size_t)addr;
}

static uint8_t rom_u8(size_t off)
{
    if (s_rom_data == NULL || off >= s_rom_size) {
        return 0xFFu;
    }
    return s_rom_data[off];
}

static uint16_t rom_u16(size_t off)
{
    uint16_t lo = (uint16_t)rom_u8(off);
    uint16_t hi = (uint16_t)rom_u8(off + 1u);
    return (uint16_t)(lo | (hi << 8));
}

static uint8_t wram_u8(uint16_t addr)
{
    if (addr < 0xC000u || addr > 0xDFFFu) {
        return 0u;
    }
    return s_wram[addr - 0xC000u];
}

static void wram_w8(uint16_t addr, uint8_t val)
{
    if (addr >= 0xC000u && addr <= 0xDFFFu) {
        s_wram[addr - 0xC000u] = val;
    }
}

static void wram_w16(uint16_t addr, uint16_t val)
{
    wram_w8(addr, (uint8_t)(val & 0xFFu));
    wram_w8((uint16_t)(addr + 1u), (uint8_t)((val >> 8) & 0xFFu));
}

static uint8_t facing_dir_to_frame(uint8_t dir)
{
    if (dir == 4u) {
        return 4u;
    }
    if (dir == 8u || dir == 12u) {
        return 8u;
    }
    return 0u;
}

static uint8_t move2_to_facing(uint8_t move2)
{
    switch (move2) {
    case 0xD0u: return 0u;   /* Down */
    case 0xD1u: return 4u;   /* Up */
    case 0xD2u: return 8u;   /* Left */
    case 0xD3u: return 12u;  /* Right */
    default:    return 0u;
    }
}

static bool is_outdoor_map(uint8_t map_id)
{
    return (map_id < 0x25u);
}

/* ============================================================================
 * Species & battle helpers
 * ========================================================================== */

static const species_info_t *lookup_species_info(uint8_t species)
{
    size_t count = sizeof(s_species_db) / sizeof(s_species_db[0]);
    for (size_t i = 0u; i < count; i++) {
        if (s_species_db[i].species == species) {
            return &s_species_db[i];
        }
    }
    return NULL;
}

static void format_species_name(uint8_t species, char *out, size_t out_sz)
{
    const species_info_t *info = lookup_species_info(species);
    if (info != NULL) {
        snprintf(out, out_sz, "%s", info->name);
    } else {
        snprintf(out, out_sz, "MON#%02X", (unsigned)species);
    }
}

static uint16_t calc_max_hp(uint8_t species, uint8_t level)
{
    const species_info_t *info = lookup_species_info(species);
    uint16_t base_hp = (info != NULL) ? (uint16_t)info->base_hp : 45u;
    uint16_t hp = (uint16_t)((((base_hp + 8u) * 2u) * (uint16_t)level) / 100u + (uint16_t)level + 10u);
    return (hp > 0u) ? hp : 1u;
}

static const move_info_t *lookup_move_info(uint8_t move_id)
{
    size_t count = sizeof(s_move_db) / sizeof(s_move_db[0]);
    for (size_t i = 0u; i < count; i++) {
        if (s_move_db[i].move_id == move_id) {
            return &s_move_db[i];
        }
    }
    return NULL;
}

static void default_moves_for_species(uint8_t species, uint8_t out_moves[4])
{
    out_moves[0] = 33u; /* TACKLE */
    out_moves[1] = 45u; /* GROWL */
    out_moves[2] = 0u;
    out_moves[3] = 0u;

    switch (species) {
    case 0x54u: /* Pikachu */
    case 0x55u: /* Raichu */
        out_moves[0] = 84u; /* THUNDERSHOCK */
        out_moves[1] = 33u; /* TACKLE */
        out_moves[2] = 98u; /* QUICK_ATTACK */
        out_moves[3] = 45u; /* GROWL */
        break;
    case 0xB0u: /* Charmander */
    case 0xB2u: /* Charmeleon */
    case 0xB4u: /* Charizard */
        out_moves[0] = 52u; /* EMBER */
        out_moves[1] = 10u; /* SCRATCH */
        out_moves[2] = 44u; /* BITE */
        out_moves[3] = 45u; /* GROWL */
        break;
    case 0x99u: /* Bulbasaur */
    case 0x09u: /* Ivysaur */
    case 0x9Au: /* Venusaur */
        out_moves[0] = 22u; /* VINE_WHIP */
        out_moves[1] = 33u; /* TACKLE */
        out_moves[2] = 75u; /* RAZOR_LEAF */
        out_moves[3] = 45u; /* GROWL */
        break;
    case 0xB1u: /* Squirtle */
    case 0xB3u: /* Wartortle */
    case 0x1Cu: /* Blastoise */
        out_moves[0] = 55u; /* WATER_GUN */
        out_moves[1] = 33u; /* TACKLE */
        out_moves[2] = 44u; /* BITE */
        out_moves[3] = 45u; /* GROWL */
        break;
    case 0x19u: /* Gastly */
    case 0x0Eu: /* Gengar */
        out_moves[0] = 122u; /* LICK */
        out_moves[1] = 93u;  /* CONFUSION */
        break;
    case 0x94u: /* Abra */
    case 0x26u: /* Kadabra */
    case 0x95u: /* Alakazam */
    case 0x15u: /* Mew */
    case 0x83u: /* Mewtwo */
        out_moves[0] = 93u; /* CONFUSION */
        out_moves[1] = 33u; /* TACKLE */
        break;
    default:
        break;
    }
}

static int single_type_eff_half(uint8_t atk, uint8_t def)
{
    switch (atk) {
    case GB_TYPE_NORMAL:
        if (def == GB_TYPE_ROCK)  return 1;
        if (def == GB_TYPE_GHOST) return 0;
        return 2;
    case GB_TYPE_FIGHTING:
        if (def == GB_TYPE_NORMAL || def == GB_TYPE_ROCK || def == GB_TYPE_ICE) return 4;
        if (def == GB_TYPE_FLYING || def == GB_TYPE_POISON ||
            def == GB_TYPE_BUG    || def == GB_TYPE_PSYCHIC) return 1;
        if (def == GB_TYPE_GHOST) return 0;
        return 2;
    case GB_TYPE_FLYING:
        if (def == GB_TYPE_FIGHTING || def == GB_TYPE_BUG || def == GB_TYPE_GRASS) return 4;
        if (def == GB_TYPE_ROCK || def == GB_TYPE_ELECTRIC) return 1;
        return 2;
    case GB_TYPE_POISON:
        if (def == GB_TYPE_GRASS || def == GB_TYPE_BUG) return 4;
        if (def == GB_TYPE_POISON || def == GB_TYPE_GROUND ||
            def == GB_TYPE_ROCK   || def == GB_TYPE_GHOST) return 1;
        return 2;
    case GB_TYPE_GROUND:
        if (def == GB_TYPE_POISON || def == GB_TYPE_ROCK ||
            def == GB_TYPE_FIRE   || def == GB_TYPE_ELECTRIC) return 4;
        if (def == GB_TYPE_BUG || def == GB_TYPE_GRASS) return 1;
        if (def == GB_TYPE_FLYING) return 0;
        return 2;
    case GB_TYPE_ROCK:
        if (def == GB_TYPE_FLYING || def == GB_TYPE_BUG ||
            def == GB_TYPE_FIRE   || def == GB_TYPE_ICE) return 4;
        if (def == GB_TYPE_FIGHTING || def == GB_TYPE_GROUND) return 1;
        return 2;
    case GB_TYPE_BUG:
        if (def == GB_TYPE_POISON || def == GB_TYPE_GRASS || def == GB_TYPE_PSYCHIC) return 4;
        if (def == GB_TYPE_FIGHTING || def == GB_TYPE_FLYING ||
            def == GB_TYPE_GHOST    || def == GB_TYPE_FIRE) return 1;
        return 2;
    case GB_TYPE_GHOST:
        if (def == GB_TYPE_GHOST) return 4;
        if (def == GB_TYPE_NORMAL || def == GB_TYPE_PSYCHIC) return 0;
        return 2;
    case GB_TYPE_FIRE:
        if (def == GB_TYPE_BUG || def == GB_TYPE_GRASS || def == GB_TYPE_ICE) return 4;
        if (def == GB_TYPE_ROCK || def == GB_TYPE_FIRE ||
            def == GB_TYPE_WATER || def == GB_TYPE_DRAGON) return 1;
        return 2;
    case GB_TYPE_WATER:
        if (def == GB_TYPE_GROUND || def == GB_TYPE_ROCK || def == GB_TYPE_FIRE) return 4;
        if (def == GB_TYPE_WATER || def == GB_TYPE_GRASS || def == GB_TYPE_DRAGON) return 1;
        return 2;
    case GB_TYPE_GRASS:
        if (def == GB_TYPE_GROUND || def == GB_TYPE_ROCK || def == GB_TYPE_WATER) return 4;
        if (def == GB_TYPE_FLYING || def == GB_TYPE_POISON || def == GB_TYPE_BUG ||
            def == GB_TYPE_FIRE   || def == GB_TYPE_GRASS  || def == GB_TYPE_DRAGON) return 1;
        return 2;
    case GB_TYPE_ELECTRIC:
        if (def == GB_TYPE_FLYING || def == GB_TYPE_WATER) return 4;
        if (def == GB_TYPE_GRASS || def == GB_TYPE_ELECTRIC || def == GB_TYPE_DRAGON) return 1;
        if (def == GB_TYPE_GROUND) return 0;
        return 2;
    case GB_TYPE_PSYCHIC:
        if (def == GB_TYPE_FIGHTING || def == GB_TYPE_POISON) return 4;
        if (def == GB_TYPE_PSYCHIC) return 1;
        return 2;
    case GB_TYPE_ICE:
        if (def == GB_TYPE_FLYING || def == GB_TYPE_GROUND ||
            def == GB_TYPE_GRASS  || def == GB_TYPE_DRAGON) return 4;
        if (def == GB_TYPE_WATER || def == GB_TYPE_ICE) return 1;
        return 2;
    case GB_TYPE_DRAGON:
        if (def == GB_TYPE_DRAGON) return 4;
        return 2;
    default:
        return 2;
    }
}

static void sync_battle_wram(void)
{
    const party_mon_t *pm = &s_party[s_active_party_idx];
    const species_info_t *pinfo = lookup_species_info(pm->species);

    wram_w8(GB_ADDR_IS_IN_BATTLE, s_in_battle);
    wram_w8(GB_ADDR_CUR_OPPONENT, s_enemy_species);
    wram_w8(GB_ADDR_CUR_ENEMY_LEVEL, s_enemy_level);

    wram_w8(GB_ADDR_ENEMY_MON_SPECIES, s_enemy_species);
    wram_w8(GB_ADDR_ENEMY_MON_HP_HI, (uint8_t)((s_enemy_hp >> 8) & 0xFFu));
    wram_w8(GB_ADDR_ENEMY_MON_HP_LO, (uint8_t)(s_enemy_hp & 0xFFu));
    wram_w8(GB_ADDR_ENEMY_MON_LEVEL, s_enemy_level);
    wram_w8(GB_ADDR_ENEMY_MON_TYPE1, s_enemy_type1);
    wram_w8(GB_ADDR_ENEMY_MON_TYPE2, s_enemy_type2);
    wram_w8(GB_ADDR_ENEMY_MON_DVS, s_enemy_dv_ad);
    wram_w8((uint16_t)(GB_ADDR_ENEMY_MON_DVS + 1u), s_enemy_dv_ss);

    wram_w8(GB_ADDR_BATTLE_MON_SPECIES, pm->species);
    wram_w8(GB_ADDR_BATTLE_MON_TYPE1, (pinfo != NULL) ? pinfo->type1 : GB_TYPE_NORMAL);
    wram_w8(GB_ADDR_BATTLE_MON_TYPE2, (pinfo != NULL) ? pinfo->type2 : GB_TYPE_NORMAL);
    for (uint16_t i = 0u; i < 4u; i++) {
        wram_w8((uint16_t)(GB_ADDR_BATTLE_MON_MOVES + i), pm->moves[i]);
    }

    wram_w8(GB_ADDR_CURRENT_MENU_ITEM,
            (s_btl_mode == BTL_FIGHT) ? s_btl_move_cursor : s_btl_main_cursor);
}

static void init_starter_party_and_box(void)
{
    static const uint8_t k_starters[3] = { 0x54u, 0xB0u, 0x99u }; /* Pikachu, Charmander, Bulbasaur */
    static const uint8_t k_box_mons[6] = { 0xB1u, 0x66u, 0x94u, 0x19u, 0x85u, 0x15u }; /* Squirtle, Eevee, Abra, Gastly, Magikarp, Mew */

    memset(s_party, 0, sizeof(s_party));
    wram_w8(GB_ADDR_PARTY_COUNT, 3u);
    for (uint16_t i = 0u; i < 6u; i++) {
        if (i < 3u) {
            uint8_t sp = k_starters[i];
            s_party[i].species = sp;
            s_party[i].level = 5u;
            s_party[i].max_hp = calc_max_hp(sp, 5u);
            s_party[i].hp = s_party[i].max_hp;
            default_moves_for_species(sp, s_party[i].moves);
            wram_w8((uint16_t)(GB_ADDR_PARTY_SPECIES + i), sp);
        } else {
            wram_w8((uint16_t)(GB_ADDR_PARTY_SPECIES + i), 0xFFu);
        }
    }
    s_active_party_idx = 0u;

    wram_w8(GB_ADDR_CUR_BOX, 0u);
    wram_w8(GB_ADDR_BOX_COUNT, 6u);
    for (uint16_t i = 0u; i < APP_MAX_BOX_MONS; i++) {
        uint8_t sp = (i < 6u) ? k_box_mons[i] : 0xFFu;
        wram_w8((uint16_t)(GB_ADDR_BOX_SPECIES + i), sp);
    }
}

/* ============================================================================
 * Map loading (ROM -> synthetic WRAM/VRAM)
 * ========================================================================== */

static void load_sprite_sheet_to_vram(uint8_t pic_id, uint8_t dst_tile, uint8_t max_tiles)
{
    if (pic_id == 0u || pic_id > 72u || max_tiles == 0u) {
        return;
    }

    size_t entry_off = ROM_SPRITE_SHEETS + (size_t)(pic_id - 1u) * 4u;
    uint16_t ptr = rom_u16(entry_off);
    uint8_t byte_count = rom_u8(entry_off + 2u);
    uint8_t bank = rom_u8(entry_off + 3u);

    size_t max_bytes = (size_t)max_tiles * 16u;
    size_t copy_bytes = (size_t)byte_count;
    if (copy_bytes > max_bytes) {
        copy_bytes = max_bytes;
    }

    size_t src_off = rom_bank_off(bank, ptr);
    size_t vram_off = (size_t)dst_tile * 16u;
    for (size_t i = 0u; i < copy_bytes && (vram_off + i) < sizeof(s_vram); i++) {
        s_vram[vram_off + i] = rom_u8(src_off + i);
    }
}

static void get_map_dims(uint8_t map_id, uint8_t *out_w, uint8_t *out_h)
{
    if (map_id >= MAX_MAPS) {
        *out_w = 0u;
        *out_h = 0u;
        return;
    }
    uint16_t hdr_ptr = rom_u16(ROM_MAP_HEADER_PTRS + (size_t)map_id * 2u);
    uint8_t hdr_bank = rom_u8(ROM_MAP_HEADER_BANKS + (size_t)map_id);
    size_t hdr_off = rom_bank_off(hdr_bank, hdr_ptr);
    *out_h = rom_u8(hdr_off + 1u);
    *out_w = rom_u8(hdr_off + 2u);
}

static void set_player_pos(uint8_t x, uint8_t y)
{
    if (s_map_w > 0u && x >= (uint8_t)(s_map_w * 2u)) {
        x = (uint8_t)(s_map_w * 2u - 1u);
    }
    if (s_map_h > 0u && y >= (uint8_t)(s_map_h * 2u)) {
        y = (uint8_t)(s_map_h * 2u - 1u);
    }

    s_player_x = x;
    s_player_y = y;
    s_prev_x = x;
    s_prev_y = y;
    s_walk_counter = 0u;
    s_walk_dx = 0;
    s_walk_dy = 0;

    if (is_outdoor_map(s_cur_map)) {
        s_last_outdoor_map = s_cur_map;
        s_last_outdoor_x = x;
        s_last_outdoor_y = y;
    }

    wram_w8(GB_ADDR_Y_COORD, y);
    wram_w8(GB_ADDR_X_COORD, x);
    wram_w8(GB_ADDR_WALK_COUNTER, 0u);
    wram_w8((uint16_t)(GB_ADDR_SPRITE_DATA1 + GB_SPRITE1_Y_STEP_VEC), 0u);
    wram_w8((uint16_t)(GB_ADDR_SPRITE_DATA1 + GB_SPRITE1_X_STEP_VEC), 0u);
    wram_w8((uint16_t)(GB_ADDR_SPRITE_DATA2 + GB_SPRITE2_MAP_Y), (uint8_t)(y + 4u));
    wram_w8((uint16_t)(GB_ADDR_SPRITE_DATA2 + GB_SPRITE2_MAP_X), (uint8_t)(x + 4u));
}

static bool load_map(uint8_t map_id, uint8_t spawn_x, uint8_t spawn_y)
{
    if (s_rom_data == NULL || map_id >= MAX_MAPS) {
        return false;
    }

    uint16_t hdr_ptr = rom_u16(ROM_MAP_HEADER_PTRS + (size_t)map_id * 2u);
    uint8_t hdr_bank = rom_u8(ROM_MAP_HEADER_BANKS + (size_t)map_id);
    size_t hdr_off = rom_bank_off(hdr_bank, hdr_ptr);

    uint8_t tileset_id = rom_u8(hdr_off + 0u);
    uint8_t map_h = rom_u8(hdr_off + 1u);
    uint8_t map_w = rom_u8(hdr_off + 2u);
    uint16_t blocks_ptr = rom_u16(hdr_off + 3u);
    uint8_t conn_flags = rom_u8(hdr_off + 9u);

    if (map_w == 0u || map_h == 0u ||
        map_w > MAP_OVERWORLD_W_LIMIT || map_h > MAP_OVERWORLD_W_LIMIT) {
        return false;
    }

    s_cur_map = map_id;
    s_tileset_id = tileset_id;
    s_map_w = map_w;
    s_map_h = map_h;

    /* Parse map connections (bit3=N, bit2=S, bit1=W, bit0=E). */
    memset(s_conns, 0, sizeof(s_conns));
    static const uint8_t k_conn_bits[4] = { 0x08u, 0x04u, 0x02u, 0x01u };
    size_t n_conn = 0u;
    for (size_t d = 0u; d < 4u; d++) {
        if ((conn_flags & k_conn_bits[d]) != 0u) {
            size_t coff = hdr_off + 10u + n_conn * 11u;
            s_conns[d].active     = true;
            s_conns[d].dst_map_id = rom_u8(coff + 0u);
            s_conns[d].dst_map_w  = rom_u8(coff + 6u);
            s_conns[d].y_align    = (int8_t)rom_u8(coff + 7u);
            s_conns[d].x_align    = (int8_t)rom_u8(coff + 8u);
            n_conn++;
        }
    }

    /* Parse object header immediately following connections. */
    uint16_t obj_ptr = rom_u16(hdr_off + 10u + n_conn * 11u);
    size_t obj_off = rom_bank_off(hdr_bank, obj_ptr);
    uint8_t border_block = rom_u8(obj_off + 0u);
    uint8_t raw_warps = rom_u8(obj_off + 1u);

    size_t cursor = obj_off + 2u;
    s_num_warps = (raw_warps <= MAX_WARPS) ? raw_warps : (uint8_t)MAX_WARPS;
    for (uint8_t i = 0u; i < raw_warps; i++) {
        if (i < MAX_WARPS) {
            s_warps[i].y           = rom_u8(cursor + 0u);
            s_warps[i].x           = rom_u8(cursor + 1u);
            s_warps[i].dst_warp_id = rom_u8(cursor + 2u);
            s_warps[i].dst_map_id  = rom_u8(cursor + 3u);
        }
        cursor += 4u;
    }

    /* Skip signs. */
    uint8_t num_signs = rom_u8(cursor);
    cursor += 1u + (size_t)num_signs * 3u;

    /* Parse sprites and upload to VRAM + wSpriteStateData1/2. */
    uint8_t num_sprites = rom_u8(cursor);
    cursor += 1u;

    memset(&s_wram[GB_ADDR_SPRITE_DATA1 - 0xC000u], 0, 0x200u);
    memset(s_vram, 0, 0x0800u);

    /* Sprite 0 = Red (12 tiles = 0xC0 bytes at 0x8000). */
    load_sprite_sheet_to_vram(1u, 0u, 12u);

    /* Deduplicate NPC sprite sheets in VRAM tiles 12..123. */
    uint8_t cached_pic[APP_MAX_NPCS];
    uint8_t cached_tile[APP_MAX_NPCS];
    uint8_t cached_tiles_cnt[APP_MAX_NPCS];
    uint8_t cache_len = 0u;
    uint8_t next_vram_tile = 12u;

    uint8_t npc_count = (num_sprites <= APP_MAX_NPCS) ? num_sprites : (uint8_t)APP_MAX_NPCS;
    for (uint8_t i = 0u; i < npc_count; i++) {
        uint8_t pic_id  = rom_u8(cursor + 0u);
        uint8_t y_plus4 = rom_u8(cursor + 1u);
        uint8_t x_plus4 = rom_u8(cursor + 2u);
        uint8_t move2   = rom_u8(cursor + 4u);
        uint8_t text_id = rom_u8(cursor + 5u);

        cursor += 6u;
        if ((text_id & 0x40u) != 0u) {
            cursor += 2u; /* Trainer */
        } else if ((text_id & 0x80u) != 0u) {
            cursor += 1u; /* Item */
        }

        if (pic_id == 0u) {
            continue;
        }

        uint8_t npc_base_tile = 0xFFu;
        uint8_t npc_tiles = 0u;

        for (uint8_t c = 0u; c < cache_len; c++) {
            if (cached_pic[c] == pic_id) {
                npc_base_tile = cached_tile[c];
                npc_tiles = cached_tiles_cnt[c];
                break;
            }
        }

        if (npc_base_tile == 0xFFu && pic_id <= 72u) {
            uint8_t sheet_bytes = rom_u8(ROM_SPRITE_SHEETS + (size_t)(pic_id - 1u) * 4u + 2u);
            uint8_t want_tiles = (sheet_bytes >= 0xC0u) ? 12u : 4u;
            if ((uint16_t)next_vram_tile + (uint16_t)want_tiles > MAX_VRAM_SPRITE_TILES) {
                want_tiles = 4u;
            }
            if ((uint16_t)next_vram_tile + (uint16_t)want_tiles <= MAX_VRAM_SPRITE_TILES) {
                npc_base_tile = next_vram_tile;
                npc_tiles = want_tiles;
                load_sprite_sheet_to_vram(pic_id, npc_base_tile, npc_tiles);
                next_vram_tile = (uint8_t)(next_vram_tile + npc_tiles);

                cached_pic[cache_len] = pic_id;
                cached_tile[cache_len] = npc_base_tile;
                cached_tiles_cnt[cache_len] = npc_tiles;
                cache_len++;
            }
        }

        uint8_t npc_dir = move2_to_facing(move2);
        uint8_t dir_off = facing_dir_to_frame(npc_dir);
        uint8_t img_idx = 0xFFu;
        if (npc_base_tile != 0xFFu) {
            img_idx = (dir_off + 4u <= npc_tiles)
                      ? (uint8_t)(npc_base_tile + dir_off)
                      : npc_base_tile;
        }

        uint16_t off = (uint16_t)((i + 1u) * 16u);
        wram_w8((uint16_t)(GB_ADDR_SPRITE_DATA1 + off + GB_SPRITE1_PICTURE_ID), pic_id);
        wram_w8((uint16_t)(GB_ADDR_SPRITE_DATA1 + off + GB_SPRITE1_MOVEMENT_STATUS), 1u);
        wram_w8((uint16_t)(GB_ADDR_SPRITE_DATA1 + off + GB_SPRITE1_IMAGE_INDEX), img_idx);
        wram_w8((uint16_t)(GB_ADDR_SPRITE_DATA1 + off + GB_SPRITE1_FACING_DIR), npc_dir);
        wram_w8((uint16_t)(GB_ADDR_SPRITE_DATA2 + off + GB_SPRITE2_MAP_Y), y_plus4);
        wram_w8((uint16_t)(GB_ADDR_SPRITE_DATA2 + off + GB_SPRITE2_MAP_X), x_plus4);
    }

    /* Parse tileset metadata and copy 128 tiles (2048 bytes) into VRAM 0x9000..0x97FF. */
    size_t ts_off = ROM_TILESETS_TABLE + (size_t)tileset_id * 12u;
    s_ts_bank       = rom_u8(ts_off + 0u);
    s_ts_blocks_ptr = rom_u16(ts_off + 1u);
    s_ts_gfx_ptr    = rom_u16(ts_off + 3u);
    s_ts_coll_ptr   = rom_u16(ts_off + 5u);
    s_grass_tile    = rom_u8(ts_off + 10u);

    size_t gfx_off = rom_bank_off(s_ts_bank, s_ts_gfx_ptr);
    for (size_t i = 0u; i < 0x800u; i++) {
        s_vram[0x1000u + i] = rom_u8(gfx_off + i);
    }

    /* Cache tileset walkability table for fast movement checks. */
    memset(s_walkable, 0, sizeof(s_walkable));
    for (uint16_t i = 0u; i < MAX_TILESET_COLL; i++) {
        uint8_t t = gb_brain_read_rom_bank(s_ts_bank, (uint16_t)(s_ts_coll_ptr + i));
        if (t == 0xFFu) {
            break;
        }
        s_walkable[t] = true;
    }
    if (s_grass_tile != 0xFFu) {
        s_walkable[s_grass_tile] = true;
    }

    /* Fill wOverworldMap: 3-block border ring + interior blocks from ROM. */
    uint32_t stride = (uint32_t)map_w + 6u;
    uint32_t total_rows = (uint32_t)map_h + 6u;
    uint32_t total_cells = stride * total_rows;
    if (total_cells > MAP_OVERWORLD_BYTES) {
        total_cells = MAP_OVERWORLD_BYTES;
    }
    memset(&s_wram[GB_ADDR_OVERWORLD_MAP - 0xC000u], border_block, total_cells);

    size_t map_blk_off = rom_bank_off(hdr_bank, blocks_ptr);
    for (uint32_t by = 0u; by < (uint32_t)map_h; by++) {
        for (uint32_t bx = 0u; bx < (uint32_t)map_w; bx++) {
            uint32_t idx = (by + 3u) * stride + (bx + 3u);
            if (idx < MAP_OVERWORLD_BYTES) {
                s_wram[(GB_ADDR_OVERWORLD_MAP - 0xC000u) + idx] =
                    rom_u8(map_blk_off + (size_t)by * (size_t)map_w + (size_t)bx);
            }
        }
    }

    /* Mirror map, tileset, and player sprite 0 into WRAM for gb_map_extract / mods. */
    wram_w8(GB_ADDR_CUR_MAP, map_id);
    wram_w8(GB_ADDR_CUR_MAP_TILESET, tileset_id);
    wram_w8(GB_ADDR_CUR_MAP_HEIGHT, map_h);
    wram_w8(GB_ADDR_CUR_MAP_WIDTH, map_w);

    wram_w8(GB_ADDR_TILESET_BANK, s_ts_bank);
    wram_w16(GB_ADDR_TILESET_BLOCKS_PTR, s_ts_blocks_ptr);
    wram_w16(GB_ADDR_TILESET_GFX_PTR, s_ts_gfx_ptr);
    wram_w16(GB_ADDR_TILESET_COLL_PTR, s_ts_coll_ptr);
    wram_w8(GB_ADDR_GRASS_TILE, s_grass_tile);

    wram_w8((uint16_t)(GB_ADDR_SPRITE_DATA1 + GB_SPRITE1_PICTURE_ID), 1u);
    wram_w8((uint16_t)(GB_ADDR_SPRITE_DATA1 + GB_SPRITE1_MOVEMENT_STATUS), 1u);
    wram_w8((uint16_t)(GB_ADDR_SPRITE_DATA1 + GB_SPRITE1_IMAGE_INDEX), facing_dir_to_frame(s_facing_dir));
    wram_w8((uint16_t)(GB_ADDR_SPRITE_DATA1 + GB_SPRITE1_Y_PIXELS), 0x3Cu);
    wram_w8((uint16_t)(GB_ADDR_SPRITE_DATA1 + GB_SPRITE1_X_PIXELS), 0x40u);
    wram_w8((uint16_t)(GB_ADDR_SPRITE_DATA1 + GB_SPRITE1_FACING_DIR), s_facing_dir);

    set_player_pos(spawn_x, spawn_y);

    return true;
}

static uint8_t cell_bottom_left_tile(int cx, int cy)
{
    if (cx < 0 || cy < 0 || cx >= (int)(s_map_w * 2u) || cy >= (int)(s_map_h * 2u)) {
        return 0xFFu;
    }
    int tx = cx * 2;
    int ty = cy * 2 + 1;
    int bx = (tx / 4) + 3;
    int by = (ty / 4) + 3;
    int sub_x = tx & 3;
    int sub_y = ty & 3;

    uint32_t stride = (uint32_t)s_map_w + 6u;
    uint32_t idx = (uint32_t)by * stride + (uint32_t)bx;
    if (idx >= MAP_OVERWORLD_BYTES) {
        return 0xFFu;
    }
    uint8_t blk = s_wram[(GB_ADDR_OVERWORLD_MAP - 0xC000u) + idx];
    uint16_t tile_addr = (uint16_t)(s_ts_blocks_ptr + (uint16_t)blk * 16u + (uint16_t)(sub_y * 4 + sub_x));
    return gb_brain_read_rom_bank(s_ts_bank, tile_addr);
}

static int find_warp_at(int cx, int cy)
{
    for (uint8_t i = 0u; i < s_num_warps; i++) {
        if ((int)s_warps[i].x == cx && (int)s_warps[i].y == cy) {
            return (int)i;
        }
    }
    return -1;
}

static bool has_npc_at(int cx, int cy)
{
    for (uint8_t i = 0u; i < APP_MAX_NPCS; i++) {
        uint16_t off = (uint16_t)((i + 1u) * 16u);
        if (wram_u8((uint16_t)(GB_ADDR_SPRITE_DATA1 + off + GB_SPRITE1_PICTURE_ID)) == 0u) {
            continue;
        }
        int nx = (int)wram_u8((uint16_t)(GB_ADDR_SPRITE_DATA2 + off + GB_SPRITE2_MAP_X)) - 4;
        int ny = (int)wram_u8((uint16_t)(GB_ADDR_SPRITE_DATA2 + off + GB_SPRITE2_MAP_Y)) - 4;
        if (nx == cx && ny == cy) {
            return true;
        }
    }
    return false;
}

static void execute_warp(const map_warp_t *w)
{
    uint8_t dst_map = w->dst_map_id;
    uint8_t dst_warp = w->dst_warp_id;

    if (is_outdoor_map(s_cur_map)) {
        s_last_outdoor_map = s_cur_map;
        s_last_outdoor_x = s_player_x;
        s_last_outdoor_y = s_player_y;
    }

    if (dst_map == 0xFFu) {
        dst_map = s_last_outdoor_map;
    }

    if (!load_map(dst_map, s_last_outdoor_x, s_last_outdoor_y)) {
        return;
    }

    if (dst_warp < s_num_warps) {
        set_player_pos(s_warps[dst_warp].x, s_warps[dst_warp].y);
    }
}

static bool try_cross_connection(int nx, int ny)
{
    int dir = -1;
    if (ny < 0) {
        dir = CONN_N;
    } else if (ny >= (int)(s_map_h * 2u)) {
        dir = CONN_S;
    } else if (nx < 0) {
        dir = CONN_W;
    } else if (nx >= (int)(s_map_w * 2u)) {
        dir = CONN_E;
    }

    if (dir < 0 || !s_conns[dir].active) {
        return false;
    }

    uint8_t dst_map = s_conns[dir].dst_map_id;
    uint8_t dst_w = 0u;
    uint8_t dst_h = 0u;
    get_map_dims(dst_map, &dst_w, &dst_h);
    if (dst_w == 0u || dst_h == 0u) {
        return false;
    }

    int target_x = (int)s_player_x;
    int target_y = (int)s_player_y;
    int max_x = (int)dst_w * 2 - 1;
    int max_y = (int)dst_h * 2 - 1;

    if (dir == CONN_N) {
        target_y = max_y;
        target_x += (int)s_conns[dir].x_align;
    } else if (dir == CONN_S) {
        target_y = 0;
        target_x += (int)s_conns[dir].x_align;
    } else if (dir == CONN_W) {
        target_x = max_x;
        target_y += (int)s_conns[dir].y_align;
    } else { /* CONN_E */
        target_x = 0;
        target_y += (int)s_conns[dir].y_align;
    }

    if (target_x < 0) target_x = 0;
    if (target_x > max_x) target_x = max_x;
    if (target_y < 0) target_y = 0;
    if (target_y > max_y) target_y = max_y;

    return load_map(dst_map, (uint8_t)target_x, (uint8_t)target_y);
}

/* ============================================================================
 * Wild encounter & turn-based battle logic
 * ========================================================================== */

static void start_wild_battle(uint8_t species, uint8_t level)
{
    const species_info_t *info = lookup_species_info(species);

    s_in_battle = 1u;
    s_btl_mode = BTL_MAIN;
    s_btl_main_cursor = 0u;
    s_btl_move_cursor = 0u;
    s_btl_exit_after_msg = false;
    s_btl_msg_timer = 0u;

    s_enemy_species = species;
    s_enemy_level = level;
    s_enemy_type1 = (info != NULL) ? info->type1 : GB_TYPE_NORMAL;
    s_enemy_type2 = (info != NULL) ? info->type2 : GB_TYPE_NORMAL;
    s_enemy_max_hp = calc_max_hp(species, level);
    s_enemy_hp = s_enemy_max_hp;

    /* Roll Gen 1 DVs; 1/64 chance of Gen 2 shiny DVs so shiny HUD can shine. */
    uint32_t r = next_rng();
    if ((r & 0x3Fu) == 0u) {
        s_enemy_dv_ad = (uint8_t)(((((r >> 6) & 0x0Fu) | 0x02u) << 4) | 10u);
        s_enemy_dv_ss = 0xAAu;
    } else {
        s_enemy_dv_ad = (uint8_t)((r >> 8) & 0xFFu);
        s_enemy_dv_ss = (uint8_t)((r >> 16) & 0xFFu);
    }
    s_enemy_shiny = (pokered_is_shiny_dv(s_enemy_dv_ad, s_enemy_dv_ss) != 0);

    /* Ensure active party mon is alive. */
    uint8_t pcount = wram_u8(GB_ADDR_PARTY_COUNT);
    if (s_active_party_idx >= pcount || s_party[s_active_party_idx].hp == 0u) {
        s_active_party_idx = 0u;
        for (uint8_t i = 0u; i < pcount && i < 6u; i++) {
            if (s_party[i].hp > 0u) {
                s_active_party_idx = i;
                break;
            }
        }
    }

    sync_battle_wram();
}

static bool try_trigger_wild_encounter(void)
{
    if (s_cur_map >= MAX_MAPS || s_grass_tile == 0xFFu) {
        return false;
    }

    uint8_t tid = cell_bottom_left_tile((int)s_player_x, (int)s_player_y);
    if (tid != s_grass_tile) {
        return false;
    }

    uint16_t wild_ptr = rom_u16(ROM_WILD_DATA_PTRS + (size_t)s_cur_map * 2u);
    size_t wild_off = rom_bank_off(0x03u, wild_ptr);
    uint8_t rate = rom_u8(wild_off);
    if (rate == 0u) {
        return false;
    }

    /* ~12% encounter chance per tall-grass step. */
    if ((next_rng() % 100u) >= 12u) {
        return false;
    }

    /* Authentic Gen 1 slot weights (sum = 256). */
    static const uint16_t k_slot_thresh[10] = {
        51u, 102u, 141u, 166u, 191u, 216u, 229u, 242u, 253u, 256u
    };
    uint16_t roll = (uint16_t)(next_rng() & 0xFFu);
    uint8_t slot = 9u;
    for (uint8_t i = 0u; i < 10u; i++) {
        if (roll < k_slot_thresh[i]) {
            slot = i;
            break;
        }
    }

    uint8_t level   = rom_u8(wild_off + 1u + (size_t)slot * 2u);
    uint8_t species = rom_u8(wild_off + 1u + (size_t)slot * 2u + 1u);
    if (species == 0u || species == 0xFFu || level == 0u) {
        return false;
    }

    start_wild_battle(species, level);
    return true;
}

static void show_battle_msg(const char *l1, const char *l2, bool exit_after)
{
    snprintf(s_btl_msg_line1, sizeof(s_btl_msg_line1), "%s", l1 ? l1 : "");
    snprintf(s_btl_msg_line2, sizeof(s_btl_msg_line2), "%s", l2 ? l2 : "");
    s_btl_mode = BTL_MSG;
    s_btl_msg_timer = 55u;
    s_btl_exit_after_msg = exit_after;
}

static void enemy_counter_attack(void)
{
    party_mon_t *pm = &s_party[s_active_party_idx];
    uint16_t dmg = (uint16_t)((s_enemy_level / 2u) + 2u + (next_rng() % 3u));
    if (dmg >= pm->hp) {
        pm->hp = 0u;
        uint8_t pcount = wram_u8(GB_ADDR_PARTY_COUNT);
        uint8_t next_alive = 0xFFu;
        for (uint8_t i = 0u; i < pcount && i < 6u; i++) {
            if (s_party[i].hp > 0u) {
                next_alive = i;
                break;
            }
        }
        if (next_alive != 0xFFu) {
            s_active_party_idx = next_alive;
            sync_battle_wram();
        } else {
            /* Whiteout recovery: heal party and return to Pallet Town. */
            for (uint8_t i = 0u; i < pcount && i < 6u; i++) {
                s_party[i].hp = s_party[i].max_hp;
            }
            s_active_party_idx = 0u;
            show_battle_msg("PARTY FAINTED!", "BACK TO PALLET!", true);
            load_map(0x00u, 5u, 6u);
        }
    } else {
        pm->hp = (uint16_t)(pm->hp - dmg);
    }
}

static void execute_player_move(uint8_t move_slot)
{
    party_mon_t *pm = &s_party[s_active_party_idx];
    uint8_t move_id = pm->moves[move_slot];
    if (move_id == 0u) {
        return;
    }

    const move_info_t *minfo = lookup_move_info(move_id);
    uint8_t power = (minfo != NULL) ? minfo->power : 35u;
    uint8_t mtype = (minfo != NULL) ? minfo->type_id : GB_TYPE_NORMAL;
    const species_info_t *pinfo = lookup_species_info(pm->species);

    if (power == 0u) {
        enemy_counter_attack();
        if (s_btl_mode != BTL_MSG) {
            show_battle_msg("USED GROWL!", "FOE ATK FELL!", false);
        }
        return;
    }

    uint32_t atk = (pinfo != NULL) ? (uint32_t)pinfo->base_atk : 50u;
    uint32_t dmg = (((2u * (uint32_t)pm->level / 5u + 2u) * (uint32_t)power * atk) / 50u) / 12u + 2u;

    /* STAB bonus (1.5x). */
    if (pinfo != NULL && (pinfo->type1 == mtype || pinfo->type2 == mtype)) {
        dmg = (dmg * 3u) / 2u;
    }

    /* Gen 1 type effectiveness. */
    int e1 = single_type_eff_half(mtype, s_enemy_type1);
    int e2 = (s_enemy_type2 != s_enemy_type1) ? single_type_eff_half(mtype, s_enemy_type2) : 2;
    dmg = (dmg * (uint32_t)(e1 * e2)) / 4u;
    if (dmg == 0u && e1 != 0 && e2 != 0) {
        dmg = 1u;
    }

    char ename[16];
    format_species_name(s_enemy_species, ename, sizeof(ename));

    if (dmg >= s_enemy_hp) {
        s_enemy_hp = 0u;
        wram_w8(GB_ADDR_ENEMY_MON_HP_HI, 0u);
        wram_w8(GB_ADDR_ENEMY_MON_HP_LO, 0u);

        char l1[28];
        snprintf(l1, sizeof(l1), "FOE %s", ename);
        show_battle_msg(l1, "FAINTED!", true);
        return;
    }

    s_enemy_hp = (uint16_t)(s_enemy_hp - (uint16_t)dmg);
    wram_w8(GB_ADDR_ENEMY_MON_HP_HI, (uint8_t)((s_enemy_hp >> 8) & 0xFFu));
    wram_w8(GB_ADDR_ENEMY_MON_HP_LO, (uint8_t)(s_enemy_hp & 0xFFu));

    enemy_counter_attack();
    if (s_btl_mode != BTL_MSG) {
        char l1[28];
        snprintf(l1, sizeof(l1), "USED %s!", (minfo != NULL) ? minfo->name : "ATTACK");
        const char *l2 = (e1 * e2 >= 8) ? "SUPER EFFECTIVE!" :
                         (e1 * e2 == 0) ? "NO EFFECT!" :
                         (e1 * e2 < 4)  ? "NOT VERY EFF..." : "FOE STRUCK BACK!";
        show_battle_msg(l1, l2, false);
    }
}

static void execute_catch_attempt(void)
{
    uint32_t hp_factor = ((uint32_t)s_enemy_max_hp * 3u - (uint32_t)s_enemy_hp * 2u) * 100u /
                         ((uint32_t)s_enemy_max_hp * 3u);
    uint32_t chance = 35u + (hp_factor * 65u) / 100u;
    if (chance > 98u) {
        chance = 98u;
    }

    char ename[16];
    format_species_name(s_enemy_species, ename, sizeof(ename));

    if ((next_rng() % 100u) < chance) {
        uint8_t pcount = wram_u8(GB_ADDR_PARTY_COUNT);
        if (pcount < 6u) {
            s_party[pcount].species = s_enemy_species;
            s_party[pcount].level = s_enemy_level;
            s_party[pcount].max_hp = s_enemy_max_hp;
            s_party[pcount].hp = s_enemy_hp ? s_enemy_hp : 1u;
            default_moves_for_species(s_enemy_species, s_party[pcount].moves);
            wram_w8((uint16_t)(GB_ADDR_PARTY_SPECIES + pcount), s_enemy_species);
            wram_w8(GB_ADDR_PARTY_COUNT, (uint8_t)(pcount + 1u));
            show_battle_msg("GOTCHA!", "ADDED TO PARTY!", true);
        } else {
            uint8_t bcount = wram_u8(GB_ADDR_BOX_COUNT);
            if (bcount < APP_MAX_BOX_MONS) {
                wram_w8((uint16_t)(GB_ADDR_BOX_SPECIES + bcount), s_enemy_species);
                wram_w8(GB_ADDR_BOX_COUNT, (uint8_t)(bcount + 1u));
                show_battle_msg("GOTCHA!", "SENT TO BOX 1!", true);
            } else {
                show_battle_msg("GOTCHA!", "BOX 1 IS FULL!", true);
            }
        }
        return;
    }

    enemy_counter_attack();
    if (s_btl_mode != BTL_MSG) {
        char l1[28];
        snprintf(l1, sizeof(l1), "%s", ename);
        show_battle_msg(l1, "BROKE FREE!", false);
    }
}

static void update_battle_frame(void)
{
    bool p_up    = s_in_cur.up    && !s_in_prev.up;
    bool p_down  = s_in_cur.down  && !s_in_prev.down;
    bool p_left  = s_in_cur.left  && !s_in_prev.left;
    bool p_right = s_in_cur.right && !s_in_prev.right;
    bool p_a     = s_in_cur.a     && !s_in_prev.a;
    bool p_b     = s_in_cur.b     && !s_in_prev.b;

    if (s_btl_mode == BTL_MSG) {
        if (s_btl_msg_timer > 0u) {
            s_btl_msg_timer--;
        }
        if (p_a || p_b || s_btl_msg_timer == 0u) {
            if (s_btl_exit_after_msg) {
                s_in_battle = 0u;
                s_enemy_species = 0u;
                s_enemy_level = 0u;
                s_enemy_shiny = false;
                wram_w8(GB_ADDR_IS_IN_BATTLE, 0u);
                wram_w8(GB_ADDR_ENEMY_MON_SPECIES, 0u);
            } else {
                s_btl_mode = BTL_MAIN;
            }
        }
        sync_battle_wram();
        return;
    }

    if (s_btl_mode == BTL_MAIN) {
        if (p_up || p_left) {
            s_btl_main_cursor = (uint8_t)((s_btl_main_cursor + 2u) % 3u);
        } else if (p_down || p_right) {
            s_btl_main_cursor = (uint8_t)((s_btl_main_cursor + 1u) % 3u);
        } else if (p_a) {
            if (s_btl_main_cursor == 0u) {
                s_btl_mode = BTL_FIGHT;
            } else if (s_btl_main_cursor == 1u) {
                execute_catch_attempt();
            } else {
                show_battle_msg("GOT AWAY", "SAFELY!", true);
            }
        }
    } else if (s_btl_mode == BTL_FIGHT) {
        if (p_up) {
            s_btl_move_cursor = (uint8_t)((s_btl_move_cursor + 3u) & 3u);
        } else if (p_down) {
            s_btl_move_cursor = (uint8_t)((s_btl_move_cursor + 1u) & 3u);
        } else if (p_left || p_right) {
            s_btl_move_cursor ^= 1u;
        } else if (p_b) {
            s_btl_mode = BTL_MAIN;
        } else if (p_a) {
            execute_player_move(s_btl_move_cursor);
        }
    }

    sync_battle_wram();
}

/* ============================================================================
 * Software 160x144 RGB565 renderers (Overworld 2D + Battle Screen)
 * ========================================================================== */

static void fb_fill_rect(int x, int y, int w, int h, uint16_t color)
{
    int x0 = (x < 0) ? 0 : x;
    int y0 = (y < 0) ? 0 : y;
    int x1 = (x + w > GB_LCD_W) ? GB_LCD_W : (x + w);
    int y1 = (y + h > GB_LCD_H) ? GB_LCD_H : (y + h);
    for (int py = y0; py < y1; py++) {
        uint16_t *row = &s_lcd_fb[py * GB_LCD_W];
        for (int px = x0; px < x1; px++) {
            row[px] = color;
        }
    }
}

static void fb_draw_rect(int x, int y, int w, int h, uint16_t color)
{
    if (w <= 0 || h <= 0) {
        return;
    }
    fb_fill_rect(x, y, w, 1, color);
    fb_fill_rect(x, y + h - 1, w, 1, color);
    fb_fill_rect(x, y, 1, h, color);
    fb_fill_rect(x + w - 1, y, 1, h, color);
}

static void fb_draw_text(int x, int y, const char *txt, uint16_t fg)
{
    if (txt == NULL) {
        return;
    }
    int cx = x;
    for (size_t i = 0u; txt[i] != '\0'; i++) {
        unsigned char ch = (unsigned char)txt[i];
        if (ch >= 32u && ch <= 126u) {
            const uint8_t *cols = s_mini_font[ch - 32u];
            for (int col = 0; col < 5; col++) {
                int px = cx + col;
                if (px < 0 || px >= GB_LCD_W) {
                    continue;
                }
                uint8_t bits = cols[col];
                for (int row = 0; row < 7; row++) {
                    if ((bits & (1u << row)) != 0u) {
                        int py = y + row;
                        if (py >= 0 && py < GB_LCD_H) {
                            s_lcd_fb[py * GB_LCD_W + px] = fg;
                        }
                    }
                }
            }
        }
        cx += 6;
    }
}

static void fb_draw_vram_sprite(int dst_x, int dst_y, uint8_t img_idx, bool flip_x,
                                uint16_t col1, uint16_t col2, uint16_t col3)
{
    static const uint8_t k_qox[4] = { 0u, 8u, 0u, 8u };
    static const uint8_t k_qoy[4] = { 0u, 0u, 8u, 8u };
    uint8_t base_tile = (uint8_t)(img_idx & 0xFCu);
    if (base_tile > MAX_VRAM_SPRITE_TILES) {
        return;
    }

    uint16_t cols[4] = { 0u, col1, col2, col3 };
    for (int q = 0; q < 4; q++) {
        size_t t_off = (size_t)(base_tile + (uint8_t)q) * 16u;
        int ox = (int)k_qox[q];
        int oy = (int)k_qoy[q];

        for (int py = 0; py < 8; py++) {
            uint8_t lo = s_vram[t_off + (size_t)(py * 2)];
            uint8_t hi = s_vram[t_off + (size_t)(py * 2 + 1)];
            for (int px = 0; px < 8; px++) {
                int bit = 7 - px;
                uint8_t shade = (uint8_t)(((lo >> bit) & 1u) | (((hi >> bit) & 1u) << 1));
                if (shade == 0u) {
                    continue;
                }
                int sx = dst_x + (flip_x ? (15 - (ox + px)) : (ox + px));
                int sy = dst_y + oy + py;
                if (sx >= 0 && sx < GB_LCD_W && sy >= 0 && sy < GB_LCD_H) {
                    s_lcd_fb[sy * GB_LCD_W + sx] = cols[shade];
                }
            }
        }
    }
}

static int floor_div4_i(int v)
{
    return (v >= 0) ? (v / 4) : -((-v + 3) / 4);
}

static void render_overworld_2d(void)
{
    static const uint16_t k_dmg_pal[4] = {
        RGB565_C(248, 244, 224),
        RGB565_C(184, 176, 144),
        RGB565_C(104, 100,  80),
        RGB565_C( 24,  24,  20)
    };

    if (s_map_w == 0u || s_map_h == 0u) {
        return;
    }

    const int cam_px = (int)s_player_x * 16 - (int)s_walk_dx * (int)s_walk_counter * 2 - 64;
    const int cam_py = (int)s_player_y * 16 - (int)s_walk_dy * (int)s_walk_counter * 2 - 64;
    const int stride = (int)s_map_w + 6;
    const int max_bx = (int)s_map_w + 5;
    const int max_by = (int)s_map_h + 5;

    for (int sy = 0; sy < GB_LCD_H; sy++) {
        int wy = cam_py + sy;
        int ty = (wy >= 0) ? (wy / 8) : -((-wy + 7) / 8);
        int py = wy - ty * 8;
        int by = floor_div4_i(ty) + 3;
        if (by < 0) by = 0;
        if (by > max_by) by = max_by;
        int sub_y = ty & 3;

        uint16_t *dst_row = &s_lcd_fb[sy * GB_LCD_W];

        for (int sx = 0; sx < GB_LCD_W; sx++) {
            int wx = cam_px + sx;
            int tx = (wx >= 0) ? (wx / 8) : -((-wx + 7) / 8);
            int px = wx - tx * 8;
            int bx = floor_div4_i(tx) + 3;
            if (bx < 0) bx = 0;
            if (bx > max_bx) bx = max_bx;
            int sub_x = tx & 3;

            uint32_t map_idx = (uint32_t)by * (uint32_t)stride + (uint32_t)bx;
            uint8_t blk = (map_idx < MAP_OVERWORLD_BYTES)
                          ? s_wram[(GB_ADDR_OVERWORLD_MAP - 0xC000u) + map_idx]
                          : 0u;
            uint16_t tile_addr = (uint16_t)(s_ts_blocks_ptr + (uint16_t)blk * 16u + (uint16_t)(sub_y * 4 + sub_x));
            uint8_t tid = (uint8_t)(gb_brain_read_rom_bank(s_ts_bank, tile_addr) & 0x7Fu);

            size_t t_off = 0x1000u + (size_t)tid * 16u + (size_t)(py * 2);
            uint8_t lo = s_vram[t_off];
            uint8_t hi = s_vram[t_off + 1u];
            int bit = 7 - px;
            uint8_t shade = (uint8_t)(((lo >> bit) & 1u) | (((hi >> bit) & 1u) << 1));
            dst_row[sx] = k_dmg_pal[shade];
        }
    }

    /* Draw NPC sprites. */
    for (uint8_t i = 0u; i < APP_MAX_NPCS; i++) {
        uint16_t off = (uint16_t)((i + 1u) * 16u);
        if (wram_u8((uint16_t)(GB_ADDR_SPRITE_DATA1 + off + GB_SPRITE1_PICTURE_ID)) == 0u) {
            continue;
        }
        uint8_t img = wram_u8((uint16_t)(GB_ADDR_SPRITE_DATA1 + off + GB_SPRITE1_IMAGE_INDEX));
        uint8_t dir = wram_u8((uint16_t)(GB_ADDR_SPRITE_DATA1 + off + GB_SPRITE1_FACING_DIR));
        int nx = ((int)wram_u8((uint16_t)(GB_ADDR_SPRITE_DATA2 + off + GB_SPRITE2_MAP_X)) - 4) * 16 - cam_px;
        int ny = ((int)wram_u8((uint16_t)(GB_ADDR_SPRITE_DATA2 + off + GB_SPRITE2_MAP_Y)) - 4) * 16 - cam_py - 4;
        if (img < MAX_VRAM_SPRITE_TILES) {
            fb_draw_vram_sprite(nx, ny, img, (dir == 12u),
                                RGB565_C(244, 218, 184), RGB565_C(72, 148, 224), RGB565_C(24, 24, 20));
        }
    }

    /* Draw Red centered at (64, 60). */
    uint8_t red_img = wram_u8((uint16_t)(GB_ADDR_SPRITE_DATA1 + GB_SPRITE1_IMAGE_INDEX));
    fb_draw_vram_sprite(64, 60, red_img, (s_facing_dir == 12u),
                        RGB565_C(248, 224, 192), RGB565_C(224, 48, 48), RGB565_C(24, 24, 20));
}

static void render_battle_hp_bar(int x, int y, int w, uint16_t hp, uint16_t max_hp)
{
    uint16_t frame_col = RGB565_C(32, 36, 44);
    uint16_t empty_col = RGB565_C(64, 68, 76);
    fb_draw_rect(x, y, w, 6, frame_col);
    fb_fill_rect(x + 1, y + 1, w - 2, 4, empty_col);

    if (max_hp == 0u || hp == 0u) {
        return;
    }
    int fill = (int)(((uint32_t)hp * (uint32_t)(w - 2)) / (uint32_t)max_hp);
    if (fill < 1) fill = 1;
    if (fill > w - 2) fill = w - 2;

    uint16_t bar_col = (hp * 2u > max_hp)
                       ? RGB565_C(72, 208, 96)
                       : ((hp * 4u > max_hp) ? RGB565_C(240, 192, 48) : RGB565_C(224, 64, 56));
    fb_fill_rect(x + 1, y + 1, fill, 4, bar_col);
}

static void render_battle_mon_badge(int cx, int cy, uint8_t species, bool shiny, bool is_player)
{
    const species_info_t *info = lookup_species_info(species);
    uint16_t body = (info != NULL) ? info->col_primary : RGB565_C(160, 168, 184);
    uint16_t acc  = (info != NULL) ? info->col_accent  : RGB565_C(224, 228, 236);
    if (shiny) {
        body = RGB565_C(255, 212, 48);
        acc  = RGB565_C(255, 248, 192);
    }

    int sz = is_player ? 30 : 26;
    int x0 = cx - sz / 2;
    int y0 = cy - sz / 2;
    fb_fill_rect(x0, y0 + 2, sz, sz - 4, body);
    fb_fill_rect(x0 + 3, y0, sz - 6, 6, acc);
    fb_draw_rect(x0, y0 + 2, sz, sz - 4, RGB565_C(24, 24, 20));

    /* Eyes */
    int eye_y = y0 + (sz / 3);
    if (!is_player) {
        fb_fill_rect(cx - 6, eye_y, 3, 4, RGB565_C(24, 24, 20));
        fb_fill_rect(cx + 3, eye_y, 3, 4, RGB565_C(24, 24, 20));
    } else {
        fb_fill_rect(x0 + 5, y0 + 8, sz - 10, 3, acc);
    }
}

static void render_battle_2d(void)
{
    const uint16_t bg_col     = RGB565_C(244, 240, 224);
    const uint16_t plate_col  = RGB565_C(216, 212, 188);
    const uint16_t box_bg     = RGB565_C(248, 246, 236);
    const uint16_t border_col = RGB565_C(32, 36, 44);
    const uint16_t text_col   = RGB565_C(24, 28, 36);
    const uint16_t sel_col    = RGB565_C(208, 48, 48);

    const party_mon_t *pm = &s_party[s_active_party_idx];
    char ename[16];
    char pname[16];
    char buf[32];
    format_species_name(s_enemy_species, ename, sizeof(ename));
    format_species_name(pm->species, pname, sizeof(pname));

    fb_fill_rect(0, 0, GB_LCD_W, GB_LCD_H, bg_col);

    /* Enemy platform + badge (top-right). */
    fb_fill_rect(96, 42, 56, 10, plate_col);
    render_battle_mon_badge(124, 28, s_enemy_species, s_enemy_shiny, false);

    /* Enemy info box (top-left). */
    fb_fill_rect(4, 4, 86, 28, box_bg);
    fb_draw_rect(4, 4, 86, 28, border_col);
    snprintf(buf, sizeof(buf), "%.10s", ename);
    fb_draw_text(8, 7, buf, text_col);
    snprintf(buf, sizeof(buf), "L%u", (unsigned)s_enemy_level);
    fb_draw_text(64, 7, buf, text_col);
    render_battle_hp_bar(8, 18, 76, s_enemy_hp, s_enemy_max_hp);
    if (s_enemy_shiny) {
        fb_draw_text(8, 25, "*", RGB565_C(220, 148, 16));
    }

    /* Player platform + badge (bottom-left). */
    fb_fill_rect(10, 84, 58, 10, plate_col);
    render_battle_mon_badge(39, 70, pm->species, false, true);

    /* Player info box (mid-right). */
    fb_fill_rect(72, 56, 84, 36, box_bg);
    fb_draw_rect(72, 56, 84, 36, border_col);
    snprintf(buf, sizeof(buf), "%.10s", pname);
    fb_draw_text(76, 59, buf, text_col);
    snprintf(buf, sizeof(buf), "L%u", (unsigned)pm->level);
    fb_draw_text(134, 59, buf, text_col);
    render_battle_hp_bar(76, 70, 74, pm->hp, pm->max_hp);
    snprintf(buf, sizeof(buf), "%u/%u", (unsigned)pm->hp, (unsigned)pm->max_hp);
    fb_draw_text(98, 80, buf, text_col);

    /* Bottom menu / message box (rows 96..143). */
    fb_fill_rect(2, 96, GB_LCD_W - 4, 46, box_bg);
    fb_draw_rect(2, 96, GB_LCD_W - 4, 46, border_col);
    fb_draw_rect(4, 98, GB_LCD_W - 8, 42, border_col);

    if (s_btl_mode == BTL_MSG) {
        fb_draw_text(10, 106, s_btl_msg_line1, text_col);
        fb_draw_text(10, 120, s_btl_msg_line2, text_col);
    } else if (s_btl_mode == BTL_FIGHT) {
        for (uint8_t i = 0u; i < 4u; i++) {
            int mx = (i & 1u) ? 84 : 10;
            int my = (i >= 2u) ? 121 : 106;
            uint8_t mid = pm->moves[i];
            const move_info_t *mi = (mid != 0u) ? lookup_move_info(mid) : NULL;
            const char *mname = (mi != NULL) ? mi->name : ((mid != 0u) ? "MOVE" : "--");
            if (s_btl_move_cursor == i) {
                fb_draw_text(mx, my, ">", sel_col);
            }
            snprintf(buf, sizeof(buf), "%.9s", mname);
            fb_draw_text(mx + 7, my, buf, (s_btl_move_cursor == i) ? sel_col : text_col);
        }
    } else {
        static const char *k_opts[3] = { "FIGHT", "CATCH", "RUN" };
        fb_draw_text(10, 106, "WILD", text_col);
        fb_draw_text(10, 120, "BATTLE!", text_col);
        fb_draw_rect(76, 98, 1, 42, border_col);

        for (uint8_t i = 0u; i < 3u; i++) {
            int ox = 84;
            int oy = 104 + (int)i * 12;
            if (s_btl_main_cursor == i) {
                fb_draw_text(ox, oy, ">", sel_col);
            }
            fb_draw_text(ox + 8, oy, k_opts[i], (s_btl_main_cursor == i) ? sel_col : text_col);
        }
    }
}

/* ============================================================================
 * Public gb_brain.h API
 * ========================================================================== */

static void detect_rom_metadata(app_state_t *state)
{
    size_t len = 0u;
    for (uint16_t addr = 0x0134u; addr <= 0x0143u; addr++) {
        uint8_t ch = rom_u8(addr);
        if (ch == 0u || ch < 0x20u || ch > 0x7Eu) {
            break;
        }
        state->rom_title[len++] = (char)ch;
    }
    state->rom_title[len] = '\0';

    if (strstr(state->rom_title, "YELLOW") != NULL) {
        state->version = GAME_VER_YELLOW;
    } else if (strstr(state->rom_title, "RED") != NULL) {
        state->version = GAME_VER_RED;
    } else if (strstr(state->rom_title, "BLUE") != NULL) {
        state->version = GAME_VER_BLUE;
    } else {
        state->version = GAME_VER_UNKNOWN;
    }
}

static void sync_snapshot(app_state_t *state)
{
    if (state == NULL) {
        return;
    }

    state->snap.cur_map      = wram_u8(GB_ADDR_CUR_MAP);
    state->snap.tileset      = wram_u8(GB_ADDR_CUR_MAP_TILESET);
    state->snap.map_w        = wram_u8(GB_ADDR_CUR_MAP_WIDTH);
    state->snap.map_h        = wram_u8(GB_ADDR_CUR_MAP_HEIGHT);
    state->snap.player_x     = wram_u8(GB_ADDR_X_COORD);
    state->snap.player_y     = wram_u8(GB_ADDR_Y_COORD);
    state->snap.prev_x       = s_prev_x;
    state->snap.prev_y       = s_prev_y;
    state->snap.facing_dir   = wram_u8((uint16_t)(GB_ADDR_SPRITE_DATA1 + GB_SPRITE1_FACING_DIR));
    state->snap.walk_counter = wram_u8(GB_ADDR_WALK_COUNTER);
    state->snap.walk_dx      = (int8_t)wram_u8((uint16_t)(GB_ADDR_SPRITE_DATA1 + GB_SPRITE1_X_STEP_VEC));
    state->snap.walk_dy      = (int8_t)wram_u8((uint16_t)(GB_ADDR_SPRITE_DATA1 + GB_SPRITE1_Y_STEP_VEC));
    state->snap.in_battle    = wram_u8(GB_ADDR_IS_IN_BATTLE);
    state->snap.ui_overlay   = (uint8_t)UI_OVERLAY_NONE;

    state->snap.party_count  = wram_u8(GB_ADDR_PARTY_COUNT);
    state->snap.lead_species = wram_u8(GB_ADDR_PARTY_SPECIES);
    for (uint16_t i = 0u; i < 6u; i++) {
        state->snap.party_species[i] = wram_u8((uint16_t)(GB_ADDR_PARTY_SPECIES + i));
    }

    state->snap.cur_box_num = (uint8_t)(wram_u8(GB_ADDR_CUR_BOX) & 0x7Fu);
    uint8_t box_cnt = wram_u8(GB_ADDR_BOX_COUNT);
    if (box_cnt > APP_MAX_BOX_MONS) {
        box_cnt = APP_MAX_BOX_MONS;
    }
    state->snap.box_count = box_cnt;
    for (uint16_t i = 0u; i < APP_MAX_BOX_MONS; i++) {
        state->snap.box_species[i] = wram_u8((uint16_t)(GB_ADDR_BOX_SPECIES + i));
    }

    for (uint16_t s = 0u; s < APP_MAX_NPCS; s++) {
        uint16_t off = (uint16_t)((s + 1u) * 16u);
        uint8_t pic = wram_u8((uint16_t)(GB_ADDR_SPRITE_DATA1 + off + GB_SPRITE1_PICTURE_ID));
        if (pic != 0u) {
            state->snap.npcs[s].active     = true;
            state->snap.npcs[s].picture_id = pic;
            state->snap.npcs[s].facing_dir = wram_u8((uint16_t)(GB_ADDR_SPRITE_DATA1 + off + GB_SPRITE1_FACING_DIR));
            state->snap.npcs[s].map_y      = (uint8_t)(wram_u8((uint16_t)(GB_ADDR_SPRITE_DATA2 + off + GB_SPRITE2_MAP_Y)) - 4u);
            state->snap.npcs[s].map_x      = (uint8_t)(wram_u8((uint16_t)(GB_ADDR_SPRITE_DATA2 + off + GB_SPRITE2_MAP_X)) - 4u);
        } else {
            state->snap.npcs[s].active     = false;
            state->snap.npcs[s].picture_id = 0u;
            state->snap.npcs[s].facing_dir = 0u;
            state->snap.npcs[s].map_x      = 0u;
            state->snap.npcs[s].map_y      = 0u;
        }
    }

    state->snap.enemy_species = wram_u8(GB_ADDR_ENEMY_MON_SPECIES);
    state->snap.enemy_type1   = wram_u8(GB_ADDR_ENEMY_MON_TYPE1);
    state->snap.enemy_type2   = wram_u8(GB_ADDR_ENEMY_MON_TYPE2);
    state->snap.enemy_level   = wram_u8(GB_ADDR_ENEMY_MON_LEVEL);
    state->snap.enemy_shiny   = (pokered_is_shiny_dv(wram_u8(GB_ADDR_ENEMY_MON_DVS),
                                                     wram_u8((uint16_t)(GB_ADDR_ENEMY_MON_DVS + 1u))) != 0);
    for (uint16_t i = 0u; i < 4u; i++) {
        state->snap.battle_moves[i] = wram_u8((uint16_t)(GB_ADDR_BATTLE_MON_MOVES + i));
    }
    state->snap.menu_cursor = wram_u8(GB_ADDR_CURRENT_MENU_ITEM);
}

bool gb_brain_init(const char *rom_path, app_state_t *state)
{
    FILE *fp;
    long file_sz;
    size_t nread;

    if (rom_path == NULL || state == NULL) {
        return false;
    }

    if (s_initialized || s_rom_data != NULL) {
        gb_brain_shutdown();
    }

    state->rom_loaded = false;
    state->version = GAME_VER_UNKNOWN;
    state->rom_title[0] = '\0';
    memset(&state->snap, 0, sizeof(state->snap));

#ifndef OF_PC
    if (of_file_slot_find(rom_path, NULL) < 0) {
        of_file_slot_register(0u, rom_path);
    }
#endif

    fp = fopen(rom_path, "rb");
    if (fp == NULL) {
        return false;
    }

    if (fseek(fp, 0L, SEEK_END) != 0) {
        fclose(fp);
        return false;
    }
    file_sz = ftell(fp);
    if (file_sz < 0x20000L || fseek(fp, 0L, SEEK_SET) != 0) {
        fclose(fp);
        return false;
    }

    s_rom_size = (size_t)file_sz;
    s_rom_data = (uint8_t *)malloc(s_rom_size);
    if (s_rom_data == NULL) {
        s_rom_size = 0u;
        fclose(fp);
        return false;
    }

    nread = fread(s_rom_data, 1u, s_rom_size, fp);
    fclose(fp);
    if (nread != s_rom_size) {
        gb_brain_shutdown();
        return false;
    }

    memset(s_wram, 0, sizeof(s_wram));
    memset(s_vram, 0, sizeof(s_vram));
    memset(s_io, 0, sizeof(s_io));
    memset(s_lcd_fb, 0, sizeof(s_lcd_fb));
    memset(&s_in_cur, 0, sizeof(s_in_cur));
    memset(&s_in_prev, 0, sizeof(s_in_prev));

    s_rom_bank = 1u;
    s_io[GB_ADDR_LCDC - 0xFF00u] = 0xE3u;
    s_io[GB_ADDR_WY - 0xFF00u] = 0x90u;
    s_rng = 0x504F4B45u;
    s_in_battle = 0u;
    s_facing_dir = 0u; /* Facing South */

    s_initialized = true;
    detect_rom_metadata(state);
    init_starter_party_and_box();

    if (!load_map(0x00u, 5u, 6u)) {
        gb_brain_shutdown();
        return false;
    }

    render_overworld_2d();
    sync_snapshot(state);
    state->rom_loaded = true;
    return true;
}

void gb_brain_shutdown(void)
{
    free(s_rom_data);
    s_rom_data = NULL;
    s_rom_size = 0u;
    s_initialized = false;
}

void gb_brain_set_input(const gb_input_t *in)
{
    if (in != NULL) {
        s_in_cur = *in;
    } else {
        memset(&s_in_cur, 0, sizeof(s_in_cur));
    }
}

void gb_brain_run_frame(app_state_t *state)
{
    if (!s_initialized) {
        return;
    }

    (void)next_rng();

    if (s_in_battle != 0u) {
        update_battle_frame();
        render_battle_2d();
    } else {
        bool just_warped = false;

        if (s_walk_counter > 0u) {
            s_walk_counter--;
            wram_w8(GB_ADDR_WALK_COUNTER, s_walk_counter);
            if (s_walk_counter == 0u) {
                s_walk_dx = 0;
                s_walk_dy = 0;
                wram_w8((uint16_t)(GB_ADDR_SPRITE_DATA1 + GB_SPRITE1_X_STEP_VEC), 0u);
                wram_w8((uint16_t)(GB_ADDR_SPRITE_DATA1 + GB_SPRITE1_Y_STEP_VEC), 0u);

                int warp_idx = find_warp_at((int)s_player_x, (int)s_player_y);
                if (warp_idx >= 0) {
                    execute_warp(&s_warps[warp_idx]);
                    just_warped = true;
                } else {
                    (void)try_trigger_wild_encounter();
                }
            }
        }

        if (!just_warped && s_in_battle == 0u && s_walk_counter == 0u) {
            int dx = 0;
            int dy = 0;
            uint8_t new_dir = s_facing_dir;
            bool move_req = false;

            if (s_in_cur.up) {
                dy = -1;
                new_dir = 4u;
                move_req = true;
            } else if (s_in_cur.down) {
                dy = 1;
                new_dir = 0u;
                move_req = true;
            } else if (s_in_cur.left) {
                dx = -1;
                new_dir = 8u;
                move_req = true;
            } else if (s_in_cur.right) {
                dx = 1;
                new_dir = 12u;
                move_req = true;
            }

            if (move_req) {
                s_facing_dir = new_dir;
                wram_w8((uint16_t)(GB_ADDR_SPRITE_DATA1 + GB_SPRITE1_FACING_DIR), s_facing_dir);
                wram_w8((uint16_t)(GB_ADDR_SPRITE_DATA1 + GB_SPRITE1_IMAGE_INDEX),
                        facing_dir_to_frame(s_facing_dir));

                int nx = (int)s_player_x + dx;
                int ny = (int)s_player_y + dy;

                if (nx < 0 || nx >= (int)(s_map_w * 2u) || ny < 0 || ny >= (int)(s_map_h * 2u)) {
                    if (!try_cross_connection(nx, ny)) {
                        int edge_warp = find_warp_at((int)s_player_x, (int)s_player_y);
                        if (edge_warp >= 0) {
                            execute_warp(&s_warps[edge_warp]);
                        }
                    }
                } else if (!has_npc_at(nx, ny)) {
                    bool can_walk = (find_warp_at(nx, ny) >= 0);
                    if (!can_walk) {
                        uint8_t tid = cell_bottom_left_tile(nx, ny);
                        can_walk = s_walkable[tid];
                    }
                    if (can_walk) {
                        s_prev_x = s_player_x;
                        s_prev_y = s_player_y;
                        s_player_x = (uint8_t)nx;
                        s_player_y = (uint8_t)ny;
                        s_walk_dx = (int8_t)dx;
                        s_walk_dy = (int8_t)dy;
                        s_walk_counter = 8u;

                        wram_w8(GB_ADDR_X_COORD, s_player_x);
                        wram_w8(GB_ADDR_Y_COORD, s_player_y);
                        wram_w8(GB_ADDR_WALK_COUNTER, s_walk_counter);
                        wram_w8((uint16_t)(GB_ADDR_SPRITE_DATA1 + GB_SPRITE1_X_STEP_VEC), (uint8_t)s_walk_dx);
                        wram_w8((uint16_t)(GB_ADDR_SPRITE_DATA1 + GB_SPRITE1_Y_STEP_VEC), (uint8_t)s_walk_dy);
                        wram_w8((uint16_t)(GB_ADDR_SPRITE_DATA2 + GB_SPRITE2_MAP_Y), (uint8_t)(s_player_y + 4u));
                        wram_w8((uint16_t)(GB_ADDR_SPRITE_DATA2 + GB_SPRITE2_MAP_X), (uint8_t)(s_player_x + 4u));
                    }
                }
            }
        }

        if (s_in_battle != 0u) {
            render_battle_2d();
        } else {
            render_overworld_2d();
        }
    }

    s_in_prev = s_in_cur;
    sync_snapshot(state);
}

uint8_t gb_brain_read8(uint16_t addr)
{
    if (!s_initialized) {
        return 0u;
    }
    if (addr < 0x4000u) {
        return rom_u8(addr);
    }
    if (addr < 0x8000u) {
        return gb_brain_read_rom_bank(s_rom_bank, addr);
    }
    if (addr < 0xA000u) {
        return s_vram[addr - 0x8000u];
    }
    if (addr >= 0xC000u && addr <= 0xDFFFu) {
        return s_wram[addr - 0xC000u];
    }
    if (addr >= 0xE000u && addr <= 0xFDFFu) {
        return s_wram[addr - 0xE000u];
    }
    if (addr >= 0xFF00u) {
        return s_io[addr - 0xFF00u];
    }
    return 0xFFu;
}

void gb_brain_write8(uint16_t addr, uint8_t val)
{
    if (!s_initialized) {
        return;
    }
    if (addr >= 0x2000u && addr <= 0x3FFFu) {
        uint8_t b = (uint8_t)(val & 0x7Fu);
        s_rom_bank = (b != 0u) ? b : 1u;
    } else if (addr >= 0x8000u && addr <= 0x9FFFu) {
        s_vram[addr - 0x8000u] = val;
    } else if (addr >= 0xC000u && addr <= 0xDFFFu) {
        s_wram[addr - 0xC000u] = val;
    } else if (addr >= 0xE000u && addr <= 0xFDFFu) {
        s_wram[addr - 0xE000u] = val;
    } else if (addr >= 0xFF00u) {
        s_io[addr - 0xFF00u] = val;
    }
}

uint8_t gb_brain_read_rom_bank(uint8_t bank, uint16_t addr)
{
    if (!s_initialized || s_rom_data == NULL || s_rom_size == 0u) {
        return 0xFFu;
    }
    if (addr >= 0x8000u) {
        return gb_brain_read8(addr);
    }
    return rom_u8(rom_bank_off(bank, addr));
}

const uint16_t *gb_brain_get_lcd_rgb565(void)
{
    return s_lcd_fb;
}

/* ============================================================================
 * No-op gb_audio.h stubs
 * ========================================================================== */

void gb_audio_init(void) {}
void gb_audio_push_frame(void) {}

uint8_t gb_audio_read(uint16_t addr)
{
    (void)addr;
    return 0xFFu;
}

void gb_audio_write(uint16_t addr, uint8_t val)
{
    (void)addr;
    (void)val;
}
