#include "mod_manager.h"
#include "gb_brain.h"
#include "renderer.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#define POPUP_ITEM_COUNT 9

static mod_descriptor_t s_mods[MOD_MAX_COUNT];
static int s_mod_count = 0;
static int s_menu_cursor = 0;

static void set_mod_enabled(const char *id, bool enabled) {
    for (int i = 0; i < s_mod_count; i++) {
        if (strcmp(s_mods[i].id, id) == 0) {
            s_mods[i].enabled = enabled;
            return;
        }
    }
}

static void toggle_mod_by_id(const char *id) {
    for (int i = 0; i < s_mod_count; i++) {
        if (strcmp(s_mods[i].id, id) == 0) {
            s_mods[i].enabled = !s_mods[i].enabled;
            return;
        }
    }
}

static int step_voxel_pitch(int current_deg, int dir) {
    static const int k_rungs[] = { 0, 15, 35, 50, 75 };
    const int n = (int)(sizeof(k_rungs) / sizeof(k_rungs[0]));
    int idx = 0;
    for (int i = 0; i < n; i++) {
        if (k_rungs[i] == current_deg) {
            idx = i;
            break;
        }
    }
    return k_rungs[(idx + n + dir) % n];
}

static const char *gb_filter_name(uint8_t mode) {
    switch (mode) {
        case 1u: return "POCKET";
        case 2u: return "SGB RED";
        case 3u: return "B&W";
        case 4u: return "DMG+GRID";
        case 5u: return "PKT+GRID";
        case 0u:
        default: return "DMG";
    }
}

static const char *daytime_label(uint8_t mode) {
    switch (mode) {
        case 1u: return "DUSK";
        case 2u: return "NIGHT";
        case 3u: return "DAWN";
        case 0u:
        default: return "DAY";
    }
}

void mod_manager_init(app_state_t *state) {
    s_mod_count = 0;
    s_menu_cursor = 0;

    mod_voxel_register();
    mod_moveinfo_register();
    mod_followers_register();
    mod_wilds_register();
    mod_gen3box_register();
    {
        static const mod_descriptor_t audio_mod = {
            .id              = "dmg_audio",
            .name            = "DMG Audio",
            .category        = "AUDIO",
            .description     = "Game Boy APU sound output",
            .enabled         = true,
            .default_enabled = true,
            .init            = NULL,
            .on_frame        = NULL,
            .on_render_3d    = NULL,
            .on_render_hud   = NULL
        };
        mod_manager_register(&audio_mod);
    }

    for (int i = 0; i < s_mod_count; i++) {
        if (s_mods[i].init != NULL) {
            s_mods[i].init(state);
        }
    }
}

bool mod_manager_register(const mod_descriptor_t *desc) {
    if (desc == NULL || desc->id == NULL || s_mod_count >= MOD_MAX_COUNT) {
        return false;
    }

    for (int i = 0; i < s_mod_count; i++) {
        if (strcmp(s_mods[i].id, desc->id) == 0) {
            return false;
        }
    }

    s_mods[s_mod_count] = *desc;
    s_mods[s_mod_count].enabled = desc->default_enabled;
    s_mod_count++;
    return true;
}

int mod_manager_count(void) {
    return s_mod_count;
}

mod_descriptor_t *mod_manager_get(int index) {
    if (index < 0 || index >= s_mod_count) {
        return NULL;
    }
    return &s_mods[index];
}

void mod_manager_toggle(int index) {
    if (index < 0 || index >= s_mod_count) {
        return;
    }
    s_mods[index].enabled = !s_mods[index].enabled;
}

bool mod_manager_is_enabled(const char *id) {
    if (id == NULL) {
        return false;
    }
    for (int i = 0; i < s_mod_count; i++) {
        if (strcmp(s_mods[i].id, id) == 0) {
            return s_mods[i].enabled;
        }
    }
    return false;
}

void mod_manager_on_frame(app_state_t *state) {
    for (int i = 0; i < s_mod_count; i++) {
        if (s_mods[i].enabled && s_mods[i].on_frame != NULL) {
            s_mods[i].on_frame(state);
        }
    }
}

