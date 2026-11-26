#ifndef RENDERER3D_H
#define RENDERER3D_H

#include <stdbool.h>
#include <stdint.h>
#include "voxel_mesher.h"

typedef struct {
    float target_x;
    float target_y;
    float target_z;
    float pitch_deg;    /* 15..75 degrees from top-down (Dramatic Shape convention) */
    float yaw_deg;      /* 0 = looking north (-Z) from south (+Z) */
    float distance;     /* camera orbit distance in world pixels */
    float fov_y_deg;    /* vertical FOV in degrees (e.g., 45.0f) */
} r3d_camera_t;

void r3d_init(void);
void r3d_begin_scene(uint16_t sky_color);
void r3d_set_camera(const r3d_camera_t *cam);

/* Rasterize a textured, Z-buffered, vertex-shaded voxel mesh. */
void r3d_draw_mesh(const voxel_mesh_t *mesh, const uint16_t *atlas_rgb565, int atlas_w, int atlas_h);

/* Draw a camera-facing 3D character/follower billboard + optional ground shadow quad. */
void r3d_draw_billboard(float wx, float wy, float wz, float width, float height,
                        uint16_t body_color, uint16_t accent_color, bool draw_shadow);

/* Draw a camera-facing 3D billboard textured with a custom RGB565 sprite (0 = transparent). */
void r3d_draw_sprite_billboard(float wx, float wy, float wz, float width, float height,
                               const uint16_t *tex_rgb565, int tex_w, int tex_h, bool draw_shadow);

void r3d_end_scene(void);

#endif /* RENDERER3D_H */
