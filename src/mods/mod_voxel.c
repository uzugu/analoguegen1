#include "mod_manager.h"
#include "gb_brain.h"
#include "gb_map.h"
#include "pokered_ram.h"
#include "voxel_mesher.h"
#include "renderer.h"
#include "renderer3d.h"

#include <stdio.h>

static gb_map_view_t s_map_view;
static voxel_mesh_t  s_mesh;
static bool          s_mesh_ready = false;
static uint8_t       s_last_map   = 0xFFu;
static uint8_t       s_last_px    = 0xFFu;
static uint8_t       s_last_py    = 0xFFu;

static const char *dir_name(uint8_t facing_dir) {
    switch (facing_dir) {
        case 0:  return "S";
        case 4:  return "N";
        case 8:  return "W";
        case 12: return "E";
        default: return "?";
    }
}

static bool decode_vram_sprite_16x16(uint8_t base_tile, uint16_t out_16x16[256],
                                     uint16_t tint_shade1, uint16_t tint_shade2) {
    if (base_tile > 124u) {
        return false;
    }

    static const uint8_t quad_ox[4] = { 0u, 8u, 0u, 8u };
    static const uint8_t quad_oy[4] = { 0u, 0u, 8u, 8u };
    const uint16_t outline_col = RGB565(24, 24, 24);
    bool has_pixels = false;

    for (int q = 0; q < 4; q++) {
        const uint16_t tile_base = (uint16_t)(0x8000u + (uint16_t)((uint16_t)base_tile + (uint16_t)q) * 16u);
        const int ox = (int)quad_ox[q];
        const int oy = (int)quad_oy[q];

        for (int py = 0; py < 8; py++) {
            const uint16_t row_addr = (uint16_t)(tile_base + (uint16_t)(py * 2));
            const uint8_t lo = gb_brain_read8(row_addr);
            const uint8_t hi = gb_brain_read8((uint16_t)(row_addr + 1u));
            uint16_t *dst_row = &out_16x16[(oy + py) * 16 + ox];

            for (int px = 0; px < 8; px++) {
                const int bit = 7 - px;
                const uint8_t shade = (uint8_t)(((lo >> bit) & 1u) | (((hi >> bit) & 1u) << 1));
                uint16_t col = 0u;
                if (shade == 1u) {
                    col = tint_shade1;
                    has_pixels = true;
                } else if (shade == 2u) {
                    col = tint_shade2;
                    has_pixels = true;
                } else if (shade == 3u) {
                    col = outline_col;
                    has_pixels = true;
                }
                dst_row[px] = col;
            }
        }
    }

    return has_pixels;
}

static void mod_voxel_init(app_state_t *state) {
    r3d_init();
    s_mesh_ready = false;
    s_last_map = 0xFFu;
    s_last_px = 0xFFu;
    s_last_py = 0xFFu;
    if (state && state->voxel_pitch_deg == 0) {
        state->voxel_pitch_deg = 35;
    }
}

static void mod_voxel_on_frame(app_state_t *state) {
    if (!state || !state->rom_loaded || state->voxel_pitch_deg <= 0 || state->snap.in_battle != 0) {
        s_mesh_ready = false;
        return;
    }

    /* Rebuild voxel mesh whenever the player moves, changes map, or every 30 frames (animated tiles). */
    bool dirty = !s_mesh_ready ||
                 s_last_map != state->snap.cur_map ||
                 s_last_px  != state->snap.player_x ||
                 s_last_py  != state->snap.player_y ||
                 (state->frame_count % 30u) == 0u;

    if (dirty) {
        if (gb_map_extract(state, &s_map_view)) {
            voxel_mesher_build(&s_map_view, state, &s_mesh);
            s_mesh_ready = (s_mesh.tri_count > 0);
            s_last_map = state->snap.cur_map;
            s_last_px  = state->snap.player_x;
            s_last_py  = state->snap.player_y;
        } else {
            s_mesh_ready = false;
        }
    }
}

