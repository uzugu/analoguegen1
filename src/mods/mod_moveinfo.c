#include "mod_manager.h"
#include "pokered_ram.h"
#include "renderer.h"

#include <stdio.h>

typedef struct {
    uint8_t move_id;
    uint8_t type_id;
} gen1_move_type_t;

/* Common Gen 1 moves (1-based pret/pokered constants/move_constants.asm).
 * Note Gen 1 specific types: Gust, Bite, Karate Chop, Sand-Attack are Normal. */
static const gen1_move_type_t s_gen1_moves[] = {
    {   1, GB_TYPE_NORMAL   }, /* POUND */
    {   2, GB_TYPE_NORMAL   }, /* KARATE_CHOP */
    {   3, GB_TYPE_NORMAL   }, /* DOUBLESLAP */
    {   4, GB_TYPE_NORMAL   }, /* COMET_PUNCH */
    {   5, GB_TYPE_NORMAL   }, /* MEGA_PUNCH */
    {   6, GB_TYPE_NORMAL   }, /* PAY_DAY */
    {   7, GB_TYPE_FIRE     }, /* FIRE_PUNCH */
    {   8, GB_TYPE_ICE      }, /* ICE_PUNCH */
    {   9, GB_TYPE_ELECTRIC }, /* THUNDERPUNCH */
    {  10, GB_TYPE_NORMAL   }, /* SCRATCH */
    {  11, GB_TYPE_NORMAL   }, /* VICEGRIP */
    {  12, GB_TYPE_NORMAL   }, /* GUILLOTINE */
    {  13, GB_TYPE_NORMAL   }, /* RAZOR_WIND */
    {  14, GB_TYPE_NORMAL   }, /* SWORDS_DANCE */
    {  15, GB_TYPE_NORMAL   }, /* CUT */
    {  16, GB_TYPE_NORMAL   }, /* GUST */
    {  17, GB_TYPE_FLYING   }, /* WING_ATTACK */
    {  18, GB_TYPE_NORMAL   }, /* WHIRLWIND */
    {  19, GB_TYPE_FLYING   }, /* FLY */
    {  20, GB_TYPE_NORMAL   }, /* BIND */
    {  21, GB_TYPE_NORMAL   }, /* SLAM */
    {  22, GB_TYPE_GRASS    }, /* VINE_WHIP */
    {  23, GB_TYPE_NORMAL   }, /* STOMP */
    {  24, GB_TYPE_FIGHTING }, /* DOUBLE_KICK */
    {  25, GB_TYPE_NORMAL   }, /* MEGA_KICK */
    {  26, GB_TYPE_FIGHTING }, /* JUMP_KICK */
    {  27, GB_TYPE_FIGHTING }, /* ROLLING_KICK */
    {  28, GB_TYPE_NORMAL   }, /* SAND_ATTACK */
    {  29, GB_TYPE_NORMAL   }, /* HEADBUTT */
    {  30, GB_TYPE_NORMAL   }, /* HORN_ATTACK */
    {  31, GB_TYPE_NORMAL   }, /* FURY_ATTACK */
    {  32, GB_TYPE_NORMAL   }, /* HORN_DRILL */
    {  33, GB_TYPE_NORMAL   }, /* TACKLE */
    {  34, GB_TYPE_NORMAL   }, /* BODY_SLAM */
    {  35, GB_TYPE_NORMAL   }, /* WRAP */
    {  36, GB_TYPE_NORMAL   }, /* TAKE_DOWN */
    {  37, GB_TYPE_NORMAL   }, /* THRASH */
    {  38, GB_TYPE_NORMAL   }, /* DOUBLE_EDGE */
    {  39, GB_TYPE_NORMAL   }, /* TAIL_WHIP */
    {  40, GB_TYPE_POISON   }, /* POISON_STING */
    {  41, GB_TYPE_BUG      }, /* TWINEEDLE */
    {  42, GB_TYPE_BUG      }, /* PIN_MISSILE */
    {  43, GB_TYPE_NORMAL   }, /* LEER */
    {  44, GB_TYPE_NORMAL   }, /* BITE */
    {  45, GB_TYPE_NORMAL   }, /* GROWL */
    {  46, GB_TYPE_NORMAL   }, /* ROAR */
    {  47, GB_TYPE_NORMAL   }, /* SING */
    {  48, GB_TYPE_NORMAL   }, /* SUPERSONIC */
    {  49, GB_TYPE_NORMAL   }, /* SONICBOOM */
    {  50, GB_TYPE_NORMAL   }, /* DISABLE */
    {  51, GB_TYPE_POISON   }, /* ACID */
    {  52, GB_TYPE_FIRE     }, /* EMBER */
    {  53, GB_TYPE_FIRE     }, /* FLAMETHROWER */
    {  54, GB_TYPE_ICE      }, /* MIST */
    {  55, GB_TYPE_WATER    }, /* WATER_GUN */
    {  56, GB_TYPE_WATER    }, /* HYDRO_PUMP */
    {  57, GB_TYPE_WATER    }, /* SURF */
    {  58, GB_TYPE_ICE      }, /* ICE_BEAM */
    {  59, GB_TYPE_ICE      }, /* BLIZZARD */
    {  60, GB_TYPE_PSYCHIC  }, /* PSYBEAM */
    {  61, GB_TYPE_WATER    }, /* BUBBLEBEAM */
    {  62, GB_TYPE_ICE      }, /* AURORA_BEAM */
    {  63, GB_TYPE_NORMAL   }, /* HYPER_BEAM */
    {  64, GB_TYPE_FLYING   }, /* PECK */
    {  65, GB_TYPE_FLYING   }, /* DRILL_PECK */
    {  66, GB_TYPE_FIGHTING }, /* SUBMISSION */
    {  67, GB_TYPE_FIGHTING }, /* LOW_KICK */
    {  68, GB_TYPE_FIGHTING }, /* COUNTER */
    {  69, GB_TYPE_FIGHTING }, /* SEISMIC_TOSS */
    {  70, GB_TYPE_NORMAL   }, /* STRENGTH */
    {  71, GB_TYPE_GRASS    }, /* ABSORB */
    {  72, GB_TYPE_GRASS    }, /* MEGA_DRAIN */
    {  73, GB_TYPE_GRASS    }, /* LEECH_SEED */
    {  74, GB_TYPE_NORMAL   }, /* GROWTH */
    {  75, GB_TYPE_GRASS    }, /* RAZOR_LEAF */
    {  76, GB_TYPE_GRASS    }, /* SOLARBEAM */
    {  77, GB_TYPE_POISON   }, /* POISONPOWDER */
    {  78, GB_TYPE_GRASS    }, /* STUN_SPORE */
    {  79, GB_TYPE_GRASS    }, /* SLEEP_POWDER */
    {  80, GB_TYPE_GRASS    }, /* PETAL_DANCE */
    {  81, GB_TYPE_BUG      }, /* STRING_SHOT */
    {  82, GB_TYPE_DRAGON   }, /* DRAGON_RAGE */
    {  83, GB_TYPE_FIRE     }, /* FIRE_SPIN */
    {  84, GB_TYPE_ELECTRIC }, /* THUNDERSHOCK */
    {  85, GB_TYPE_ELECTRIC }, /* THUNDERBOLT */
    {  86, GB_TYPE_ELECTRIC }, /* THUNDER_WAVE */
    {  87, GB_TYPE_ELECTRIC }, /* THUNDER */
    {  88, GB_TYPE_ROCK     }, /* ROCK_THROW */
    {  89, GB_TYPE_GROUND   }, /* EARTHQUAKE */
    {  90, GB_TYPE_GROUND   }, /* FISSURE */
    {  91, GB_TYPE_GROUND   }, /* DIG */
    {  92, GB_TYPE_POISON   }, /* TOXIC */
    {  93, GB_TYPE_PSYCHIC  }, /* CONFUSION */
    {  94, GB_TYPE_PSYCHIC  }, /* PSYCHIC_M */
    {  95, GB_TYPE_PSYCHIC  }, /* HYPNOSIS */
    {  96, GB_TYPE_PSYCHIC  }, /* MEDITATE */
    {  97, GB_TYPE_PSYCHIC  }, /* AGILITY */
    {  98, GB_TYPE_NORMAL   }, /* QUICK_ATTACK */
    {  99, GB_TYPE_NORMAL   }, /* RAGE */
    { 100, GB_TYPE_PSYCHIC  }, /* TELEPORT */
    { 101, GB_TYPE_GHOST    }, /* NIGHT_SHADE */
    { 102, GB_TYPE_NORMAL   }, /* MIMIC */
    { 103, GB_TYPE_NORMAL   }, /* SCREECH */
    { 104, GB_TYPE_NORMAL   }, /* DOUBLE_TEAM */
    { 105, GB_TYPE_NORMAL   }, /* RECOVER */
    { 106, GB_TYPE_NORMAL   }, /* HARDEN */
    { 107, GB_TYPE_NORMAL   }, /* MINIMIZE */
    { 108, GB_TYPE_NORMAL   }, /* SMOKESCREEN */
    { 109, GB_TYPE_GHOST    }, /* CONFUSE_RAY */
    { 110, GB_TYPE_WATER    }, /* WITHDRAW */
    { 111, GB_TYPE_NORMAL   }, /* DEFENSE_CURL */
    { 112, GB_TYPE_PSYCHIC  }, /* BARRIER */
    { 113, GB_TYPE_PSYCHIC  }, /* LIGHT_SCREEN */
    { 114, GB_TYPE_ICE      }, /* HAZE */
    { 115, GB_TYPE_PSYCHIC  }, /* REFLECT */
    { 116, GB_TYPE_NORMAL   }, /* FOCUS_ENERGY */
    { 117, GB_TYPE_NORMAL   }, /* BIDE */
    { 118, GB_TYPE_NORMAL   }, /* METRONOME */
    { 119, GB_TYPE_FLYING   }, /* MIRROR_MOVE */
    { 120, GB_TYPE_NORMAL   }, /* SELFDESTRUCT */
    { 121, GB_TYPE_NORMAL   }, /* EGG_BOMB */
    { 122, GB_TYPE_GHOST    }, /* LICK */
    { 123, GB_TYPE_POISON   }, /* SMOG */
    { 124, GB_TYPE_POISON   }, /* SLUDGE */
    { 125, GB_TYPE_GROUND   }, /* BONE_CLUB */
    { 126, GB_TYPE_FIRE     }, /* FIRE_BLAST */
    { 127, GB_TYPE_WATER    }, /* WATERFALL */
    { 128, GB_TYPE_WATER    }, /* CLAMP */
    { 129, GB_TYPE_NORMAL   }, /* SWIFT */
    { 130, GB_TYPE_NORMAL   }, /* SKULL_BASH */
    { 131, GB_TYPE_NORMAL   }, /* SPIKE_CANNON */
    { 132, GB_TYPE_NORMAL   }, /* CONSTRICT */
    { 133, GB_TYPE_PSYCHIC  }, /* AMNESIA */
    { 134, GB_TYPE_PSYCHIC  }, /* KINESIS */
    { 135, GB_TYPE_NORMAL   }, /* SOFTBOILED */
    { 136, GB_TYPE_FIGHTING }, /* HI_JUMP_KICK */
    { 137, GB_TYPE_NORMAL   }, /* GLARE */
    { 138, GB_TYPE_PSYCHIC  }, /* DREAM_EATER */
    { 139, GB_TYPE_POISON   }, /* POISON_GAS */
    { 140, GB_TYPE_NORMAL   }, /* BARRAGE */
    { 141, GB_TYPE_BUG      }, /* LEECH_LIFE */
    { 142, GB_TYPE_NORMAL   }, /* LOVELY_KISS */
    { 143, GB_TYPE_FLYING   }, /* SKY_ATTACK */
    { 144, GB_TYPE_NORMAL   }, /* TRANSFORM */
    { 145, GB_TYPE_WATER    }, /* BUBBLE */
    { 146, GB_TYPE_NORMAL   }, /* DIZZY_PUNCH */
    { 147, GB_TYPE_GRASS    }, /* SPORE */
    { 148, GB_TYPE_NORMAL   }, /* FLASH */
    { 149, GB_TYPE_PSYCHIC  }, /* PSYWAVE */
    { 150, GB_TYPE_NORMAL   }, /* SPLASH */
    { 151, GB_TYPE_POISON   }, /* ACID_ARMOR */
    { 152, GB_TYPE_WATER    }, /* CRABHAMMER */
    { 153, GB_TYPE_NORMAL   }, /* EXPLOSION */
    { 154, GB_TYPE_NORMAL   }, /* FURY_SWIPES */
    { 155, GB_TYPE_GROUND   }, /* BONEMERANG */
    { 156, GB_TYPE_PSYCHIC  }, /* REST */
    { 157, GB_TYPE_ROCK     }, /* ROCK_SLIDE */
    { 158, GB_TYPE_NORMAL   }, /* HYPER_FANG */
    { 159, GB_TYPE_NORMAL   }, /* SHARPEN */
    { 160, GB_TYPE_NORMAL   }, /* CONVERSION */
    { 161, GB_TYPE_NORMAL   }, /* TRI_ATTACK */
    { 162, GB_TYPE_NORMAL   }, /* SUPER_FANG */
    { 163, GB_TYPE_NORMAL   }, /* SLASH */
    { 164, GB_TYPE_NORMAL   }, /* SUBSTITUTE */
    { 165, GB_TYPE_NORMAL   }  /* STRUGGLE */
};