void mod_manager_on_render_3d(const app_state_t *state) {
    for (int i = 0; i < s_mod_count; i++) {
        if (s_mods[i].enabled && s_mods[i].on_render_3d != NULL) {
            s_mods[i].on_render_3d(state);
        }
    }
}

void mod_manager_on_render_hud(const app_state_t *state) {
    for (int i = 0; i < s_mod_count; i++) {
        if (s_mods[i].enabled && s_mods[i].on_render_hud != NULL) {
            s_mods[i].on_render_hud(state);
        }
    }
}

void mod_manager_menu_input(app_state_t *state, bool up, bool down, bool left, bool right, bool confirm, bool close) {
    if (state == NULL) {
        return;
    }
    if (close) {
        state->mods_menu_open = false;
        return;
    }
    if (up) {
        s_menu_cursor = (s_menu_cursor + POPUP_ITEM_COUNT - 1) % POPUP_ITEM_COUNT;
    }
    if (down) {
        s_menu_cursor = (s_menu_cursor + 1) % POPUP_ITEM_COUNT;
    }

    int dir = 0;
    if (left) {
        dir = -1;
    } else if (right || confirm) {
        dir = 1;
    }
    if (dir == 0) {
        return;
    }

    switch (s_menu_cursor) {
        case 0: /* 3D Voxel Pitch (OFF / 15 / 35 / 50 / 75) */
            state->voxel_pitch_deg = step_voxel_pitch(state->voxel_pitch_deg, dir);
            set_mod_enabled("dramatic_shape", state->voxel_pitch_deg > 0);
            break;
        case 1: /* 3D Daytime */
            state->daytime_mode = (uint8_t)((state->daytime_mode + 4u + (unsigned)dir) % 4u);
            break;
        case 2: /* GB Display Filter */
            state->gb_filter_mode = (uint8_t)((state->gb_filter_mode + 6u + (unsigned)dir) % 6u);
            gb_brain_set_filter(state->gb_filter_mode);
            break;
        case 3: /* DMG Audio */
            toggle_mod_by_id("dmg_audio");
            break;
        case 4: /* Battle Move Info */
            toggle_mod_by_id("battle_move_info");
            break;
        case 5: /* PokePC Followers */
            toggle_mod_by_id("pokepc_followers");
            break;
        case 6: /* Wilds of Kanto */
            toggle_mod_by_id("wilds_of_kanto");
            break;
        case 7: /* Gen 3 PC Box launcher */
            if (confirm) {
                set_mod_enabled("gen3_box", true);
                state->mods_menu_open = false;
                state->gen3box_open = true;
            }
            break;
        case 8: /* FPS / Perf HUD */
            state->show_debug_hud = !state->show_debug_hud;
            break;
        default:
            break;
    }
}

