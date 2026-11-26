#include "mod_manager.h"
#include "renderer.h"

#include <stdio.h>

static const char *facing_to_str(uint8_t facing_dir) {
    switch (facing_dir) {
    case 0:  return "DN";
    case 4:  return "UP";
    case 8:  return "LT";
    case 12: return "RT";
    default: return "--";
    }
}

static void voxel_init(app_state_t *state) {
    if (state != NULL && state->voxel_pitch_deg == 0) {
        state->voxel_pitch_deg = 35;
    }
}

static void voxel_on_frame(app_state_t *state) {
    (void)state;
    /* Phase 2: build visible voxel chunk mesh from WRAM map + tileset data */
}

static void voxel_on_render_3d(const app_state_t *state) {
    (void)state;
    /* Phase 2: submit 3D Dramatic-Shape geometry via renderer / of_gpu */
}

static void voxel_on_render_hud(const app_state_t *state) {
    if (state == NULL) {
        return;
    }

    /* Top margin badge above the centered 160x144 GB LCD (y=0..44) */
    const int x = 4;
    const int y = 4;
    const int w = RENDER_W - 8;
    const int h = 38;

    const uint16_t bg     = RGB565(16, 24, 42);
    const uint16_t border = RGB565(75, 145, 225);
    const uint16_t fg     = RGB565(230, 240, 255);
    const uint16_t accent = RGB565(120, 215, 255);
    const uint16_t warn   = RGB565(245, 195, 80);

    renderer_fill_rect(x, y, w, h, bg);
    renderer_draw_rect(x, y, w, h, border);

    char line1[64];
    char line2[64];

    if (state->rom_loaded) {
        snprintf(line1, sizeof(line1),
                 "VOXEL  MAP:%02X  TS:%02X  PITCH:%d",
                 state->snap.cur_map,
                 state->snap.tileset,
                 state->voxel_pitch_deg);
        snprintf(line2, sizeof(line2),
                 "XY:%02u,%02u  DIM:%02ux%02u  DIR:%s",
                 (unsigned)state->snap.player_x,
                 (unsigned)state->snap.player_y,
                 (unsigned)state->snap.map_w,
                 (unsigned)state->snap.map_h,
                 facing_to_str(state->snap.facing_dir));
    } else {
        snprintf(line1, sizeof(line1),
                 "VOXEL  MAP:--  TS:--  PITCH:%d",
                 state->voxel_pitch_deg);
        snprintf(line2, sizeof(line2),
                 "XY:--,--  WAITING FOR ROM");
    }

    renderer_draw_text(x + 6, y + 6,  line1, accent, bg, false);
    renderer_draw_text(x + 6, y + 20, line2, state->rom_loaded ? fg : warn, bg, false);
}

void mod_voxel_register(void) {
    static const mod_descriptor_t desc = {
        .id              = "dramatic_shape",
        .name            = "Dramatic Shape Voxel",
        .category        = "GRAPHICS",
        .description     = "3D voxel overworld & camera pitch",
        .enabled         = true,
        .default_enabled = true,
        .init            = voxel_init,
        .on_frame        = voxel_on_frame,
        .on_render_3d    = voxel_on_render_3d,
        .on_render_hud   = voxel_on_render_hud
    };
    mod_manager_register(&desc);
}
