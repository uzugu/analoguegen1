#define _DEFAULT_SOURCE

#include "gb_brain.h"
#include "gb_audio.h"
#include "pokered_ram.h"
#include "of_audio.h"
#ifndef OF_PC
#include "of_file.h"
#endif

#include "gbrt.h"
#include "ppu.h"
#include "platform_sdl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ============================================================================
 * Generated ROM entry points (weak fallbacks allow compilation without ROM C)
 * ========================================================================== */

__attribute__((weak)) const GBConfig *pokered_default_config(void)
{
    return NULL;
}

__attribute__((weak)) void pokered_init(GBContext *ctx)
{
    (void)ctx;
}

__attribute__((weak)) void pokered_run(GBContext *ctx)
{
    if (ctx != NULL) {
        gb_dispatch(ctx, ctx->pc);
    }
}

/* ============================================================================
 * Minimal bare-metal platform stubs (replaces platform_sdl.cpp / ImGui)
 * ========================================================================== */

uint8_t g_joypad_buttons = 0xFFU;
uint8_t g_joypad_dpad = 0xFFU;

bool gb_platform_init(int scale) { (void)scale; return true; }
void gb_platform_register_context(GBContext *ctx) { (void)ctx; }
void gb_platform_submit_port_frame(void *user, const GBPortFrame *frame) { (void)user; (void)frame; }
void gb_platform_set_benchmark_mode(bool enabled) { (void)enabled; }
void gb_platform_set_launcher_return_enabled(bool enabled) { (void)enabled; }
GBPlatformExitAction gb_platform_get_exit_action(void) { return GB_PLATFORM_EXIT_QUIT; }
void gb_platform_shutdown(void) {}
bool gb_platform_poll_events(GBContext *ctx) { (void)ctx; return true; }
void gb_platform_render_frame(const uint32_t *framebuffer) { (void)framebuffer; }
void gb_platform_present_framebuffer(const uint32_t *framebuffer) { (void)framebuffer; }
void gb_platform_render_lcd_off_frame(void) {}
bool gb_platform_set_input_script(const char *script) { (void)script; return false; }
bool gb_platform_set_persistence_dir(const char *path) { (void)path; return true; }
void gb_platform_test_inject_persistence_fault(GBPersistenceTestTarget target, GBPersistenceTestFault fault) { (void)target; (void)fault; }
void gb_platform_set_input_record_file(const char *path) { (void)path; }
void gb_platform_set_dump_frames(const char *frames) { (void)frames; }
void gb_platform_set_dump_present_frames(const char *frames) { (void)frames; }
void gb_platform_set_screenshot_prefix(const char *prefix) { (void)prefix; }
void gb_platform_get_timing_info(GBPlatformTimingInfo *out) { if (out != NULL) { memset(out, 0, sizeof(*out)); } }
uint8_t gb_platform_get_joypad(void) { return (uint8_t)(g_joypad_buttons & g_joypad_dpad); }
void gb_platform_vsync(uint32_t frame_cycles) { (void)frame_cycles; }
bool gb_platform_get_smooth_lcd_transitions(void) { return false; }
void gb_platform_set_smooth_lcd_transitions(bool enabled) { (void)enabled; }
void gb_platform_set_title(const char *title) { (void)title; }

/* ============================================================================
 * Backend state
 * ========================================================================== */

#define RECOMP_AUDIO_OUT_CAP 2048U
#define RECOMP_IN_RATE       44100U
#define RECOMP_OUT_RATE      48000U

static GBContext     *s_ctx = NULL;
static GBJoypadState  s_joypad = { 0xFFU, 0xFFU };
static char          *s_sav_path = NULL;
static bool           s_initialized = false;

static uint32_t       s_eram_last_hash = 0U;
static bool           s_eram_dirty = false;
static uint32_t       s_eram_dirty_frames = 0U;

static uint16_t       lcd_fb[GB_LCD_W * GB_LCD_H];

static bool           s_audio_ready = false;
static bool           s_audio_capture_enabled = false;
static int16_t        s_audio_48k[RECOMP_AUDIO_OUT_CAP * 2U];
static uint32_t       s_audio_out_count = 0U;
static uint32_t       s_audio_phase = 0U;
static int16_t        s_audio_prev_l = 0;
static int16_t        s_audio_prev_r = 0;

