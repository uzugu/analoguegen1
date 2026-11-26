#include "voxel_mesher.h"
#include "renderer.h"

#include <stddef.h>

#define VOXEL_TILE_SIZE 8.0f
#define VOXEL_UV_INSET  0.15f
#define VOXEL_UV_SPAN   7.70f

/* Dramatic Shape directional face shading multipliers */
#define FACE_SHADE_TOP    1.00f
#define FACE_SHADE_SOUTH  0.90f
#define FACE_SHADE_EAST   0.84f
#define FACE_SHADE_WEST   0.72f
#define FACE_SHADE_NORTH  0.68f

static float tile_height(uint8_t cls) {
    switch (cls) {
    case TILE_CLASS_WATER:  return -2.0f;
    case TILE_CLASS_GROUND: return  0.0f;
    case TILE_CLASS_LEDGE:  return  6.0f;
    case TILE_CLASS_SOLID:  return 14.0f;
    case TILE_CLASS_TALL:   return 26.0f;
    default:                return  0.0f;
    }
}

static float neighbor_height(const gb_map_view_t *map, int vx, int vy) {
    if (vx < 0 || vx >= GB_MAP_VIEW_W || vy < 0 || vy >= GB_MAP_VIEW_H) {
        return 0.0f;
    }
    return tile_height(map->tile_class[vy][vx]);
}

static uint16_t shade_rgb565(uint8_t cls, float shade) {
    float tr = 1.0f;
    float tg = 1.0f;
    float tb = 1.0f;

    if (cls == TILE_CLASS_WATER) {
        /* Subtle soft sky-blue tint for water */
        tr = 0.82f;
        tg = 0.92f;
        tb = 1.00f;
    } else if (cls == TILE_CLASS_TALL) {
        /* Subtle warm tint for tall structures/roofs */
        tr = 1.00f;
        tg = 0.96f;
        tb = 0.90f;
    }

    int r = (int)(shade * tr * 255.0f + 0.5f);
    int g = (int)(shade * tg * 255.0f + 0.5f);
    int b = (int)(shade * tb * 255.0f + 0.5f);

    if (r < 0) r = 0; else if (r > 255) r = 255;
    if (g < 0) g = 0; else if (g > 255) g = 255;
    if (b < 0) b = 0; else if (b > 255) b = 255;

    return RGB565((uint8_t)r, (uint8_t)g, (uint8_t)b);
}

static voxel_vert_t make_vert(float x, float y, float z, float u, float v, uint16_t rgb) {
    voxel_vert_t vert;
    vert.x   = x;
    vert.y   = y;
    vert.z   = z;
    vert.u   = u;
    vert.v   = v;
    vert.rgb = rgb;
    return vert;
}

/* Emit a quad (tl -> bl -> br -> tr) as two CCW triangles. */
static void emit_quad(voxel_mesh_t *out,
                      voxel_vert_t tl, voxel_vert_t bl,
                      voxel_vert_t br, voxel_vert_t tr) {
    if (out->tri_count + 2 > VOXEL_MAX_TRIS) {
        return;
    }

    voxel_tri_t *t0 = &out->tris[out->tri_count++];
    t0->v[0] = tl;
    t0->v[1] = bl;
    t0->v[2] = br;

    voxel_tri_t *t1 = &out->tris[out->tri_count++];
    t1->v[0] = tl;
    t1->v[1] = br;
    t1->v[2] = tr;
}

void voxel_mesher_build(const gb_map_view_t *map, const app_state_t *state, voxel_mesh_t *out) {
    (void)state;

    if (out == NULL) {
        return;
    }
    out->tri_count = 0;

    if (map == NULL || !map->valid) {
        return;
    }

    for (int vy = 0; vy < GB_MAP_VIEW_H; vy++) {
        for (int vx = 0; vx < GB_MAP_VIEW_W; vx++) {
            const uint8_t cls = map->tile_class[vy][vx];
            const float h = tile_height(cls);

            const float x0 = (float)vx * VOXEL_TILE_SIZE;
            const float x1 = x0 + VOXEL_TILE_SIZE;
            const float z0 = (float)vy * VOXEL_TILE_SIZE;
            const float z1 = z0 + VOXEL_TILE_SIZE;

            const uint8_t tid = (uint8_t)(map->tile_id[vy][vx] & 0x7Fu);
            const float u0 = (float)(tid & 15u) * 8.0f + VOXEL_UV_INSET;
            const float v0 = (float)(tid >> 4)  * 8.0f + VOXEL_UV_INSET;
            const float u1 = u0 + VOXEL_UV_SPAN;
            const float v1 = v0 + VOXEL_UV_SPAN;

            /* Top face (+Y): always emitted, CCW viewed from +Y */
            {
                const uint16_t c_top = shade_rgb565(cls, FACE_SHADE_TOP);
                emit_quad(out,
                          make_vert(x0, h, z0, u0, v0, c_top),
                          make_vert(x0, h, z1, u0, v1, c_top),
                          make_vert(x1, h, z1, u1, v1, c_top),
                          make_vert(x1, h, z0, u1, v0, c_top));
            }

            /* South face (+Z): visible if this tile stands higher than vy + 1 */
            {
                const float nh = neighbor_height(map, vx, vy + 1);
                if (h > nh) {
                    const uint16_t c_south = shade_rgb565(cls, FACE_SHADE_SOUTH);
                    emit_quad(out,
                              make_vert(x0, h,  z1, u0, v0, c_south),
                              make_vert(x0, nh, z1, u0, v1, c_south),
                              make_vert(x1, nh, z1, u1, v1, c_south),
                              make_vert(x1, h,  z1, u1, v0, c_south));
                }
            }

            /* North face (-Z): visible if this tile stands higher than vy - 1 */
            {
                const float nh = neighbor_height(map, vx, vy - 1);
                if (h > nh) {
                    const uint16_t c_north = shade_rgb565(cls, FACE_SHADE_NORTH);
                    emit_quad(out,
                              make_vert(x1, h,  z0, u0, v0, c_north),
                              make_vert(x1, nh, z0, u0, v1, c_north),
                              make_vert(x0, nh, z0, u1, v1, c_north),
                              make_vert(x0, h,  z0, u1, v0, c_north));
                }
            }

            /* East face (+X): visible if this tile stands higher than vx + 1 */
            {
                const float nh = neighbor_height(map, vx + 1, vy);
                if (h > nh) {
                    const uint16_t c_east = shade_rgb565(cls, FACE_SHADE_EAST);
                    emit_quad(out,
                              make_vert(x1, h,  z1, u0, v0, c_east),
                              make_vert(x1, nh, z1, u0, v1, c_east),
                              make_vert(x1, nh, z0, u1, v1, c_east),
                              make_vert(x1, h,  z0, u1, v0, c_east));
                }
            }

            /* West face (-X): visible if this tile stands higher than vx - 1 */
            {
                const float nh = neighbor_height(map, vx - 1, vy);
                if (h > nh) {
                    const uint16_t c_west = shade_rgb565(cls, FACE_SHADE_WEST);
                    emit_quad(out,
                              make_vert(x0, h,  z0, u0, v0, c_west),
                              make_vert(x0, nh, z0, u0, v1, c_west),
                              make_vert(x0, nh, z1, u1, v1, c_west),
                              make_vert(x0, h,  z1, u1, v0, c_west));
                }
            }

            if (out->tri_count + 2 > VOXEL_MAX_TRIS) {
                return;
            }
        }
    }
}
