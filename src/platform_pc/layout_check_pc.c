/*
 * layout_check_pc.c - structs shared with the assembly data.
 *
 * Two kinds:
 *   - structs that keep their GBA layout on PC (PC_PTR32 pointer fields):
 *     the .s/.inc files and mapjson / mid2agb output lay them out with
 *     4-byte pointers. A failure means a field lost its PC_PTR32.
 *   - sprite tables written with the macros of include/macros.inc (PC
 *     versions under PLATFORM_PC) or widened by pp_s.py --wide: the sizes
 *     below are what those macros emit on PC.
 */
#include <stddef.h>

#include "global.h"
#include "gba/m4a_internal.h"
#include "sprite.h"
#include "scanline_effect.h"
#include "pokemon_storage_system.h"

#define SIZE(T, n) _Static_assert(sizeof(T) == (n), #T " must be " #n " bytes")
#define AT(T, f, n) _Static_assert(offsetof(T, f) == (n), #T "." #f " must stay at " #n)

/* data/maps, data/layouts (mapjson), data/tilesets */
SIZE(struct Tileset, 0x18);
AT(struct Tileset, callback, 0x14);
SIZE(struct MapLayout, 0x18);
SIZE(struct ObjectEventTemplate, 0x18);
AT(struct ObjectEventTemplate, script, 0x10);
SIZE(struct WarpEvent, 0x08);
SIZE(struct CoordEvent, 0x10);
AT(struct CoordEvent, script, 0x0C);
SIZE(struct BgEvent, 0x0C);
SIZE(struct MapEvents, 0x14);
SIZE(struct MapConnection, 0x0C);
SIZE(struct MapConnections, 0x08);
SIZE(struct MapHeader, 0x1C);
AT(struct MapHeader, music, 0x10);

/* sound (sound/ .inc files, mid2agb songs, tracks = 0x50 bytes each) */
SIZE(struct ToneData, 0x0C);
SIZE(struct SongHeader, 0x0C);
SIZE(struct Song, 0x08);
SIZE(struct MusicPlayer, 0x0C);
SIZE(struct MusicPlayerTrack, 0x50);

/* include/macros.inc, PLATFORM_PC versions */
SIZE(struct SpriteSheet, 16);              /* obj_tiles */
SIZE(struct CompressedSpriteSheet, 16);    /* obj_tiles */
SIZE(struct SpritePalette, 16);            /* obj_pal */
SIZE(struct CompressedSpritePalette, 16);  /* obj_pal */
SIZE(struct SpriteFrameImage, 16);         /* obj_frame_tiles */
SIZE(struct SpriteTemplate, 48);           /* spr_template */
AT(struct SpriteTemplate, oam, 8);
AT(struct SpriteTemplate, callback, 40);
SIZE(union AnimCmd, 4);                    /* obj_image_anim_* */
SIZE(union AffineAnimCmd, 8);              /* obj_rot_scal_anim_* (PC_APCS) */

/* data/btl_attrs.s, data/pokemon_storage_system.s (PLATFORM_PC blocks, --wide) */
SIZE(struct ScanlineEffectParams, 16);
AT(struct ScanlineEffectParams, initState, 12);
SIZE(struct PokemonStorageSystemFunc, 16);

/* GBA struct sizes are rounded up to 4 bytes (PC_APCS): tables in assembly
 * (data/graphics/pokemon/..._coords.inc, data/trainer_money.inc, affine
 * animations) are laid out that way. */
#include "data2.h"
SIZE(struct MonCoords, 4);
