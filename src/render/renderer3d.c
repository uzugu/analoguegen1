#include "renderer3d.h"
#include "renderer.h"

#include <math.h>
#include <stddef.h>
#include <string.h>

#define R3D_NEAR     4.0f
#define R3D_FAR      600.0f
#define R3D_DEG2RAD  0.01745329251994329577f

#define R3D_SW_W     160
#define R3D_SW_H     144

#define R3D_SHADOW_W 8
#define R3D_SHADOW_H 8
#define R3D_SPRITE_W 8
#define R3D_SPRITE_H 12

typedef uint32_t __attribute__((__may_alias__)) r3d_u32_t;

typedef struct {
    float x, y, z;
} r3d_vec3_t;

typedef struct {
    float sx, sy;
    float depth;
    float inv_w;
    float u, v;
} r3d_proj_vert_t;

typedef struct {
    int32_t x, u, v;
    int64_t z;
    int32_t dx, du, dv;
    int64_t dz;
} r3d_left_edge_t;

typedef struct {
    int32_t x;
    int32_t dx;
} r3d_right_edge_t;

/* 16-bit depth buffer for HW path (320x240, GEQUAL on 1/w depth, 0 = far). */
static uint16_t s_zbuf[RENDER_W * RENDER_H];

/* Half-res 160x120 software render target + linear Z-buffer (smaller = closer, 0xFFFF = far). */
static uint16_t s_sw_fb[R3D_SW_W * R3D_SW_H] __attribute__((aligned(4)));
static uint16_t s_sw_zbuf[R3D_SW_W * R3D_SW_H] __attribute__((aligned(4)));

static uint16_t *s_fb     = NULL;
static int       s_stride = RENDER_W;

/* Camera state, right-handed look-at view + perspective projection matrices. */
static r3d_camera_t s_cam;
static r3d_vec3_t   s_cam_pos;
static r3d_vec3_t   s_cam_right;
static r3d_vec3_t   s_cam_up;
static r3d_vec3_t   s_cam_forward;

static float s_view[4][4];
static float s_proj[4][4];

/* Rounded 8x8 ground contact shadow alpha mask (1 = opaque shadow texel, 0 = transparent). */
static const uint8_t s_shadow_mask[R3D_SHADOW_H][R3D_SHADOW_W] = {
    { 0, 0, 1, 1, 1, 1, 0, 0 },
    { 0, 1, 1, 1, 1, 1, 1, 0 },
    { 1, 1, 1, 1, 1, 1, 1, 1 },
    { 1, 1, 1, 1, 1, 1, 1, 1 },
    { 1, 1, 1, 1, 1, 1, 1, 1 },
    { 1, 1, 1, 1, 1, 1, 1, 1 },
    { 0, 1, 1, 1, 1, 1, 1, 0 },
    { 0, 0, 1, 1, 1, 1, 0, 0 }
};

/*
 * Stylized 8x12 pixel-art character sprite indices:
 *   0 = transparent
 *   1 = dark outline / eyes / boots
 *   2 = body_color (torso & sleeves)
 *   3 = accent_color (hat/head crest & chest emblem)
 *   4 = face / hands highlight
 */
static const uint8_t s_char_sprite[R3D_SPRITE_H][R3D_SPRITE_W] = {
    { 0, 0, 3, 3, 3, 3, 0, 0 },
    { 0, 3, 3, 3, 3, 3, 3, 0 },
    { 0, 3, 3, 3, 3, 3, 3, 0 },
    { 0, 4, 1, 4, 4, 1, 4, 0 },
    { 0, 4, 4, 4, 4, 4, 4, 0 },
    { 0, 2, 2, 2, 2, 2, 2, 0 },
    { 2, 2, 2, 3, 3, 2, 2, 2 },
    { 4, 2, 2, 2, 2, 2, 2, 4 },
    { 0, 2, 2, 2, 2, 2, 2, 0 },
    { 0, 1, 1, 0, 0, 1, 1, 0 },
    { 0, 1, 1, 0, 0, 1, 1, 0 },
    { 0, 1, 1, 0, 0, 1, 1, 0 }
};

