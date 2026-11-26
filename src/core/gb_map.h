#ifndef GB_MAP_H
#define GB_MAP_H

#include <stdbool.h>
#include <stdint.h>
#include "app_state.h"

/* Visible 8x8 tile window extracted around the player (14x12 tiles = 112x96 world px). */
#define GB_MAP_VIEW_W 14
#define GB_MAP_VIEW_H 12

/* Decoded tileset atlas: 16x8 grid of 8x8 tiles (128 tiles = 0x00..0x7F). */
#define GB_ATLAS_W    128
#define GB_ATLAS_H    64

typedef enum {
    TILE_CLASS_GROUND = 0,  /* flat walkable path/ground (y = 0) */
    TILE_CLASS_GRASS,       /* tall grass / lawn (y = 0, lush green tint) */
    TILE_CLASS_WATER,       /* recessed animated water (y = -2, blue tint) */
    TILE_CLASS_LEDGE,       /* low step/ledge (h = 6, earthy tan) */
    TILE_CLASS_FENCE,       /* fences, signs, bushes (h = 10) */
    TILE_CLASS_SOLID,       /* trees, walls, obstacles (h = 16) */
    TILE_CLASS_TALL         /* buildings, roofs, tall walls (h = 26) */
} tile_class_t;

typedef struct {
    bool     valid;
    uint8_t  cur_map;
    uint8_t  tileset;
    /* World-tile origin (in 8x8 tiles) of tiles[0][0] */
    int      origin_tx;
    int      origin_ty;
    /* Player center in local view tile coordinates */
    float    player_local_tx;
    float    player_local_ty;
    uint8_t  tile_id[GB_MAP_VIEW_H][GB_MAP_VIEW_W];
    uint8_t  tile_class[GB_MAP_VIEW_H][GB_MAP_VIEW_W]; /* tile_class_t */
    /* Decoded 128x64 RGB565 tileset texture from GB VRAM 0x9000..0x97FF */
    uint16_t atlas_rgb565[GB_ATLAS_W * GB_ATLAS_H];
} gb_map_view_t;

/*
 * Extract the visible 20x18 tile window + tile classes + decoded VRAM tileset
 * atlas from live Peanut-GB WRAM/VRAM/ROM. Returns true when in a valid map.
 */
bool gb_map_extract(const app_state_t *state, gb_map_view_t *out);

#endif /* GB_MAP_H */
