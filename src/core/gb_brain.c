#include "gb_brain.h"
#include "gb_audio.h"
#include "pokered_ram.h"
#ifndef OF_PC
#include "of_file.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef ENABLE_SOUND
#define ENABLE_SOUND 0
#endif

#ifndef ENABLE_LCD
#define ENABLE_LCD 1
#endif

#ifndef PEANUT_GB_12_COLOUR
#define PEANUT_GB_12_COLOUR 0
#endif

#ifndef PEANUT_GB_HIGH_LCD_ACCURACY
#define PEANUT_GB_HIGH_LCD_ACCURACY 0
#endif

uint8_t audio_read(const uint16_t addr)
{
    return gb_audio_read(addr);
}

void audio_write(const uint16_t addr, const uint8_t val)
{
    gb_audio_write(addr, val);
}

#include "peanut_gb.h"

static struct gb_s gb;
static uint8_t    *rom_data = NULL;
static size_t      rom_size = 0U;
static size_t      rom_mask = 0U; /* rom_size - 1 if rom_size is a power of two, else 0 */
static uint8_t    *cart_ram = NULL;
static size_t      cart_ram_size = 0U;
static char       *sav_path = NULL;
static bool        initialized = false;
static bool        cart_ram_dirty = false;
static uint32_t    cart_ram_dirty_frames = 0U;

static uint16_t    lcd_fb[GB_LCD_W * GB_LCD_H];
static bool        s_skip_ppu = false;
static uint8_t     s_filter_mode = 0U;

static uint8_t gb_rom_read(struct gb_s *g, const uint_fast32_t addr)
{
    (void)g;
    if (rom_mask != 0U) {
        /* Only non-zero while rom_data is loaded, so the masked index is in bounds. */
        return rom_data[addr & rom_mask];
    }
    if (rom_data == NULL || addr >= rom_size) {
        return 0xFFU;
    }
    return rom_data[addr];
}

static uint8_t gb_cart_ram_read(struct gb_s *g, const uint_fast32_t addr)
{
    (void)g;
    if (cart_ram == NULL || addr >= cart_ram_size) {
        return 0xFFU;
    }
    return cart_ram[addr];
}

static void gb_cart_ram_write(struct gb_s *g, const uint_fast32_t addr, const uint8_t val)
{
    (void)g;
    if (cart_ram != NULL && addr < cart_ram_size) {
        if (cart_ram[addr] != val) {
            cart_ram[addr] = val;
            cart_ram_dirty = true;
            cart_ram_dirty_frames = 0U;
        }
    }
}

static void gb_flush_save_file(void)
{
    if (cart_ram != NULL && cart_ram_size > 0U && sav_path != NULL) {
        FILE *sf = fopen(sav_path, "wb");
        if (sf != NULL) {
            size_t swr = fwrite(cart_ram, 1U, cart_ram_size, sf);
            (void)swr;
            fclose(sf);
        }
        cart_ram_dirty = false;
        cart_ram_dirty_frames = 0U;
    }
}

static void gb_error_cb(struct gb_s *g, const enum gb_error_e err, const uint16_t addr)
{
    (void)g;
    fprintf(stderr, "Peanut-GB fatal error %d at 0x%04X\n", (int)err, (unsigned int)addr);
    exit(EXIT_FAILURE);
}

static void lcd_draw_line(struct gb_s *g, const uint8_t *pixels, const uint_fast8_t line)
{
    /* 4 Game Boy palettes (0=DMG-01 Green, 1=GB Pocket, 2=GB Light Teal, 3=SGB Red). */
    static const uint16_t k_palettes[4][4] = {
        { 0x9DE3U, 0x8562U, 0x3306U, 0x09C2U }, /* 0/4: Authentic DMG-01 Olive-Green */
        { 0xCF78U, 0x9CD1U, 0x5289U, 0x10A2U }, /* 1/5: Authentic Game Boy Pocket */
        { 0x07F8U, 0x05D2U, 0x038CU, 0x0186U }, /* 2:   Game Boy Light Backlight */
        { 0xFFBCU, 0xF4CFU, 0xAA08U, 0x20C3U }  /* 3:   SGB Pokemon Red */
    };
    const uint16_t *pal;
    uint8_t pal_idx;
    uint16_t *dst;
    unsigned int x;

    (void)g;

    if (s_skip_ppu) {
        return;
    }

    if (line >= GB_LCD_H) {
        return;
    }

    if (s_filter_mode == 1U || s_filter_mode == 5U) {
        pal_idx = 1U;
    } else if (s_filter_mode == 2U) {
        pal_idx = 2U;
    } else if (s_filter_mode == 3U) {
        pal_idx = 3U;
    } else {
        pal_idx = 0U;
    }
    pal = k_palettes[pal_idx];

    dst = &lcd_fb[(size_t)line * GB_LCD_W];
    for (x = 0U; x < GB_LCD_W; x++) {
        dst[x] = pal[pixels[x] & 3U];
    }
}