void mod_manager_menu_render(const app_state_t *state) {
    if (state == NULL || !state->mods_menu_open) {
        return;
    }

    /* Compact floating popup centered over the live game frame (does not consume screen). */
    const int box_w = 224;
    const int box_h = 172;
    const int box_x = (RENDER_W - box_w) / 2;
    const int box_y = (RENDER_H - box_h) / 2;

    const uint16_t col_shadow   = RGB565(6, 8, 14);
    const uint16_t col_bg       = RGB565(14, 18, 30);
    const uint16_t col_header   = RGB565(26, 42, 72);
    const uint16_t col_border   = RGB565(100, 175, 250);
    const uint16_t col_sel_bg   = RGB565(36, 62, 108);
    const uint16_t col_sel_edge = RGB565(135, 215, 255);
    const uint16_t col_text     = RGB565(238, 244, 254);
    const uint16_t col_dim      = RGB565(150, 172, 202);
    const uint16_t col_val      = RGB565(250, 214, 104);
    const uint16_t col_on       = RGB565(92, 236, 134);
    const uint16_t col_off      = RGB565(232, 98, 98);

    /* Drop shadow + floating panel */
    renderer_fill_rect(box_x + 3, box_y + 3, box_w, box_h, col_shadow);
    renderer_fill_rect(box_x, box_y, box_w, box_h, col_bg);
    renderer_draw_rect(box_x, box_y, box_w, box_h, col_border);

    /* Compact header */
    renderer_fill_rect(box_x + 1, box_y + 1, box_w - 2, 16, col_header);
    renderer_draw_rect(box_x, box_y + 16, box_w, 1, col_border);
    renderer_draw_text(box_x + 8, box_y + 5, "MODS & DISPLAY  (L+R)", col_text, col_header, false);

    static const char *const k_labels[POPUP_ITEM_COUNT] = {
        "3D Voxel Pitch",
        "3D Daytime",
        "GB Filter",
        "DMG Audio",
        "Battle Move Info",
        "PokePC Followers",
        "Wilds of Kanto",
        "Gen 3 PC Box",
        "FPS / Perf HUD"
    };

    const int list_y = box_y + 21;
    const int row_h = 15;

    for (int i = 0; i < POPUP_ITEM_COUNT; i++) {
        const int row_y = list_y + i * row_h;
        const bool selected = (i == s_menu_cursor);
        const uint16_t row_bg = selected ? col_sel_bg : col_bg;

        if (selected) {
            renderer_fill_rect(box_x + 4, row_y, box_w - 8, row_h - 1, col_sel_bg);
            renderer_draw_rect(box_x + 4, row_y, box_w - 8, row_h - 1, col_sel_edge);
        }

        char left_txt[24];
        snprintf(left_txt, sizeof(left_txt), "%c%s", selected ? '>' : ' ', k_labels[i]);
        renderer_draw_text(box_x + 7, row_y + 3, left_txt, col_text, row_bg, false);

        char val_txt[16];
        uint16_t val_col = col_val;
        switch (i) {
            case 0:
                if (state->voxel_pitch_deg > 0) {
                    snprintf(val_txt, sizeof(val_txt), "%d DEG", state->voxel_pitch_deg);
                    val_col = col_on;
                } else {
                    snprintf(val_txt, sizeof(val_txt), "OFF");
                    val_col = col_off;
                }
                break;
            case 1:
                snprintf(val_txt, sizeof(val_txt), "%s", daytime_label(state->daytime_mode));
                break;
            case 2:
                snprintf(val_txt, sizeof(val_txt), "%s", gb_filter_name(state->gb_filter_mode));
                break;
            case 3: {
                bool on = mod_manager_is_enabled("dmg_audio");
                snprintf(val_txt, sizeof(val_txt), "%s", on ? "ON" : "OFF");
                val_col = on ? col_on : col_off;
                break;
            }
            case 4: {
                bool on = mod_manager_is_enabled("battle_move_info");
                snprintf(val_txt, sizeof(val_txt), "%s", on ? "ON" : "OFF");
                val_col = on ? col_on : col_off;
                break;
            }
            case 5: {
                bool on = mod_manager_is_enabled("pokepc_followers");
                snprintf(val_txt, sizeof(val_txt), "%s", on ? "ON" : "OFF");
                val_col = on ? col_on : col_off;
                break;
            }
            case 6: {
                bool on = mod_manager_is_enabled("wilds_of_kanto");
                snprintf(val_txt, sizeof(val_txt), "%s", on ? "ON" : "OFF");
                val_col = on ? col_on : col_off;
                break;
            }
            case 7:
                snprintf(val_txt, sizeof(val_txt), "OPEN >");
                val_col = col_sel_edge;
                break;
            case 8:
                snprintf(val_txt, sizeof(val_txt), "%s", state->show_debug_hud ? "ON" : "OFF");
                val_col = state->show_debug_hud ? col_on : col_off;
                break;
            default:
                val_txt[0] = '\0';
                break;
        }

        int val_len = (int)strlen(val_txt);
        int val_x = box_x + box_w - 8 - val_len * 6;
        renderer_draw_text(val_x, row_y + 3, val_txt, val_col, row_bg, false);
    }

    const int foot_y = box_y + box_h - 14;
    renderer_draw_rect(box_x, foot_y - 3, box_w, 1, RGB565(42, 68, 108));
    renderer_draw_text(box_x + 8, foot_y + 1, "A/L-R:Change  B:Close", col_dim, col_bg, false);
}
