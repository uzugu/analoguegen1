#include "mod_manager.h"
#include "gb_map.h"
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

    /* Player billboard (Red: red cap + blue/white outfit) */
    r3d_draw_billboard(px, 0.0f, pz, 10.0f, 14.0f,
                       RGB565(220, 48, 48), RGB565(245, 220, 185), true);

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

    r3d_end_scene();
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