static inline float r3d_dot3(r3d_vec3_t a, r3d_vec3_t b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

static inline uint16_t r3d_modulate_rgb565(uint16_t texel, uint32_t sr8, uint32_t sg8, uint32_t sb8) {
    uint32_t tr = (uint32_t)((texel >> 11) & 0x1Fu);
    uint32_t tg = (uint32_t)((texel >> 5)  & 0x3Fu);
    uint32_t tb = (uint32_t)( texel        & 0x1Fu);

    uint32_t r = (tr * sr8) >> 8;
    uint32_t g = (tg * sg8) >> 8;
    uint32_t b = (tb * sb8) >> 8;

    return (uint16_t)((r << 11) | (g << 5) | b);
}

static inline float r3d_edge_fn(float ax, float ay, float bx, float by, float px, float py) {
    return (px - ax) * (by - ay) - (py - ay) * (bx - ax);
}

static inline int32_t r3d_float_to_fx16(float val) {
    float s = val * 65536.0f;
    if (s >= 2147483520.0f) {
        return INT32_MAX;
    }
    if (s <= -2147483520.0f) {
        return INT32_MIN;
    }
    return (int32_t)s;
}

static inline int64_t r3d_float_to_fx16_64(float val) {
    float s = val * 16384.0f;
    if (s >= 2147483520.0f) {
        return (int64_t)INT32_MAX * 4;
    }
    if (s <= -2147483520.0f) {
        return (int64_t)INT32_MIN * 4;
    }
    return (int64_t)((int32_t)s) * 4;
}

static inline float r3d_vert_z(const r3d_proj_vert_t *p, bool use_sw) {
    float z = use_sw ? p->depth : (p->inv_w * 16384.0f);
    if (z < 1.0f) {
        z = 1.0f;
    } else if (z > 65534.0f) {
        z = 65534.0f;
    }
    return z;
}

static bool r3d_project_vert(const voxel_vert_t *in, r3d_proj_vert_t *out) {
    const float vx = s_view[0][0] * in->x + s_view[0][1] * in->y + s_view[0][2] * in->z + s_view[0][3];
    const float vy = s_view[1][0] * in->x + s_view[1][1] * in->y + s_view[1][2] * in->z + s_view[1][3];
    const float vz = s_view[2][0] * in->x + s_view[2][1] * in->y + s_view[2][2] * in->z + s_view[2][3];

    /* Trivially reject vertices on or behind the near plane (right-handed -Z forward). */
    if (vz >= -R3D_NEAR) {
        return false;
    }

    const float clip_x = s_proj[0][0] * vx;
    const float clip_y = s_proj[1][1] * vy;
    const float clip_w = s_proj[3][2] * vz; /* -vz > 0 */
    const float inv_w  = 1.0f / clip_w;

    const float ndc_x = clip_x * inv_w;
    const float ndc_y = clip_y * inv_w;

    const bool use_sw = !renderer_has_hw_tri();
    const float half_w = use_sw ? (0.5f * (float)R3D_SW_W) : (0.5f * (float)RENDER_W);
    const float half_h = use_sw ? (0.5f * (float)R3D_SW_H) : (0.5f * (float)RENDER_H);

    out->sx = (ndc_x + 1.0f) * half_w;
    out->sy = (1.0f - ndc_y) * half_h;

    float depth_f = (clip_w - R3D_NEAR) * (65535.0f / (R3D_FAR - R3D_NEAR));
    if (depth_f < 0.0f) {
        depth_f = 0.0f;
    } else if (depth_f > 65535.0f) {
        depth_f = 65535.0f;
    }
    out->depth = depth_f;
    out->inv_w = inv_w;
    out->u     = in->u;
    out->v     = in->v;
    return true;
}

static void r3d_setup_left_edge(r3d_left_edge_t *e,
                                const r3d_proj_vert_t *va,
                                const r3d_proj_vert_t *vb,
                                float za, float zb,
                                int y_clip_start) {
    const float fdy = vb->sy - va->sy;
    if (fdy > 1e-5f) {
        const float inv_fdy = 1.0f / fdy;
        const float dx_dy   = (vb->sx - va->sx) * inv_fdy;
        const float du_dy   = (vb->u  - va->u ) * inv_fdy;
        const float dv_dy   = (vb->v  - va->v ) * inv_fdy;
        const float dz_dy   = (zb     - za    ) * inv_fdy;
        const float pre     = ((float)y_clip_start + 0.5f) - va->sy;

        e->x  = r3d_float_to_fx16(va->sx + pre * dx_dy);
        e->u  = r3d_float_to_fx16(va->u  + pre * du_dy);
        e->v  = r3d_float_to_fx16(va->v  + pre * dv_dy);
        e->z  = r3d_float_to_fx16_64(za  + pre * dz_dy);
        e->dx = r3d_float_to_fx16(dx_dy);
        e->du = r3d_float_to_fx16(du_dy);
        e->dv = r3d_float_to_fx16(dv_dy);
        e->dz = r3d_float_to_fx16_64(dz_dy);
    } else {
        e->x  = r3d_float_to_fx16(0.5f * (va->sx + vb->sx));
        e->u  = r3d_float_to_fx16(0.5f * (va->u  + vb->u ));
        e->v  = r3d_float_to_fx16(0.5f * (va->v  + vb->v ));
        e->z  = r3d_float_to_fx16_64(0.5f * (za + zb));
        e->dx = 0;
        e->du = 0;
        e->dv = 0;
        e->dz = 0;
    }
}

static void r3d_setup_right_edge(r3d_right_edge_t *e,
                                 const r3d_proj_vert_t *va,
                                 const r3d_proj_vert_t *vb,
                                 int y_clip_start) {
    const float fdy = vb->sy - va->sy;
    if (fdy > 1e-5f) {
        const float dx_dy = (vb->sx - va->sx) / fdy;
        const float pre   = ((float)y_clip_start + 0.5f) - va->sy;
        e->x  = r3d_float_to_fx16(va->sx + pre * dx_dy);
        e->dx = r3d_float_to_fx16(dx_dy);
    } else {
        e->x  = r3d_float_to_fx16(0.5f * (va->sx + vb->sx));
        e->dx = 0;
    }
}

/* Fast integer span loop for SW path with power-of-2 texture, opaque, identity shade. */
static inline void r3d_span_sw_pot_id(uint16_t *fb_row, uint16_t *z_row,
                                      int xl, int xr,
                                      uint32_t z, int32_t dz_dx,
                                      int32_t u, int32_t du_dx,
                                      int32_t v, int32_t dv_dx,
                                      const uint16_t *atlas_rgb565,
                                      uint32_t u_mask, uint32_t v_mask,
                                      int w_shift) {
    for (int x = xl; x < xr; x++) {
        const uint16_t d = (uint16_t)(z >> 16);
        if (d < z_row[x]) {
            const uint32_t tx = (uint32_t)(u >> 16) & u_mask;
            const uint32_t ty = (uint32_t)(v >> 16) & v_mask;
            z_row[x]  = d;
            fb_row[x] = atlas_rgb565[(ty << w_shift) + tx];
        }
        z += (uint32_t)dz_dx;
        u += du_dx;
        v += dv_dx;
    }
}

/* Fast integer span loop for SW path with power-of-2 texture, opaque, modulated shade. */
static inline void r3d_span_sw_pot_mod(uint16_t *fb_row, uint16_t *z_row,
                                       int xl, int xr,
                                       uint32_t z, int32_t dz_dx,
                                       int32_t u, int32_t du_dx,
                                       int32_t v, int32_t dv_dx,
                                       const uint16_t *atlas_rgb565,
                                       uint32_t u_mask, uint32_t v_mask,
                                       int w_shift,
                                       uint32_t sr8, uint32_t sg8, uint32_t sb8) {
    for (int x = xl; x < xr; x++) {
        const uint16_t d = (uint16_t)(z >> 16);
        if (d < z_row[x]) {
            const uint32_t tx = (uint32_t)(u >> 16) & u_mask;
            const uint32_t ty = (uint32_t)(v >> 16) & v_mask;
            const uint16_t texel = atlas_rgb565[(ty << w_shift) + tx];
            z_row[x]  = d;
            fb_row[x] = r3d_modulate_rgb565(texel, sr8, sg8, sb8);
        }
        z += (uint32_t)dz_dx;
        u += du_dx;
        v += dv_dx;
    }
}

/* General integer span loop for alpha-keyed sprites, non-POT textures, untextured, or HW billboards. */
static inline void r3d_span_general(uint16_t *fb_row, uint16_t *z_row,
                                    int xl, int xr,
                                    uint32_t z, int32_t dz_dx,
                                    int32_t u, int32_t du_dx,
                                    int32_t v, int32_t dv_dx,
                                    bool use_sw, bool has_tex, bool pot_tex,
                                    bool alpha_key, bool identity_shade,
                                    const uint16_t *atlas_rgb565,
                                    int atlas_w, int atlas_h,
                                    uint32_t u_mask, uint32_t v_mask,
                                    uint16_t shade_rgb,
                                    uint32_t sr8, uint32_t sg8, uint32_t sb8) {
    for (int x = xl; x < xr; x++) {
        const uint16_t d = (uint16_t)(z >> 16);
        const bool z_pass = use_sw ? (d < z_row[x]) : (d >= z_row[x]);
        if (z_pass) {
            uint16_t out_col = shade_rgb;
            bool write_px = true;
            if (has_tex) {
                int tx, ty;
                if (pot_tex) {
                    tx = (int)((uint32_t)(u >> 16) & u_mask);
                    ty = (int)((uint32_t)(v >> 16) & v_mask);
                } else {
                    tx = u >> 16;
                    ty = v >> 16;
                    if (tx < 0) tx = 0;
                    else if (tx >= atlas_w) tx = atlas_w - 1;
                    if (ty < 0) ty = 0;
                    else if (ty >= atlas_h) ty = atlas_h - 1;
                }
                const uint16_t texel = atlas_rgb565[ty * atlas_w + tx];
                if (alpha_key && texel == 0u) {
                    write_px = false;
                } else {
                    out_col = identity_shade ? texel : r3d_modulate_rgb565(texel, sr8, sg8, sb8);
                }
            }
            if (write_px) {
                z_row[x]  = d;
                fb_row[x] = out_col;
            }
        }
        z += (uint32_t)dz_dx;
        u += du_dx;
        v += dv_dx;
    }
}

static void r3d_rasterize_tri(const r3d_proj_vert_t *p0,
                              const r3d_proj_vert_t *p1,
                              const r3d_proj_vert_t *p2,
                              uint16_t shade_rgb,
                              const uint16_t *atlas_rgb565,
                              int atlas_w,
                              int atlas_h,
                              bool alpha_key) {
    /*
     * 2D signed area in Y-down screen coordinates for 3D CCW outward triangles:
     * edge(p0, p1, p2) > 0 means front-facing; <= 0 means backfacing or degenerate.
     */
    const float area = r3d_edge_fn(p0->sx, p0->sy, p1->sx, p1->sy, p2->sx, p2->sy);
    if (area <= 1e-4f) {
        return;
    }

    const bool use_sw = !renderer_has_hw_tri();
    const int fb_w = use_sw ? R3D_SW_W : RENDER_W;
    const int fb_h = use_sw ? R3D_SW_H : RENDER_H;

    if ((p0->sx < 0.0f && p1->sx < 0.0f && p2->sx < 0.0f) ||
        (p0->sx >= (float)fb_w && p1->sx >= (float)fb_w && p2->sx >= (float)fb_w) ||
        (p0->sy < 0.0f && p1->sy < 0.0f && p2->sy < 0.0f) ||
        (p0->sy >= (float)fb_h && p1->sy >= (float)fb_h && p2->sy >= (float)fb_h)) {
        return;
    }

    const float zp0 = r3d_vert_z(p0, use_sw);
    const float zp1 = r3d_vert_z(p1, use_sw);
    const float zp2 = r3d_vert_z(p2, use_sw);

    /* Constant per-triangle horizontal gradients in 16.16 fixed-point. */
    const float inv_area = 1.0f / area;
    const float db0_dx = (p2->sy - p1->sy) * inv_area;
    const float db1_dx = (p0->sy - p2->sy) * inv_area;
    const float db2_dx = (p1->sy - p0->sy) * inv_area;

    const int32_t du_dx = r3d_float_to_fx16(p0->u * db0_dx + p1->u * db1_dx + p2->u * db2_dx);
    const int32_t dv_dx = r3d_float_to_fx16(p0->v * db0_dx + p1->v * db1_dx + p2->v * db2_dx);
    const int32_t dz_dx = r3d_float_to_fx16(zp0   * db0_dx + zp1   * db1_dx + zp2   * db2_dx);

    /* Sort projected vertices by integer scanline Y (breaking ties by sy). */
    const r3d_proj_vert_t *v0 = p0, *v1 = p1, *v2 = p2;
    float vz0 = zp0, vz1 = zp1, vz2 = zp2;
    if (v0->sy > v1->sy) {
        const r3d_proj_vert_t *tv = v0; v0 = v1; v1 = tv;
        float tz = vz0; vz0 = vz1; vz1 = tz;
    }
    if (v1->sy > v2->sy) {
        const r3d_proj_vert_t *tv = v1; v1 = v2; v2 = tv;
        float tz = vz1; vz1 = vz2; vz2 = tz;
    }
    if (v0->sy > v1->sy) {
        const r3d_proj_vert_t *tv = v0; v0 = v1; v1 = tv;
        float tz = vz0; vz0 = vz1; vz1 = tz;
    }

    const int y0 = (int)ceilf(v0->sy - 0.5f);
    const int y1 = (int)ceilf(v1->sy - 0.5f);
    const int y2 = (int)ceilf(v2->sy - 0.5f);
    if (y0 >= fb_h || y2 <= 0 || y0 >= y2) {
        return;
    }

    const bool mid_is_left = (r3d_edge_fn(v0->sx, v0->sy, v1->sx, v1->sy, v2->sx, v2->sy) > 0.0f);

    const bool has_tex = (atlas_rgb565 != NULL && atlas_w > 0 && atlas_h > 0);
    const bool pot_tex = has_tex &&
                         ((atlas_w & (atlas_w - 1)) == 0) &&
                         ((atlas_h & (atlas_h - 1)) == 0);
    const uint32_t u_mask = (uint32_t)(atlas_w - 1);
    const uint32_t v_mask = (uint32_t)(atlas_h - 1);
    int w_shift = 0;
    if (pot_tex) {
        int w = atlas_w;
        while (w > 1) {
            w >>= 1;
            w_shift++;
        }
    }

    const bool identity_shade = (shade_rgb == 0xFFFFu);
    const uint32_t sr8 = identity_shade ? 256u : ((((uint32_t)((shade_rgb >> 11) & 0x1Fu)) * 256u + 15u) / 31u);
    const uint32_t sg8 = identity_shade ? 256u : ((((uint32_t)((shade_rgb >> 5)  & 0x3Fu)) * 256u + 31u) / 63u);
    const uint32_t sb8 = identity_shade ? 256u : ((((uint32_t)( shade_rgb        & 0x1Fu)) * 256u + 15u) / 31u);
    const bool fast_sw_pot = (use_sw && has_tex && pot_tex && !alpha_key);

    uint16_t * const fb_base   = use_sw ? s_sw_fb   : s_fb;
    uint16_t * const zbuf_base = use_sw ? s_sw_zbuf : s_zbuf;
    const int        fb_stride = use_sw ? R3D_SW_W  : s_stride;
    const int        z_stride  = use_sw ? R3D_SW_W  : RENDER_W;

    r3d_left_edge_t  left;
    r3d_right_edge_t right;
    const int y_long_start = (y0 < 0) ? 0 : y0;
    if (mid_is_left) {
        r3d_setup_right_edge(&right, v0, v2, y_long_start);
    } else {
        r3d_setup_left_edge(&left, v0, v2, vz0, vz2, y_long_start);
    }

    for (int half = 0; half < 2; half++) {
        int ys, ye;
        if (half == 0) {
            ys = (y0 < 0) ? 0 : y0;
            ye = (y1 > fb_h) ? fb_h : y1;
            if (ys >= ye) {
                continue;
            }
            if (mid_is_left) {
                r3d_setup_left_edge(&left, v0, v1, vz0, vz1, ys);
            } else {
                r3d_setup_right_edge(&right, v0, v1, ys);
            }
        } else {
            ys = (y1 < 0) ? 0 : y1;
            ye = (y2 > fb_h) ? fb_h : y2;
            if (ys >= ye) {
                continue;
            }
            if (mid_is_left) {
                r3d_setup_left_edge(&left, v1, v2, vz1, vz2, ys);
            } else {
                r3d_setup_right_edge(&right, v1, v2, ys);
            }
        }

        uint16_t *fb_row = fb_base   + (uint32_t)ys * (uint32_t)fb_stride;
        uint16_t *z_row  = zbuf_base + (uint32_t)ys * (uint32_t)z_stride;

        for (int y = ys; y < ye; y++) {
            int xl = (left.x  >> 16) + (((left.x  & 0xFFFF) > 0x8000) ? 1 : 0);
            int xr = (right.x >> 16) + (((right.x & 0xFFFF) > 0x8000) ? 1 : 0);

            if (xl < 0) {
                xl = 0;
            }
            if (xr > fb_w) {
                xr = fb_w;
            }

            if (xl < xr) {
                const int32_t dx_sub = ((xl << 16) + 0x8000) - left.x;
                const int32_t u = left.u + (int32_t)(((int64_t)du_dx * dx_sub) >> 16);
                const int32_t v = left.v + (int32_t)(((int64_t)dv_dx * dx_sub) >> 16);
                int64_t z64 = left.z + (((int64_t)dz_dx * dx_sub) >> 16);
                if (z64 < 0x00010000LL) {
                    z64 = 0x00010000LL;
                } else if (z64 > 0xFFFE0000LL) {
                    z64 = 0xFFFE0000LL;
                }
                const uint32_t z = (uint32_t)z64;

                if (fast_sw_pot) {
                    if (identity_shade) {
                        r3d_span_sw_pot_id(fb_row, z_row, xl, xr, z, dz_dx, u, du_dx, v, dv_dx,
                                           atlas_rgb565, u_mask, v_mask, w_shift);
                    } else {
                        r3d_span_sw_pot_mod(fb_row, z_row, xl, xr, z, dz_dx, u, du_dx, v, dv_dx,
                                            atlas_rgb565, u_mask, v_mask, w_shift, sr8, sg8, sb8);
                    }
                } else {
                    r3d_span_general(fb_row, z_row, xl, xr, z, dz_dx, u, du_dx, v, dv_dx,
                                     use_sw, has_tex, pot_tex, alpha_key, identity_shade,
                                     atlas_rgb565, atlas_w, atlas_h, u_mask, v_mask,
                                     shade_rgb, sr8, sg8, sb8);
                }
            }

            left.x  += left.dx;
            left.u  += left.du;
            left.v  += left.dv;
            left.z  += left.dz;
            right.x += right.dx;
            fb_row  += fb_stride;
            z_row   += z_stride;
        }
    }
}

static void r3d_submit_tri(const voxel_tri_t *tri,
                           const uint16_t *atlas_rgb565,
                           int atlas_w,
                           int atlas_h,
                           bool alpha_key) {
    r3d_proj_vert_t p0, p1, p2;
    if (!r3d_project_vert(&tri->v[0], &p0) ||
        !r3d_project_vert(&tri->v[1], &p1) ||
        !r3d_project_vert(&tri->v[2], &p2)) {
        return;
    }

    if (p0.depth >= 65535.0f && p1.depth >= 65535.0f && p2.depth >= 65535.0f) {
        return;
    }

    r3d_rasterize_tri(&p0, &p1, &p2, tri->v[0].rgb, atlas_rgb565, atlas_w, atlas_h, alpha_key);
}

void r3d_init(void) {
    memset(s_zbuf, renderer_has_hw_tri() ? 0 : 0xFF, sizeof(s_zbuf));
    memset(s_sw_zbuf, 0xFF, sizeof(s_sw_zbuf));
    s_fb     = NULL;
    s_stride = RENDER_W;

    r3d_camera_t def_cam;
    def_cam.target_x  = 0.0f;
    def_cam.target_y  = 0.0f;
    def_cam.target_z  = 0.0f;
    def_cam.pitch_deg = 45.0f;
    def_cam.yaw_deg   = 0.0f;
    def_cam.distance  = 160.0f;
    def_cam.fov_y_deg = 45.0f;
    r3d_set_camera(&def_cam);
}

void r3d_begin_scene(uint16_t sky_color) {
    const bool hw_tri = renderer_has_hw_tri();

    int stride = RENDER_W;
    uint16_t *fb = renderer_get_draw_fb(&stride);
    s_fb     = fb;
    s_stride = (stride >= RENDER_W) ? stride : RENDER_W;

    if (!hw_tri) {
        /* SW tests < on linear depth (0xFFFF = far). */
        memset(s_sw_zbuf, 0xFF, sizeof(s_sw_zbuf));
    } else {
        /* Hardware Z_TEST_WRITE tests GEQUAL on 1/w depth (0 = far). */
        memset(s_zbuf, 0, sizeof(s_zbuf));
    }

    if (s_fb == NULL) {
        return;
    }

    const uint32_t base_r = (uint32_t)((sky_color >> 11) & 0x1Fu);
    const uint32_t base_g = (uint32_t)((sky_color >> 5)  & 0x3Fu);
    const uint32_t base_b = (uint32_t)( sky_color        & 0x1Fu);

    if (!hw_tri) {
        /* Fill half-res 160x120 SW buffer with sky gradient via 32-bit paired pixel writes. */
        for (int y = 0; y < R3D_SW_H; y++) {
            const uint32_t scale = 216u + (uint32_t)((y * 56) / (R3D_SW_H - 1));
            uint32_t r = (base_r * scale) >> 8;
            uint32_t g = (base_g * scale) >> 8;
            uint32_t b = (base_b * scale) >> 8;
            if (r > 31u) r = 31u;
            if (g > 63u) g = 63u;
            if (b > 31u) b = 31u;

            const uint16_t row_col = (uint16_t)((r << 11) | (g << 5) | b);
            const uint32_t p2 = ((uint32_t)row_col << 16) | row_col;
            r3d_u32_t *row32 = (r3d_u32_t *)(s_sw_fb + (uint32_t)y * R3D_SW_W);
            for (int x = 0; x < R3D_SW_W / 2; x++) {
                row32[x] = p2;
            }
        }
    } else {
        /* Subtle vertical atmospheric gradient on full 320x240 buffer for HW path. */
        for (int y = 0; y < RENDER_H; y++) {
            const uint32_t scale = 216u + (uint32_t)((y * 56) / (RENDER_H - 1));
            uint32_t r = (base_r * scale) >> 8;
            uint32_t g = (base_g * scale) >> 8;
            uint32_t b = (base_b * scale) >> 8;
            if (r > 31u) r = 31u;
            if (g > 63u) g = 63u;
            if (b > 31u) b = 31u;

            const uint16_t row_col = (uint16_t)((r << 11) | (g << 5) | b);
            uint16_t *row = s_fb + (uint32_t)y * (uint32_t)s_stride;
            for (int x = 0; x < RENDER_W; x++) {
                row[x] = row_col;
            }
        }
    }
}

void r3d_set_camera(const r3d_camera_t *cam) {
    if (cam == NULL) {
        return;
    }

    s_cam = *cam;
    if (s_cam.fov_y_deg <= 1.0f || s_cam.fov_y_deg >= 175.0f) {
        s_cam.fov_y_deg = 45.0f;
    }
    if (s_cam.distance < 1.0f) {
        s_cam.distance = 1.0f;
    }

    const float pitch_rad = s_cam.pitch_deg * R3D_DEG2RAD;
    const float yaw_rad   = s_cam.yaw_deg   * R3D_DEG2RAD;

    const float sp = sinf(pitch_rad);
    const float cp = cosf(pitch_rad);
    const float sy = sinf(yaw_rad);
    const float cy = cosf(yaw_rad);

    /* Orbit camera around target (+X east, +Y up, +Z south). */
    s_cam_pos.x = s_cam.target_x + s_cam.distance * sp * sy;
    s_cam_pos.y = s_cam.target_y + s_cam.distance * cp;
    s_cam_pos.z = s_cam.target_z + s_cam.distance * sp * cy;

    /* Analytic orthonormal right-handed basis (gimbal-lock free even at pitch_deg == 0). */
    s_cam_forward.x = -sp * sy;
    s_cam_forward.y = -cp;
    s_cam_forward.z = -sp * cy;

    s_cam_right.x =  cy;
    s_cam_right.y =  0.0f;
    s_cam_right.z = -sy;

    s_cam_up.x = -cp * sy;
    s_cam_up.y =  sp;
    s_cam_up.z = -cp * cy;

    /* Right-handed look-at view matrix (camera looks along -Z). */
    s_view[0][0] =  s_cam_right.x;
    s_view[0][1] =  s_cam_right.y;
    s_view[0][2] =  s_cam_right.z;
    s_view[0][3] = -r3d_dot3(s_cam_right, s_cam_pos);

    s_view[1][0] =  s_cam_up.x;
    s_view[1][1] =  s_cam_up.y;
    s_view[1][2] =  s_cam_up.z;
    s_view[1][3] = -r3d_dot3(s_cam_up, s_cam_pos);

    s_view[2][0] = -s_cam_forward.x;
    s_view[2][1] = -s_cam_forward.y;
    s_view[2][2] = -s_cam_forward.z;
    s_view[2][3] =  r3d_dot3(s_cam_forward, s_cam_pos);

    s_view[3][0] = 0.0f;
    s_view[3][1] = 0.0f;
    s_view[3][2] = 0.0f;
    s_view[3][3] = 1.0f;

    /* Right-handed perspective projection matrix. */
    const float aspect = (float)RENDER_W / (float)RENDER_H;
    const float f      = 1.0f / tanf(0.5f * s_cam.fov_y_deg * R3D_DEG2RAD);

    memset(s_proj, 0, sizeof(s_proj));
    s_proj[0][0] = f / aspect;
    s_proj[1][1] = f;
    s_proj[2][2] = -(R3D_FAR + R3D_NEAR) / (R3D_FAR - R3D_NEAR);
    s_proj[2][3] = -(2.0f * R3D_FAR * R3D_NEAR) / (R3D_FAR - R3D_NEAR);
    s_proj[3][2] = -1.0f;
}

void r3d_draw_mesh(const voxel_mesh_t *mesh, const uint16_t *atlas_rgb565, int atlas_w, int atlas_h) {
    if (s_fb == NULL || mesh == NULL || mesh->tri_count <= 0) {
        return;
    }

    int count = mesh->tri_count;
    if (count > VOXEL_MAX_TRIS) {
        count = VOXEL_MAX_TRIS;
    }

    if (renderer_has_hw_tri() && atlas_rgb565 != NULL && atlas_w > 0 && atlas_h > 0) {
        renderer_hw_tri_begin(atlas_rgb565, atlas_w, atlas_h, s_zbuf);

        for (int i = 0; i < count; i++) {
            const voxel_tri_t *tri = &mesh->tris[i];
            float sx[3], sy[3], vz[3];
            bool visible = true;

            for (int k = 0; k < 3; k++) {
                const voxel_vert_t *in = &tri->v[k];
                const float vx = s_view[0][0] * in->x + s_view[0][1] * in->y + s_view[0][2] * in->z + s_view[0][3];
                const float vy = s_view[1][0] * in->x + s_view[1][1] * in->y + s_view[1][2] * in->z + s_view[1][3];
                const float vzk = s_view[2][0] * in->x + s_view[2][1] * in->y + s_view[2][2] * in->z + s_view[2][3];

                if (vzk >= -R3D_NEAR) {
                    visible = false;
                    break;
                }

                const float inv_w = 1.0f / (s_proj[3][2] * vzk);
                const float ndc_x = (s_proj[0][0] * vx) * inv_w;
                const float ndc_y = (s_proj[1][1] * vy) * inv_w;

                sx[k] = (ndc_x + 1.0f) * (0.5f * (float)RENDER_W);
                sy[k] = (1.0f - ndc_y) * (0.5f * (float)RENDER_H);
                vz[k] = vzk;
            }

            if (!visible) {
                continue;
            }
            if (-vz[0] >= R3D_FAR && -vz[1] >= R3D_FAR && -vz[2] >= R3D_FAR) {
                continue;
            }
            if (r3d_edge_fn(sx[0], sy[0], sx[1], sy[1], sx[2], sy[2]) <= 1e-4f) {
                continue;
            }
            if ((sx[0] < 0.0f && sx[1] < 0.0f && sx[2] < 0.0f) ||
                (sx[0] >= (float)RENDER_W && sx[1] >= (float)RENDER_W && sx[2] >= (float)RENDER_W) ||
                (sy[0] < 0.0f && sy[1] < 0.0f && sy[2] < 0.0f) ||
                (sy[0] >= (float)RENDER_H && sy[1] >= (float)RENDER_H && sy[2] >= (float)RENDER_H)) {
                continue;
            }

            int16_t  x[3], y[3];
            int32_t  s[3], t[3], zi[3], depth[3];
            uint16_t rgb[3];

            for (int k = 0; k < 3; k++) {
                float sxc = sx[k];
                float syc = sy[k];
                if (sxc < -2047.0f) sxc = -2047.0f;
                else if (sxc > 2047.0f) sxc = 2047.0f;
                if (syc < -2047.0f) syc = -2047.0f;
                else if (syc > 2047.0f) syc = 2047.0f;

                const float inv_z = 1.0f / (-vz[k]);
                x[k]     = (int16_t)(sxc * 16.0f);
                y[k]     = (int16_t)syc;
                s[k]     = (int32_t)(tri->v[k].u * 65536.0f);
                t[k]     = (int32_t)(tri->v[k].v * 65536.0f);
                zi[k]    = (int32_t)(inv_z * 65536.0f);
                depth[k] = (int32_t)(inv_z * 1073741824.0f);
                rgb[k]   = tri->v[k].rgb;
            }

            renderer_hw_tri_submit(x, y, s, t, zi, rgb, depth);
        }

        renderer_hw_tri_end();
        return;
    }

    for (int i = 0; i < count; i++) {
        r3d_submit_tri(&mesh->tris[i], atlas_rgb565, atlas_w, atlas_h, false);
    }
}

void r3d_draw_sprite_billboard(float wx, float wy, float wz, float width, float height,
                               const uint16_t *tex_rgb565, int tex_w, int tex_h, bool draw_shadow) {
    if (s_fb == NULL || width <= 0.0f || height <= 0.0f ||
        tex_rgb565 == NULL || tex_w <= 0 || tex_h <= 0) {
        return;
    }

    /* 1. Optional rounded dark ground shadow quad at (wx, wy + 0.25f, wz). */
    if (draw_shadow) {
        uint16_t shadow_tex[R3D_SHADOW_H * R3D_SHADOW_W];
        const uint16_t shadow_col = RGB565(18, 22, 32);

        for (int r = 0; r < R3D_SHADOW_H; r++) {
            for (int c = 0; c < R3D_SHADOW_W; c++) {
                shadow_tex[r * R3D_SHADOW_W + c] = s_shadow_mask[r][c] ? shadow_col : 0u;
            }
        }

        const float hw = width * 0.42f;
        const float hd = width * 0.36f;
        const float sy_ground = wy + 0.25f;

        /* CCW outward (+Y normal) ground quad: NW -> SW -> SE -> NE. */
        const float su_min = 0.01f;
        const float sv_min = 0.01f;
        const float su_max = (float)R3D_SHADOW_W - 0.01f;
        const float sv_max = (float)R3D_SHADOW_H - 0.01f;
        voxel_tri_t s0 = {{
            { wx - hw, sy_ground, wz - hd, su_min, sv_min, 0xFFFFu },
            { wx - hw, sy_ground, wz + hd, su_min, sv_max, 0xFFFFu },
            { wx + hw, sy_ground, wz + hd, su_max, sv_max, 0xFFFFu }
        }};
        voxel_tri_t s1 = {{
            { wx - hw, sy_ground, wz - hd, su_min, sv_min, 0xFFFFu },
            { wx + hw, sy_ground, wz + hd, su_max, sv_max, 0xFFFFu },
            { wx + hw, sy_ground, wz - hd, su_max, sv_min, 0xFFFFu }
        }};

        r3d_submit_tri(&s0, shadow_tex, R3D_SHADOW_W, R3D_SHADOW_H, true);
        r3d_submit_tri(&s1, shadow_tex, R3D_SHADOW_W, R3D_SHADOW_H, true);
    }

    /*
     * 2. Camera-facing billboard quad: use pure world +Y vertical axis at diorama
     * pitches (>= 30 deg) so sprites standing directly in front of buildings/trees
     * do not lean into walls, and smoothly tilt toward camera up at shallow
     * top-down pitches (< 30 deg) so the billboard stays clearly visible even at
     * pitch_deg == 0.
     */
    float up_x = 0.0f;
    float up_y = 1.0f;
    float up_z = 0.0f;
    const float abs_pitch = fabsf(s_cam.pitch_deg);
    if (abs_pitch < 30.0f) {
        const float t = (30.0f - abs_pitch) / 30.0f;
        up_x = s_cam_up.x * t;
        up_y = (1.0f - t) + s_cam_up.y * t;
        up_z = s_cam_up.z * t;
        const float len = sqrtf(up_x * up_x + up_y * up_y + up_z * up_z);
        if (len > 1e-5f) {
            const float inv_len = 1.0f / len;
            up_x *= inv_len;
            up_y *= inv_len;
            up_z *= inv_len;
        }
    }

    const float hw = width * 0.5f;
    const float rx = s_cam_right.x * hw;
    const float rz = s_cam_right.z * hw;
    const float hx = up_x * height;
    const float hy = up_y * height;
    const float hz = up_z * height;
    const float u_min = 0.01f;
    const float v_min = 0.01f;
    const float u_max = (float)tex_w - 0.01f;
    const float v_max = (float)tex_h - 0.01f;

    /* Quad corners: TL -> BL -> BR -> TR (CCW facing camera). */
    const voxel_vert_t v_tl = { wx - rx + hx, wy + hy, wz - rz + hz, u_min, v_min, 0xFFFFu };
    const voxel_vert_t v_bl = { wx - rx,      wy,      wz - rz,      u_min, v_max, 0xFFFFu };
    const voxel_vert_t v_br = { wx + rx,      wy,      wz + rz,      u_max, v_max, 0xFFFFu };
    const voxel_vert_t v_tr = { wx + rx + hx, wy + hy, wz + rz + hz, u_max, v_min, 0xFFFFu };

    voxel_tri_t t0 = {{ v_tl, v_bl, v_br }};
    voxel_tri_t t1 = {{ v_tl, v_br, v_tr }};

    r3d_submit_tri(&t0, tex_rgb565, tex_w, tex_h, true);
    r3d_submit_tri(&t1, tex_rgb565, tex_w, tex_h, true);
}

void r3d_draw_billboard(float wx, float wy, float wz, float width, float height,
                        uint16_t body_color, uint16_t accent_color, bool draw_shadow) {
    if (s_fb == NULL || width <= 0.0f || height <= 0.0f) {
        return;
    }

    /* Build stylized 8x12 RGB565 character texture (0 = transparent alpha key). */
    const uint16_t outline_col = RGB565(32, 36, 48);
    const uint16_t face_col    = RGB565(244, 214, 184);
    const uint16_t safe_body   = (body_color   != 0u) ? body_color   : RGB565(16, 16, 24);
    const uint16_t safe_accent = (accent_color != 0u) ? accent_color : RGB565(16, 16, 24);

    uint16_t sprite_tex[R3D_SPRITE_H * R3D_SPRITE_W];
    for (int r = 0; r < R3D_SPRITE_H; r++) {
        for (int c = 0; c < R3D_SPRITE_W; c++) {
            uint16_t col = 0u;
            switch (s_char_sprite[r][c]) {
            case 1: col = outline_col; break;
            case 2: col = safe_body;   break;
            case 3: col = safe_accent; break;
            case 4: col = face_col;    break;
            default: col = 0u;         break;
            }
            sprite_tex[r * R3D_SPRITE_W + c] = col;
        }
    }

    r3d_draw_sprite_billboard(wx, wy, wz, width, height,
                              sprite_tex, R3D_SPRITE_W, R3D_SPRITE_H, draw_shadow);
}

void r3d_end_scene(void) {
    if (!renderer_has_hw_tri() && s_fb != NULL) {
        if ((((uintptr_t)s_fb & 3u) == 0u) && ((s_stride & 1) == 0)) {
            for (int y = 0; y < R3D_SW_H; y++) {
                const uint16_t *src = s_sw_fb + (uint32_t)y * R3D_SW_W;
                r3d_u32_t *dst0 = (r3d_u32_t *)(s_fb + (uint32_t)(y * 2)     * (uint32_t)s_stride);
                r3d_u32_t *dst1 = (r3d_u32_t *)(s_fb + (uint32_t)(y * 2 + 1) * (uint32_t)s_stride);
                for (int x = 0; x < R3D_SW_W; x++) {
                    const uint16_t c  = src[x];
                    const uint32_t p2 = ((uint32_t)c << 16) | c;
                    dst0[x] = p2;
                    dst1[x] = p2;
                }
            }
        } else {
            for (int y = 0; y < R3D_SW_H; y++) {
                const uint16_t *src = s_sw_fb + (uint32_t)y * R3D_SW_W;
                uint16_t *dst0 = s_fb + (uint32_t)(y * 2)     * (uint32_t)s_stride;
                uint16_t *dst1 = s_fb + (uint32_t)(y * 2 + 1) * (uint32_t)s_stride;
                for (int x = 0; x < R3D_SW_W; x++) {
                    const uint16_t c = src[x];
                    const int dx = x * 2;
                    dst0[dx]     = c;
                    dst0[dx + 1] = c;
                    dst1[dx]     = c;
                    dst1[dx + 1] = c;
                }
            }
        }
    }
    s_fb = NULL;
}
