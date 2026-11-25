/*
 * lang_tables_pc.c - French names stored inside tables (fixed-size fields:
 * species, moves, items, trainers...), copied from the player's French ROM
 * table by table (lang_pc.c checks the ROM first).
 *
 * French offsets: scripts/lang/find_fr_tables.py on the French ROM of each
 * game, revision 1 (Ruby AXVF, Sapphire AXPF). GBA stride and field offset
 * describe the ROM table; the PC side uses sizeof / offsetof (pointers are
 * 8 bytes here). Not found (no name is the same in both languages, nothing
 * to vote with): gDecorations, which stays English.
 */
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "pc_hal.h"
#include "platform_pc.h"

#include "global.h"
#include "data2.h"
#include "pokedex.h"
#include "item.h"

void pc_lang_make_writable(const void *p, size_t n);   /* lang_pc.c */

/* src/item.c keeps struct Item to itself: same definition, PC layout. */
struct Item
{
    u8 name[14];
    u16 itemId;
    u16 price;
    u8 holdEffect;
    u8 holdEffectParam;
    const u8 *description;
    u8 importance;
    u8 exitsBagOnUse;
    u8 pocket;
    u8 type;
    ItemUseFunc fieldUseFunc;
    u8 battleUsage;
    ItemUseFunc battleUseFunc;
    u8 secondaryId;
};
extern const struct Item gItems[];
extern const struct Berry gBerries[];

struct LangTable {
    const char *name;
    const void *table;   /* first PC entry */
    size_t pcStride, pcField;
    uint32_t frOffset, frStride, frField, size, count;
};

#define TABLE(sym, field, fr, frStride, frField, count)                          \
    { #sym, sym, sizeof(sym[0]), offsetof(__typeof__(sym[0]), field), fr, frStride, \
      frField, sizeof(((__typeof__(sym[0]) *)0)->field), count }
#define NAMES(sym, fr, count) \
    { #sym, sym, sizeof(sym[0]), 0, fr, sizeof(sym[0]), 0, sizeof(sym[0]), count }

static const struct LangTable sTables[] = {
#if defined(SAPPHIRE)
    /* Pokemon - Version Saphir (France) rev 1 (AXPF) */
#include "lang/fr_axpf_tables.inc"
#else
    /* Pokemon - Version Rubis (France) rev 1 (AXVF) */
    NAMES(gSpeciesNames, 0x1FF574, 412),
    NAMES(gMoveNames, 0x200728, 355),
    NAMES(gAbilityNames, 0x202694, 78),
    NAMES(gTypeNames, 0x201C78, 18),
    NAMES(gTrainerClassNames, 0x1F8610, 58),
    TABLE(gItems, name, 0x3CCFC4, 44, 0, 349),
    TABLE(gTrainers, trainerName, 0x1F8904, 40, 4, 694),
    TABLE(gBerries, name, 0x3D4DC4, 28, 0, 43),
    TABLE(gPokedexEntries, categoryName, 0x3B923C, 36, 0, 387),
#endif
};

void pc_lang_tables_apply(const uint8_t *rom) {
    for (size_t t = 0; t < sizeof sTables / sizeof sTables[0]; t++) {
        const struct LangTable *lt = &sTables[t];
        int copied = 0;
        for (uint32_t k = 0; k < lt->count; k++) {
            const uint8_t *src = rom + lt->frOffset + k * lt->frStride + lt->frField;
            if (memchr(src, 0xFF, lt->size) == NULL) {
                continue;                       /* not a name: keep English */
            }
            uint8_t *dst = (uint8_t *)lt->table + k * lt->pcStride + lt->pcField;
            pc_lang_make_writable(dst, lt->size);
            memcpy(dst, src, lt->size);
            copied++;
        }
        pc_log("lang: %s: %d/%u French names", lt->name, copied, (unsigned)lt->count);
    }
}
