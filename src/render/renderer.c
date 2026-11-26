#include "renderer.h"

#include <stddef.h>
#include <string.h>

#include "of.h"
#ifndef OF_PC
#include "of_cache.h"
#include "of_gpu.h"
#endif

#define GB_SRC_W 160
#define GB_SRC_H 144

#define FONT_W 5
#define FONT_H 7
#define FONT_CELL_W 6
#define FONT_CELL_H 8

/* Internal 16-bit backing buffer used when runtime only supports 8-bit indexed. */
static uint16_t s_fb_rgb565[RENDER_W * RENDER_H];

static bool     s_rgb565_mode     = false;
static uint16_t s_fb_stride_px    = RENDER_W;
static uint16_t s_fb_stride_bytes = RENDER_W;

static uint16_t *s_draw_fb     = s_fb_rgb565;
static uint16_t  s_draw_stride = RENDER_W;
static bool      s_hw_tri      = false;
#ifndef OF_PC
static bool      s_hw_tri_active = false;
#endif

/* Compact 5x7 ASCII bitmap font for ASCII 32..126 (column-major, LSB = top row). */
static const uint8_t s_font5x7[95][FONT_W] = {
    {0x00, 0x00, 0x00, 0x00, 0x00}, /* 32 ' ' */
    {0x00, 0x00, 0x5F, 0x00, 0x00}, /* 33 '!' */
    {0x00, 0x07, 0x00, 0x07, 0x00}, /* 34 '"' */
    {0x14, 0x7F, 0x14, 0x7F, 0x14}, /* 35 '#' */
    {0x24, 0x2A, 0x7F, 0x2A, 0x12}, /* 36 '$' */
    {0x23, 0x13, 0x08, 0x64, 0x62}, /* 37 '%' */
    {0x36, 0x49, 0x55, 0x22, 0x50}, /* 38 '&' */
    {0x00, 0x05, 0x03, 0x00, 0x00}, /* 39 ''' */
    {0x00, 0x1C, 0x22, 0x41, 0x00}, /* 40 '(' */
    {0x00, 0x41, 0x22, 0x1C, 0x00}, /* 41 ')' */
    {0x14, 0x08, 0x3E, 0x08, 0x14}, /* 42 '*' */
    {0x08, 0x08, 0x3E, 0x08, 0x08}, /* 43 '+' */
    {0x00, 0x50, 0x30, 0x00, 0x00}, /* 44 ',' */
    {0x08, 0x08, 0x08, 0x08, 0x08}, /* 45 '-' */
    {0x00, 0x60, 0x60, 0x00, 0x00}, /* 46 '.' */
    {0x20, 0x10, 0x08, 0x04, 0x02}, /* 47 '/' */
    {0x3E, 0x51, 0x49, 0x45, 0x3E}, /* 48 '0' */
    {0x00, 0x42, 0x7F, 0x40, 0x00}, /* 49 '1' */
    {0x42, 0x61, 0x51, 0x49, 0x46}, /* 50 '2' */
    {0x21, 0x41, 0x45, 0x4B, 0x31}, /* 51 '3' */
    {0x18, 0x14, 0x12, 0x7F, 0x10}, /* 52 '4' */
    {0x27, 0x45, 0x45, 0x45, 0x39}, /* 53 '5' */
    {0x3C, 0x4A, 0x49, 0x49, 0x30}, /* 54 '6' */
    {0x01, 0x71, 0x09, 0x05, 0x03}, /* 55 '7' */
    {0x36, 0x49, 0x49, 0x49, 0x36}, /* 56 '8' */
    {0x06, 0x49, 0x49, 0x29, 0x1E}, /* 57 '9' */
    {0x00, 0x36, 0x36, 0x00, 0x00}, /* 58 ':' */
    {0x00, 0x56, 0x36, 0x00, 0x00}, /* 59 ';' */
    {0x08, 0x14, 0x22, 0x41, 0x00}, /* 60 '<' */
    {0x14, 0x14, 0x14, 0x14, 0x14}, /* 61 '=' */
    {0x00, 0x41, 0x22, 0x14, 0x08}, /* 62 '>' */
    {0x02, 0x01, 0x51, 0x09, 0x06}, /* 63 '?' */
    {0x32, 0x49, 0x79, 0x41, 0x3E}, /* 64 '@' */
    {0x7E, 0x11, 0x11, 0x11, 0x7E}, /* 65 'A' */
    {0x7F, 0x49, 0x49, 0x49, 0x36}, /* 66 'B' */
    {0x3E, 0x41, 0x41, 0x41, 0x22}, /* 67 'C' */
    {0x7F, 0x41, 0x41, 0x22, 0x1C}, /* 68 'D' */
    {0x7F, 0x49, 0x49, 0x49, 0x41}, /* 69 'E' */
    {0x7F, 0x09, 0x09, 0x09, 0x01}, /* 70 'F' */
    {0x3E, 0x41, 0x49, 0x49, 0x7A}, /* 71 'G' */
    {0x7F, 0x08, 0x08, 0x08, 0x7F}, /* 72 'H' */
    {0x00, 0x41, 0x7F, 0x41, 0x00}, /* 73 'I' */
    {0x20, 0x40, 0x41, 0x3F, 0x01}, /* 74 'J' */
    {0x7F, 0x08, 0x14, 0x22, 0x41}, /* 75 'K' */
    {0x7F, 0x40, 0x40, 0x40, 0x40}, /* 76 'L' */
    {0x7F, 0x02, 0x0C, 0x02, 0x7F}, /* 77 'M' */
    {0x7F, 0x04, 0x08, 0x10, 0x7F}, /* 78 'N' */
    {0x3E, 0x41, 0x41, 0x41, 0x3E}, /* 79 'O' */
    {0x7F, 0x09, 0x09, 0x09, 0x06}, /* 80 'P' */
    {0x3E, 0x41, 0x51, 0x21, 0x5E}, /* 81 'Q' */
    {0x7F, 0x09, 0x19, 0x29, 0x46}, /* 82 'R' */
    {0x46, 0x49, 0x49, 0x49, 0x31}, /* 83 'S' */
    {0x01, 0x01, 0x7F, 0x01, 0x01}, /* 84 'T' */
    {0x3F, 0x40, 0x40, 0x40, 0x3F}, /* 85 'U' */
    {0x1F, 0x20, 0x40, 0x20, 0x1F}, /* 86 'V' */
    {0x3F, 0x40, 0x38, 0x40, 0x3F}, /* 87 'W' */
    {0x63, 0x14, 0x08, 0x14, 0x63}, /* 88 'X' */
    {0x07, 0x08, 0x70, 0x08, 0x07}, /* 89 'Y' */
    {0x61, 0x51, 0x49, 0x45, 0x43}, /* 90 'Z' */
    {0x00, 0x7F, 0x41, 0x41, 0x00}, /* 91 '[' */
    {0x02, 0x04, 0x08, 0x10, 0x20}, /* 92 '\' */
    {0x00, 0x41, 0x41, 0x7F, 0x00}, /* 93 ']' */
    {0x04, 0x02, 0x01, 0x02, 0x04}, /* 94 '^' */
    {0x40, 0x40, 0x40, 0x40, 0x40}, /* 95 '_' */
    {0x00, 0x01, 0x02, 0x04, 0x00}, /* 96 '`' */
    {0x20, 0x54, 0x54, 0x54, 0x78}, /* 97 'a' */
    {0x7F, 0x48, 0x44, 0x44, 0x38}, /* 98 'b' */
    {0x38, 0x44, 0x44, 0x44, 0x20}, /* 99 'c' */
    {0x38, 0x44, 0x44, 0x48, 0x7F}, /* 100 'd' */
    {0x38, 0x54, 0x54, 0x54, 0x18}, /* 101 'e' */
    {0x08, 0x7E, 0x09, 0x01, 0x02}, /* 102 'f' */
    {0x0C, 0x52, 0x52, 0x52, 0x3E}, /* 103 'g' */
    {0x7F, 0x08, 0x04, 0x04, 0x78}, /* 104 'h' */
    {0x00, 0x44, 0x7D, 0x40, 0x00}, /* 105 'i' */
    {0x20, 0x40, 0x44, 0x3D, 0x00}, /* 106 'j' */
    {0x7F, 0x10, 0x28, 0x44, 0x00}, /* 107 'k' */
    {0x00, 0x41, 0x7F, 0x40, 0x00}, /* 108 'l' */
    {0x7C, 0x04, 0x18, 0x04, 0x78}, /* 109 'm' */
    {0x7C, 0x08, 0x04, 0x04, 0x78}, /* 110 'n' */
    {0x38, 0x44, 0x44, 0x44, 0x38}, /* 111 'o' */
    {0x7C, 0x14, 0x14, 0x14, 0x08}, /* 112 'p' */
    {0x08, 0x14, 0x14, 0x18, 0x7C}, /* 113 'q' */
    {0x7C, 0x08, 0x04, 0x04, 0x08}, /* 114 'r' */
    {0x48, 0x54, 0x54, 0x54, 0x20}, /* 115 's' */
    {0x04, 0x3F, 0x44, 0x40, 0x20}, /* 116 't' */
    {0x3C, 0x40, 0x40, 0x20, 0x7C}, /* 117 'u' */
    {0x1C, 0x20, 0x40, 0x20, 0x1C}, /* 118 'v' */
    {0x3C, 0x40, 0x30, 0x40, 0x3C}, /* 119 'w' */
    {0x44, 0x28, 0x10, 0x28, 0x44}, /* 120 'x' */
    {0x0C, 0x50, 0x50, 0x50, 0x3C}, /* 121 'y' */
    {0x44, 0x64, 0x54, 0x4C, 0x44}, /* 122 'z' */
    {0x00, 0x08, 0x36, 0x41, 0x00}, /* 123 '{' */
    {0x00, 0x00, 0x7F, 0x00, 0x00}, /* 124 '|' */
    {0x00, 0x41, 0x36, 0x08, 0x00}, /* 125 '}' */
    {0x10, 0x08, 0x08, 0x10, 0x08}  /* 126 '~' */
};

