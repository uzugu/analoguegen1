#include "mod_manager.h"
#include "gb_brain.h"
#include "pokered_ram.h"
#include "renderer.h"

#include <stdio.h>

#define BOX_COLS   5
#define BOX_ROWS   4
#define PARTY_SLOTS 6

typedef struct {
    uint8_t     species;
    const char *short_name;
    uint16_t    tile_color;
    uint16_t    accent_color;
} gen1_species_visual_t;

/* Compact Gen 1 internal-ID table (pret/pokered constants/pokemon_constants.asm). */
static const gen1_species_visual_t s_species_table[] = {
    { 0x01u, "RHYDN",  RGB565(132, 138, 148), RGB565(200, 206, 214) },
    { 0x04u, "CLEFY",  RGB565(242, 178, 206), RGB565(252, 224, 236) },
    { 0x05u, "SPEAR",  RGB565(178, 118,  72), RGB565(232,  96,  80) },
    { 0x07u, "NIDOK",  RGB565(146,  92, 188), RGB565(216, 198, 232) },
    { 0x09u, "IVYSR",  RGB565( 64, 178, 136), RGB565(228,  96, 128) },
    { 0x0Eu, "GENGR",  RGB565(112,  76, 158), RGB565(232,  72,  86) },
    { 0x10u, "NIDOQ",  RGB565( 82, 144, 204), RGB565(234, 214, 168) },
    { 0x13u, "LAPRS",  RGB565( 78, 152, 222), RGB565(224, 226, 196) },
    { 0x14u, "ARCAN",  RGB565(228, 112,  52), RGB565(248, 226, 172) },
    { 0x15u, "MEW",    RGB565(244, 172, 208), RGB565(140, 214, 250) },
    { 0x16u, "GYARD",  RGB565( 56, 132, 222), RGB565(236, 208, 128) },
    { 0x19u, "GASTL",  RGB565( 96,  64, 132), RGB565(212, 198, 234) },
    { 0x1Au, "SCYTH",  RGB565( 96, 194,  92), RGB565(224, 236, 192) },
    { 0x1Cu, "BLAST",  RGB565( 64, 138, 216), RGB565(188, 140,  86) },
    { 0x21u, "GROWL",  RGB565(228, 118,  56), RGB565(246, 224, 172) },
    { 0x22u, "ONIX",   RGB565(142, 144, 152), RGB565(198, 202, 210) },
    { 0x24u, "PIDGY",  RGB565(198, 154,  96), RGB565(242, 226, 186) },
    { 0x26u, "KADAB",  RGB565(232, 192,  68), RGB565(172, 116,  68) },
    { 0x2Fu, "PSYDK",  RGB565(236, 202,  76), RGB565(238, 232, 214) },
    { 0x3Bu, "DIGLT",  RGB565(168, 118,  78), RGB565(232, 132, 144) },
    { 0x42u, "DRGNT",  RGB565(236, 156,  68), RGB565(116, 186, 142) },
    { 0x49u, "MOLTR",  RGB565(244, 132,  48), RGB565(252, 224,  76) },
    { 0x4Au, "ARTIC",  RGB565( 98, 182, 244), RGB565(224, 244, 255) },
    { 0x4Bu, "ZAPDS",  RGB565(248, 212,  52), RGB565( 48,  52,  64) },
    { 0x4Cu, "DITTO",  RGB565(196, 128, 214), RGB565(234, 188, 242) },
    { 0x4Du, "MEOWT",  RGB565(236, 218, 174), RGB565(236, 198,  68) },
    { 0x52u, "VULPX",  RGB565(212, 102,  62), RGB565(236, 168,  96) },
    { 0x54u, "PIKA",   RGB565(248, 208,  48), RGB565(224,  64,  48) },
    { 0x55u, "RAICH",  RGB565(236, 152,  48), RGB565(246, 226, 146) },
    { 0x58u, "DRATN",  RGB565(108, 146, 224), RGB565(232, 240, 252) },
    { 0x64u, "JIGGY",  RGB565(246, 178, 206), RGB565( 88, 198, 176) },
    { 0x66u, "EEVEE",  RGB565(184, 132,  82), RGB565(238, 218, 176) },
    { 0x67u, "FLARN",  RGB565(232,  96,  48), RGB565(246, 216, 128) },
    { 0x68u, "JOLTN",  RGB565(246, 212,  56), RGB565(244, 244, 232) },
    { 0x69u, "VAPRN",  RGB565( 82, 168, 234), RGB565(212, 238, 252) },
    { 0x6Bu, "ZUBAT",  RGB565(102, 112, 206), RGB565(198, 118, 198) },
    { 0x70u, "WEEDL",  RGB565(196, 152,  88), RGB565(232, 142, 162) },
    { 0x7Bu, "CATER",  RGB565( 88, 198,  84), RGB565(240, 180,  56) },
    { 0x7Du, "BUTTR",  RGB565(142, 118, 214), RGB565(238, 242, 250) },
    { 0x7Eu, "MACHM",  RGB565(132, 156, 182), RGB565(226, 202,  86) },
    { 0x83u, "MEWTW",  RGB565(196, 182, 214), RGB565(166,  92, 204) },
    { 0x84u, "SNORL",  RGB565( 64, 124, 144), RGB565(236, 222, 186) },
    { 0x85u, "MKARP",  RGB565(224,  88,  64), RGB565(244, 212,  88) },
    { 0x94u, "ABRA",   RGB565(238, 204,  76), RGB565(162, 122,  86) },
    { 0x95u, "ALAKZ",  RGB565(238, 198,  68), RGB565(156, 112,  74) },
    { 0x96u, "PIDGO",  RGB565(204, 148,  92), RGB565(234, 112,  92) },
    { 0x97u, "PIDGT",  RGB565(212, 156,  96), RGB565(244, 206,  86) },
    { 0x99u, "BULBA",  RGB565( 82, 194, 132), RGB565( 64, 152,  76) },
    { 0x9Au, "VENUS",  RGB565( 68, 172, 122), RGB565(228,  86, 122) },
    { 0xA5u, "RATTA",  RGB565(162,  96, 202), RGB565(236, 222, 180) },
    { 0xA6u, "RATIC",  RGB565(186, 138,  84), RGB565(238, 222, 184) },
    { 0xA9u, "GEODU",  RGB565(148, 138, 126), RGB565(198, 190, 178) },
    { 0xADu, "MAGNM",  RGB565(172, 188, 204), RGB565(224,  78,  78) },
    { 0xB0u, "CHARM",  RGB565(236, 116,  52), RGB565(248, 214,  84) },
    { 0xB1u, "SQUIR",  RGB565( 84, 174, 224), RGB565(186, 134,  82) },
    { 0xB2u, "CHARL",  RGB565(222,  72,  52), RGB565(246, 208,  78) },
    { 0xB3u, "WARTR",  RGB565( 74, 152, 224), RGB565(222, 232, 244) },
    { 0xB4u, "CHARZ",  RGB565(234, 122,  48), RGB565( 72, 168, 176) },
    { 0xB9u, "ODDIS",  RGB565( 72, 108, 212), RGB565( 72, 204,  88) }
};

