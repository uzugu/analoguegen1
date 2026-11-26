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
static bool          s_mesh_ready   = false;
static uint8_t       s_last_map     = 0xFFu;
static uint8_t       s_last_tileset = 0xFFu;
static uint8_t       s_last_px      = 0xFFu;
static uint8_t       s_last_py      = 0xFFu;
static uint8_t       s_last_daytime = 0xFFu;
static int           s_vram_refresh = 0;

static const char *dir_name(uint8_t facing_dir) {
    switch (facing_dir) {
        case 0:  return "S";
        case 4:  return "N";
        case 8:  return "W";
        case 12: return "E";
        default: return "?";
    }
}

static const char *daytime_name(uint8_t daytime_mode) {
    switch (daytime_mode) {
        case 1u:  return "DUSK";
        case 2u:  return "NIGHT";
        case 3u:  return "DAWN";
        case 0u:
        default:  return "DAY";
    }
}

static uint16_t sky_color_for_daytime(uint8_t daytime_mode) {
    switch (daytime_mode) {
        case 1u:  return RGB565(224, 128, 88);   /* DUSK */
        case 2u:  return RGB565(18, 28, 64);     /* NIGHT */
        case 3u:  return RGB565(236, 168, 140);  /* DAWN */
        case 0u:
        default:  return RGB565(96, 156, 232);   /* DAY */
    }
}

static void type_palette_colors(uint8_t type_id, uint16_t *out_primary, uint16_t *out_accent) {
    switch (type_id) {
        case GB_TYPE_NORMAL:
            *out_primary = RGB565(204, 184, 148);
            *out_accent  = RGB565(240, 228, 200);
            break;
        case GB_TYPE_FIGHTING:
            *out_primary = RGB565(192, 82, 64);
            *out_accent  = RGB565(236, 178, 128);
            break;
        case GB_TYPE_FLYING:
            *out_primary = RGB565(156, 176, 232);
            *out_accent  = RGB565(236, 240, 250);
            break;
        case GB_TYPE_POISON:
            *out_primary = RGB565(162, 86, 196);
            *out_accent  = RGB565(224, 168, 240);
            break;
        case GB_TYPE_GROUND:
            *out_primary = RGB565(206, 168, 98);
            *out_accent  = RGB565(240, 218, 160);
            break;
        case GB_TYPE_ROCK:
            *out_primary = RGB565(168, 152, 118);
            *out_accent  = RGB565(218, 208, 182);
            break;
        case GB_TYPE_BUG:
            *out_primary = RGB565(142, 194, 68);
            *out_accent  = RGB565(224, 238, 132);
            break;
        case GB_TYPE_GHOST:
            *out_primary = RGB565(112, 76, 158);
            *out_accent  = RGB565(220, 86, 124);
            break;
        case GB_TYPE_FIRE:
            *out_primary = RGB565(236, 108, 48);
            *out_accent  = RGB565(250, 212, 84);
            break;
        case GB_TYPE_WATER:
            *out_primary = RGB565(68, 148, 232);
            *out_accent  = RGB565(196, 232, 252);
            break;
        case GB_TYPE_GRASS:
            *out_primary = RGB565(78, 194, 92);
            *out_accent  = RGB565(228, 104, 128);
            break;
        case GB_TYPE_ELECTRIC:
            *out_primary = RGB565(248, 208, 48);
            *out_accent  = RGB565(224, 64, 48);
            break;
        case GB_TYPE_PSYCHIC:
            *out_primary = RGB565(240, 112, 168);
            *out_accent  = RGB565(248, 212, 236);
            break;
        case GB_TYPE_ICE:
            *out_primary = RGB565(116, 208, 244);
            *out_accent  = RGB565(232, 248, 255);
            break;
        case GB_TYPE_DRAGON:
            *out_primary = RGB565(108, 124, 224);
            *out_accent  = RGB565(240, 178, 84);
            break;
        default:
            *out_primary = RGB565(168, 176, 192);
            *out_accent  = RGB565(228, 232, 240);
            break;
    }
}

static void battle_mon_colors(uint8_t type1, uint8_t type2,
                              uint16_t *out_body, uint16_t *out_accent) {
    type_palette_colors(type1, out_body, out_accent);
    if (type2 != type1) {
        uint16_t t2_primary = 0u;
        uint16_t t2_accent  = 0u;
        type_palette_colors(type2, &t2_primary, &t2_accent);
        (void)t2_accent;
        *out_accent = t2_primary;
    }
}

