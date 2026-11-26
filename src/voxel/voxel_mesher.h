#ifndef VOXEL_MESHER_H
#define VOXEL_MESHER_H

#include <stdint.h>
#include "app_state.h"
#include "gb_map.h"

#define VOXEL_MAX_TRIS 2400

typedef struct {
    float    x, y, z;   /* world coordinates (8.0 units = 1 8x8 tile; +X east, +Y up, +Z south) */
    float    u, v;      /* texel coordinates in [0..GB_ATLAS_W, 0..GB_ATLAS_H] */
    uint16_t rgb;       /* per-vertex RGB565 shade/tint */
} voxel_vert_t;

typedef struct {
    voxel_vert_t v[3];
} voxel_tri_t;

typedef struct {
    int         tri_count;
    voxel_tri_t tris[VOXEL_MAX_TRIS];
} voxel_mesh_t;

/*
 * Build a 3D extruded voxel mesh from the extracted 20x18 tile view using
 * Dramatic Shape's directional FACE_SHADE lighting and hidden-face culling.
 */
void voxel_mesher_build(const gb_map_view_t *map, const app_state_t *state, voxel_mesh_t *out);

#endif /* VOXEL_MESHER_H */