static bool s_in_party      = false;
static int  s_box_col       = 0;
static int  s_box_row       = 0;
static int  s_party_row     = 0;
static bool s_marked_active = false;
static bool s_marked_party  = false;
static int  s_marked_idx    = 0;

static bool species_is_empty(uint8_t sp) {
    return (sp == 0x00u || sp == 0xFFu);
}

static void species_visual_lookup(uint8_t sp, char *out_label, size_t label_sz,
                                  uint16_t *out_tile, uint16_t *out_accent) {
    if (species_is_empty(sp)) {
        snprintf(out_label, label_sz, "--");
        *out_tile   = RGB565(18, 46, 48);
        *out_accent = RGB565(42, 92, 88);
        return;
    }

    const size_t n = sizeof(s_species_table) / sizeof(s_species_table[0]);
    for (size_t i = 0; i < n; i++) {
        if (s_species_table[i].species == sp) {
            snprintf(out_label, label_sz, "%s", s_species_table[i].short_name);
            *out_tile   = s_species_table[i].tile_color;
            *out_accent = s_species_table[i].accent_color;
            return;
        }
    }

    snprintf(out_label, label_sz, "#%02X", (unsigned)sp);
    *out_tile   = RGB565( 64, 168, 152);
    *out_accent = RGB565(168, 234, 218);
}