/*
 * Decode Red's live 16x16 sprite directly from Pokemon Red's wShadowOAM[0..3]
 * (0xC300..0xC30F): each 4-byte OAM entry holds the exact VRAM tile index and
 * X/Y flip attributes computed by the game itself for standing, walking, biking,
 * and surfing animations.
 */
static bool decode_shadow_oam_player_16x16(uint16_t out_16x16[256],
                                           uint16_t tint_shade1, uint16_t tint_shade2) {
    static const uint8_t quad_ox[4] = { 0u, 8u, 0u, 8u };
    static const uint8_t quad_oy[4] = { 0u, 0u, 8u, 8u };
    const uint16_t outline_col = RGB565(24, 28, 36);
    bool has_pixels = false;

    for (int i = 0; i < 256; i++) {
        out_16x16[i] = 0u;
    }

    /* Verify OAM entry 0 is on-screen (Y in 1..159). */
    const uint8_t oam0_y = gb_brain_read8(0xC300u);
    if (oam0_y == 0u || oam0_y >= 160u) {
        return false;
    }

    for (int q = 0; q < 4; q++) {
        const uint16_t oam_addr = (uint16_t)(0xC300u + (uint16_t)(q * 4));
        const uint8_t oy_scr = gb_brain_read8(oam_addr);
        const uint8_t tile   = gb_brain_read8((uint16_t)(oam_addr + 2u));
        const uint8_t flags  = gb_brain_read8((uint16_t)(oam_addr + 3u));
        if (oy_scr == 0u || oy_scr >= 160u) {
            continue;
        }
        const bool flip_x = (flags & 0x20u) != 0u;
        const bool flip_y = (flags & 0x40u) != 0u;
        const uint16_t tile_base = (uint16_t)(0x8000u + (uint16_t)tile * 16u);
        const int ox = (int)quad_ox[q];
        const int oy = (int)quad_oy[q];

        for (int py = 0; py < 8; py++) {
            const int src_py = flip_y ? (7 - py) : py;
            const uint16_t row_addr = (uint16_t)(tile_base + (uint16_t)(src_py * 2));
            const uint8_t lo = gb_brain_read8(row_addr);
            const uint8_t hi = gb_brain_read8((uint16_t)(row_addr + 1u));

            for (int px = 0; px < 8; px++) {
                const int src_px = flip_x ? (7 - px) : px;
                const int bit = 7 - src_px;
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
                out_16x16[(oy + py) * 16 + (ox + px)] = col;
            }
        }
    }

    return has_pixels;
}

static bool decode_vram_sprite_16x16(uint8_t img_idx, bool flip_x, bool flip_bottom_x,
                                     uint16_t out_16x16[256],
                                     uint16_t tint_shade1, uint16_t tint_shade2) {
    if (img_idx == 0xFFu) {
        return false;
    }
    uint8_t base_tile = (uint8_t)(img_idx & 0xFCu);
    if (base_tile > 252u) {
        return false;
    }

    static const uint8_t quad_ox[4] = { 0u, 8u, 0u, 8u };
    static const uint8_t quad_oy[4] = { 0u, 0u, 8u, 8u };
    const uint16_t outline_col = RGB565(24, 28, 36);
    bool has_pixels = false;

    for (int q = 0; q < 4; q++) {
        const uint16_t tile_base = (uint16_t)(0x8000u + (uint16_t)((uint16_t)base_tile + (uint16_t)q) * 16u);
        const int ox = (int)quad_ox[q];
        const int oy = (int)quad_oy[q];
        const bool q_flip = (q >= 2) ? (flip_x ^ flip_bottom_x) : flip_x;

        for (int py = 0; py < 8; py++) {
            const uint16_t row_addr = (uint16_t)(tile_base + (uint16_t)(py * 2));
            const uint8_t lo = gb_brain_read8(row_addr);
            const uint8_t hi = gb_brain_read8((uint16_t)(row_addr + 1u));

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
                int dst_x = ox + px;
                if (q_flip) {
                    dst_x = 15 - dst_x;
                }
                out_16x16[(oy + py) * 16 + dst_x] = col;
            }
        }
    }

    return has_pixels;
}

static void mod_voxel_init(app_state_t *state) {
    (void)state;
    r3d_init();
    s_mesh_ready   = false;
    s_last_map     = 0xFFu;
    s_last_tileset = 0xFFu;
    s_last_px      = 0xFFu;
    s_last_py      = 0xFFu;
    s_last_daytime = 0xFFu;
    s_vram_refresh = 0;
}

