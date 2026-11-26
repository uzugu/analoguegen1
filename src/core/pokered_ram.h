#ifndef POKERED_RAM_H
#define POKERED_RAM_H

/*
 * Verified Pokemon Red / Blue (US) WRAM & SRAM addresses from pret/pokered.
 * Yellow offsets differ slightly for a few fields and will be parameterized
 * by game version once Yellow detection is added in Phase 2.
 */

#include <stdint.h>

/* Overworld & map state (WRAM: 0xC000..0xDFFF) */
#define GB_ADDR_CUR_MAP              0xD35E  /* uint8_t  wCurMap */
#define GB_ADDR_Y_COORD              0xD361  /* uint8_t  wYCoord (in 16x16 blocks) */
#define GB_ADDR_X_COORD              0xD362  /* uint8_t  wXCoord (in 16x16 blocks) */
#define GB_ADDR_CUR_MAP_TILESET      0xD367  /* uint8_t  wCurMapTileset */
#define GB_ADDR_CUR_MAP_HEIGHT       0xD368  /* uint8_t  wCurMapHeight (blocks) */
#define GB_ADDR_CUR_MAP_WIDTH        0xD369  /* uint8_t  wCurMapWidth  (blocks) */
#define GB_ADDR_WALK_COUNTER         0xCFC5  /* uint8_t  wWalkCounter (8..0 during step) */

/* Player sprite (sprite 0 in wSpriteStateData1: 0xC100..0xC1FF, 16 bytes/sprite) */
#define GB_ADDR_SPRITE_DATA1         0xC100  /* 16 sprites * 16 bytes */
#define GB_ADDR_SPRITE_DATA2         0xC200  /* 16 sprites * 16 bytes */
#define GB_SPRITE1_PICTURE_ID        0x00
#define GB_SPRITE1_MOVEMENT_STATUS   0x01
#define GB_SPRITE1_IMAGE_INDEX       0x02
#define GB_SPRITE1_Y_STEP_VEC        0x03
#define GB_SPRITE1_Y_PIXELS          0x04
#define GB_SPRITE1_X_STEP_VEC        0x05
#define GB_SPRITE1_X_PIXELS          0x06
#define GB_SPRITE1_FACING_DIR        0x09    /* 0=down, 4=up, 8=left, 12=right */

/* Battle state */
#define GB_ADDR_IS_IN_BATTLE         0xD057  /* 0=overworld, 1=wild, 2=trainer */
#define GB_ADDR_ENEMY_MON_SPECIES    0xCFE5  /* internal species ID */
#define GB_ADDR_ENEMY_MON_HP_HI      0xCFE6
#define GB_ADDR_ENEMY_MON_HP_LO      0xCFE7
#define GB_ADDR_ENEMY_MON_LEVEL      0xCFF3
#define GB_ADDR_ENEMY_MON_TYPE1      0xCFEA
#define GB_ADDR_ENEMY_MON_TYPE2      0xCFEB
#define GB_ADDR_ENEMY_MON_DVS        0xCFF1  /* 2 bytes: [Atk|Def], [Spd|Spc] */

#define GB_ADDR_BATTLE_MON_SPECIES   0xD014  /* internal species ID */
#define GB_ADDR_BATTLE_MON_TYPE1     0xD019
#define GB_ADDR_BATTLE_MON_TYPE2     0xD01A
#define GB_ADDR_BATTLE_MON_MOVES     0xD01C  /* 4 bytes: move IDs 0..3 */
#define GB_ADDR_BATTLE_MON_DVS       0xD020  /* 2 bytes: [Atk|Def], [Spd|Spc] */
#define GB_ADDR_CURRENT_MENU_ITEM    0xCC26  /* uint8_t wCurrentMenuItem */
#define GB_ADDR_MOVE_MENU_TYPE       0xCCDB  /* uint8_t wMoveMenuType */
#define GB_ADDR_PLAYER_SELECTED_MOVE 0xCCDC  /* uint8_t wPlayerSelectedMove */

/* Party (WRAM) */
#define GB_ADDR_PARTY_COUNT          0xD163  /* uint8_t wPartyCount (0..6) */
#define GB_ADDR_PARTY_SPECIES        0xD164  /* 6 bytes: internal species IDs */

/* Tilemap (20x18 BG tile buffer) */
#define GB_ADDR_TILEMAP              0xC3A0  /* 360 bytes */

/* Gen 1 type IDs (pokered constants/type_constants.asm) */
#define GB_TYPE_NORMAL    0x00
#define GB_TYPE_FIGHTING  0x01
#define GB_TYPE_FLYING    0x02
#define GB_TYPE_POISON    0x03
#define GB_TYPE_GROUND    0x04
#define GB_TYPE_ROCK      0x05
#define GB_TYPE_BUG       0x07
#define GB_TYPE_GHOST     0x08
#define GB_TYPE_FIRE      0x14
#define GB_TYPE_WATER     0x15
#define GB_TYPE_GRASS     0x16
#define GB_TYPE_ELECTRIC  0x17
#define GB_TYPE_PSYCHIC   0x18
#define GB_TYPE_ICE       0x19
#define GB_TYPE_DRAGON    0x1A

/* Gen 2 shiny rule on Gen 1 DVs: Def=10, Spd=10, Spc=10, Atk in {2,3,6,7,10,11,14,15} */
static inline int pokered_is_shiny_dv(uint8_t dv_atk_def, uint8_t dv_spd_spc) {
    uint8_t atk = (dv_atk_def >> 4) & 0x0F;
    uint8_t def = dv_atk_def & 0x0F;
    uint8_t spd = (dv_spd_spc >> 4) & 0x0F;
    uint8_t spc = dv_spd_spc & 0x0F;
    return (def == 10 && spd == 10 && spc == 10 && ((atk & 0x02) != 0));
}

#endif /* POKERED_RAM_H */
