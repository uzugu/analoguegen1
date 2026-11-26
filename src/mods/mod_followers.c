#include "mod_manager.h"
#include "renderer.h"

#include <stdio.h>

static uint8_t s_lead_species = 0;
static uint8_t s_follow_x = 0;
static uint8_t s_follow_y = 0;
static bool    s_trailing = false;

static void followers_init(app_state_t *state) {
    s_lead_species = 0;
    s_follow_x = 0;
    s_follow_y = 0;
    s_trailing = false;

    if (state != NULL) {
        s_lead_species = state->snap.lead_species;
        s_follow_x = state->snap.prev_x;
        s_follow_y = state->snap.prev_y;
    }
}

static void followers_on_frame(app_state_t *state) {
    if (state == NULL || !state->rom_loaded) {
        s_trailing = false;
        return;
    }

    s_lead_species = state->snap.lead_species;
    s_follow_x = state->snap.prev_x;
    s_follow_y = state->snap.prev_y;
    s_trailing = (state->snap.party_count > 0 &&
                  state->snap.in_battle == 0 &&
                  (s_follow_x != state->snap.player_x ||
                   s_follow_y != state->snap.player_y));
}

static void followers_on_render_3d(const app_state_t *state) {
    (void)state;
    /* Phase 2: draw follower 3D billboard sprite at (s_follow_x, s_follow_y) */
}

static void followers_on_render_hud(const app_state_t *state) {
    if (state == NULL) {
        return;
    }

    /* Left margin tiny indicator (x=2..76, y=48..106) */
    const int x = 2;
    const int y = 48;
    const int w = 75;
    const int h = 58;

    const uint16_t bg     = RGB565(18, 26, 38);
    const uint16_t border = RGB565(90, 175, 145);
    const uint16_t title  = RGB565(135, 235, 185);
    const uint16_t fg     = RGB565(230, 240, 250);
    const uint16_t dim    = RGB565(145, 165, 185);

    renderer_fill_rect(x, y, w, h, bg);
    renderer_draw_rect(x, y, w, h, border);
    renderer_draw_text(x + 4, y + 5, "FOLLOW", title, bg, false);

    char line2[24];
    char line3[24];
    char line4[24];

    if (!state->rom_loaded || state->snap.party_count == 0) {
        snprintf(line2, sizeof(line2), "SP:--");
        snprintf(line3, sizeof(line3), "AT:--,--");
        snprintf(line4, sizeof(line4), "%s", state->rom_loaded ? "NO PTY" : "NO ROM");
        renderer_draw_text(x + 4, y + 19, line2, dim, bg, false);
        renderer_draw_text(x + 4, y + 31, line3, dim, bg, false);
        renderer_draw_text(x + 4, y + 43, line4, dim, bg, false);
        return;
    }

    snprintf(line2, sizeof(line2), "SP:0x%02X", (unsigned)s_lead_species);
    snprintf(line3, sizeof(line3), "%02u,%02u", (unsigned)s_follow_x, (unsigned)s_follow_y);
    snprintf(line4, sizeof(line4), "%s",
             state->snap.in_battle ? "IN BTL" : (s_trailing ? "TRAIL" : "IDLE"));

    renderer_draw_text(x + 4, y + 19, line2, fg, bg, false);
    renderer_draw_text(x + 4, y + 31, line3, fg, bg, false);
    renderer_draw_text(x + 4, y + 43, line4, s_trailing ? title : dim, bg, false);
}

void mod_followers_register(void) {
    static const mod_descriptor_t desc = {
        .id              = "pokepc_followers",
        .name            = "PokePC Followers",
        .category        = "GRAPHICS",
        .description     = "Lead Pokemon trails behind player",
        .enabled         = true,
        .default_enabled = true,
        .init            = followers_init,
        .on_frame        = followers_on_frame,
        .on_render_3d    = followers_on_render_3d,
        .on_render_hud   = followers_on_render_hud
    };
    mod_manager_register(&desc);
}