/* ============================================================================
 * Audio bridge (44.1 kHz gbrt APU -> 48 kHz openfpgaOS audio ring)
 * ========================================================================== */

void gb_audio_init(void)
{
    of_audio_init();
    s_audio_ready = true;
    s_audio_out_count = 0U;
    s_audio_phase = 0U;
    s_audio_prev_l = 0;
    s_audio_prev_r = 0;
}

static void recomp_on_audio_sample(GBContext *ctx, int16_t left, int16_t right)
{
    (void)ctx;
    if (!s_audio_ready || !s_audio_capture_enabled) {
        return;
    }

    while (s_audio_phase < RECOMP_OUT_RATE) {
        if (s_audio_out_count < RECOMP_AUDIO_OUT_CAP) {
            int32_t t = (int32_t)s_audio_phase;
            int16_t l_out = (int16_t)(s_audio_prev_l + (((int32_t)(left - s_audio_prev_l) * t) / (int32_t)RECOMP_OUT_RATE));
            int16_t r_out = (int16_t)(s_audio_prev_r + (((int32_t)(right - s_audio_prev_r) * t) / (int32_t)RECOMP_OUT_RATE));
            s_audio_48k[s_audio_out_count * 2U] = l_out;
            s_audio_48k[s_audio_out_count * 2U + 1U] = r_out;
            s_audio_out_count++;
        }
        s_audio_phase += RECOMP_IN_RATE;
    }
    s_audio_phase -= RECOMP_OUT_RATE;
    s_audio_prev_l = left;
    s_audio_prev_r = right;
}

static void recomp_flush_audio(void)
{
    if (!s_audio_ready || !s_audio_capture_enabled || s_audio_out_count == 0U) {
        s_audio_out_count = 0U;
        return;
    }

    int free_pairs = of_audio_free();
    if (free_pairs > 0) {
        uint32_t to_write = (s_audio_out_count <= (uint32_t)free_pairs)
                            ? s_audio_out_count
                            : (uint32_t)free_pairs;
        (void)of_audio_write(s_audio_48k, (int)to_write);
    }
    s_audio_out_count = 0U;
}

/* ============================================================================
 * Battery SRAM persistence callbacks
 * ========================================================================== */

static uint32_t fnv1a_hash(const uint8_t *data, size_t size)
{
    uint32_t h = 2166136261U;
    size_t i;

    if (data == NULL || size == 0U) {
        return 0U;
    }
    for (i = 0U; i < size; i++) {
        h ^= (uint32_t)data[i];
        h *= 16777619U;
    }
    return h;
}

static bool recomp_load_battery_ram(GBContext *ctx, const char *rom_name, void *data, size_t size)
{
    FILE *sf;
    size_t nread;

    (void)ctx;
    (void)rom_name;
    if (s_sav_path == NULL || data == NULL || size == 0U) {
        return false;
    }

    sf = fopen(s_sav_path, "rb");
    if (sf == NULL) {
        return false;
    }
    nread = fread(data, 1U, size, sf);
    fclose(sf);
    return (nread > 0U);
}

static bool recomp_save_battery_ram(GBContext *ctx, const char *rom_name, const void *data, size_t size)
{
    FILE *sf;
    size_t nwritten;

    (void)ctx;
    (void)rom_name;
    if (s_sav_path == NULL || data == NULL || size == 0U) {
        return false;
    }

    sf = fopen(s_sav_path, "wb");
    if (sf == NULL) {
        return false;
    }
    nwritten = fwrite(data, 1U, size, sf);
    fclose(sf);
    return (nwritten == size);
}

/* ============================================================================
 * ROM loading & metadata helpers
 * ========================================================================== */

static bool load_rom_file_into_context(GBContext *ctx, const char *rom_path)
{
    FILE *fp;
    long file_sz;
    uint8_t *buf;
    size_t nread;
    bool ok;

    if (ctx == NULL || rom_path == NULL || rom_path[0] == '\0') {
        return false;
    }

    fp = fopen(rom_path, "rb");
    if (fp == NULL) {
        return false;
    }

    if (fseek(fp, 0L, SEEK_END) != 0) {
        fclose(fp);
        return false;
    }
    file_sz = ftell(fp);
    if (file_sz < 0x0150L || fseek(fp, 0L, SEEK_SET) != 0) {
        fclose(fp);
        return false;
    }

    buf = (uint8_t *)malloc((size_t)file_sz);
    if (buf == NULL) {
        fclose(fp);
        return false;
    }

    nread = fread(buf, 1U, (size_t)file_sz, fp);
    fclose(fp);
    if (nread != (size_t)file_sz) {
        free(buf);
        return false;
    }

    ok = gb_context_load_rom(ctx, buf, (size_t)file_sz);
    free(buf);
    if (!ok) {
        return false;
    }
    gb_context_reset(ctx, true);
    return true;
}

