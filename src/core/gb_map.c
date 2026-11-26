#include "gb_map.h"
#include "gb_brain.h"
#include "pokered_ram.h"
#include "renderer.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

static int floor_div4(int v)
{
    return (v >= 0) ? (v / 4) : -((-v + 3) / 4);
}

static int mod4(int v)
{
    int r = v % 4;
    return (r < 0) ? (r + 4) : r;
}

static bool is_even(int v)
{
    return (v % 2) == 0;
}

static uint16_t read_le16(uint16_t addr)
{
    uint16_t lo = (uint16_t)gb_brain_read8(addr);
    uint16_t hi = (uint16_t)gb_brain_read8((uint16_t)(addr + 1U));
    return (uint16_t)(lo | (hi << 8));
}

static bool is_flat_walkable(uint8_t cls)
{
    return cls == (uint8_t)TILE_CLASS_GROUND || cls == (uint8_t)TILE_CLASS_GRASS;
}

static uint8_t classify_tile(uint8_t tid, uint8_t tileset, uint8_t grass_tile,
                             const bool walkable[256])
{
    if (tid == 0x7FU || (tileset != 0U && tid == 0x00U)) {
        /* Blank space / indoor black room border void stays flat so it never blocks the camera. */
        return (uint8_t)TILE_CLASS_GROUND;
    }
    if (tid == 0x14U) {
        return (uint8_t)TILE_CLASS_WATER;
    }
    if ((grass_tile != 0xFFU && tid == grass_tile) ||
        (tileset == 0U && (tid == 0x03U || tid == 0x52U || tid == 0x2CU))) {
        return (uint8_t)TILE_CLASS_GRASS;
    }
    if (walkable[tid]) {
        return (uint8_t)TILE_CLASS_GROUND;
    }
    if (tileset == 0U &&
        (tid == 0x27U || tid == 0x36U || tid == 0x37U || tid == 0x38U)) {
        return (uint8_t)TILE_CLASS_LEDGE;
    }
    if (tileset == 0U &&
        (tid == 0x01U || tid == 0x02U || tid == 0x32U ||
         tid == 0x33U || tid == 0x34U || tid == 0x35U)) {
        return (uint8_t)TILE_CLASS_FENCE;
    }
    if (tileset == 0U &&
        ((tid >= 0x05U && tid <= 0x0FU) || (tid >= 0x15U && tid <= 0x1FU))) {
        return (uint8_t)TILE_CLASS_TALL;
    }
    return (uint8_t)TILE_CLASS_SOLID;
}

static void decode_vram_atlas(uint16_t *atlas_rgb565)
{
    /* Warm GBC/DMG 4-shade RGB565 palette matching gb_brain.c (0=lightest..3=darkest). */
    static const uint16_t palette[4] = {
        RGB565(248, 244, 224),
        RGB565(184, 176, 144),
        RGB565(104, 100,  80),
        RGB565( 24,  24,  20)
    };

    for (int t = 0; t < 128; t++) {
        int tile_col = t % 16;
        int tile_row = t / 16;
        int ax0 = tile_col * 8;
        int ay0 = tile_row * 8;
        uint16_t tile_base = (uint16_t)(GB_ADDR_VRAM_TILES_BASE + (uint16_t)(t * 16));

        for (int py = 0; py < 8; py++) {
            uint16_t row_addr = (uint16_t)(tile_base + (uint16_t)(py * 2));
            uint8_t lo = gb_brain_read8(row_addr);
            uint8_t hi = gb_brain_read8((uint16_t)(row_addr + 1U));
            uint16_t *dst_row = &atlas_rgb565[(ay0 + py) * GB_ATLAS_W + ax0];

            for (int px = 0; px < 8; px++) {
                int bit = 7 - px;
                uint8_t shade = (uint8_t)(((lo >> bit) & 1U) | (((hi >> bit) & 1U) << 1));
                dst_row[px] = palette[shade];
            }
        }
    }
}