static uint8_t lookup_move_type(uint8_t move_id) {
    if (move_id == 0) {
        return 0xFF;
    }
    size_t count = sizeof(s_gen1_moves) / sizeof(s_gen1_moves[0]);
    for (size_t i = 0; i < count; i++) {
        if (s_gen1_moves[i].move_id == move_id) {
            return s_gen1_moves[i].type_id;
        }
    }
    return 0xFF;
}

static const char *type_abbr(uint8_t type_id) {
    switch (type_id) {
    case GB_TYPE_NORMAL:   return "NRM";
    case GB_TYPE_FIGHTING: return "FGT";
    case GB_TYPE_FLYING:   return "FLY";
    case GB_TYPE_POISON:   return "PSN";
    case GB_TYPE_GROUND:   return "GRN";
    case GB_TYPE_ROCK:     return "RCK";
    case GB_TYPE_BUG:      return "BUG";
    case GB_TYPE_GHOST:    return "GHO";
    case GB_TYPE_FIRE:     return "FIR";
    case GB_TYPE_WATER:    return "WTR";
    case GB_TYPE_GRASS:    return "GRS";
    case GB_TYPE_ELECTRIC: return "ELC";
    case GB_TYPE_PSYCHIC:  return "PSY";
    case GB_TYPE_ICE:      return "ICE";
    case GB_TYPE_DRAGON:   return "DRG";
    default:               return "???";
    }
}

