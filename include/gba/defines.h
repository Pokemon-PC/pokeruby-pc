#ifndef GUARD_GBA_DEFINES
#define GUARD_GBA_DEFINES

#include <stddef.h>

#define TRUE  1
#define FALSE 0

#ifdef PLATFORM_PC
// Plain globals on PC: the build gathers all game variables in one
// section, saved at startup and restored by a soft reset.
#define BSS_DATA
#define IWRAM_DATA
#define EWRAM_DATA
// Pointer field or table element kept 32-bit wide, as laid out by the
// assembly data (.4byte); zero-extended when read. MinGW defines
// __ptr32/__uptr as empty macros for GCC: undo that for Clang.
#undef __ptr32
#undef __uptr
#define PC_PTR32 __ptr32 __uptr
// The GBA compilers (agbcc, GCC -mabi=apcs-gnu) round every struct up to a
// multiple of 4 bytes; the assembly data relies on it (e.g. 2-byte structs
// laid out as 4). Clang on x86 does not: PC_APCS restores it where needed.
#define PC_APCS __attribute__((aligned(4)))
#else
#define PC_PTR32
#define PC_APCS
#define BSS_DATA   __attribute__((section(".bss")))
#define IWRAM_DATA __attribute__((section("iwram_data")))
#define EWRAM_DATA __attribute__((section("ewram_data")))
#endif
#define UNUSED __attribute__((unused))
#ifdef PLATFORM_PC
// ARM assembly body, no C version: an error if the game uses it on PC.
#define NAKED __attribute__((deprecated("ARM-only function: needs a C version on PC")))
#else
#define NAKED __attribute__((naked))
#endif

#define ALIGNED(n) __attribute__((aligned(n)))

#define SOUND_INFO_PTR (*(struct SoundInfo **)0x3007FF0)
#define INTR_CHECK     (*(u16 *)0x3007FF8)
#define INTR_VECTOR    (*(void **)0x3007FFC)

#define EWRAM 0x2000000
#define IWRAM 0x3000000

#define PLTT      0x5000000
#define PLTT_SIZE 0x400

#define BG_PLTT      PLTT
#define BG_PLTT_SIZE 0x200

#define OBJ_PLTT      (PLTT + 0x200)
#define OBJ_PLTT_SIZE 0x200

#define VRAM      0x6000000
#define VRAM_SIZE 0x18000

#define BG_VRAM           VRAM
#define BG_VRAM_SIZE      0x10000
#define BG_CHAR_ADDR(n)   (void *)(BG_VRAM + (0x4000 * (n)))
#define BG_SCREEN_ADDR(n) (void *)(BG_VRAM + (0x800 * (n)))
#define BG_TILE_ADDR(n)    (void *)(BG_VRAM + (0x80 * (n)))

// text-mode BG
#define OBJ_VRAM0      (void *)(VRAM + 0x10000)
#define OBJ_VRAM0_SIZE 0x8000

// bitmap-mode BG
#define OBJ_VRAM1      (void *)(VRAM + 0x14000)
#define OBJ_VRAM1_SIZE 0x4000

#define OAM      0x7000000
#define OAM_SIZE 0x400

#define DISPLAY_WIDTH  240
#define DISPLAY_HEIGHT 160

#define TILE_SIZE_4BPP 32
#define TILE_SIZE_8BPP 64

#define TOTAL_OBJ_TILE_COUNT 1024

#define PLTT_SIZEOF(n) ((n) * sizeof(u16))
#define PLTT_SIZE_4BPP PLTT_SIZEOF(16)
#define PLTT_SIZE_8BPP PLTT_SIZEOF(256)

#define PLTT_OFFSET_4BPP(n) ((n) * PLTT_SIZE_4BPP)

#define RGB(r, g, b) ((r) | ((g) << 5) | ((b) << 10))

#define RGB_BLACK RGB(0, 0, 0)
#define RGB_WHITE RGB(31, 31, 31)

#define WIN_RANGE(a, b) (((a) << 8) | (b))

#endif // GUARD_GBA_DEFINES
