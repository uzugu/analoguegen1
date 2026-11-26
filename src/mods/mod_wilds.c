#include "mod_manager.h"
#include "gb_map.h"
#include "renderer.h"
#include "renderer3d.h"

#include <stddef.h>

#define WILD_MAX_SPAWNS   5
#define WILD_RADIUS_CELLS 6
#define WILD_WANDER_FRAMES 45u

typedef struct {
    uint8_t  species;
    uint16_t body_color;
    uint16_t accent_color;
} wild_species_info_t;

typedef struct {
    bool     active;
    uint8_t  species;
    int      x;
    int      y;
    bool     shiny;
    uint16_t body_color;
    uint16_t accent_color;
} wild_spawn_t;

/* Gen 1 internal species IDs from pret/pokered constants/pokemon_constants.asm */
static const wild_species_info_t s_wild_pool[] = {
    { 0x24u, RGB565(198, 154,  96), RGB565(242, 226, 186) }, /* Pidgey */
    { 0xA5u, RGB565(162,  96, 202), RGB565(236, 222, 180) }, /* Rattata */
    { 0x54u, RGB565(248, 208,  48), RGB565(224,  64,  48) }, /* Pikachu */
    { 0x7Bu, RGB565( 88, 198,  84), RGB565(240, 180,  56) }, /* Caterpie */
    { 0xB9u, RGB565( 72, 108, 212), RGB565( 72, 204,  88) }  /* Oddish */
};

static wild_spawn_t s_spawns[WILD_MAX_SPAWNS];
static uint32_t     s_rng          = 0x57494C44u;
static uint8_t      s_last_map     = 0xFFu;
static uint8_t      s_last_px      = 0xFFu;
static uint8_t      s_last_py      = 0xFFu;
static uint32_t     s_wander_timer = 0u;

static uint32_t wild_xorshift32(void) {
    uint32_t x = s_rng;
    if (x == 0u) {
        x = 0x57494C44u;
    }
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    s_rng = x;
    return x;
}

static int iabs_val(int v) {
    return (v < 0) ? -v : v;
}

static bool wild_in_radius(int sx, int sy, int px, int py) {
    int dx = sx - px;
    int dy = sy - py;
    if (iabs_val(dx) > WILD_RADIUS_CELLS || iabs_val(dy) > WILD_RADIUS_CELLS) {
        return false;
    }
    return (dx * dx + dy * dy) <= (WILD_RADIUS_CELLS * WILD_RADIUS_CELLS);
}

static void wild_spawn_slot(int slot, int px, int py, int map_w, int map_h) {
    if (slot < 0 || slot >= WILD_MAX_SPAWNS) {
        return;
    }

    const size_t pool_count = sizeof(s_wild_pool) / sizeof(s_wild_pool[0]);
    const wild_species_info_t *info = &s_wild_pool[wild_xorshift32() % pool_count];

    int sx = px;
    int sy = py;
    int max_x = (map_w > 0) ? (map_w * 2 - 1) : 255;
    int max_y = (map_h > 0) ? (map_h * 2 - 1) : 255;

    for (int attempt = 0; attempt < 8; attempt++) {
        int dx = (int)(wild_xorshift32() % 11u) - 5; /* -5..+5 */
        int dy = (int)(wild_xorshift32() % 11u) - 5;
        if (dx == 0 && dy == 0) {
            dx = 2;
        }
        int cx = px + dx;
        int cy = py + dy;
        if (cx < 0) cx = 0;
        if (cy < 0) cy = 0;
        if (cx > max_x) cx = max_x;
        if (cy > max_y) cy = max_y;
        if ((cx != px || cy != py) && wild_in_radius(cx, cy, px, py)) {
            sx = cx;
            sy = cy;
            break;
        }
    }

    if (sx == px && sy == py) {
        sx = (px + 2 <= max_x) ? (px + 2) : ((px >= 2) ? (px - 2) : px);
    }

    s_spawns[slot].active       = true;
    s_spawns[slot].species      = info->species;
    s_spawns[slot].x            = sx;
    s_spawns[slot].y            = sy;
    s_spawns[slot].shiny        = ((wild_xorshift32() & 0x7Fu) == 0u); /* 1/128 shiny roll */
    s_spawns[slot].body_color   = info->body_color;
    s_spawns[slot].accent_color = info->accent_color;
}

static void wilds_clear_spawns(void) {
    for (int i = 0; i < WILD_MAX_SPAWNS; i++) {
        s_spawns[i].active = false;
        s_spawns[i].shiny  = false;
    }
}

static void wilds_init(app_state_t *state) {
    (void)state;
    wilds_clear_spawns();
    s_rng          = 0x57494C44u;
    s_last_map     = 0xFFu;
    s_last_px      = 0xFFu;
    s_last_py      = 0xFFu;
    s_wander_timer = 0u;
}

