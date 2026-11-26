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

typedef struct {
    uint8_t  cur_map;
    uint8_t  tileset;
    uint8_t  map_w;
    uint8_t  map_h;
    uint8_t  player_x;
    uint8_t  player_y;
    uint8_t  prev_x;
    uint8_t  prev_y;
    uint8_t  facing_dir;     /* 0=down, 4=up, 8=left, 12=right */
    uint8_t  walk_counter;
    uint8_t  in_battle;      /* 0=overworld, 1=wild, 2=trainer */
    uint8_t  party_count;
    uint8_t  lead_species;
    uint8_t  enemy_species;
    uint8_t  enemy_type1;
    uint8_t  enemy_type2;
    uint8_t  enemy_level;
    bool     enemy_shiny;
    uint8_t  battle_moves[4];
    uint8_t  menu_cursor;
} game_snapshot_t;

typedef struct {
    bool            running;
    bool            rom_loaded;
    bool            mods_menu_open;
    game_version_t  version;
    char            rom_title[17];
    uint32_t        frame_count;
    game_snapshot_t snap;
    /* Camera pitch rung for Dramatic Shape Voxel mod: 0=OFF, 15, 35, 50, 75 */
    int             voxel_pitch_deg;
} app_state_t;

#endif /* APP_STATE_H */