static void detect_rom_metadata(const GBContext *ctx, app_state_t *state)
{
    size_t len = 0U;
    uint16_t addr;

    if (ctx == NULL || ctx->rom == NULL || ctx->rom_size < 0x0150U) {
        return;
    }

    for (addr = 0x0134U; addr <= 0x0143U; addr++) {
        uint8_t ch = ctx->rom[addr];
        if (ch == 0U || ch < 0x20U || ch > 0x7EU) {
            break;
        }
        state->rom_title[len++] = (char)ch;
    }
    state->rom_title[len] = '\0';

    if (strstr(state->rom_title, "YELLOW") != NULL) {
        state->version = GAME_VER_YELLOW;
    } else if (strstr(state->rom_title, "RED") != NULL) {
        state->version = GAME_VER_RED;
    } else if (strstr(state->rom_title, "BLUE") != NULL) {
        state->version = GAME_VER_BLUE;
    } else {
        state->version = GAME_VER_UNKNOWN;
    }
}

/* ============================================================================
 * Framebuffer conversion & WRAM snapshot mirror (matches gb_brain.c)
 * ========================================================================== */

static void convert_framebuffer_rgb565(GBContext *ctx)
{
    const uint32_t *src = gb_get_framebuffer(ctx);
    size_t i;

    if (src == NULL) {
        return;
    }

    for (i = 0U; i < (size_t)(GB_LCD_W * GB_LCD_H); i++) {
        uint32_t argb = src[i];
        uint16_t r5 = (uint16_t)((argb >> 19) & 0x1FU);
        uint16_t g6 = (uint16_t)((argb >> 10) & 0x3FU);
        uint16_t b5 = (uint16_t)((argb >> 3)  & 0x1FU);
        lcd_fb[i] = (uint16_t)((r5 << 11) | (g6 << 5) | b5);
    }
}