static inline uint8_t rgb565_to_rgb332(uint16_t c) {
    return (uint8_t)(((c >> 8) & 0xE0u) | ((c >> 6) & 0x1Cu) | ((c >> 3) & 0x03u));
}

static void install_rgb332_palette(void) {
    uint32_t pal[256];
    for (int i = 0; i < 256; i++) {
        uint32_t r3 = (uint32_t)((i >> 5) & 0x07);
        uint32_t g3 = (uint32_t)((i >> 2) & 0x07);
        uint32_t b2 = (uint32_t)(i & 0x03);
        uint32_t r8 = (r3 << 5) | (r3 << 2) | (r3 >> 1);
        uint32_t g8 = (g3 << 5) | (g3 << 2) | (g3 >> 1);
        uint32_t b8 = (b2 << 6) | (b2 << 4) | (b2 << 2) | b2;
        pal[i] = (r8 << 16) | (g8 << 8) | b8;
    }
    of_video_palette_bulk(pal, 256);
}

bool renderer_init(renderer_caps_t *out_caps) {
    of_video_init();

    s_rgb565_mode = false;
    s_fb_stride_px = RENDER_W;
    s_fb_stride_bytes = RENDER_W;
    s_draw_fb = s_fb_rgb565;
    s_draw_stride = RENDER_W;

    of_video_caps_t vcaps;
    memset(&vcaps, 0, sizeof(vcaps));
    of_video_get_caps(&vcaps);

    if ((vcaps.color_mode_mask & OF_VIDEO_MODE_MASK_RGB565) != 0u) {
        of_video_mode_t want;
        of_video_mode_t norm;
        memset(&want, 0, sizeof(want));
        memset(&norm, 0, sizeof(norm));
        want.width = RENDER_W;
        want.height = RENDER_H;
        want.color_mode = OF_VIDEO_MODE_RGB565;

        if (of_video_check_mode(&want, &norm) == 0 &&
            of_video_set_mode(&norm) == 0) {
            of_video_mode_t actual;
            memset(&actual, 0, sizeof(actual));
            of_video_get_mode(&actual);
            if (actual.color_mode == OF_VIDEO_MODE_RGB565 &&
                actual.width == RENDER_W &&
                actual.height == RENDER_H) {
                uint16_t stride_bytes = actual.stride ? actual.stride : (uint16_t)(RENDER_W * 2u);
                s_rgb565_mode = true;
                s_fb_stride_bytes = stride_bytes;
                s_fb_stride_px = (uint16_t)(stride_bytes / 2u);
            }
        }
    }

    if (!s_rgb565_mode) {
        of_video_mode_t actual;
        memset(&actual, 0, sizeof(actual));
        of_video_get_mode(&actual);
        if (actual.color_mode != OF_VIDEO_MODE_8BIT ||
            actual.width != RENDER_W ||
            actual.height != RENDER_H) {
            of_video_mode_t mode8;
            memset(&mode8, 0, sizeof(mode8));
            mode8.width = RENDER_W;
            mode8.height = RENDER_H;
            mode8.color_mode = OF_VIDEO_MODE_8BIT;
            if (of_video_set_mode(&mode8) != 0) {
                of_video_set_color_mode(OF_VIDEO_MODE_8BIT);
            }
            of_video_get_mode(&actual);
        }
        s_fb_stride_bytes = actual.stride ? actual.stride : RENDER_W;
        s_fb_stride_px = RENDER_W;
        install_rgb332_palette();
    }

    of_video_set_display_mode(OF_DISPLAY_FRAMEBUFFER);

    renderer_caps_t caps;
    memset(&caps, 0, sizeof(caps));
    caps.rgb565_mode = s_rgb565_mode;

#ifndef OF_PC
    {
        const struct of_capabilities *sys_caps = of_get_caps();
        if (sys_caps && sys_caps->gpu_base != 0u && of_has_feature(OF_HW_GPU_SPAN)) {
            of_gpu_init();
            caps.hw_gpu_available = true;
            caps.hw_vert_tri = (of_has_feature(OF_HW_GPU_VERT_TRI) != 0);
            caps.hw_vcolor   = (of_has_feature(OF_HW_GPU_VCOLOR) != 0);
        }
    }
#else
    caps.hw_gpu_available = false;
    caps.hw_vert_tri = false;
    caps.hw_vcolor = false;
#endif

    s_hw_tri = (caps.hw_vert_tri && caps.hw_vcolor);

    if (out_caps) {
        *out_caps = caps;
    }

    return true;
}

