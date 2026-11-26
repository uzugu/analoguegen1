#ifndef RENDERER_H
#define RENDERER_H

#include <stdbool.h>
#include <stdint.h>

#define RENDER_W 320
#define RENDER_H 288

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
void renderer_begin_frame_noclear(void);
void renderer_end_frame(void);

/* Blit 160x144 RGB565 Game Boy LCD centered (1x), exact 2x2 (320x288) with optional LCD grid, or scaled. */
void renderer_blit_gb_lcd(const uint16_t *lcd_rgb565, int dst_x, int dst_y);
void renderer_blit_gb_lcd_2x(const uint16_t *lcd_rgb565, bool lcd_grid);
void renderer_blit_gb_lcd_2x_direct(const uint16_t *lcd_rgb565, bool lcd_grid);
void renderer_blit_gb_lcd_scaled(const uint16_t *lcd_rgb565, int dst_x, int dst_y, int dst_w, int dst_h);

/* Minimal 2D primitives for HUD, battle move info, and Mods menu */
void renderer_fill_rect(int x, int y, int w, int h, uint16_t color);
void renderer_draw_rect(int x, int y, int w, int h, uint16_t color);
void renderer_draw_text(int x, int y, const char *text, uint16_t fg, uint16_t bg, bool draw_bg);

/* Low-level framebuffer & hardware 3D triangle access (Phase 2/3). */
uint16_t *renderer_get_draw_fb(int *out_stride_px);
bool      renderer_has_hw_tri(void);

/* Composite a sub-rectangle of the 160x144 GB LCD (e.g. dialogue box) onto the 320x240 buffer. */
void renderer_blit_gb_subrect(const uint16_t *lcd_rgb565,
                              int src_x, int src_y, int src_w, int src_h,
                              int dst_x, int dst_y);

/* Pocket os30 hardware triangle batching (implemented inside renderer.c via of_gpu.h).
 * renderer_hw_scene_begin() flushes the draw framebuffer and clears zbuf_16 in FPGA hardware. */
void renderer_hw_scene_begin(uint16_t *zbuf_16);
void renderer_hw_scene_end(void);
void renderer_set_cpu_overlay_needed(bool needed);
void renderer_hw_tri_begin(const uint16_t *atlas_rgb565, int atlas_w, int atlas_h,
                           uint16_t *zbuf_16, bool alpha_key);
void renderer_hw_tri_submit(const int16_t x[3], const int16_t y[3],
                            const int32_t s[3], const int32_t t[3],
                            const int32_t zi[3], const uint16_t rgb[3],
                            const int32_t depth[3]);
void renderer_hw_tri_end(void);

#endif /* RENDERER_H */