static void sync_party_wram_and_snap(app_state_t *state, uint8_t party_count,
                                     const uint8_t party_sp[PARTY_SLOTS]) {
    gb_brain_write8(GB_ADDR_PARTY_COUNT, party_count);
    state->snap.party_count = party_count;
    for (int i = 0; i < PARTY_SLOTS; i++) {
        gb_brain_write8((uint16_t)(GB_ADDR_PARTY_SPECIES + (uint16_t)i), party_sp[i]);
        state->snap.party_species[i] = party_sp[i];
    }
    state->snap.lead_species = (party_count > 0u) ? party_sp[0] : 0u;
}

static void sync_box_slot_and_count(app_state_t *state, int idx, uint8_t sp,
                                    const uint8_t box_sp[APP_MAX_BOX_MONS]) {
    gb_brain_write8((uint16_t)(GB_ADDR_BOX_SPECIES + (uint16_t)idx), sp);
    state->snap.box_species[idx] = sp;

    uint8_t cnt = 0u;
    for (int i = 0; i < APP_MAX_BOX_MONS; i++) {
        if (!species_is_empty(box_sp[i])) {
            cnt++;
        }
    }
    gb_brain_write8(GB_ADDR_BOX_COUNT, cnt);
    state->snap.box_count = cnt;
}

static bool slot_is_occupied_wram(bool is_party, int idx) {
    if (is_party) {
        if (idx < 0 || idx >= PARTY_SLOTS) {
            return false;
        }
        uint8_t party_count = gb_brain_read8(GB_ADDR_PARTY_COUNT);
        if (party_count > PARTY_SLOTS) {
            party_count = PARTY_SLOTS;
        }
        if (idx >= (int)party_count) {
            return false;
        }
        uint8_t sp = gb_brain_read8((uint16_t)(GB_ADDR_PARTY_SPECIES + (uint16_t)idx));
        return !species_is_empty(sp);
    }

    if (idx < 0 || idx >= APP_MAX_BOX_MONS) {
        return false;
    }
    uint8_t sp = gb_brain_read8((uint16_t)(GB_ADDR_BOX_SPECIES + (uint16_t)idx));
    return !species_is_empty(sp);
}

static void gen3box_perform_swap(app_state_t *state,
                                 bool src_party, int src_idx,
                                 bool dst_party, int dst_idx) {
    uint8_t party_count = gb_brain_read8(GB_ADDR_PARTY_COUNT);
    if (party_count > PARTY_SLOTS) {
        party_count = PARTY_SLOTS;
    }

    uint8_t party_sp[PARTY_SLOTS];
    for (int i = 0; i < PARTY_SLOTS; i++) {
        party_sp[i] = gb_brain_read8((uint16_t)(GB_ADDR_PARTY_SPECIES + (uint16_t)i));
    }

    uint8_t box_sp[APP_MAX_BOX_MONS];
    for (int i = 0; i < APP_MAX_BOX_MONS; i++) {
        box_sp[i] = gb_brain_read8((uint16_t)(GB_ADDR_BOX_SPECIES + (uint16_t)i));
    }

    if (!src_party && !dst_party) {
        if (src_idx < 0 || src_idx >= APP_MAX_BOX_MONS ||
            dst_idx < 0 || dst_idx >= APP_MAX_BOX_MONS ||
            species_is_empty(box_sp[src_idx])) {
            return;
        }
        uint8_t tmp = box_sp[src_idx];
        box_sp[src_idx] = species_is_empty(box_sp[dst_idx]) ? 0xFFu : box_sp[dst_idx];
        box_sp[dst_idx] = tmp;

        gb_brain_write8((uint16_t)(GB_ADDR_BOX_SPECIES + (uint16_t)src_idx), box_sp[src_idx]);
        state->snap.box_species[src_idx] = box_sp[src_idx];
        sync_box_slot_and_count(state, dst_idx, box_sp[dst_idx], box_sp);
        return;
    }

    if (src_party && dst_party) {
        if (src_idx < 0 || src_idx >= (int)party_count ||
            dst_idx < 0 || dst_idx >= (int)party_count ||
            species_is_empty(party_sp[src_idx]) ||
            species_is_empty(party_sp[dst_idx])) {
            return;
        }
        uint8_t tmp = party_sp[src_idx];
        party_sp[src_idx] = party_sp[dst_idx];
        party_sp[dst_idx] = tmp;

        sync_party_wram_and_snap(state, party_count, party_sp);
        return;
    }

    int p_idx = src_party ? src_idx : dst_idx;
    int b_idx = src_party ? dst_idx : src_idx;
    if (p_idx < 0 || p_idx >= PARTY_SLOTS || b_idx < 0 || b_idx >= APP_MAX_BOX_MONS) {
        return;
    }

    bool p_occ = (p_idx < (int)party_count) && !species_is_empty(party_sp[p_idx]);
    bool b_occ = !species_is_empty(box_sp[b_idx]);

    if (p_occ && b_occ) {
        uint8_t tmp = party_sp[p_idx];
        party_sp[p_idx] = box_sp[b_idx];
        box_sp[b_idx]  = tmp;

        sync_party_wram_and_snap(state, party_count, party_sp);
        sync_box_slot_and_count(state, b_idx, box_sp[b_idx], box_sp);
    } else if (p_occ && !b_occ) {
        if (party_count <= 1u) {
            return;
        }
        box_sp[b_idx] = party_sp[p_idx];
        for (int i = p_idx; i < (int)party_count - 1; i++) {
            party_sp[i] = party_sp[i + 1];
        }
        party_count--;
        for (int i = (int)party_count; i < PARTY_SLOTS; i++) {
            party_sp[i] = 0xFFu;
        }

        sync_party_wram_and_snap(state, party_count, party_sp);
        sync_box_slot_and_count(state, b_idx, box_sp[b_idx], box_sp);
    } else if (!p_occ && b_occ) {
        if (party_count >= PARTY_SLOTS) {
            return;
        }
        party_sp[party_count] = box_sp[b_idx];
        party_count++;
        for (int i = (int)party_count; i < PARTY_SLOTS; i++) {
            party_sp[i] = 0xFFu;
        }
        box_sp[b_idx] = 0xFFu;

        sync_party_wram_and_snap(state, party_count, party_sp);
        sync_box_slot_and_count(state, b_idx, box_sp[b_idx], box_sp);
    }
}