void renderer_begin_frame(uint16_t clear_color) {
    if (s_rgb565_mode) {
        uint16_t *surf = of_video_surface16();
        if (surf) {
            s_draw_fb = surf;
            s_draw_stride = s_fb_stride_px;
        } else {
            s_draw_fb = s_fb_rgb565;
            s_draw_stride = RENDER_W;
        }
    } else {
        s_draw_fb = s_fb_rgb565;
        s_draw_stride = RENDER_W;
    }

    if (clear_color == 0u && s_draw_stride == RENDER_W) {
        memset(s_draw_fb, 0, (size_t)RENDER_W * RENDER_H * sizeof(uint16_t));
    } else {
        for (int y = 0; y < RENDER_H; y++) {
            uint16_t *row = s_draw_fb + (uint32_t)y * s_draw_stride;
            if (clear_color == 0u) {
                memset(row, 0, (size_t)RENDER_W * sizeof(uint16_t));
            } else {
                for (int x = 0; x < RENDER_W; x++) {
                    row[x] = clear_color;
                }
            }
        }
    }
}

void renderer_end_frame(void) {
    if (!s_rgb565_mode) {
        uint8_t *dst8 = of_video_surface();
        if (dst8) {
            for (int y = 0; y < RENDER_H; y++) {
                const uint16_t *src = s_fb_rgb565 + (uint32_t)y * RENDER_W;
                uint8_t *dst = dst8 + (uint32_t)y * s_fb_stride_bytes;
                for (int x = 0; x < RENDER_W; x++) {
                    dst[x] = rgb565_to_rgb332(src[x]);
                }
            }
        }
    }
    of_video_flip();
}

