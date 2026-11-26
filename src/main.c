#define _DEFAULT_SOURCE

#include "of.h"
#include "app_state.h"
#include "gb_brain.h"
#include "mod_manager.h"
#include "renderer.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int cycle_voxel_pitch(int current_deg) {
    static const int k_rungs[] = { 0, 15, 35, 50, 75 };
    const int n = (int)(sizeof(k_rungs) / sizeof(k_rungs[0]));
    for (int i = 0; i < n; i++) {
        if (k_rungs[i] == current_deg) {
            return k_rungs[(i + 1) % n];
        }
    }
    return 35;
}

static void render_missing_rom_screen(const char *rom_path) {
    const int x = (RENDER_W - GB_LCD_W) / 2;
    const int y = (RENDER_H - GB_LCD_H) / 2;
    const int w = GB_LCD_W;
    const int h = GB_LCD_H;

    const uint16_t bg     = RGB565(22, 26, 40);
    const uint16_t border = RGB565(235, 165, 70);
    const uint16_t title  = RGB565(255, 205, 90);
    const uint16_t fg     = RGB565(230, 238, 250);
    const uint16_t dim    = RGB565(150, 175, 205);

    renderer_fill_rect(x, y, w, h, bg);
    renderer_draw_rect(x, y, w, h, border);

    renderer_draw_text(x + 8, y + 12, "NO ROM LOADED", title, bg, false);
    renderer_draw_text(x + 8, y + 34, "Drop pokered.gb", fg, bg, false);
    renderer_draw_text(x + 8, y + 48, "next to binary /", fg, bg, false);
    renderer_draw_text(x + 8, y + 62, "SD card.", fg, bg, false);

    char path_buf[24];
    snprintf(path_buf, sizeof(path_buf), "%.18s", rom_path ? rom_path : "pokered.gb");
    renderer_draw_text(x + 8, y + 88,  path_buf, dim, bg, false);
    renderer_draw_text(x + 8, y + 116, "X/Y or L+R: Mods", RGB565(115, 220, 155), bg, false);
}

static void render_footer_bar(const app_state_t *state) {
    const int y = 198;
    const uint16_t bg     = RGB565(14, 18, 30);
    const uint16_t border = RGB565(50, 75, 115);
    const uint16_t fg     = RGB565(215, 225, 240);
    const uint16_t dim    = RGB565(135, 160, 195);

    renderer_fill_rect(4, y, RENDER_W - 8, 38, bg);
    renderer_draw_rect(4, y, RENDER_W - 8, 38, border);

    char line1[48];
    if (state->rom_loaded) {
        const char *title = state->rom_title[0] != '\0' ? state->rom_title : "GAME BOY";
        snprintf(line1, sizeof(line1), "%.16s  F:%lu",
                 title, (unsigned long)state->frame_count);
    } else {
        snprintf(line1, sizeof(line1), "analoguegen1  NO ROM");
    }

    renderer_draw_text(10, y + 6,  line1, fg, bg, false);
    renderer_draw_text(10, y + 20, "L+R/X/Y:Mods  L:Pitch  SELECT/START", dim, bg, false);
}

int main(int argc, char **argv) {
    const char *rom_path = (argc > 1 && argv[1] != NULL && argv[1][0] != '\0')
                           ? argv[1]
                           : "pokered.gb";

    app_state_t state;
    memset(&state, 0, sizeof(state));
    state.running = true;
    state.voxel_pitch_deg = 35;

    renderer_caps_t caps;
    memset(&caps, 0, sizeof(caps));
    if (!renderer_init(&caps)) {
        return 1;
    }
    (void)caps;

    gb_brain_init(rom_path, &state);
    mod_manager_init(&state);

    const int gb_dst_x = (RENDER_W - GB_LCD_W) / 2;
    const int gb_dst_y = (RENDER_H - GB_LCD_H) / 2;

    while (state.running) {
        of_input_poll();

        bool lr_combo = (of_btn(OF_BTN_L1) && of_btn_pressed(OF_BTN_R1)) ||
                        (of_btn(OF_BTN_R1) && of_btn_pressed(OF_BTN_L1));
        bool menu_toggle = lr_combo ||
                           of_btn_pressed(OF_BTN_X) ||
                           of_btn_pressed(OF_BTN_Y);

        if (state.mods_menu_open) {
            bool up     = of_btn_pressed(OF_BTN_UP);
            bool down   = of_btn_pressed(OF_BTN_DOWN);
            bool toggle = of_btn_pressed(OF_BTN_A);
            bool close  = menu_toggle || of_btn_pressed(OF_BTN_B);

            mod_manager_menu_input(&state, up, down, toggle, close);

            gb_input_t empty_in;
            memset(&empty_in, 0, sizeof(empty_in));
            gb_brain_set_input(&empty_in);
        } else {
            if (menu_toggle) {
                state.mods_menu_open = true;
            } else if (of_btn_pressed(OF_BTN_L1) && !of_btn(OF_BTN_R1)) {
                state.voxel_pitch_deg = cycle_voxel_pitch(state.voxel_pitch_deg);
            }

            gb_input_t gb_in;
            gb_in.up     = of_btn(OF_BTN_UP) != 0;
            gb_in.down   = of_btn(OF_BTN_DOWN) != 0;
            gb_in.left   = of_btn(OF_BTN_LEFT) != 0;
            gb_in.right  = of_btn(OF_BTN_RIGHT) != 0;
            gb_in.a      = of_btn(OF_BTN_A) != 0;
            gb_in.b      = of_btn(OF_BTN_B) != 0;
            gb_in.start  = of_btn(OF_BTN_START) != 0;
            gb_in.select = of_btn(OF_BTN_SELECT) != 0;

            gb_brain_set_input(&gb_in);
        }

        if (state.rom_loaded) {
            gb_brain_run_frame(&state);
            mod_manager_on_frame(&state);
        }
        state.frame_count++;

        renderer_begin_frame(RGB565(10, 14, 24));
        mod_manager_on_render_3d(&state);

        if (state.rom_loaded) {
            renderer_draw_rect(gb_dst_x - 1, gb_dst_y - 1,
                               GB_LCD_W + 2, GB_LCD_H + 2,
                               RGB565(65, 95, 140));
            renderer_blit_gb_lcd(gb_brain_get_lcd_rgb565(), gb_dst_x, gb_dst_y);
        } else {
            render_missing_rom_screen(rom_path);
        }

        mod_manager_on_render_hud(&state);
        render_footer_bar(&state);

        if (state.mods_menu_open) {
            mod_manager_menu_render(&state);
        }

        renderer_end_frame();
        usleep(16600);
    }

    gb_brain_shutdown();
    return 0;
}
