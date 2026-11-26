#ifndef APP_STATE_H
#define APP_STATE_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    GAME_VER_UNKNOWN = 0,
    GAME_VER_RED,
    GAME_VER_BLUE,
    GAME_VER_YELLOW
} game_version_t;

#define APP_MAX_NPCS      15
#define APP_MAX_BOX_MONS  20

typedef struct {
    bool    active;
    uint8_t picture_id;
    uint8_t map_x;          /* 16x16 walk cell X */
    uint8_t map_y;          /* 16x16 walk cell Y */
    uint8_t facing_dir;
} npc_snap_t;

typedef enum {
    UI_OVERLAY_NONE = 0,
    UI_OVERLAY_DIALOGUE,
    UI_OVERLAY_START_MENU,
    UI_OVERLAY_FULLSCREEN_2D
} ui_overlay_mode_t;

typedef struct {
    uint8_t    cur_map;
    uint8_t    tileset;
    uint8_t    map_w;
    uint8_t    map_h;
    uint8_t    player_x;
    uint8_t    player_y;
    uint8_t    prev_x;
    uint8_t    prev_y;
    uint8_t    facing_dir;     /* 0=down, 4=up, 8=left, 12=right */
    uint8_t    walk_counter;
    int8_t     walk_dx;
    int8_t     walk_dy;
    uint8_t    in_battle;      /* 0=overworld, 1=wild, 2=trainer */
    uint8_t    ui_overlay;     /* ui_overlay_mode_t */
    uint8_t    party_count;
    uint8_t    lead_species;
    uint8_t    party_species[6];
    uint8_t    cur_box_num;    /* 0..11 */
    uint8_t    box_count;      /* 0..20 */
    uint8_t    box_species[APP_MAX_BOX_MONS];
    uint8_t    enemy_species;
    uint8_t    enemy_type1;
    uint8_t    enemy_type2;
    uint8_t    enemy_level;
    bool       enemy_shiny;
    uint8_t    battle_moves[4];
    uint8_t    menu_cursor;
    npc_snap_t npcs[APP_MAX_NPCS];
} game_snapshot_t;

typedef struct {
    bool            running;
    bool            rom_loaded;
    bool            mods_menu_open;
    bool            gen3box_open;
    bool            show_debug_hud;
    game_version_t  version;
    char            rom_title[17];
    uint32_t        frame_count;
    game_snapshot_t snap;
    /* Camera pitch rung for Dramatic Shape Voxel mod: 0=OFF, 15, 35, 50, 75 */
    int             voxel_pitch_deg;
    /* Live frame performance counters */
    uint16_t        perf_fps;
    uint16_t        perf_gb_ms;
    uint16_t        perf_3d_ms;
} app_state_t;

#endif /* APP_STATE_H */