static void gen3box_init(app_state_t *state) {
    (void)state;
    s_in_party      = false;
    s_box_col       = 0;
    s_box_row       = 0;
    s_party_row     = 0;
    s_marked_active = false;
    s_marked_party  = false;
    s_marked_idx    = 0;
}

void mod_gen3box_input(app_state_t *state, bool up, bool down, bool left, bool right, bool action, bool close) {
    if (state == NULL) {
        return;
    }

    if (close) {
        state->gen3box_open = false;
        return;
    }

    if (s_in_party) {
        if (up) {
            s_party_row = (s_party_row + PARTY_SLOTS - 1) % PARTY_SLOTS;
        }
        if (down) {
            s_party_row = (s_party_row + 1) % PARTY_SLOTS;
        }
        if (right) {
            s_in_party = false;
            s_box_col  = 0;
            s_box_row  = (s_party_row < BOX_ROWS) ? s_party_row : (BOX_ROWS - 1);
        } else if (left) {
            s_in_party = false;
            s_box_col  = BOX_COLS - 1;
            s_box_row  = (s_party_row < BOX_ROWS) ? s_party_row : (BOX_ROWS - 1);
        }
    } else {
        if (up) {
            s_box_row = (s_box_row + BOX_ROWS - 1) % BOX_ROWS;
        }
        if (down) {
            s_box_row = (s_box_row + 1) % BOX_ROWS;
        }
        if (left) {
            if (s_box_col > 0) {
                s_box_col--;
            } else {
                s_in_party  = true;
                s_party_row = s_box_row;
            }
        }
        if (right) {
            if (s_box_col < BOX_COLS - 1) {
                s_box_col++;
            } else {
                s_in_party  = true;
                s_party_row = s_box_row;
            }
        }
    }

    if (action) {
        int cur_idx = s_in_party ? s_party_row : (s_box_row * BOX_COLS + s_box_col);
        if (s_marked_active) {
            if (s_marked_party == s_in_party && s_marked_idx == cur_idx) {
                s_marked_active = false;
            } else {
                gen3box_perform_swap(state, s_marked_party, s_marked_idx, s_in_party, cur_idx);
                s_marked_active = false;
            }
        } else if (slot_is_occupied_wram(s_in_party, cur_idx)) {
            s_marked_active = true;
            s_marked_party  = s_in_party;
            s_marked_idx    = cur_idx;
        }
    }
}

