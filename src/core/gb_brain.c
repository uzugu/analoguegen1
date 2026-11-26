#include "gb_brain.h"
#include "pokered_ram.h"

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

#include "peanut_gb.h"

static struct gb_s gb;
static uint8_t    *rom_data = NULL;
static size_t      rom_size = 0U;
static uint8_t    *cart_ram = NULL;
static size_t      cart_ram_size = 0U;
static char       *sav_path = NULL;
static bool        initialized = false;

static uint16_t    lcd_fb[GB_LCD_W * GB_LCD_H];

static uint8_t gb_rom_read(struct gb_s *g, const uint_fast32_t addr)
{
    (void)g;
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
        cart_ram[addr] = val;
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
    /* Classic warm DMG / GBC-friendly RGB565 4-shade palette (0=lightest..3=darkest). */
    static const uint16_t dmg_palette[4] = {
        0xFFBCU, /* Shade 0: RGB(248, 244, 224) warm cream white */
        0xBD92U, /* Shade 1: RGB(184, 176, 144) warm light olive-tan */
        0x6B2AU, /* Shade 2: RGB(104, 100,  80) warm dark olive-slate */
        0x18C2U  /* Shade 3: RGB( 24,  24,  20) deep warm charcoal */
    };
    uint16_t *dst;
    unsigned int x;

    (void)g;

    if (line >= GB_LCD_H) {
        return;
    }

    dst = &lcd_fb[(size_t)line * GB_LCD_W];
    for (x = 0U; x < GB_LCD_W; x++) {
        dst[x] = dmg_palette[pixels[x] & 3U];
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
    gb_init_lcd(&gb, lcd_draw_line);

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

void gb_brain_run_frame(app_state_t *state)
{
    uint8_t new_x;
    uint8_t new_y;
    uint8_t dv_atk_def;
    uint8_t dv_spd_spc;
    unsigned int i;

    if (!initialized) {
        return;
    }

    gb_run_frame(&gb);

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
    state->snap.in_battle = gb_brain_read8(GB_ADDR_IS_IN_BATTLE);
    state->snap.party_count = gb_brain_read8(GB_ADDR_PARTY_COUNT);
    state->snap.lead_species = gb_brain_read8(GB_ADDR_PARTY_SPECIES);
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
