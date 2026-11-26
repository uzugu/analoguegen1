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

static float tile_height_for(uint8_t cls, uint8_t tileset) {
    switch (cls) {
    case TILE_CLASS_WATER:  return -2.0f;
    case TILE_CLASS_GROUND: return  0.0f;
    case TILE_CLASS_GRASS:  return  0.0f;
    case TILE_CLASS_LEDGE:  return  5.0f;
    case TILE_CLASS_FENCE:  return  8.0f;
    case TILE_CLASS_SOLID:  return (tileset == 0U) ? 12.0f : 9.0f;
    case TILE_CLASS_TALL:   return 20.0f;
    default:                return  0.0f;
    }
}

static float neighbor_height(const gb_map_view_t *map, int vx, int vy) {
    if (vx < 0 || vx >= GB_MAP_VIEW_W || vy < 0 || vy >= GB_MAP_VIEW_H) {
        return 0.0f;
    }
    return tile_height_for(map->tile_class[vy][vx], map->tileset);
}

static void class_tint(uint8_t cls, uint8_t tileset, float *tr, float *tg, float *tb) {
    switch (cls) {
    case TILE_CLASS_GRASS:
        *tr = 0.58f; *tg = 0.96f; *tb = 0.56f;
        break;
    case TILE_CLASS_WATER:
        *tr = 0.42f; *tg = 0.76f; *tb = 1.00f;
        break;
    case TILE_CLASS_LEDGE:
        *tr = 0.96f; *tg = 0.86f; *tb = 0.68f;
        break;
    case TILE_CLASS_FENCE:
        *tr = 0.92f; *tg = 0.78f; *tb = 0.58f;
        break;
    case TILE_CLASS_SOLID:
        if (tileset == 0U) {
            *tr = 0.54f; *tg = 0.88f; *tb = 0.52f;
        } else {
            *tr = 0.88f; *tg = 0.88f; *tb = 0.92f;
        }
        break;
    case TILE_CLASS_TALL:
        *tr = 1.00f; *tg = 0.84f; *tb = 0.76f;
        break;
    case TILE_CLASS_GROUND:
    default:
        *tr = 0.96f; *tg = 0.95f; *tb = 0.88f;
        break;
    }
}

static void daytime_tint(uint8_t daytime_mode, float *dr, float *dg, float *db) {
    switch (daytime_mode) {
    case 1U: /* DUSK */
        *dr = 1.00f; *dg = 0.78f; *db = 0.62f;
        break;
    case 2U: /* NIGHT */
        *dr = 0.48f; *dg = 0.56f; *db = 0.82f;
        break;
    case 3U: /* DAWN */
        *dr = 1.00f; *dg = 0.86f; *db = 0.78f;
        break;
    case 0U: /* DAY */
    default:
        *dr = 1.00f; *dg = 1.00f; *db = 1.00f;
        break;
    }
}