void mod_gen3box_render(const app_state_t *state) {
    if (state == NULL || !state->gen3box_open) {
        return;
    }

    const uint16_t col_bg         = RGB565(  8,  28,  30);
    const uint16_t col_header     = RGB565( 14,  66,  60);
    const uint16_t col_panel      = RGB565( 12,  42,  44);
    const uint16_t col_border     = RGB565( 64, 196, 160);
    const uint16_t col_slot_bg    = RGB565( 10,  32,  34);
    const uint16_t col_slot_edge  = RGB565( 38,  96,  92);
    const uint16_t col_cursor     = RGB565(255, 224,  72);
    const uint16_t col_marked     = RGB565(112, 232, 255);
    const uint16_t col_text       = RGB565(236, 248, 244);
    const uint16_t col_dim        = RGB565(142, 198, 186);

    /* Full-screen 320x240 dark-teal / emerald backdrop */
    renderer_fill_rect(0, 0, RENDER_W, RENDER_H, col_bg);
    renderer_draw_rect(0, 0, RENDER_W, RENDER_H, col_border);

    /* Header bar */
    renderer_fill_rect(1, 1, RENDER_W - 2, 22, col_header);
    renderer_draw_rect(0, 23, RENDER_W, 1, col_border);

    unsigned box_num   = (unsigned)(state->snap.cur_box_num + 1u);
    unsigned box_count = (unsigned)state->snap.box_count;
    if (box_count > APP_MAX_BOX_MONS) {
        box_count = APP_MAX_BOX_MONS;
    }

    char header_buf[32];
    snprintf(header_buf, sizeof(header_buf), "BOX %u (%u/20)", box_num, box_count);
    renderer_draw_text(8, 8, header_buf, col_text, col_header, false);

    /* Selected slot label on right side of header */
    uint8_t cur_species = 0u;
    if (s_in_party) {
        if (s_party_row >= 0 && s_party_row < PARTY_SLOTS &&
            (uint8_t)s_party_row < state->snap.party_count) {
            cur_species = state->snap.party_species[s_party_row];
        }
    } else {
        int idx = s_box_row * BOX_COLS + s_box_col;
        if (idx >= 0 && idx < APP_MAX_BOX_MONS) {
            cur_species = state->snap.box_species[idx];
        }
    }

    char sel_name[12];
    uint16_t sel_tile = 0u;
    uint16_t sel_acc  = 0u;
    species_visual_lookup(cur_species, sel_name, sizeof(sel_name), &sel_tile, &sel_acc);
    char sel_buf[24];
    snprintf(sel_buf, sizeof(sel_buf), "%s:%s", s_in_party ? "PTY" : "BOX", sel_name);
    renderer_draw_text(RENDER_W - 84, 8, sel_buf, col_cursor, col_header, false);

    /* Left Party Strip (6 slots) */
    const int p_panel_x = 6;
    const int p_panel_y = 28;
    const int p_panel_w = 78;
    const int p_panel_h = 188;
    renderer_fill_rect(p_panel_x, p_panel_y, p_panel_w, p_panel_h, col_panel);
    renderer_draw_rect(p_panel_x, p_panel_y, p_panel_w, p_panel_h, col_border);

    unsigned pty_count = (unsigned)state->snap.party_count;
    if (pty_count > PARTY_SLOTS) {
        pty_count = PARTY_SLOTS;
    }
    char pty_hdr[16];
    snprintf(pty_hdr, sizeof(pty_hdr), "PARTY (%u)", pty_count);
    renderer_draw_text(p_panel_x + 5, p_panel_y + 5, pty_hdr, col_dim, col_panel, false);

    for (int i = 0; i < PARTY_SLOTS; i++) {
        const int sx = p_panel_x + 4;
        const int sy = p_panel_y + 16 + i * 28;
        const int sw = p_panel_w - 8;
        const int sh = 25;

        uint8_t sp = ((unsigned)i < pty_count) ? state->snap.party_species[i] : 0u;
        bool is_cursor = (s_in_party && s_party_row == i);
        bool is_marked = (s_marked_active && s_marked_party && s_marked_idx == i);

        char sp_name[12];
        uint16_t tile_col = 0u;
        uint16_t acc_col  = 0u;
        species_visual_lookup(sp, sp_name, sizeof(sp_name), &tile_col, &acc_col);

        renderer_fill_rect(sx, sy, sw, sh, col_slot_bg);
        renderer_draw_rect(sx, sy, sw, sh, is_marked ? col_marked : col_slot_edge);

        if (!species_is_empty(sp)) {
            renderer_fill_rect(sx + 3, sy + 4, 14, 17, tile_col);
            renderer_fill_rect(sx + 3, sy + 4, 14,  4, acc_col);
            renderer_draw_rect(sx + 3, sy + 4, 14, 17, col_text);
            renderer_draw_text(sx + 20, sy + 9, sp_name, col_text, col_slot_bg, false);
        } else {
            renderer_fill_rect(sx + 6, sy + 9, 8, 7, tile_col);
            renderer_draw_text(sx + 20, sy + 9, "--", col_dim, col_slot_bg, false);
        }

        if (is_cursor) {
            renderer_draw_rect(sx, sy, sw, sh, col_cursor);
            renderer_draw_rect(sx + 1, sy + 1, sw - 2, sh - 2, col_cursor);
        }
    }

    /* Right 5x4 PC Box Grid (20 slots) */
    const int b_panel_x = 88;
    const int b_panel_y = 28;
    const int b_panel_w = 226;
    const int b_panel_h = 188;
    renderer_fill_rect(b_panel_x, b_panel_y, b_panel_w, b_panel_h, col_panel);
    renderer_draw_rect(b_panel_x, b_panel_y, b_panel_w, b_panel_h, col_border);

    const int grid_x0 = b_panel_x + 4;
    const int grid_y0 = b_panel_y + 6;
    const int slot_w  = 42;
    const int slot_h  = 42;
    const int step_x  = 44;
    const int step_y  = 45;

    for (int r = 0; r < BOX_ROWS; r++) {
        for (int c = 0; c < BOX_COLS; c++) {
            const int idx = r * BOX_COLS + c;
            const int sx  = grid_x0 + c * step_x;
            const int sy  = grid_y0 + r * step_y;

            uint8_t sp = state->snap.box_species[idx];
            bool is_cursor = (!s_in_party && s_box_row == r && s_box_col == c);
            bool is_marked = (s_marked_active && !s_marked_party && s_marked_idx == idx);

            char sp_name[12];
            uint16_t tile_col = 0u;
            uint16_t acc_col  = 0u;
            species_visual_lookup(sp, sp_name, sizeof(sp_name), &tile_col, &acc_col);

            renderer_fill_rect(sx, sy, slot_w, slot_h, col_slot_bg);
            renderer_draw_rect(sx, sy, slot_w, slot_h, is_marked ? col_marked : col_slot_edge);

            if (!species_is_empty(sp)) {
                /* Colored species icon tile + accent crown */
                renderer_fill_rect(sx + 9, sy + 5, 24, 20, tile_col);
                renderer_fill_rect(sx + 9, sy + 5, 24,  5, acc_col);
                renderer_draw_rect(sx + 9, sy + 5, 24, 20, col_text);

                int name_len = 0;
                while (sp_name[name_len] != '\0' && name_len < 6) {
                    name_len++;
                }
                int tx = sx + (slot_w - name_len * 6) / 2;
                renderer_draw_text(tx, sy + 30, sp_name, col_text, col_slot_bg, false);
            } else {
                renderer_fill_rect(sx + 16, sy + 12, 10, 8, tile_col);
                renderer_draw_text(sx + 15, sy + 29, "--", col_dim, col_slot_bg, false);
            }

            if (is_cursor) {
                renderer_draw_rect(sx, sy, slot_w, slot_h, col_cursor);
                renderer_draw_rect(sx + 1, sy + 1, slot_w - 2, slot_h - 2, col_cursor);
            }
        }
    }

    /* Footer bar */
    const int foot_y = RENDER_H - 20;
    renderer_fill_rect(1, foot_y, RENDER_W - 2, 19, col_header);
    renderer_draw_rect(0, foot_y - 1, RENDER_W, 1, col_border);
    renderer_draw_text(8, foot_y + 6, "D-PAD:Move  A:Pick/Swap  B/R+SEL:Close", col_dim, col_header, false);
}

void mod_gen3box_register(void) {
    static const mod_descriptor_t desc = {
        .id              = "gen3_box",
        .name            = "Gen 3 Box",
        .category        = "UI",
        .description     = "Visual 5x4 Gen 3 PC Box & Party grid (R+SELECT)",
        .enabled         = true,
        .default_enabled = true,
        .init            = gen3box_init,
        .on_frame        = NULL,
        .on_render_3d    = NULL,
        .on_render_hud   = NULL
    };
    mod_manager_register(&desc);
}