static void detect_rom_metadata(app_state_t *state)
{
    size_t len = 0U;
    uint16_t addr;

    for (addr = 0x0134U; addr <= 0x0143U; addr++) {
        uint8_t ch = rom_data[addr];
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

bool gb_brain_init(const char *rom_path, app_state_t *state)
{
    FILE *fp;
    long file_sz;
    size_t nread;
    enum gb_init_error_e init_err;

    if (rom_path == NULL || state == NULL) {
        return false;
    }

    if (initialized || rom_data != NULL || cart_ram != NULL || sav_path != NULL) {
        gb_brain_shutdown();
    }

    state->rom_loaded = false;
    state->version = GAME_VER_UNKNOWN;
    state->rom_title[0] = '\0';
    memset(&state->snap, 0, sizeof(state->snap));

#ifndef OF_PC
    if (of_file_slot_find(rom_path, NULL) < 0) {
        of_file_slot_register(0U, rom_path);
    }
#endif

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

    rom_size = (size_t)file_sz;
    rom_data = (uint8_t *)malloc(rom_size);
    if (rom_data == NULL) {
        rom_size = 0U;
        fclose(fp);
        return false;
    }

    nread = fread(rom_data, 1U, rom_size, fp);
    fclose(fp);
    if (nread != rom_size) {
        gb_brain_shutdown();
        return false;
    }
    rom_mask = ((rom_size & (rom_size - 1U)) == 0U) ? (rom_size - 1U) : 0U;

    init_err = gb_init(&gb, gb_rom_read, gb_cart_ram_read, gb_cart_ram_write, gb_error_cb, NULL);
    if (init_err != GB_INIT_NO_ERROR) {
        gb_brain_shutdown();
        return false;
    }

    detect_rom_metadata(state);

    if (gb_get_save_size_s(&gb, &cart_ram_size) < 0) {
        gb_brain_shutdown();
        return false;
    }

    if (cart_ram_size > 0U) {
        size_t sav_len = strlen(rom_path) + 5U; /* ".sav" + NUL */
        FILE *sf;

        cart_ram = (uint8_t *)calloc(1U, cart_ram_size);
        sav_path = (char *)malloc(sav_len);
        if (cart_ram == NULL || sav_path == NULL) {
            gb_brain_shutdown();
            return false;
        }
        snprintf(sav_path, sav_len, "%s.sav", rom_path);

        sf = fopen(sav_path, "rb");
        if (sf != NULL) {
            size_t srd = fread(cart_ram, 1U, cart_ram_size, sf);
            (void)srd;
            fclose(sf);
        }
    }

    memset(lcd_fb, 0, sizeof(lcd_fb));
    gb_init_lcd(&gb, s_skip_ppu ? NULL : lcd_draw_line);

    initialized = true;
    state->rom_loaded = true;
    return true;
}

void gb_brain_shutdown(void)
{
    if (initialized && cart_ram != NULL && cart_ram_size > 0U && sav_path != NULL) {
        FILE *sf = fopen(sav_path, "wb");
        if (sf != NULL) {
            size_t swr = fwrite(cart_ram, 1U, cart_ram_size, sf);
            (void)swr;
            fclose(sf);
        }
    }

    free(sav_path);
    sav_path = NULL;

    free(cart_ram);
    cart_ram = NULL;
    cart_ram_size = 0U;

    free(rom_data);
    rom_data = NULL;
    rom_size = 0U;
    rom_mask = 0U;

    initialized = false;
}

void gb_brain_set_input(const gb_input_t *in)
{
    uint8_t joypad = 0xFFU;

    if (!initialized) {
        return;
    }

    if (in != NULL) {
        if (in->a)      { joypad &= (uint8_t)~JOYPAD_A; }
        if (in->b)      { joypad &= (uint8_t)~JOYPAD_B; }
        if (in->select) { joypad &= (uint8_t)~JOYPAD_SELECT; }
        if (in->start)  { joypad &= (uint8_t)~JOYPAD_START; }
        if (in->right)  { joypad &= (uint8_t)~JOYPAD_RIGHT; }
        if (in->left)   { joypad &= (uint8_t)~JOYPAD_LEFT; }
        if (in->up)     { joypad &= (uint8_t)~JOYPAD_UP; }
        if (in->down)   { joypad &= (uint8_t)~JOYPAD_DOWN; }
    }

    gb.direct.joypad = joypad;
}

void gb_brain_set_skip_ppu(bool skip)
{
    s_skip_ppu = skip;
    if (initialized) {
        gb.display.lcd_draw_line = skip ? NULL : lcd_draw_line;
    }
}

void gb_brain_set_filter(uint8_t filter_mode)
{
    s_filter_mode = filter_mode;
}

void gb_brain_run_frame(app_state_t *state)
{
    uint8_t new_x;
    uint8_t new_y;
    uint8_t dv_atk_def;
    uint8_t dv_spd_spc;
    uint8_t box_cnt;
    unsigned int i;
    unsigned int s;

    if (!initialized) {
        return;
    }

    gb_run_frame(&gb);
    if (state != NULL && state->audio_enabled) {
        gb_audio_push_frame();
    }

    /* Auto-flush SRAM ~2 seconds after the last write so saves survive power-off. */
    if (cart_ram_dirty) {
        cart_ram_dirty_frames++;
        if (cart_ram_dirty_frames >= 120U) {
            gb_flush_save_file();
        }
    }

    if (state == NULL) {
        return;
    }

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
    {
        uint8_t lcdc   = gb_brain_read8(GB_ADDR_LCDC);
        uint8_t bgp    = gb_brain_read8(0xFF47U);
        uint8_t p0_pic = gb_brain_read8((uint16_t)(GB_ADDR_SPRITE_DATA1 + GB_SPRITE1_PICTURE_ID));
        uint8_t p0_img = gb_brain_read8((uint16_t)(GB_ADDR_SPRITE_DATA1 + GB_SPRITE1_IMAGE_INDEX));
        state->snap.in_overworld_ready =
            (state->snap.in_battle == 0U &&
             state->snap.map_w > 0U && state->snap.map_h > 0U &&
             state->snap.map_w <= 64U && state->snap.map_h <= 64U &&
             (lcdc & 0x80U) != 0U &&
             bgp != 0x00U && bgp != 0xFFU &&
             p0_pic != 0U && p0_img != 0xFFU);
    }
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
        uint8_t img = gb_brain_read8((uint16_t)(GB_ADDR_SPRITE_DATA1 + off + GB_SPRITE1_IMAGE_INDEX));
        if (pic != 0U && img != 0xFFU) { /* image index 0xFF = sprite hidden by the game */
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

uint8_t gb_brain_read8(uint16_t addr)
{
    if (!initialized) {
        return 0U;
    }
    return __gb_read(&gb, addr);
}

void gb_brain_write8(uint16_t addr, uint8_t val)
{
    if (!initialized) {
        return;
    }
    __gb_write(&gb, addr, val);
}

uint8_t gb_brain_read_rom_bank(uint8_t bank, uint16_t addr)
{
    size_t offset;
    if (!initialized || rom_data == NULL || rom_size == 0U) {
        return 0xFFU;
    }
    if (addr < 0x4000U) {
        offset = (size_t)addr;
    } else if (addr < 0x8000U) {
        offset = ((size_t)bank * 0x4000U) + (size_t)(addr - 0x4000U);
    } else {
        return gb_brain_read8(addr);
    }
    return (offset < rom_size) ? rom_data[offset] : 0xFFU;
}

const uint16_t *gb_brain_get_lcd_rgb565(void)
{
    return lcd_fb;
}