static void wilds_on_frame(app_state_t *state) {
    if (state == NULL || !state->rom_loaded) {
        wilds_clear_spawns();
        s_last_map = 0xFFu;
        return;
    }

    if (state->snap.in_battle != 0) {
        return;
    }

    const int px    = (int)state->snap.player_x;
    const int py    = (int)state->snap.player_y;
    const int map_w = (int)state->snap.map_w;
    const int map_h = (int)state->snap.map_h;

    /* Respawn full pack deterministically on map entry. */
    if (state->snap.cur_map != s_last_map) {
        uint32_t seed = 0x57494C44u ^
                        ((uint32_t)state->snap.cur_map << 16) ^
                        ((uint32_t)state->snap.player_x << 8) ^
                        (uint32_t)state->snap.player_y;
        s_rng = (seed != 0u) ? seed : 0x57494C44u;
        s_wander_timer = 0u;

        for (int i = 0; i < WILD_MAX_SPAWNS; i++) {
            wild_spawn_slot(i, px, py, map_w, map_h);
        }

        s_last_map = state->snap.cur_map;
        s_last_px  = state->snap.player_x;
        s_last_py  = state->snap.player_y;
        return;
    }

    /* On player movement, replace any inactive or out-of-range spawns. */
    if (state->snap.player_x != s_last_px || state->snap.player_y != s_last_py) {
        for (int i = 0; i < WILD_MAX_SPAWNS; i++) {
            if (!s_spawns[i].active || !wild_in_radius(s_spawns[i].x, s_spawns[i].y, px, py)) {
                wild_spawn_slot(i, px, py, map_w, map_h);
            }
        }
        s_last_px = state->snap.player_x;
        s_last_py = state->snap.player_y;
    }

    /* Every ~45 frames, each active spawn wanders +/-1 cell within 6-cell radius. */
    s_wander_timer++;
    if (s_wander_timer >= WILD_WANDER_FRAMES) {
        s_wander_timer = 0u;
        int max_x = (map_w > 0) ? (map_w * 2 - 1) : 255;
        int max_y = (map_h > 0) ? (map_h * 2 - 1) : 255;

        for (int i = 0; i < WILD_MAX_SPAWNS; i++) {
            if (!s_spawns[i].active) {
                continue;
            }
            int step_x = (int)(wild_xorshift32() % 3u) - 1;
            int step_y = (int)(wild_xorshift32() % 3u) - 1;
            int nx = s_spawns[i].x + step_x;
            int ny = s_spawns[i].y + step_y;

            if (nx < 0 || ny < 0 || nx > max_x || ny > max_y) {
                continue;
            }
            if (nx == px && ny == py) {
                continue;
            }
            if (wild_in_radius(nx, ny, px, py)) {
                s_spawns[i].x = nx;
                s_spawns[i].y = ny;
            }
        }
    }
}

static void wilds_on_render_3d(const app_state_t *state) {
    if (state == NULL || !state->rom_loaded ||
        state->voxel_pitch_deg <= 0 || state->snap.in_battle != 0) {
        return;
    }

    const bool sparkle_phase = ((state->frame_count >> 3) & 1u) != 0u;

    for (int i = 0; i < WILD_MAX_SPAWNS; i++) {
        if (!s_spawns[i].active) {
            continue;
        }

        int dx = s_spawns[i].x - (int)state->snap.player_x;
        int dy = s_spawns[i].y - (int)state->snap.player_y;
        if (dx < -WILD_RADIUS_CELLS || dx > WILD_RADIUS_CELLS ||
            dy < -WILD_RADIUS_CELLS || dy > WILD_RADIUS_CELLS) {
            continue;
        }

        /* Matches gb_map.c player_local_tx/ty = (GB_MAP_VIEW_W/2 + 1, GB_MAP_VIEW_H/2 + 1) */
        float wx = (float)((GB_MAP_VIEW_W / 2) + 1) * 8.0f + (float)dx * 16.0f;
        float wz = (float)((GB_MAP_VIEW_H / 2) + 1) * 8.0f + (float)dy * 16.0f;

        uint16_t body_col   = s_spawns[i].body_color;
        uint16_t accent_col = s_spawns[i].accent_color;
        if (s_spawns[i].shiny) {
            body_col   = RGB565(255, 212, 48);
            accent_col = sparkle_phase ? RGB565(255, 252, 200) : RGB565(255, 172, 32);
        }

        r3d_draw_billboard(wx, 0.0f, wz, 9.0f, 11.0f, body_col, accent_col, true);
    }
}

static void wilds_on_render_hud(const app_state_t *state) {
    if (state == NULL || !state->rom_loaded) {
        return;
    }

    bool shiny_nearby = false;
    for (int i = 0; i < WILD_MAX_SPAWNS; i++) {
        if (s_spawns[i].active && s_spawns[i].shiny &&
            wild_in_radius(s_spawns[i].x, s_spawns[i].y,
                           (int)state->snap.player_x, (int)state->snap.player_y)) {
            shiny_nearby = true;
            break;
        }
    }

    if (!shiny_nearby) {
        return;
    }

    /* Top-right corner badge (14 chars * 6px = 84px + padding = 92px) */
    const int badge_w = 92;
    const int badge_h = 14;
    const int badge_x = RENDER_W - badge_w - 4;
    const int badge_y = 30;

    const uint16_t bg     = RGB565(42, 30, 6);
    const uint16_t border = RGB565(255, 215, 55);
    const uint16_t fg     = RGB565(255, 240, 110);

    renderer_fill_rect(badge_x, badge_y, badge_w, badge_h, bg);
    renderer_draw_rect(badge_x, badge_y, badge_w, badge_h, border);
    renderer_draw_text(badge_x + 4, badge_y + 3, "*SHINY NEARBY*", fg, bg, false);
}

void mod_wilds_register(void) {
    static const mod_descriptor_t desc = {
        .id              = "wilds_of_kanto",
        .name            = "Wilds of Kanto",
        .category        = "MECHANIC",
        .description     = "Visible wandering wild Pokemon in 3D overworld",
        .enabled         = true,
        .default_enabled = true,
        .init            = wilds_init,
        .on_frame        = wilds_on_frame,
        .on_render_3d    = wilds_on_render_3d,
        .on_render_hud   = wilds_on_render_hud
    };
    mod_manager_register(&desc);
}