void renderer_blit_gb_lcd(const uint16_t *lcd_rgb565, int dst_x, int dst_y) {
    if (!lcd_rgb565 || !s_draw_fb) {
        return;
    }

    int src_x = 0;
    int src_y = 0;
    int copy_w = GB_SRC_W;
    int copy_h = GB_SRC_H;

    if (dst_x < 0) {
        src_x = -dst_x;
        copy_w += dst_x;
        dst_x = 0;
    }
    if (dst_y < 0) {
        src_y = -dst_y;
        copy_h += dst_y;
        dst_y = 0;
    }
    if (dst_x + copy_w > RENDER_W) {
        copy_w = RENDER_W - dst_x;
    }
    if (dst_y + copy_h > RENDER_H) {
        copy_h = RENDER_H - dst_y;
    }
    if (copy_w <= 0 || copy_h <= 0) {
        return;
    }

    for (int y = 0; y < copy_h; y++) {
        const uint16_t *src = lcd_rgb565 + (uint32_t)(src_y + y) * GB_SRC_W + src_x;
        uint16_t *dst = s_draw_fb + (uint32_t)(dst_y + y) * s_draw_stride + dst_x;
        memcpy(dst, src, (size_t)copy_w * sizeof(uint16_t));
    }
}

void renderer_fill_rect(int x, int y, int w, int h, uint16_t color) {
    if (!s_draw_fb || w <= 0 || h <= 0) {
        return;
    }

    if (x < 0) {
        w += x;
        x = 0;
    }
    if (y < 0) {
        h += y;
        y = 0;
    }
    if (x + w > RENDER_W) {
        w = RENDER_W - x;
    }
    if (y + h > RENDER_H) {
        h = RENDER_H - y;
    }
    if (w <= 0 || h <= 0) {
        return;
    }

    for (int ry = 0; ry < h; ry++) {
        uint16_t *row = s_draw_fb + (uint32_t)(y + ry) * s_draw_stride + x;
        for (int rx = 0; rx < w; rx++) {
            row[rx] = color;
        }
    }
}