/* Returns Gen 1 single-type effectiveness in halves: 0=0x, 1=0.5x, 2=1x, 4=2x. */
static int gen1_single_eff_half(uint8_t atk, uint8_t def) {
    switch (atk) {
    case GB_TYPE_NORMAL:
        if (def == GB_TYPE_ROCK)  return 1;
        if (def == GB_TYPE_GHOST) return 0;
        return 2;
    case GB_TYPE_FIGHTING:
        if (def == GB_TYPE_NORMAL || def == GB_TYPE_ROCK || def == GB_TYPE_ICE) return 4;
        if (def == GB_TYPE_FLYING || def == GB_TYPE_POISON ||
            def == GB_TYPE_BUG    || def == GB_TYPE_PSYCHIC) return 1;
        if (def == GB_TYPE_GHOST) return 0;
        return 2;
    case GB_TYPE_FLYING:
        if (def == GB_TYPE_FIGHTING || def == GB_TYPE_BUG || def == GB_TYPE_GRASS) return 4;
        if (def == GB_TYPE_ROCK || def == GB_TYPE_ELECTRIC) return 1;
        return 2;
    case GB_TYPE_POISON:
        if (def == GB_TYPE_GRASS || def == GB_TYPE_BUG) return 4; /* Gen 1: Poison -> Bug 2x */
        if (def == GB_TYPE_POISON || def == GB_TYPE_GROUND ||
            def == GB_TYPE_ROCK   || def == GB_TYPE_GHOST) return 1;
        return 2;
    case GB_TYPE_GROUND:
        if (def == GB_TYPE_POISON || def == GB_TYPE_ROCK ||
            def == GB_TYPE_FIRE   || def == GB_TYPE_ELECTRIC) return 4;
        if (def == GB_TYPE_BUG || def == GB_TYPE_GRASS) return 1;
        if (def == GB_TYPE_FLYING) return 0;
        return 2;
    case GB_TYPE_ROCK:
        if (def == GB_TYPE_FLYING || def == GB_TYPE_BUG ||
            def == GB_TYPE_FIRE   || def == GB_TYPE_ICE) return 4;
        if (def == GB_TYPE_FIGHTING || def == GB_TYPE_GROUND) return 1;
        return 2;
    case GB_TYPE_BUG:
        if (def == GB_TYPE_POISON || def == GB_TYPE_GRASS || def == GB_TYPE_PSYCHIC) return 4; /* Gen 1: Bug -> Poison 2x */
        if (def == GB_TYPE_FIGHTING || def == GB_TYPE_FLYING ||
            def == GB_TYPE_GHOST    || def == GB_TYPE_FIRE) return 1;
        return 2;
    case GB_TYPE_GHOST:
        if (def == GB_TYPE_GHOST) return 4;
        if (def == GB_TYPE_NORMAL || def == GB_TYPE_PSYCHIC) return 0; /* Gen 1: Ghost -> Psychic 0x */
        return 2;
    case GB_TYPE_FIRE:
        if (def == GB_TYPE_BUG || def == GB_TYPE_GRASS || def == GB_TYPE_ICE) return 4;
        if (def == GB_TYPE_ROCK || def == GB_TYPE_FIRE ||
            def == GB_TYPE_WATER || def == GB_TYPE_DRAGON) return 1;
        return 2;
    case GB_TYPE_WATER:
        if (def == GB_TYPE_GROUND || def == GB_TYPE_ROCK || def == GB_TYPE_FIRE) return 4;
        if (def == GB_TYPE_WATER || def == GB_TYPE_GRASS || def == GB_TYPE_DRAGON) return 1;
        return 2;
    case GB_TYPE_GRASS:
        if (def == GB_TYPE_GROUND || def == GB_TYPE_ROCK || def == GB_TYPE_WATER) return 4;
        if (def == GB_TYPE_FLYING || def == GB_TYPE_POISON || def == GB_TYPE_BUG ||
            def == GB_TYPE_FIRE   || def == GB_TYPE_GRASS  || def == GB_TYPE_DRAGON) return 1;
        return 2;
    case GB_TYPE_ELECTRIC:
        if (def == GB_TYPE_FLYING || def == GB_TYPE_WATER) return 4;
        if (def == GB_TYPE_GRASS || def == GB_TYPE_ELECTRIC || def == GB_TYPE_DRAGON) return 1;
        if (def == GB_TYPE_GROUND) return 0;
        return 2;
    case GB_TYPE_PSYCHIC:
        if (def == GB_TYPE_FIGHTING || def == GB_TYPE_POISON) return 4;
        if (def == GB_TYPE_PSYCHIC) return 1;
        return 2;
    case GB_TYPE_ICE:
        if (def == GB_TYPE_FLYING || def == GB_TYPE_GROUND ||
            def == GB_TYPE_GRASS  || def == GB_TYPE_DRAGON) return 4;
        if (def == GB_TYPE_WATER || def == GB_TYPE_ICE) return 1; /* Gen 1: Ice -> Fire 1x */
        return 2;
    case GB_TYPE_DRAGON:
        if (def == GB_TYPE_DRAGON) return 4;
        return 2;
    default:
        return 2;
    }
}