static void mod_voxel_on_frame(app_state_t *state) {
    if (!state || !state->rom_loaded || state->voxel_pitch_deg <= 0 ||
        !state->snap.in_overworld_ready) {
        s_mesh_ready = false;
        return;
    }

    if (s_last_map != state->snap.cur_map || s_last_tileset != state->snap.tileset) {
        s_vram_refresh = 24;
    }

    bool periodic_vram = false;
    if (s_vram_refresh > 0) {
        s_vram_refresh--;
        if ((s_vram_refresh % 6) == 0) {
            periodic_vram = true;
        }
    }

    /* Rebuild voxel mesh when player moves, map/tileset/daytime changes, or post-warp VRAM settles. */
    bool dirty = !s_mesh_ready ||
                 periodic_vram ||
                 s_last_map     != state->snap.cur_map ||
                 s_last_tileset != state->snap.tileset ||
                 s_last_px      != state->snap.player_x ||
                 s_last_py      != state->snap.player_y ||
                 s_last_daytime != state->daytime_mode;

    if (dirty) {
        if (gb_map_extract(state, &s_map_view)) {
            voxel_mesher_build(&s_map_view, state, &s_mesh);
            s_mesh_ready   = (s_mesh.tri_count > 0);
            s_last_map     = state->snap.cur_map;
            s_last_tileset = state->snap.tileset;
            s_last_px      = state->snap.player_x;
            s_last_py      = state->snap.player_y;
            s_last_daytime = state->daytime_mode;
        } else {
            s_mesh_ready = false;
        }
    }
}

static void mod_voxel_on_render_3d(const app_state_t *state) {
    if (!state || !state->rom_loaded || !s_mesh_ready ||
        state->voxel_pitch_deg <= 0 ||
        !state->snap.in_overworld_ready ||
        state->snap.in_battle != 0 ||
        state->snap.ui_overlay == (uint8_t)UI_OVERLAY_FULLSCREEN_2D) {
        return;
    }

    r3d_begin_scene(sky_color_for_daytime(state->daytime_mode));

    float px = s_map_view.player_local_tx * 8.0f;
    float pz = s_map_view.player_local_ty * 8.0f;

    /* Smooth sub-tile walk interpolation: wXCoord/wYCoord stay at old tile while wWalkCounter counts 8 -> 1, then advance at 0. */
    if (state->snap.in_battle == 0 && state->snap.walk_counter > 0 && state->snap.walk_counter <= 8) {
        float frac = (float)(8 - state->snap.walk_counter) / 8.0f;
        px += (float)state->snap.walk_dx * frac * 16.0f;
        pz += (float)state->snap.walk_dy * frac * 16.0f;
    }

    r3d_camera_t cam;
    cam.target_x  = px;
    cam.target_z  = pz;
    cam.fov_y_deg = 45.0f;
    if (state->snap.in_battle != 0) {
        cam.target_y  = 6.0f;
        cam.pitch_deg = 62.0f;
        cam.yaw_deg   = 22.0f;
        cam.distance  = 76.0f;
    } else {
        cam.target_y  = 4.0f;
        cam.pitch_deg = (float)state->voxel_pitch_deg;
        cam.yaw_deg   = 0.0f;
        cam.distance  = 115.0f;
    }
    r3d_set_camera(&cam);

    r3d_draw_mesh(&s_mesh, s_map_view.atlas_rgb565, GB_ATLAS_W, GB_ATLAS_H);

    if (state->snap.in_battle != 0) {
        const uint8_t active_species = gb_brain_read8(GB_ADDR_BATTLE_MON_SPECIES);
        const uint8_t p_type1 = (active_species != 0u)
                                ? gb_brain_read8(GB_ADDR_BATTLE_MON_TYPE1)
                                : GB_TYPE_FIRE;
        const uint8_t p_type2 = (active_species != 0u)
                                ? gb_brain_read8(GB_ADDR_BATTLE_MON_TYPE2)
                                : p_type1;

        uint16_t p_body = 0u;
        uint16_t p_acc  = 0u;
        battle_mon_colors(p_type1, p_type2, &p_body, &p_acc);
        r3d_draw_billboard(px - 12.0f, 0.0f, pz + 10.0f, 12.0f, 14.0f,
                           p_body, p_acc, true);

        uint16_t e_body = 0u;
        uint16_t e_acc  = 0u;
        battle_mon_colors(state->snap.enemy_type1, state->snap.enemy_type2, &e_body, &e_acc);
        if (state->snap.enemy_shiny) {
            const bool sparkle_phase = ((state->frame_count >> 3) & 1u) != 0u;
            e_body = RGB565(255, 212, 48);
            e_acc  = sparkle_phase ? RGB565(255, 252, 200) : RGB565(255, 172, 32);
        }
        r3d_draw_billboard(px + 14.0f, 0.0f, pz - 12.0f, 12.0f, 14.0f,
                           e_body, e_acc, true);
        return;
    }

    /* Player billboard: decode directly from live wShadowOAM[0..3] for 100% accurate walk animation */
    uint16_t spr_tex[256];
    if (decode_shadow_oam_player_16x16(spr_tex, RGB565(248, 224, 192), RGB565(224, 48, 48))) {
        r3d_draw_sprite_billboard(px, 0.0f, pz, 14.0f, 16.0f, spr_tex, 16, 16, false);
    } else {
        const uint8_t red_img  = gb_brain_read8((uint16_t)(GB_ADDR_SPRITE_DATA1 + GB_SPRITE1_IMAGE_INDEX));
        const uint8_t red_anim = gb_brain_read8((uint16_t)(GB_ADDR_SPRITE_DATA1 + 0x08u));
        const bool red_flip    = (state->snap.facing_dir == 12u);
        const bool red_legflip = ((state->snap.facing_dir == 0u || state->snap.facing_dir == 4u) &&
                                  red_anim == 3u);
        if (red_img != 0xFFu &&
            decode_vram_sprite_16x16(red_img, red_flip, red_legflip, spr_tex, RGB565(248, 224, 192), RGB565(224, 48, 48))) {
            r3d_draw_sprite_billboard(px, 0.0f, pz, 14.0f, 16.0f, spr_tex, 16, 16, false);
        } else {
            r3d_draw_billboard(px, 0.0f, pz, 10.0f, 14.0f,
                               RGB565(220, 48, 48), RGB565(245, 220, 185), false);
        }
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
        const uint8_t npc_img  = gb_brain_read8((uint16_t)(GB_ADDR_SPRITE_DATA1 + off + GB_SPRITE1_IMAGE_INDEX));
        const uint8_t npc_anim = gb_brain_read8((uint16_t)(GB_ADDR_SPRITE_DATA1 + off + 0x08u));
        if (npc_img == 0xFFu) {
            continue; /* hidden / off-screen sprite */
        }
        const bool npc_flip    = (npc->facing_dir == 12u);
        const bool npc_legflip = ((npc->facing_dir == 0u || npc->facing_dir == 4u) &&
                                  npc_anim == 3u);
        if (decode_vram_sprite_16x16(npc_img, npc_flip, npc_legflip, spr_tex, RGB565(244, 218, 184), RGB565(72, 148, 224))) {
            r3d_draw_sprite_billboard(nx, 0.0f, nz, 14.0f, 16.0f, spr_tex, 16, 16, false);
        } else {
            r3d_draw_billboard(nx, 0.0f, nz, 10.0f, 13.0f,
                               RGB565(72, 148, 224), RGB565(240, 210, 175), false);
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
                               RGB565(248, 208, 48), RGB565(220, 72, 48), false);
        }
    }
}

