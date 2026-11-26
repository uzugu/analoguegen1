#ifndef GB_BRAIN_H
#define GB_BRAIN_H

#include <stdbool.h>
#include <stdint.h>
#include "app_state.h"

#define GB_LCD_W 160
#define GB_LCD_H 144

typedef struct {
    bool     up;
    bool     down;
    bool     left;
    bool     right;
    bool     a;
    bool     b;
    bool     start;
    bool     select;
} gb_input_t;

/* Initialize the Peanut-GB core and load a .gb ROM from disk (or SD slot). */
bool gb_brain_init(const char *rom_path, app_state_t *state);

/* Clean up allocated ROM / RAM buffers and flush SRAM save if present. */
void gb_brain_shutdown(void);

/* Update GB joypad inputs for the next frame. */
void gb_brain_set_input(const gb_input_t *in);

/* Run exactly one Game Boy frame (~59.7 fps) and update state->snap. */
void gb_brain_run_frame(app_state_t *state);

/* Skip software LCD output; the LCD buffer keeps its last contents (no-op outside Peanut-GB). */
void gb_brain_set_skip_ppu(bool skip);

/* Read/write arbitrary GB address space (WRAM, SRAM, ROM, VRAM). */
uint8_t gb_brain_read8(uint16_t addr);
void    gb_brain_write8(uint16_t addr, uint8_t val);

/* Read a byte from a specific ROM bank (bank 0: 0x0000..0x3FFF, bank N: 0x4000..0x7FFF). */
uint8_t gb_brain_read_rom_bank(uint8_t bank, uint16_t addr);

/* Pointer to the 160x144 RGB565 software LCD buffer produced by Peanut-GB. */
const uint16_t *gb_brain_get_lcd_rgb565(void);

#endif /* GB_BRAIN_H */