bool gb_map_extract(const app_state_t *state, gb_map_view_t *out)
{
    if (out == NULL) {
        return false;
    }
    out->valid = false;

    if (state == NULL || !state->rom_loaded ||
        state->snap.map_w == 0U || state->snap.map_h == 0U ||
        state->snap.map_w > 64U || state->snap.map_h > 64U) {
        return false;
    }

    uint8_t tileset_bank = gb_brain_read8(GB_ADDR_TILESET_BANK);
    uint16_t blocks_ptr = read_le16(GB_ADDR_TILESET_BLOCKS_PTR);
    uint16_t coll_ptr = read_le16(GB_ADDR_TILESET_COLL_PTR);
    uint8_t grass_tile = gb_brain_read8(GB_ADDR_GRASS_TILE);

    bool walkable[256];
    memset(walkable, 0, sizeof(walkable));
    for (int i = 0; i < 64; i++) {
        uint8_t t = gb_brain_read_rom_bank(tileset_bank, (uint16_t)(coll_ptr + (uint16_t)i));
        if (t == 0xFFU) {
            break;
        }
        walkable[t] = true;
    }
    if (grass_tile != 0xFFU) {
        walkable[grass_tile] = true;
    }

    const int map_w = (int)state->snap.map_w;
    const int map_h = (int)state->snap.map_h;
    const int stride = map_w + 6;
    const uint8_t tileset = state->snap.tileset;

    const int player_tx = (int)state->snap.player_x * 2;
    const int player_ty = (int)state->snap.player_y * 2;
    const int origin_tx = player_tx - (GB_MAP_VIEW_W / 2);
    const int origin_ty = player_ty - (GB_MAP_VIEW_H / 2);

    out->cur_map = state->snap.cur_map;
    out->tileset = tileset;
    out->origin_tx = origin_tx;
    out->origin_ty = origin_ty;

    for (int vy = 0; vy < GB_MAP_VIEW_H; vy++) {
        int wty = origin_ty + vy;
        int by = floor_div4(wty) + 3;
        if (by < 0) {
            by = 0;
        } else if (by > map_h + 5) {
            by = map_h + 5;
        }
        int sub_y = mod4(wty);

        for (int vx = 0; vx < GB_MAP_VIEW_W; vx++) {
            int wtx = origin_tx + vx;
            int bx = floor_div4(wtx) + 3;
            if (bx < 0) {
                bx = 0;
            } else if (bx > map_w + 5) {
                bx = map_w + 5;
            }
            int sub_x = mod4(wtx);

            uint16_t map_addr = (uint16_t)(GB_ADDR_OVERWORLD_MAP + by * stride + bx);
            uint8_t blk = gb_brain_read8(map_addr);

            uint16_t tile_addr = (uint16_t)(blocks_ptr + (uint16_t)blk * 16U +
                                            (uint16_t)(sub_y * 4 + sub_x));
            uint8_t tid = gb_brain_read_rom_bank(tileset_bank, tile_addr);

            out->tile_id[vy][vx] = tid;
            out->tile_class[vy][vx] = classify_tile(tid, tileset, grass_tile, walkable);
        }
    }

    /*
     * Cell-level pass (Dramatic Shape rule 3): for every 2x2 walk cell, if its
     * bottom-left 8x8 tile is flat walkable (TILE_CLASS_GROUND or TILE_CLASS_GRASS),
     * flatten non-flat decorations in the 2x2 cell while preserving grass tiles.
     */
    if (!is_even(origin_ty)) {
        for (int vx = 0; vx + 1 < GB_MAP_VIEW_W; vx++) {
            uint8_t bl = out->tile_class[0][vx];
            if (is_even(origin_tx + vx) && is_flat_walkable(bl)) {
                if (!is_flat_walkable(out->tile_class[0][vx + 1])) {
                    out->tile_class[0][vx + 1] = bl;
                }
            }
        }
    }
    for (int vy = 0; vy + 1 < GB_MAP_VIEW_H; vy++) {
        if (!is_even(origin_ty + vy)) {
            continue;
        }
        for (int vx = 0; vx + 1 < GB_MAP_VIEW_W; vx++) {
            if (!is_even(origin_tx + vx)) {
                continue;
            }
            uint8_t bl = out->tile_class[vy + 1][vx];
            if (is_flat_walkable(bl)) {
                if (!is_flat_walkable(out->tile_class[vy][vx])) {
                    out->tile_class[vy][vx] = bl;
                }
                if (!is_flat_walkable(out->tile_class[vy][vx + 1])) {
                    out->tile_class[vy][vx + 1] = bl;
                }
                if (!is_flat_walkable(out->tile_class[vy + 1][vx + 1])) {
                    out->tile_class[vy + 1][vx + 1] = bl;
                }
            }
        }
    }

    decode_vram_atlas(out->atlas_rgb565);

    out->player_local_tx = (float)(player_tx - origin_tx) + 1.0f;
    out->player_local_ty = (float)(player_ty - origin_ty) + 1.0f;
    out->valid = true;

    return true;
}