static void mod_voxel_on_render_hud(const app_state_t *state) {
    if (!state || !state->show_debug_hud) {
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
    } else if (s_mesh_ready && state->voxel_pitch_deg > 0 && state->snap.in_battle != 0) {
        snprintf(line, sizeof(line), "3D-BTL FPS:%2u GB:%u 3D:%u T:%d %s",
                 (unsigned)state->perf_fps,
                 (unsigned)state->perf_gb_ms,
                 (unsigned)state->perf_3d_ms,
                 s_mesh.tri_count,
                 daytime_name(state->daytime_mode));
    } else if (s_mesh_ready && state->voxel_pitch_deg > 0) {
        snprintf(line, sizeof(line), "3D FPS:%2u GB:%u 3D:%u P:%d T:%d %s %s",
                 (unsigned)state->perf_fps,
                 (unsigned)state->perf_gb_ms,
                 (unsigned)state->perf_3d_ms,
                 state->voxel_pitch_deg,
                 s_mesh.tri_count,
                 dir_name(state->snap.facing_dir),
                 daytime_name(state->daytime_mode));
    } else {
        snprintf(line, sizeof(line), "2D FPS:%2u GB:%u MAP:%02X XY:%u,%u P:%d",
                 (unsigned)state->perf_fps,
                 (unsigned)state->perf_gb_ms,
                 (unsigned)state->snap.cur_map,
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
        .enabled         = false,
        .default_enabled = false,
        .init            = mod_voxel_init,
        .on_frame        = mod_voxel_on_frame,
        .on_render_3d    = mod_voxel_on_render_3d,
        .on_render_hud   = mod_voxel_on_render_hud
    };
    mod_manager_register(&desc);
}