void renderer_draw_rect(int x, int y, int w, int h, uint16_t color) {
    if (!s_draw_fb || w <= 0 || h <= 0) {
        return;
    }

    if (h == 1) {
        renderer_fill_rect(x, y, w, 1, color);
        return;
    }
    if (w == 1) {
        renderer_fill_rect(x, y, 1, h, color);
        return;
    }

    renderer_fill_rect(x, y, w, 1, color);
    renderer_fill_rect(x, y + h - 1, w, 1, color);
    renderer_fill_rect(x, y + 1, 1, h - 2, color);
    renderer_fill_rect(x + w - 1, y + 1, 1, h - 2, color);
}

void renderer_draw_text(int x, int y, const char *text, uint16_t fg, uint16_t bg, bool draw_bg) {
    if (!text || !s_draw_fb) {
        return;
    }

    int cur_x = x;
    int cur_y = y;

    for (const char *p = text; *p != '\0'; p++) {
        unsigned char ch = (unsigned char)*p;
        if (ch == '\n') {
            cur_x = x;
            cur_y += FONT_CELL_H;
            continue;
        }
        if (ch == '\r') {
            continue;
        }

        uint8_t glyph_idx;
        if (ch < 32u) {
            glyph_idx = 0u; /* space for control characters */
        } else if (ch > 126u) {
            glyph_idx = (uint8_t)('?' - 32);
        } else {
            glyph_idx = (uint8_t)(ch - 32u);
        }

        if (cur_x + FONT_CELL_W > 0 && cur_x < RENDER_W &&
            cur_y + FONT_CELL_H > 0 && cur_y < RENDER_H) {
            const uint8_t *cols = s_font5x7[glyph_idx];
            int py_start = (cur_y < 0) ? -cur_y : 0;
            int py_end = (cur_y + FONT_CELL_H > RENDER_H) ? (RENDER_H - cur_y) : FONT_CELL_H;
            int px_start = (cur_x < 0) ? -cur_x : 0;
            int px_end = (cur_x + FONT_CELL_W > RENDER_W) ? (RENDER_W - cur_x) : FONT_CELL_W;

            for (int py = py_start; py < py_end; py++) {
                uint16_t *row_base = s_draw_fb + (uint32_t)(cur_y + py) * s_draw_stride;
                for (int px = px_start; px < px_end; px++) {
                    bool lit = (px < FONT_W && py < FONT_H) &&
                               ((cols[px] & (uint8_t)(1u << py)) != 0u);
                    if (lit) {
                        row_base[cur_x + px] = fg;
                    } else if (draw_bg) {
                        row_base[cur_x + px] = bg;
                    }
                }
            }
        }

        cur_x += FONT_CELL_W;
    }
}

uint16_t *renderer_get_draw_fb(int *out_stride_px) {
    if (out_stride_px) {
        *out_stride_px = (int)s_draw_stride;
    }
    return s_draw_fb;
}

bool renderer_has_hw_tri(void) {
    return s_hw_tri;
}