static uint16_t shade_rgb565(uint8_t cls, uint8_t tileset, uint8_t daytime_mode, float shade) {
    float tr, tg, tb;
    float dr, dg, db;
    class_tint(cls, tileset, &tr, &tg, &tb);
    daytime_tint(daytime_mode, &dr, &dg, &db);

    int r = (int)(shade * tr * dr * 255.0f + 0.5f);
    int g = (int)(shade * tg * dg * 255.0f + 0.5f);
    int b = (int)(shade * tb * db * 255.0f + 0.5f);

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

/* Emit a single CCW triangle. */
static void emit_tri(voxel_mesh_t *out,
                     voxel_vert_t v0, voxel_vert_t v1, voxel_vert_t v2) {
    if (out->tri_count + 1 > VOXEL_MAX_TRIS) {
        return;
    }
    voxel_tri_t *t = &out->tris[out->tri_count++];
    t->v[0] = v0;
    t->v[1] = v1;
    t->v[2] = v2;
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

static bool neighbor_is_tall(const gb_map_view_t *map, int vx, int vy) {
    if (vx < 0 || vx >= GB_MAP_VIEW_W || vy < 0 || vy >= GB_MAP_VIEW_H) {
        return false;
    }
    return map->tile_class[vy][vx] == (uint8_t)TILE_CLASS_TALL;
}

static bool is_mergeable_flat_tile(uint8_t cls, uint8_t tid) {
    if (cls != (uint8_t)TILE_CLASS_GROUND && cls != (uint8_t)TILE_CLASS_GRASS) {
        return false;
    }
    return (tid == 0x7FU || tid == 0x00U);
}

void voxel_mesher_build(const gb_map_view_t *map, const app_state_t *state, voxel_mesh_t *out) {
    if (out == NULL) {
        return;
    }
    out->tri_count = 0;

    if (map == NULL || !map->valid) {
        return;
    }

    const uint8_t tileset = map->tileset;
    const uint8_t daytime_mode = (state != NULL) ? state->daytime_mode : 0U;
    const bool water_shimmer = (state != NULL && ((state->frame_count >> 4) & 1u) != 0u);
    const float water_u_shift = water_shimmer ? 0.5f : 0.0f;

    for (int vy = 0; vy < GB_MAP_VIEW_H; vy++) {
        const float z0 = (float)vy * VOXEL_TILE_SIZE;
        const float z1 = z0 + VOXEL_TILE_SIZE;

        for (int vx = 0; vx < GB_MAP_VIEW_W; vx++) {
            const uint8_t cls = map->tile_class[vy][vx];
            const float h = tile_height_for(cls, tileset);
            const uint8_t tid = (uint8_t)(map->tile_id[vy][vx] & 0x7Fu);

            const float tile_u = (float)(tid & 15u) * 8.0f;
            const float tile_v = (float)(tid >> 4)  * 8.0f;
            float u0 = tile_u + VOXEL_UV_INSET;
            const float v0 = tile_v + VOXEL_UV_INSET;
            float u1 = u0 + VOXEL_UV_SPAN;
            const float v1 = v0 + VOXEL_UV_SPAN;

            /*
             * Horizontal run merging for solid blank/void flat tiles (tid 0x7F or 0x00).
             * Stretching UVs across a uniform blank/void tile looks identical while
             * collapsing a full 20-tile border row from 40 tris down to 2 tris.
             * Textured ground/grass tiles keep 1 quad per tile so UVs never stretch.
             */
            if (is_mergeable_flat_tile(cls, tid)) {
                int run_end = vx + 1;
                while (run_end < GB_MAP_VIEW_W &&
                       map->tile_class[vy][run_end] == cls &&
                       (uint8_t)(map->tile_id[vy][run_end] & 0x7Fu) == tid) {
                    run_end++;
                }

                const float rx0 = (float)vx * VOXEL_TILE_SIZE;
                const float rx1 = (float)run_end * VOXEL_TILE_SIZE;
                const uint16_t c_top = shade_rgb565(cls, tileset, daytime_mode, FACE_SHADE_TOP);
                emit_quad(out,
                          make_vert(rx0, h, z0, u0, v0, c_top),
                          make_vert(rx0, h, z1, u0, v1, c_top),
                          make_vert(rx1, h, z1, u1, v1, c_top),
                          make_vert(rx1, h, z0, u1, v0, c_top));

                /* Emit drop walls only where the run borders recessed tiles (e.g., water). */
                for (int kx = vx; kx < run_end; kx++) {
                    const float nh_south = neighbor_height(map, kx, vy + 1);
                    if (h > nh_south) {
                        const float kx0 = (float)kx * VOXEL_TILE_SIZE;
                        const float kx1 = kx0 + VOXEL_TILE_SIZE;
                        const uint16_t c_south = shade_rgb565(cls, tileset, daytime_mode, FACE_SHADE_SOUTH);
                        emit_quad(out,
                                  make_vert(kx0, h,        z1, u0, v0, c_south),
                                  make_vert(kx0, nh_south, z1, u0, v1, c_south),
                                  make_vert(kx1, nh_south, z1, u1, v1, c_south),
                                  make_vert(kx1, h,        z1, u1, v0, c_south));
                    }
                }
                {
                    const float nh_east = neighbor_height(map, run_end, vy);
                    if (h > nh_east) {
                        const uint16_t c_east = shade_rgb565(cls, tileset, daytime_mode, FACE_SHADE_EAST);
                        emit_quad(out,
                                  make_vert(rx1, h,       z1, u0, v0, c_east),
                                  make_vert(rx1, nh_east, z1, u0, v1, c_east),
                                  make_vert(rx1, nh_east, z0, u1, v1, c_east),
                                  make_vert(rx1, h,       z0, u1, v0, c_east));
                    }
                }
                {
                    const float nh_west = neighbor_height(map, vx - 1, vy);
                    if (h > nh_west) {
                        const uint16_t c_west = shade_rgb565(cls, tileset, daytime_mode, FACE_SHADE_WEST);
                        emit_quad(out,
                                  make_vert(rx0, h,       z0, u0, v0, c_west),
                                  make_vert(rx0, nh_west, z0, u0, v1, c_west),
                                  make_vert(rx0, nh_west, z1, u1, v1, c_west),
                                  make_vert(rx0, h,       z1, u1, v0, c_west));
                    }
                }

                vx = run_end - 1;
                if (out->tri_count + 2 > VOXEL_MAX_TRIS) {
                    return;
                }
                continue;
            }

            const float x0 = (float)vx * VOXEL_TILE_SIZE;
            const float x1 = x0 + VOXEL_TILE_SIZE;

            if (cls == (uint8_t)TILE_CLASS_WATER && water_shimmer) {
                const float u_max = tile_u + 8.0f - VOXEL_UV_INSET;
                u0 += water_u_shift;
                u1 += water_u_shift;
                if (u1 > u_max) {
                    u1 = u_max;
                }
            }

            /* Top face (+Y): pitched roof for outdoor buildings (TALL), flat top otherwise */
            if (cls == (uint8_t)TILE_CLASS_TALL) {
                const float zm = 0.5f * (z0 + z1);
                const float vm = 0.5f * (v0 + v1);
                const float um = 0.5f * (u0 + u1);
                const float hr = h + 5.0f;
                const uint16_t c_south_roof = shade_rgb565(cls, tileset, daytime_mode, FACE_SHADE_TOP);
                const uint16_t c_north_roof = shade_rgb565(cls, tileset, daytime_mode, FACE_SHADE_NORTH);

                /* Front (South-facing) sloped roof plane */
                emit_quad(out,
                          make_vert(x0, hr, zm, u0, vm, c_south_roof),
                          make_vert(x0, h,  z1, u0, v1, c_south_roof),
                          make_vert(x1, h,  z1, u1, v1, c_south_roof),
                          make_vert(x1, hr, zm, u1, vm, c_south_roof));

                /* Back (North-facing) sloped roof plane */
                emit_quad(out,
                          make_vert(x0, h,  z0, u0, v0, c_north_roof),
                          make_vert(x0, hr, zm, u0, vm, c_north_roof),
                          make_vert(x1, hr, zm, u1, vm, c_north_roof),
                          make_vert(x1, h,  z0, u1, v0, c_north_roof));

                /* East gable end (+X) when not adjoining another TALL tile */
                if (!neighbor_is_tall(map, vx + 1, vy)) {
                    const uint16_t c_east = shade_rgb565(cls, tileset, daytime_mode, FACE_SHADE_EAST);
                    emit_tri(out,
                             make_vert(x1, hr, zm, um, v0, c_east),
                             make_vert(x1, h,  z1, u0, v1, c_east),
                             make_vert(x1, h,  z0, u1, v1, c_east));
                }

                /* West gable end (-X) when not adjoining another TALL tile */
                if (!neighbor_is_tall(map, vx - 1, vy)) {
                    const uint16_t c_west = shade_rgb565(cls, tileset, daytime_mode, FACE_SHADE_WEST);
                    emit_tri(out,
                             make_vert(x0, hr, zm, um, v0, c_west),
                             make_vert(x0, h,  z0, u0, v1, c_west),
                             make_vert(x0, h,  z1, u1, v1, c_west));
                }
            } else {
                const uint16_t c_top = shade_rgb565(cls, tileset, daytime_mode, FACE_SHADE_TOP);
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
                    const uint16_t c_south = shade_rgb565(cls, tileset, daytime_mode, FACE_SHADE_SOUTH);
                    emit_quad(out,
                              make_vert(x0, h,  z1, u0, v0, c_south),
                              make_vert(x0, nh, z1, u0, v1, c_south),
                              make_vert(x1, nh, z1, u1, v1, c_south),
                              make_vert(x1, h,  z1, u1, v0, c_south));
                }
            }

            /* North face (-Z): skipped because diorama camera always looks North (yaw == 0). */

            /* East face (+X): visible if this tile stands higher than vx + 1 */
            {
                const float nh = neighbor_height(map, vx + 1, vy);
                if (h > nh) {
                    const uint16_t c_east = shade_rgb565(cls, tileset, daytime_mode, FACE_SHADE_EAST);
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
                    const uint16_t c_west = shade_rgb565(cls, tileset, daytime_mode, FACE_SHADE_WEST);
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