static void update_snapshot(app_state_t *state)
{
    uint8_t new_x;
    uint8_t new_y;
    uint8_t dv_atk_def;
    uint8_t dv_spd_spc;
    uint8_t box_cnt;
    unsigned int i;
    unsigned int s;

    state->snap.cur_map = gb_brain_read8(GB_ADDR_CUR_MAP);
    state->snap.tileset = gb_brain_read8(GB_ADDR_CUR_MAP_TILESET);
    state->snap.map_w = gb_brain_read8(GB_ADDR_CUR_MAP_WIDTH);
    state->snap.map_h = gb_brain_read8(GB_ADDR_CUR_MAP_HEIGHT);

    new_x = gb_brain_read8(GB_ADDR_X_COORD);
    new_y = gb_brain_read8(GB_ADDR_Y_COORD);
    if (new_x != state->snap.player_x || new_y != state->snap.player_y) {
        state->snap.prev_x = state->snap.player_x;
        state->snap.prev_y = state->snap.player_y;
        state->snap.player_x = new_x;
        state->snap.player_y = new_y;
    }

    state->snap.facing_dir = gb_brain_read8((uint16_t)(GB_ADDR_SPRITE_DATA1 + GB_SPRITE1_FACING_DIR));
    state->snap.walk_counter = gb_brain_read8(GB_ADDR_WALK_COUNTER);
    state->snap.walk_dx = (int8_t)gb_brain_read8((uint16_t)(GB_ADDR_SPRITE_DATA1 + GB_SPRITE1_X_STEP_VEC));
    state->snap.walk_dy = (int8_t)gb_brain_read8((uint16_t)(GB_ADDR_SPRITE_DATA1 + GB_SPRITE1_Y_STEP_VEC));
    state->snap.in_battle = gb_brain_read8(GB_ADDR_IS_IN_BATTLE);
    state->snap.party_count = gb_brain_read8(GB_ADDR_PARTY_COUNT);
    state->snap.lead_species = gb_brain_read8(GB_ADDR_PARTY_SPECIES);
    for (i = 0U; i < 6U; i++) {
        state->snap.party_species[i] = gb_brain_read8((uint16_t)(GB_ADDR_PARTY_SPECIES + i));
    }

    state->snap.cur_box_num = (uint8_t)(gb_brain_read8(GB_ADDR_CUR_BOX) & 0x7FU);
    box_cnt = gb_brain_read8(GB_ADDR_BOX_COUNT);
    if (box_cnt > APP_MAX_BOX_MONS) {
        box_cnt = APP_MAX_BOX_MONS;
    }
    state->snap.box_count = box_cnt;
    for (i = 0U; i < APP_MAX_BOX_MONS; i++) {
        state->snap.box_species[i] = gb_brain_read8((uint16_t)(GB_ADDR_BOX_SPECIES + i));
    }

    {
        uint8_t top_left    = gb_brain_read8(GB_ADDR_TILEMAP);
        uint8_t top_mid     = gb_brain_read8((uint16_t)(GB_ADDR_TILEMAP + 1U));
        uint8_t row12_col0  = gb_brain_read8((uint16_t)(GB_ADDR_TILEMAP + (12U * 20U)));
        uint8_t row12_col1  = gb_brain_read8((uint16_t)(GB_ADDR_TILEMAP + (12U * 20U) + 1U));
        uint8_t start_tl    = gb_brain_read8((uint16_t)(GB_ADDR_TILEMAP + 10U));
        uint8_t start_tr    = gb_brain_read8((uint16_t)(GB_ADDR_TILEMAP + 19U));
        uint8_t row1_col11  = gb_brain_read8((uint16_t)(GB_ADDR_TILEMAP + 20U + 11U));

        /* Full-screen menus (Party, Bag, Pokedex, Options, Save, PC) draw borders/blank tiles at (0,0) */
        if (top_left == 0x79U || (top_left == 0x7FU && top_mid == 0x7FU)) {
            state->snap.ui_overlay = (uint8_t)UI_OVERLAY_FULLSCREEN_2D;
        } else if (start_tl == 0x79U && start_tr == 0x7BU && row1_col11 != 0x00U) {
            state->snap.ui_overlay = (uint8_t)UI_OVERLAY_START_MENU;
        } else if (row12_col0 == 0x79U && row12_col1 == 0x7AU) {
            state->snap.ui_overlay = (uint8_t)UI_OVERLAY_DIALOGUE;
        } else {
            state->snap.ui_overlay = (uint8_t)UI_OVERLAY_NONE;
        }
    }

    for (s = 0U; s < APP_MAX_NPCS; s++) {
        uint16_t off = (uint16_t)((s + 1U) * 16U);
        uint8_t pic = gb_brain_read8((uint16_t)(GB_ADDR_SPRITE_DATA1 + off + GB_SPRITE1_PICTURE_ID));
        if (pic != 0U) {
            state->snap.npcs[s].active = true;
            state->snap.npcs[s].picture_id = pic;
            state->snap.npcs[s].facing_dir = gb_brain_read8((uint16_t)(GB_ADDR_SPRITE_DATA1 + off + GB_SPRITE1_FACING_DIR));
            state->snap.npcs[s].map_y = (uint8_t)(gb_brain_read8((uint16_t)(GB_ADDR_SPRITE_DATA2 + off + GB_SPRITE2_MAP_Y)) - 4U);
            state->snap.npcs[s].map_x = (uint8_t)(gb_brain_read8((uint16_t)(GB_ADDR_SPRITE_DATA2 + off + GB_SPRITE2_MAP_X)) - 4U);
        } else {
            state->snap.npcs[s].active = false;
            state->snap.npcs[s].picture_id = 0U;
            state->snap.npcs[s].facing_dir = 0U;
            state->snap.npcs[s].map_x = 0U;
            state->snap.npcs[s].map_y = 0U;
        }
    }

    state->snap.enemy_species = gb_brain_read8(GB_ADDR_ENEMY_MON_SPECIES);
    state->snap.enemy_type1 = gb_brain_read8(GB_ADDR_ENEMY_MON_TYPE1);
    state->snap.enemy_type2 = gb_brain_read8(GB_ADDR_ENEMY_MON_TYPE2);
    state->snap.enemy_level = gb_brain_read8(GB_ADDR_ENEMY_MON_LEVEL);

    dv_atk_def = gb_brain_read8(GB_ADDR_ENEMY_MON_DVS);
    dv_spd_spc = gb_brain_read8((uint16_t)(GB_ADDR_ENEMY_MON_DVS + 1U));
    state->snap.enemy_shiny = (pokered_is_shiny_dv(dv_atk_def, dv_spd_spc) != 0);

    for (i = 0U; i < 4U; i++) {
        state->snap.battle_moves[i] = gb_brain_read8((uint16_t)(GB_ADDR_BATTLE_MON_MOVES + i));
    }

    state->snap.menu_cursor = gb_brain_read8(GB_ADDR_CURRENT_MENU_ITEM);
}