void renderer_blit_gb_subrect(const uint16_t *lcd_rgb565,
                              int src_x, int src_y, int src_w, int src_h,
                              int dst_x, int dst_y) {
    if (!lcd_rgb565 || !s_draw_fb || src_w <= 0 || src_h <= 0) {
        return;
    }

    /* Clip against source Game Boy LCD bounds [0..160) x [0..144). */
    if (src_x < 0) {
        int delta = -src_x;
        src_x = 0;
        dst_x += delta;
        src_w -= delta;
    }
    if (src_y < 0) {
        int delta = -src_y;
        src_y = 0;
        dst_y += delta;
        src_h -= delta;
    }
    if (src_x >= GB_SRC_W || src_y >= GB_SRC_H) {
        return;
    }
    if (src_w > GB_SRC_W - src_x) {
        src_w = GB_SRC_W - src_x;
    }
    if (src_h > GB_SRC_H - src_y) {
        src_h = GB_SRC_H - src_y;
    }
    if (src_w <= 0 || src_h <= 0) {
        return;
    }

    /* Clip against destination framebuffer bounds [0..320) x [0..240). */
    if (dst_x < 0) {
        int delta = -dst_x;
        dst_x = 0;
        src_x += delta;
        src_w -= delta;
    }
    if (dst_y < 0) {
        int delta = -dst_y;
        dst_y = 0;
        src_y += delta;
        src_h -= delta;
    }
    if (dst_x >= RENDER_W || dst_y >= RENDER_H) {
        return;
    }
    if (src_w > RENDER_W - dst_x) {
        src_w = RENDER_W - dst_x;
    }
    if (src_h > RENDER_H - dst_y) {
        src_h = RENDER_H - dst_y;
    }
    if (src_w <= 0 || src_h <= 0) {
        return;
    }

    for (int y = 0; y < src_h; y++) {
        const uint16_t *src = lcd_rgb565 + (uint32_t)(src_y + y) * GB_SRC_W + src_x;
        uint16_t *dst = s_draw_fb + (uint32_t)(dst_y + y) * s_draw_stride + dst_x;
        memcpy(dst, src, (size_t)src_w * sizeof(uint16_t));
    }
}

void renderer_hw_tri_begin(const uint16_t *atlas_rgb565, int atlas_w, int atlas_h, uint16_t *zbuf_16) {
#ifndef OF_PC
    s_hw_tri_active = false;
    if (!s_hw_tri || !s_rgb565_mode || !s_draw_fb || !atlas_rgb565 || !zbuf_16 ||
        atlas_w <= 0 || atlas_h <= 0) {
        return;
    }

    of_cache_flush_range((void *)(uintptr_t)atlas_rgb565, (uint32_t)(atlas_w * atlas_h * 2));
    of_cache_flush_range((void *)zbuf_16, (uint32_t)(RENDER_W * RENDER_H * 2));
    of_cache_flush_range((void *)s_draw_fb, (uint32_t)(RENDER_W * RENDER_H * 2));

    of_gpu_tri_state_t st;
    memset(&st, 0, sizeof(st));
    st.fb_base       = (uint32_t)(uintptr_t)s_draw_fb;
    st.fb_major_step = (int32_t)s_fb_stride_bytes;
    st.fb_minor_step = 2;
    st.tex_addr      = (uint32_t)(uintptr_t)atlas_rgb565;
    st.tex_width     = (uint16_t)atlas_w;
    st.tex_w_mask    = (uint16_t)(atlas_w - 1);
    st.tex_h_mask    = (uint16_t)(atlas_h - 1);
    st.z_base        = (uint32_t)(uintptr_t)zbuf_16;
    st.z_major_step  = RENDER_W * 2;
    st.z_minor_step  = 2;
    st.z_mode        = OF_GPU_PARAM_Z_TEST_WRITE;
    st.flags         = OF_GPU_SPAN_TRUECOLOR | OF_GPU_SPAN_PERSP;
    st.clip_x0       = 0;
    st.clip_x1       = RENDER_W - 1;
    st.clip_y0       = 0;
    st.clip_y1       = RENDER_H - 1;

    of_gpu_set_tri_state(&st);
    s_hw_tri_active = true;
#else
    (void)atlas_rgb565;
    (void)atlas_w;
    (void)atlas_h;
    (void)zbuf_16;
#endif
}

void renderer_hw_tri_submit(const int16_t x[3], const int16_t y[3],
                            const int32_t s[3], const int32_t t[3],
                            const int32_t zi[3], const uint16_t rgb[3],
                            const int32_t depth[3]) {
#ifndef OF_PC
    if (!s_hw_tri_active) {
        return;
    }
    of_gpu_draw_vert_tri_rgb(x, y, s, t, zi, rgb, 0u, depth, NULL);
#else
    (void)x;
    (void)y;
    (void)s;
    (void)t;
    (void)zi;
    (void)rgb;
    (void)depth;
#endif
}

void renderer_hw_tri_end(void) {
#ifndef OF_PC
    if (!s_hw_tri_active) {
        return;
    }
    of_gpu_finish();
    of_cache_inval_range((void *)s_draw_fb, (uint32_t)(RENDER_W * RENDER_H * 2));
    s_hw_tri_active = false;
#endif
}