/* Returns combined effectiveness in quarters: 0, 1(1/4), 2(1/2), 4(1x), 8(2x), 16(4x). */
static int gen1_combined_eff_quarter(uint8_t atk, uint8_t def1, uint8_t def2) {
    int e1 = gen1_single_eff_half(atk, def1);
    if (def2 == def1) {
        return e1 * 2;
    }
    int e2 = gen1_single_eff_half(atk, def2);
    return e1 * e2;
}

static const char *eff_label(int eff_q) {
    switch (eff_q) {
    case 16: return "4x";
    case 8:  return "2x";
    case 4:  return "1x";
    case 2:  return "1/2";
    case 1:  return "1/4";
    case 0:  return "0x";
    default: return "?";
    }
}

static uint16_t eff_color(int eff_q) {
    if (eff_q >= 8) {
        return RGB565(90, 245, 120);
    }
    if (eff_q == 4) {
        return RGB565(225, 232, 245);
    }
    if (eff_q >= 1) {
        return RGB565(245, 175, 70);
    }
    return RGB565(245, 80, 80);
}

static void moveinfo_init(app_state_t *state) {
    (void)state;
}

static void moveinfo_on_frame(app_state_t *state) {
    (void)state;
}

static void moveinfo_on_render_3d(const app_state_t *state) {
    (void)state;
}

