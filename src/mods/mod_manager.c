#include "mod_manager.h"
#include "renderer.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

static mod_descriptor_t s_mods[MOD_MAX_COUNT];
static int s_mod_count = 0;
static int s_menu_cursor = 0;

void mod_manager_init(app_state_t *state) {
    s_mod_count = 0;
    s_menu_cursor = 0;

    mod_voxel_register();
    mod_moveinfo_register();
    mod_followers_register();
    mod_wilds_register();
    mod_gen3box_register();

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

void mod_manager_menu_input(app_state_t *state, bool up, bool down, bool toggle, bool close) {
    if (state == NULL) {
        return;
    }
    if (close) {
        state->mods_menu_open = false;
        return;
    }
    if (s_mod_count <= 0) {
        return;
    }
    if (up) {
        s_menu_cursor = (s_menu_cursor + s_mod_count - 1) % s_mod_count;
    }
    if (down) {
        s_menu_cursor = (s_menu_cursor + 1) % s_mod_count;
    }
    if (toggle) {
        mod_manager_toggle(s_menu_cursor);
    }
}

void mod_manager_menu_render(const app_state_t *state) {
    if (state == NULL || !state->mods_menu_open) {
        return;
    }

    const int box_x = 10;
    const int box_y = 14;
    const int box_w = RENDER_W - 20;
    const int box_h = RENDER_H - 28;

    const uint16_t col_bg       = RGB565(12, 16, 28);
    const uint16_t col_header   = RGB565(22, 36, 62);
    const uint16_t col_border   = RGB565(90, 160, 240);
    const uint16_t col_sel_bg   = RGB565(34, 58, 98);
    const uint16_t col_sel_edge = RGB565(120, 200, 255);
    const uint16_t col_text     = RGB565(235, 242, 252);
    const uint16_t col_dim      = RGB565(150, 170, 198);
    const uint16_t col_cat      = RGB565(130, 205, 255);
    const uint16_t col_on       = RGB565(90, 235, 130);
    const uint16_t col_off      = RGB565(230, 95, 95);

    /* Outer shadow + dark panel */
    renderer_fill_rect(box_x + 2, box_y + 2, box_w, box_h, RGB565(4, 6, 12));
    renderer_fill_rect(box_x, box_y, box_w, box_h, col_bg);
    renderer_draw_rect(box_x, box_y, box_w, box_h, col_border);

    /* Header bar */
    renderer_fill_rect(box_x + 1, box_y + 1, box_w - 2, 20, col_header);
    renderer_draw_rect(box_x, box_y + 20, box_w, 1, col_border);

    char header[40];
    snprintf(header, sizeof(header), "MODS MENU (%d/%d)", s_mod_count, MOD_MAX_COUNT);
    renderer_draw_text(box_x + 8, box_y + 7, header, col_text, col_header, false);

    /* List area: up to 5 visible rows at 30 px each */
    const int list_y = box_y + 26;
    const int row_h = 30;
    const int max_vis = 5;

    if (s_mod_count == 0) {
        renderer_draw_text(box_x + 12, list_y + 10, "No mods registered.", col_dim, col_bg, false);
    } else {
        int start = 0;
        if (s_menu_cursor >= max_vis) {
            start = s_menu_cursor - max_vis + 1;
        }
        int end = start + max_vis;
        if (end > s_mod_count) {
            end = s_mod_count;
        }

        for (int i = start; i < end; i++) {
            const mod_descriptor_t *m = &s_mods[i];
            int row_y = list_y + (i - start) * row_h;
            bool selected = (i == s_menu_cursor);
            uint16_t row_bg = selected ? col_sel_bg : col_bg;

            if (selected) {
                renderer_fill_rect(box_x + 4, row_y, box_w - 8, row_h - 2, col_sel_bg);
                renderer_draw_rect(box_x + 4, row_y, box_w - 8, row_h - 2, col_sel_edge);
            }

            /* Category tag + mod name */
            char line1[36];
            const char *cat = m->category ? m->category : "MOD";
            const char *name = m->name ? m->name : m->id;
            snprintf(line1, sizeof(line1), "%c[%.8s] %.18s",
                     selected ? '>' : ' ', cat, name);
            renderer_draw_text(box_x + 8, row_y + 4, line1, col_text, row_bg, false);

            /* Status badge right-aligned */
            const char *badge = m->enabled ? "[ON]" : "[OFF]";
            uint16_t badge_col = m->enabled ? col_on : col_off;
            renderer_draw_text(box_x + box_w - 46, row_y + 4, badge, badge_col, row_bg, false);

            /* Description line (truncated cleanly to panel width) */
            if (m->description != NULL) {
                char desc[36];
                snprintf(desc, sizeof(desc), " %.33s", m->description);
                renderer_draw_text(box_x + 10, row_y + 15, desc,
                                   selected ? col_cat : col_dim, row_bg, false);
            }
        }
    }

    /* Footer controls hint */
    const int foot_y = box_y + box_h - 18;
    renderer_draw_rect(box_x, foot_y - 3, box_w, 1, RGB565(40, 65, 105));
    renderer_draw_text(box_x + 8, foot_y + 2,
                       "A:Toggle  B/X/L+R:Close  UP/DN",
                       col_dim, col_bg, false);
}
