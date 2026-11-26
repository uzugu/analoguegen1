#include "renderer3d.h"
#include "renderer.h"

#include <math.h>
#include <stddef.h>
#include <string.h>

#define R3D_NEAR     4.0f
#define R3D_FAR      600.0f
#define R3D_DEG2RAD  0.01745329251994329577f

#define R3D_SHADOW_W 8
#define R3D_SHADOW_H 8
#define R3D_SPRITE_W 8
#define R3D_SPRITE_H 12

typedef struct {
    float x, y, z;
} r3d_vec3_t;

typedef struct {
    float sx, sy;
    float depth;
    float u, v;
} r3d_proj_vert_t;

/* 16-bit depth buffer (smaller = closer, 0xFFFF = far clear). */
static uint16_t s_zbuf[RENDER_W * RENDER_H];

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

    out->sx = (ndc_x + 1.0f) * (0.5f * (float)RENDER_W);
    out->sy = (1.0f - ndc_y) * (0.5f * (float)RENDER_H);

    float depth_f = (clip_w - R3D_NEAR) * (65535.0f / (R3D_FAR - R3D_NEAR));
    if (depth_f < 0.0f) {
        depth_f = 0.0f;
    } else if (depth_f > 65535.0f) {
        depth_f = 65535.0f;
    }
    out->depth = depth_f;
    out->u     = in->u;
    out->v     = in->v;
    return true;
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

    float min_xf = p0->sx;
    if (p1->sx < min_xf) min_xf = p1->sx;
    if (p2->sx < min_xf) min_xf = p2->sx;

    float max_xf = p0->sx;
    if (p1->sx > max_xf) max_xf = p1->sx;
    if (p2->sx > max_xf) max_xf = p2->sx;

    float min_yf = p0->sy;
    if (p1->sy < min_yf) min_yf = p1->sy;
    if (p2->sy < min_yf) min_yf = p2->sy;

    float max_yf = p0->sy;
    if (p1->sy > max_yf) max_yf = p1->sy;
    if (p2->sy > max_yf) max_yf = p2->sy;

    int min_x = (int)floorf(min_xf);
    int max_x = (int)ceilf(max_xf);
    int min_y = (int)floorf(min_yf);
    int max_y = (int)ceilf(max_yf);

    if (min_x < 0) min_x = 0;
    if (min_y < 0) min_y = 0;
    if (max_x >= RENDER_W) max_x = RENDER_W - 1;
    if (max_y >= RENDER_H) max_y = RENDER_H - 1;

    if (min_x > max_x || min_y > max_y) {
        return;
    }

    const float inv_area = 1.0f / area;

    /* Normalized barycentric steps per pixel (+1 X, +1 Y). */
    const float db0_dx = (p2->sy - p1->sy) * inv_area;
    const float db0_dy = (p1->sx - p2->sx) * inv_area;
    const float db1_dx = (p0->sy - p2->sy) * inv_area;
    const float db1_dy = (p2->sx - p0->sx) * inv_area;
    const float db2_dx = (p1->sy - p0->sy) * inv_area;
    const float db2_dy = (p0->sx - p1->sx) * inv_area;

    const float start_px = (float)min_x + 0.5f;
    const float start_py = (float)min_y + 0.5f;

    float b0_row = r3d_edge_fn(p1->sx, p1->sy, p2->sx, p2->sy, start_px, start_py) * inv_area;
    float b1_row = r3d_edge_fn(p2->sx, p2->sy, p0->sx, p0->sy, start_px, start_py) * inv_area;
    float b2_row = r3d_edge_fn(p0->sx, p0->sy, p1->sx, p1->sy, start_px, start_py) * inv_area;

    const bool has_tex = (atlas_rgb565 != NULL && atlas_w > 0 && atlas_h > 0);
    const bool identity_shade = (shade_rgb == 0xFFFFu);
    const uint32_t sr8 = (((uint32_t)((shade_rgb >> 11) & 0x1Fu)) * 256u + 15u) / 31u;
    const uint32_t sg8 = (((uint32_t)((shade_rgb >> 5)  & 0x3Fu)) * 256u + 31u) / 63u;
    const uint32_t sb8 = (((uint32_t)( shade_rgb        & 0x1Fu)) * 256u + 15u) / 31u;

    const float edge_eps = -1e-4f;
    const float z0 = p0->depth;
    const float z1 = p1->depth;
    const float z2 = p2->depth;
    const float u0 = p0->u;
    const float u1 = p1->u;
    const float u2 = p2->u;
    const float v0 = p0->v;
    const float v1 = p1->v;
    const float v2 = p2->v;

    const bool use_sw_z = !renderer_has_hw_tri();

    for (int y = min_y; y <= max_y; y++) {
        float b0 = b0_row;
        float b1 = b1_row;
        float b2 = b2_row;

        uint16_t *fb_row = s_fb + (uint32_t)y * (uint32_t)s_stride;
        uint16_t *z_row  = s_zbuf + (uint32_t)y * (uint32_t)RENDER_W;
        bool entered = false;

        for (int x = min_x; x <= max_x; x++) {
            if (b0 >= edge_eps && b1 >= edge_eps && b2 >= edge_eps) {
                entered = true;

                float zf = b0 * z0 + b1 * z1 + b2 * z2;
                if (zf < 0.0f) {
                    zf = 0.0f;
                } else if (zf > 65535.0f) {
                    zf = 65535.0f;
                }
                const uint16_t depth = (uint16_t)(zf + 0.5f);

                if (!use_sw_z || depth < z_row[x]) {
                    uint16_t out_col = shade_rgb;

                    if (has_tex) {
                        int tx = (int)(b0 * u0 + b1 * u1 + b2 * u2);
                        int ty = (int)(b0 * v0 + b1 * v1 + b2 * v2);
                        if (tx < 0) tx = 0;
                        else if (tx >= atlas_w) tx = atlas_w - 1;
                        if (ty < 0) ty = 0;
                        else if (ty >= atlas_h) ty = atlas_h - 1;

                        const uint16_t texel = atlas_rgb565[ty * atlas_w + tx];
                        if (alpha_key && texel == 0u) {
                            b0 += db0_dx;
                            b1 += db1_dx;
                            b2 += db2_dx;
                            continue;
                        }
                        out_col = identity_shade ? texel : r3d_modulate_rgb565(texel, sr8, sg8, sb8);
                    }

                    if (use_sw_z) {
                        z_row[x] = depth;
                    }
                    fb_row[x] = out_col;
                }
            } else if (entered) {
                /* Triangles are convex; once we exit along this scanline, we cannot re-enter. */
                break;
            }

            b0 += db0_dx;
            b1 += db1_dx;
            b2 += db2_dx;
        }

        b0_row += db0_dy;
        b1_row += db1_dy;
        b2_row += db2_dy;
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
    memset(s_zbuf, 0xFF, sizeof(s_zbuf));
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
    /* Hardware Z_TEST_WRITE tests GEQUAL on 1/w depth (0 = far); SW tests < on linear depth (0xFFFF = far). */
    memset(s_zbuf, renderer_has_hw_tri() ? 0 : 0xFF, sizeof(s_zbuf));

    int stride = RENDER_W;
    uint16_t *fb = renderer_get_draw_fb(&stride);
    s_fb     = fb;
    s_stride = (stride >= RENDER_W) ? stride : RENDER_W;

    if (s_fb == NULL) {
        return;
    }

    const uint32_t base_r = (uint32_t)((sky_color >> 11) & 0x1Fu);
    const uint32_t base_g = (uint32_t)((sky_color >> 5)  & 0x3Fu);
    const uint32_t base_b = (uint32_t)( sky_color        & 0x1Fu);

    /* Subtle vertical atmospheric gradient from slightly deeper zenith to horizon sky_color. */
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

void r3d_draw_billboard(float wx, float wy, float wz, float width, float height,
                        uint16_t body_color, uint16_t accent_color, bool draw_shadow) {
    if (s_fb == NULL || width <= 0.0f || height <= 0.0f) {
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
        voxel_tri_t s0 = {{
            { wx - hw, sy_ground, wz - hd, 0.0f,               0.0f,               0xFFFFu },
            { wx - hw, sy_ground, wz + hd, 0.0f,               (float)R3D_SHADOW_H, 0xFFFFu },
            { wx + hw, sy_ground, wz + hd, (float)R3D_SHADOW_W, (float)R3D_SHADOW_H, 0xFFFFu }
        }};
        voxel_tri_t s1 = {{
            { wx - hw, sy_ground, wz - hd, 0.0f,               0.0f,               0xFFFFu },
            { wx + hw, sy_ground, wz + hd, (float)R3D_SHADOW_W, (float)R3D_SHADOW_H, 0xFFFFu },
            { wx + hw, sy_ground, wz - hd, (float)R3D_SHADOW_W, 0.0f,               0xFFFFu }
        }};

        r3d_submit_tri(&s0, shadow_tex, R3D_SHADOW_W, R3D_SHADOW_H, true);
        r3d_submit_tri(&s1, shadow_tex, R3D_SHADOW_W, R3D_SHADOW_H, true);
    }

    /* 2. Build stylized 8x12 RGB565 character texture (0 = transparent alpha key). */
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

    /*
     * Use pure world +Y vertical axis at diorama pitches (>= 30 deg) so sprites
     * standing directly in front of buildings/trees do not lean into walls, and
     * smoothly tilt toward camera up at shallow top-down pitches (< 30 deg) so
     * the billboard stays clearly visible even at pitch_deg == 0.
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

    /* Quad corners: TL -> BL -> BR -> TR (CCW facing camera). */
    const voxel_vert_t v_tl = { wx - rx + hx, wy + hy, wz - rz + hz, 0.0f,               0.0f,               0xFFFFu };
    const voxel_vert_t v_bl = { wx - rx,      wy,      wz - rz,      0.0f,               (float)R3D_SPRITE_H, 0xFFFFu };
    const voxel_vert_t v_br = { wx + rx,      wy,      wz + rz,      (float)R3D_SPRITE_W, (float)R3D_SPRITE_H, 0xFFFFu };
    const voxel_vert_t v_tr = { wx + rx + hx, wy + hy, wz + rz + hz, (float)R3D_SPRITE_W, 0.0f,               0xFFFFu };

    voxel_tri_t t0 = {{ v_tl, v_bl, v_br }};
    voxel_tri_t t1 = {{ v_tl, v_br, v_tr }};

    r3d_submit_tri(&t0, sprite_tex, R3D_SPRITE_W, R3D_SPRITE_H, true);
    r3d_submit_tri(&t1, sprite_tex, R3D_SPRITE_W, R3D_SPRITE_H, true);
}

void r3d_end_scene(void) {
    s_fb = NULL;
}