static void moveinfo_on_render_hud(const app_state_t *state) {
    if (state == NULL || !state->rom_loaded || state->snap.in_battle == 0) {
        return;
    }

    /* Right-side compact HUD panel next to centered 160x144 GB LCD (x=243..318) */
    const int x = 243;
    const int y = 48;
    const int w = RENDER_W - x - 2;
    const int h = 144;

    const uint16_t bg     = RGB565(18, 22, 36);
    const uint16_t border = RGB565(95, 155, 225);
    const uint16_t fg     = RGB565(235, 242, 252);
    const uint16_t dim    = RGB565(145, 168, 198);
    const uint16_t shiny  = RGB565(255, 220, 70);
    const uint16_t sel_bg = RGB565(36, 56, 94);

    renderer_fill_rect(x, y, w, h, bg);
    renderer_draw_rect(x, y, w, h, border);

    char buf[24];

    /* Enemy level */
    snprintf(buf, sizeof(buf), "FOE LV:%u", (unsigned)state->snap.enemy_level);
    renderer_draw_text(x + 4, y + 6, buf, fg, bg, false);

    /* Enemy type(s) */
    if (state->snap.enemy_type1 == state->snap.enemy_type2) {
        snprintf(buf, sizeof(buf), "%s", type_abbr(state->snap.enemy_type1));
    } else {
        snprintf(buf, sizeof(buf), "%s/%s",
                 type_abbr(state->snap.enemy_type1),
                 type_abbr(state->snap.enemy_type2));
    }
    renderer_draw_text(x + 4, y + 20, buf, RGB565(135, 210, 255), bg, false);

    /* Shiny indicator or battle kind */
    if (state->snap.enemy_shiny) {
        renderer_draw_text(x + 4, y + 34, "*SHINY*", shiny, bg, false);
    } else {
        renderer_draw_text(x + 4, y + 34,
                           state->snap.in_battle == 2 ? "TRAINER" : "WILD",
                           dim, bg, false);
    }

    renderer_draw_rect(x + 2, y + 48, w - 4, 1, RGB565(45, 70, 110));
    renderer_draw_text(x + 4, y + 54, "MOVES", dim, bg, false);

    for (int i = 0; i < 4; i++) {
        int row_y = y + 68 + i * 18;
        uint8_t move_id = state->snap.battle_moves[i];
        bool is_sel = (state->snap.menu_cursor == (uint8_t)i);
        uint16_t row_bg = is_sel ? sel_bg : bg;

        if (is_sel) {
            renderer_fill_rect(x + 2, row_y - 2, w - 4, 14, sel_bg);
        }

        if (move_id == 0) {
            snprintf(buf, sizeof(buf), "%d:--", i + 1);
            renderer_draw_text(x + 4, row_y, buf, dim, row_bg, false);
            continue;
        }

        uint8_t mtype = lookup_move_type(move_id);
        if (mtype == 0xFF) {
            snprintf(buf, sizeof(buf), "%d:?", i + 1);
            renderer_draw_text(x + 4, row_y, buf, fg, row_bg, false);
            renderer_draw_text(x + w - 24, row_y, "?", dim, row_bg, false);
        } else {
            int eff_q = gen1_combined_eff_quarter(mtype,
                                                  state->snap.enemy_type1,
                                                  state->snap.enemy_type2);
            snprintf(buf, sizeof(buf), "%d:%s", i + 1, type_abbr(mtype));
            renderer_draw_text(x + 4, row_y, buf, fg, row_bg, false);
            renderer_draw_text(x + w - 26, row_y, eff_label(eff_q),
                               eff_color(eff_q), row_bg, false);
        }
    }
}

void mod_moveinfo_register(void) {
    static const mod_descriptor_t desc = {
        .id              = "battle_move_info",
        .name            = "Battle Move Info",
        .category        = "GAMEPLAY",
        .description     = "Gen 1 type chart & shiny battle HUD",
        .enabled         = true,
        .default_enabled = true,
        .init            = moveinfo_init,
        .on_frame        = moveinfo_on_frame,
        .on_render_3d    = moveinfo_on_render_3d,
        .on_render_hud   = moveinfo_on_render_hud
    };
    mod_manager_register(&desc);
}