/* ============================================================================
 * Public gb_brain.h API
 * ========================================================================== */

bool gb_brain_init(const char *rom_path, app_state_t *state)
{
    const char *eff_path;
    const GBConfig *gen_cfg;
    GBConfig cfg;
    GBPlatformCallbacks cbs;
    size_t sav_len;

    if (state == NULL) {
        return false;
    }

    if (s_initialized || s_ctx != NULL || s_sav_path != NULL) {
        gb_brain_shutdown();
    }

    state->rom_loaded = false;
    state->version = GAME_VER_UNKNOWN;
    state->rom_title[0] = '\0';
    memset(&state->snap, 0, sizeof(state->snap));

    eff_path = (rom_path != NULL && rom_path[0] != '\0') ? rom_path : "pokered.gb";

#ifndef OF_PC
    if (rom_path != NULL && rom_path[0] != '\0' && of_file_slot_find(rom_path, NULL) < 0) {
        of_file_slot_register(0U, rom_path);
    }
#endif

    gen_cfg = pokered_default_config();
    if (gen_cfg != NULL) {
        cfg = *gen_cfg;
    } else {
        memset(&cfg, 0, sizeof(cfg));
        cfg.model = GB_MODEL_DMG;
        cfg.enable_audio = true;
        cfg.enable_serial = true;
        cfg.speed_percent = 100U;
    }
    cfg.enable_audio = true;

    s_ctx = gb_context_create(&cfg);
    if (s_ctx == NULL) {
        return false;
    }

    gb_context_set_save_id(s_ctx, "pokered");

    sav_len = strlen(eff_path) + 5U; /* ".sav" + NUL */
    s_sav_path = (char *)malloc(sav_len);
    if (s_sav_path == NULL) {
        gb_brain_shutdown();
        return false;
    }
    snprintf(s_sav_path, sav_len, "%s.sav", eff_path);

    memset(&cbs, 0, sizeof(cbs));
    cbs.on_audio_sample = recomp_on_audio_sample;
    cbs.load_battery_ram = recomp_load_battery_ram;
    cbs.save_battery_ram = recomp_save_battery_ram;
    gb_set_platform_callbacks(s_ctx, &cbs);

    /* Prefer embedded statically recompiled ROM/code when linked. */
    pokered_init(s_ctx);

    /* Fallback for stub builds without generated ROM C: load .gb from disk. */
    if (s_ctx->rom == NULL || s_ctx->rom_size < 0x0150U) {
        if (!load_rom_file_into_context(s_ctx, eff_path)) {
            gb_brain_shutdown();
            return false;
        }
    }

    s_joypad.dpad = 0xFFU;
    s_joypad.buttons = 0xFFU;
    g_joypad_dpad = 0xFFU;
    g_joypad_buttons = 0xFFU;
    s_ctx->joypad = &s_joypad;

    s_eram_last_hash = fnv1a_hash(s_ctx->eram, s_ctx->eram_size);
    s_eram_dirty = false;
    s_eram_dirty_frames = 0U;

    detect_rom_metadata(s_ctx, state);
    memset(lcd_fb, 0, sizeof(lcd_fb));

    s_initialized = true;
    state->rom_loaded = true;
    return true;
}

