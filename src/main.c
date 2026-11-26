#define _DEFAULT_SOURCE

#include "of.h"
#include "app_state.h"
#include "gb_audio.h"
#include "gb_brain.h"
#include "mod_manager.h"
#include "renderer.h"
#include "renderer3d.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

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
    renderer_draw_text(x + 8, y + 116, "L+R: Mods  L+SEL: HUD", RGB565(115, 220, 155), bg, false);
}

static void render_footer_bar(const app_state_t *state) {
    if (!state->show_debug_hud) {
        return;
    }
    const int y = RENDER_H - 26;
    const uint16_t bg     = RGB565(14, 18, 30);
    const uint16_t border = RGB565(50, 75, 115);
    const uint16_t dim    = RGB565(135, 160, 195);

    renderer_fill_rect(4, y, RENDER_W - 8, 22, bg);
    renderer_draw_rect(4, y, RENDER_W - 8, 22, border);
    renderer_draw_text(10, y + 7, "L+R:Menu  R+SEL:Box  L+SEL:HUD", dim, bg, false);
}

int main(int argc, char **argv) {
    const char *rom_path = (argc > 1 && argv[1] != NULL && argv[1][0] != '\0')
                           ? argv[1]
                           : "pokered.gb";

    app_state_t state;
    memset(&state, 0, sizeof(state));
    state.running = true;
    state.voxel_pitch_deg = 0;
    state.gb_filter_mode = 0;

    renderer_caps_t caps;
    memset(&caps, 0, sizeof(caps));
    if (!renderer_init(&caps)) {
        return 1;
    }
    (void)caps;

    gb_audio_init();
    gb_brain_init(rom_path, &state);
    gb_brain_set_filter(state.gb_filter_mode);
    mod_manager_init(&state);

    const int gb_dst_x = (RENDER_W - GB_LCD_W) / 2;

    unsigned int fps_window_start = of_time_ms();
    uint32_t     fps_frames = 0;

    while (state.running) {
        unsigned int frame_start_us = of_time_us();
        of_input_poll();

        bool lr_combo = (of_btn(OF_BTN_L1) && of_btn_pressed(OF_BTN_R1)) ||
                        (of_btn(OF_BTN_R1) && of_btn_pressed(OF_BTN_L1));
        bool box_combo = (of_btn(OF_BTN_R1) && of_btn_pressed(OF_BTN_SELECT)) ||
                         (of_btn(OF_BTN_SELECT) && of_btn_pressed(OF_BTN_R1));
        bool hud_combo = (of_btn(OF_BTN_L1) && of_btn_pressed(OF_BTN_SELECT)) ||
                         (of_btn(OF_BTN_SELECT) && of_btn_pressed(OF_BTN_L1));
        bool menu_toggle = lr_combo;

        if (hud_combo) {
            state.show_debug_hud = !state.show_debug_hud;
        }

        if (state.mods_menu_open) {
            bool up      = of_btn_pressed(OF_BTN_UP);
            bool down    = of_btn_pressed(OF_BTN_DOWN);
            bool left    = of_btn_pressed(OF_BTN_LEFT);
            bool right   = of_btn_pressed(OF_BTN_RIGHT);
            bool confirm = of_btn_pressed(OF_BTN_A);
            bool close   = menu_toggle || of_btn_pressed(OF_BTN_B);

            mod_manager_menu_input(&state, up, down, left, right, confirm, close);

            gb_input_t empty_in;
            memset(&empty_in, 0, sizeof(empty_in));
            gb_brain_set_input(&empty_in);
        } else if (state.gen3box_open) {
            bool up     = of_btn_pressed(OF_BTN_UP);
            bool down   = of_btn_pressed(OF_BTN_DOWN);
            bool left   = of_btn_pressed(OF_BTN_LEFT);
            bool right  = of_btn_pressed(OF_BTN_RIGHT);
            bool action = of_btn_pressed(OF_BTN_A);
            bool close  = box_combo || of_btn_pressed(OF_BTN_B);

            mod_gen3box_input(&state, up, down, left, right, action, close);

            gb_input_t empty_in;
            memset(&empty_in, 0, sizeof(empty_in));
            gb_brain_set_input(&empty_in);
        } else {
            if (menu_toggle) {
                state.mods_menu_open = true;
            } else if (box_combo && mod_manager_is_enabled("gen3_box")) {
                state.gen3box_open = true;
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

        state.audio_enabled = mod_manager_is_enabled("dmg_audio");
        unsigned int t_gb0 = of_time_us();
        if (state.rom_loaded) {
            /* Decided from last frame's snapshot: skip LCD output while no GB pixels are shown. */
            gb_brain_set_skip_ppu(state.rom_loaded &&
                                  mod_manager_is_enabled("dramatic_shape") &&
                                  state.voxel_pitch_deg > 0 &&
                                  state.snap.in_overworld_ready &&
                                  state.snap.in_battle == 0 &&
                                  state.snap.ui_overlay == (uint8_t)UI_OVERLAY_NONE);
            gb_brain_run_frame(&state);
            mod_manager_on_frame(&state);
        }
        state.perf_gb_ms = (uint16_t)((of_time_us() - t_gb0) / 1000u);
        state.frame_count++;

        bool voxel_3d_active = state.rom_loaded &&
                               mod_manager_is_enabled("dramatic_shape") &&
                               state.voxel_pitch_deg > 0 &&
                               state.snap.in_overworld_ready &&
                               state.snap.in_battle == 0 &&
                               state.snap.ui_overlay != (uint8_t)UI_OVERLAY_FULLSCREEN_2D;

        if (state.rom_loaded && !voxel_3d_active) {
            /* 2D full-screen 2x2 blit overwrites every pixel; skip the 184 KB clear. */
            renderer_begin_frame_noclear();
        } else {
            renderer_begin_frame(RGB565(10, 14, 24));
        }

        unsigned int t_3d0 = of_time_us();
        mod_manager_on_render_3d(&state);
        if (voxel_3d_active) {
            r3d_end_scene();
        }
        state.perf_3d_ms = (uint16_t)((of_time_us() - t_3d0) / 1000u);

        if (state.rom_loaded) {
            if (!voxel_3d_active) {
                /* Exact 2x2 integer blit into 320x288 (10:9) -> 5x Pocket HW scale = exact 10x GB LCD! */
                renderer_blit_gb_lcd_2x(gb_brain_get_lcd_rgb565(), state.gb_filter_mode >= 4u);
            } else if (state.snap.in_battle != 0 ||
                       state.snap.ui_overlay == (uint8_t)UI_OVERLAY_DIALOGUE) {
                /* Composite bottom dialogue strip (rows 96..143) over 3D scene */
                const int dlg_y = RENDER_H - 52;
                renderer_draw_rect(gb_dst_x - 1, dlg_y - 1, GB_LCD_W + 2, 50, RGB565(220, 228, 240));
                renderer_blit_gb_subrect(gb_brain_get_lcd_rgb565(), 0, 96, GB_LCD_W, 48, gb_dst_x, dlg_y);
            } else if (state.snap.ui_overlay == (uint8_t)UI_OVERLAY_START_MENU) {
                /* Composite right-side Start Menu (80x128) cleanly over the 3D scene */
                renderer_draw_rect(RENDER_W - 88 - 1, 7, 82, 130, RGB565(220, 228, 240));
                renderer_blit_gb_subrect(gb_brain_get_lcd_rgb565(), 80, 0, 80, 128, RENDER_W - 88, 8);
            }
        } else {
            render_missing_rom_screen(rom_path);
        }

        mod_manager_on_render_hud(&state);
        render_footer_bar(&state);

        if (state.gen3box_open) {
            mod_gen3box_render(&state);
        }
        if (state.mods_menu_open) {
            mod_manager_menu_render(&state);
        }

        renderer_end_frame();

        fps_frames++;
        unsigned int now_ms = of_time_ms();
        if (now_ms - fps_window_start >= 1000u) {
            state.perf_fps = (uint16_t)fps_frames;
            fps_frames = 0;
            fps_window_start = now_ms;
        }

        /* Only sleep remaining budget on PC; Pocket paces naturally via vblank/work. */
#ifdef OF_PC
        unsigned int elapsed_us = of_time_us() - frame_start_us;
        if (elapsed_us < 16667u) {
            usleep(16667u - elapsed_us);
        }
#else
        (void)frame_start_us;
#endif
    }

    gb_brain_shutdown();
    return 0;
}