static void mod_voxel_on_render_3d(const app_state_t *state) {
    if (!state || !state->rom_loaded || !s_mesh_ready ||
        state->voxel_pitch_deg <= 0 || state->snap.in_battle != 0) {
        return;
    }

    r3d_begin_scene(RGB565(96, 152, 224));

    float px = s_map_view.player_local_tx * 8.0f;
    float pz = s_map_view.player_local_ty * 8.0f;

    /* Smooth sub-tile walk interpolation: wXCoord/wYCoord advance at step start while wWalkCounter counts 8 -> 0. */
    if (state->snap.walk_counter > 0 && state->snap.walk_counter <= 8) {
        float frac = (float)state->snap.walk_counter / 8.0f;
        px -= (float)state->snap.walk_dx * frac * 16.0f;
        pz -= (float)state->snap.walk_dy * frac * 16.0f;
    }

    r3d_camera_t cam;
    cam.target_x  = px;
    cam.target_y  = 4.0f;
    cam.target_z  = pz;
    cam.pitch_deg = (float)state->voxel_pitch_deg;
    cam.yaw_deg   = 0.0f;
    cam.distance  = 115.0f;
    cam.fov_y_deg = 45.0f;
    r3d_set_camera(&cam);

    r3d_draw_mesh(&s_mesh, s_map_view.atlas_rgb565, GB_ATLAS_W, GB_ATLAS_H);

    /* Player billboard (Red: live 16x16 VRAM sprite with stylized fallback) */
    uint16_t spr_tex[256];
    const uint8_t red_img = gb_brain_read8((uint16_t)(GB_ADDR_SPRITE_DATA1 + GB_SPRITE1_IMAGE_INDEX));
    if (red_img != 0xFFu &&
        decode_vram_sprite_16x16(red_img, spr_tex, RGB565(245, 220, 185), RGB565(220, 48, 48))) {
        r3d_draw_sprite_billboard(px, 0.0f, pz, 14.0f, 16.0f, spr_tex, 16, 16, true);
    } else {
        r3d_draw_billboard(px, 0.0f, pz, 10.0f, 14.0f,
                           RGB565(220, 48, 48), RGB565(245, 220, 185), true);
    }

    /* Live NPC billboards from wSpriteStateData1/2 */
    for (int i = 0; i < APP_MAX_NPCS; i++) {
        const npc_snap_t *npc = &state->snap.npcs[i];
        if (!npc->active) {
            continue;
        }
        int dx = (int)npc->map_x - (int)state->snap.player_x;
        int dy = (int)npc->map_y - (int)state->snap.player_y;
        if (dx < -6 || dx > 6 || dy < -5 || dy > 5) {
            continue;
        }
        float nx = s_map_view.player_local_tx * 8.0f + (float)(dx * 16);
        float nz = s_map_view.player_local_ty * 8.0f + (float)(dy * 16);
        const uint16_t off = (uint16_t)((i + 1) * 16);
        const uint8_t npc_img = gb_brain_read8((uint16_t)(GB_ADDR_SPRITE_DATA1 + off + GB_SPRITE1_IMAGE_INDEX));
        if (npc_img < 124u &&
            decode_vram_sprite_16x16(npc_img, spr_tex, RGB565(240, 210, 175), RGB565(72, 148, 224))) {
            r3d_draw_sprite_billboard(nx, 0.0f, nz, 14.0f, 16.0f, spr_tex, 16, 16, true);
        } else {
            r3d_draw_billboard(nx, 0.0f, nz, 10.0f, 13.0f,
                               RGB565(72, 148, 224), RGB565(240, 210, 175), true);
        }
    }

    /* Optional PokéPC Follower billboard one tile behind player */
    if (mod_manager_is_enabled("pokepc_followers") &&
        state->snap.party_count > 0 && state->snap.lead_species != 0) {
        int dx = (int)state->snap.prev_x - (int)state->snap.player_x;
        int dy = (int)state->snap.prev_y - (int)state->snap.player_y;
        if (dx >= -2 && dx <= 2 && dy >= -2 && dy <= 2 && (dx != 0 || dy != 0)) {
            float fx = px + (float)(dx * 16);
            float fz = pz + (float)(dy * 16);
            r3d_draw_billboard(fx, 0.0f, fz, 9.0f, 11.0f,
                               RGB565(248, 208, 48), RGB565(220, 72, 48), true);
        }
    }
}

static void mod_voxel_on_render_hud(const app_state_t *state) {
    if (!state) {
        return;
    }

    const int x = 4;
    const int y = 4;
    const int w = RENDER_W - 8;
    const int h = 24;

    const uint16_t bg     = RGB565(14, 20, 34);
    const uint16_t border = RGB565(65, 110, 175);
    const uint16_t accent = RGB565(120, 215, 255);
    const uint16_t fg     = RGB565(225, 232, 245);

    renderer_fill_rect(x, y, w, h, bg);
    renderer_draw_rect(x, y, w, h, border);

    char line[56];
    if (!state->rom_loaded) {
        snprintf(line, sizeof(line), "VOXEL 3D | PITCH:%2d | WAITING FOR ROM",
                 state->voxel_pitch_deg);
    } else if (s_mesh_ready && state->voxel_pitch_deg > 0 && state->snap.in_battle == 0) {
        snprintf(line, sizeof(line), "3D MAP:%02X XY:%u,%u %s P:%d T:%d",
                 (unsigned)state->snap.cur_map,
                 (unsigned)state->snap.player_x,
                 (unsigned)state->snap.player_y,
                 dir_name(state->snap.facing_dir),
                 state->voxel_pitch_deg,
                 s_mesh.tri_count);
    } else {
        snprintf(line, sizeof(line), "2D MAP:%02X TS:%02X XY:%u,%u PITCH:%d",
                 (unsigned)state->snap.cur_map,
                 (unsigned)state->snap.tileset,
                 (unsigned)state->snap.player_x,
                 (unsigned)state->snap.player_y,
                 state->voxel_pitch_deg);
    }
    renderer_draw_text(x + 6, y + 8, line, s_mesh_ready ? accent : fg, bg, false);
}

void mod_voxel_register(void) {
    mod_descriptor_t desc = {
        .id              = "dramatic_shape",
        .name            = "Dramatic Shape Voxel",
        .category        = "GRAPHICS",
        .description     = "3D diorama overworld with extruded voxels & camera pitch",
        .enabled         = true,
        .default_enabled = true,
        .init            = mod_voxel_init,
        .on_frame        = mod_voxel_on_frame,
        .on_render_3d    = mod_voxel_on_render_3d,
        .on_render_hud   = mod_voxel_on_render_hud
    };
    mod_manager_register(&desc);
}
