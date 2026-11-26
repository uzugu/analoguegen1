#ifndef RENDERER_H
#define RENDERER_H

#include <stdbool.h>
#include <stdint.h>

#define RENDER_W 320
#define RENDER_H 240

/* RGB565 helper macro (R:0..31, G:0..63, B:0..31) */
#define RGB565(r8, g8, b8) \
    ((uint16_t)((((r8) & 0xF8) << 8) | (((g8) & 0xFC) << 3) | (((b8) & 0xF8) >> 3)))

typedef struct {
    bool hw_gpu_available;
    bool hw_vert_tri;
    bool hw_vcolor;
    bool rgb565_mode;
} renderer_caps_t;

bool renderer_init(renderer_caps_t *out_caps);
void renderer_begin_frame(uint16_t clear_color);
void renderer_end_frame(void);

/* Blit 160x144 RGB565 Game Boy LCD centered (or 1x) into the 320x240 buffer. */
void renderer_blit_gb_lcd(const uint16_t *lcd_rgb565, int dst_x, int dst_y);

/* Minimal 2D primitives for HUD, battle move info, and Mods menu */
void renderer_fill_rect(int x, int y, int w, int h, uint16_t color);
void renderer_draw_rect(int x, int y, int w, int h, uint16_t color);
void renderer_draw_text(int x, int y, const char *text, uint16_t fg, uint16_t bg, bool draw_bg);

#endif /* RENDERER_H */