void gb_brain_shutdown(void)
{
    if (s_ctx != NULL) {
        gb_context_destroy(s_ctx);
        s_ctx = NULL;
    }

    free(s_sav_path);
    s_sav_path = NULL;

    s_eram_last_hash = 0U;
    s_eram_dirty = false;
    s_eram_dirty_frames = 0U;
    s_audio_out_count = 0U;
    s_audio_phase = 0U;
    s_audio_prev_l = 0;
    s_audio_prev_r = 0;
    s_initialized = false;
}

void gb_brain_set_input(const gb_input_t *in)
{
    uint8_t dpad = 0xFFU;
    uint8_t buttons = 0xFFU;
    uint8_t prev_dpad;
    uint8_t prev_buttons;

    if (!s_initialized || s_ctx == NULL) {
        return;
    }

    prev_dpad = s_joypad.dpad;
    prev_buttons = s_joypad.buttons;

    if (in != NULL) {
        if (in->right)  { dpad &= (uint8_t)~0x01U; }
        if (in->left)   { dpad &= (uint8_t)~0x02U; }
        if (in->up)     { dpad &= (uint8_t)~0x04U; }
        if (in->down)   { dpad &= (uint8_t)~0x08U; }
        if (in->a)      { buttons &= (uint8_t)~0x01U; }
        if (in->b)      { buttons &= (uint8_t)~0x02U; }
        if (in->select) { buttons &= (uint8_t)~0x04U; }
        if (in->start)  { buttons &= (uint8_t)~0x08U; }
    }

    s_joypad.dpad = dpad;
    s_joypad.buttons = buttons;
    g_joypad_dpad = dpad;
    g_joypad_buttons = buttons;

    if (((prev_dpad & (uint8_t)~dpad) & 0x0FU) != 0U ||
        ((prev_buttons & (uint8_t)~buttons) & 0x0FU) != 0U) {
        s_ctx->io[0x0F] |= 0x10U;
        if (s_ctx->halted) {
            s_ctx->halted = 0U;
        }
    }
}

void gb_brain_set_skip_ppu(bool skip) { (void)skip; }

void gb_brain_set_filter(uint8_t filter_mode) { (void)filter_mode; }

void gb_brain_run_frame(app_state_t *state)
{
    if (!s_initialized || s_ctx == NULL) {
        return;
    }

    s_audio_capture_enabled = (state != NULL && state->audio_enabled);
    s_audio_out_count = 0U;

    gb_run_frame(s_ctx);
    gbrt_audio_sync(s_ctx);
    recomp_flush_audio();
    convert_framebuffer_rgb565(s_ctx);

    if (s_ctx->eram != NULL && s_ctx->eram_size > 0U) {
        uint32_t cur_hash = fnv1a_hash(s_ctx->eram, s_ctx->eram_size);
        if (cur_hash != s_eram_last_hash) {
            s_eram_last_hash = cur_hash;
            s_eram_dirty = true;
            s_eram_dirty_frames = 0U;
        } else if (s_eram_dirty) {
            s_eram_dirty_frames++;
            if (s_eram_dirty_frames >= 120U) {
                (void)gb_context_save_ram(s_ctx);
                s_eram_dirty = false;
                s_eram_dirty_frames = 0U;
            }
        }
    }

    if (state == NULL) {
        return;
    }

    update_snapshot(state);
}

uint8_t gb_brain_read8(uint16_t addr)
{
    if (!s_initialized || s_ctx == NULL) {
        return 0U;
    }
    return gb_read8(s_ctx, addr);
}

void gb_brain_write8(uint16_t addr, uint8_t val)
{
    if (!s_initialized || s_ctx == NULL) {
        return;
    }
    gb_write8(s_ctx, addr, val);
}

uint8_t gb_brain_read_rom_bank(uint8_t bank, uint16_t addr)
{
    size_t offset;

    if (!s_initialized || s_ctx == NULL || s_ctx->rom == NULL || s_ctx->rom_size == 0U) {
        return 0xFFU;
    }
    if (addr < 0x4000U) {
        offset = (size_t)addr;
    } else if (addr < 0x8000U) {
        offset = ((size_t)bank * 0x4000U) + (size_t)(addr - 0x4000U);
    } else {
        return gb_brain_read8(addr);
    }
    return (offset < s_ctx->rom_size) ? s_ctx->rom[offset] : 0xFFU;
}

const uint16_t *gb_brain_get_lcd_rgb565(void)
{
    return lcd_fb;
}
