/*
 * hw_pc.h - the GBA memory map, reproduced at its real addresses inside the
 * host process (method of the HeartGold PC port, adapted to the GBA).
 *
 * The game reaches the hardware through fixed addresses (REG_DISPCNT is
 * *(vu16 *)0x04000000, VRAM is 0x06000000...) and often keeps pointers in
 * 32-bit integers. pc_hw_init() reserves the very same ranges in the low
 * 4 GiB of the 64-bit process, so all of that works unmodified.
 */
#ifndef PRS_HW_PC_H
#define PRS_HW_PC_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Real GBA sizes, then the 64 KiB-aligned reservation (Windows granularity). */
#define PC_EWRAM_BASE  0x02000000u
#define PC_EWRAM_SIZE  0x00100000u   /* 256 KiB on the GBA; 1 MiB here: the game
                                      * heap (4x, see include/malloc.h) lives there */
#define PC_IWRAM_BASE  0x03000000u
#define PC_IWRAM_SIZE  0x00008000u   /* 32 KiB  */
#define PC_IO_BASE     0x04000000u
#define PC_IO_SIZE     0x00000400u
#define PC_PLTT_BASE   0x05000000u
#define PC_PLTT_SIZE   0x00000400u
#define PC_VRAM_BASE   0x06000000u
#define PC_VRAM_SIZE   0x00018000u   /* 96 KiB  */
#define PC_OAM_BASE    0x07000000u
#define PC_OAM_SIZE    0x00000400u

#define PC_GBA_WIDTH   240
#define PC_GBA_HEIGHT  160

/* Reserves and zero-fills the map, then sets power-on register values.
 * Must run before any game code. Returns false (and logs why) on failure. */
bool pc_hw_init(void);
void pc_hw_shutdown(void);
void pc_hw_reset(void);
bool pc_hw_is_mapped(uint32_t address, uint32_t size);

static inline volatile uint16_t *pc_hw_reg16(uint32_t a) { return (volatile uint16_t *)(uintptr_t)a; }
static inline volatile uint32_t *pc_hw_reg32(uint32_t a) { return (volatile uint32_t *)(uintptr_t)a; }

/* KEYINPUT (0x04000130), active-low: A B Select Start Right Left Up Down R L. */
void pc_hw_set_keys(uint16_t keys_active_high);
/* VCOUNT / DISPSTAT flags (VBlank 160-226, HBlank, VCount match). */
void pc_hw_set_vcount(uint16_t line, bool hblank);

/* Runs fn(arg) on a stack below 4 GiB (Linux/macOS; direct call on Windows). */
void pc_run_on_low_stack(void (*fn)(void *), void *arg);

#ifdef __cplusplus
}
#endif

#endif /* PRS_HW_PC_H */
