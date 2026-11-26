#ifndef MOD_MANAGER_H
#define MOD_MANAGER_H

#include <stdbool.h>
#include <stdint.h>
#include "app_state.h"

#define MOD_MAX_COUNT 16

typedef struct mod_descriptor {
    const char *id;
    const char *name;
    const char *category;      /* "GRAPHICS", "GAMEPLAY", "UI", "VISUAL", "MECHANIC" */
    const char *description;
    bool        enabled;
    bool        default_enabled;

    void (*init)(app_state_t *state);
    void (*on_frame)(app_state_t *state);
    void (*on_render_3d)(const app_state_t *state);
    void (*on_render_hud)(const app_state_t *state);
} mod_descriptor_t;

void mod_manager_init(app_state_t *state);
bool mod_manager_register(const mod_descriptor_t *desc);
int  mod_manager_count(void);
mod_descriptor_t *mod_manager_get(int index);
void mod_manager_toggle(int index);
bool mod_manager_is_enabled(const char *id);

/* Dispatch hooks to all enabled mods */
void mod_manager_on_frame(app_state_t *state);
void mod_manager_on_render_3d(const app_state_t *state);
void mod_manager_on_render_hud(const app_state_t *state);

/* Interactive Unified Floating Popup Menu (navigation + rendering) */
void mod_manager_menu_input(app_state_t *state, bool up, bool down, bool left, bool right, bool confirm, bool close);
void mod_manager_menu_render(const app_state_t *state);

/* Built-in mod registrars */
void mod_voxel_register(void);
void mod_moveinfo_register(void);
void mod_followers_register(void);
void mod_wilds_register(void);
void mod_gen3box_register(void);

/* Gen 3 Box interactive UI hooks */
void mod_gen3box_input(app_state_t *state, bool up, bool down, bool left, bool right, bool action, bool close);
void mod_gen3box_render(const app_state_t *state);

#endif /* MOD_MANAGER_H */
