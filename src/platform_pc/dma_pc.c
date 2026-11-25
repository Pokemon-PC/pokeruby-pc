/*
 * dma_pc.c - the four GBA DMA channels.
 *
 * Immediate transfers run at once. VBlank/HBlank transfers are armed and run
 * by the frame driver (irq_pc.c) at the matching moment, honouring repeat and
 * the "increment/reload" destination mode used by the game's scanline
 * effects (src/scanline_effect.c: HBlank DMA into scroll/window registers).
 * Special timing (sound FIFO, channels 1-2) is left to the sound HAL.
 */
#include "pc_hal.h"

#include "hw_pc.h"
#include "platform_pc.h"

#include <string.h>

#define DMA_DEST_MODE(c)  (((c) >> 5) & 3)    /* 0 inc, 1 dec, 2 fixed, 3 inc+reload */
#define DMA_SRC_MODE(c)   (((c) >> 7) & 3)    /* 0 inc, 1 dec, 2 fixed */
#define DMA_REPEAT_BIT    0x0200
#define DMA_32BIT_BIT     0x0400
#define DMA_TIMING(c)     (((c) >> 12) & 3)   /* 0 now, 1 VBlank, 2 HBlank, 3 special */
#define DMA_ENABLE_BIT    0x8000

typedef struct {
    bool      armed;
    uint16_t  cnt;          /* DMAxCNT_H */
    uint32_t  count;        /* units per transfer */
    uintptr_t src, dest;    /* internal (running) addresses */
    uintptr_t dest_reload;  /* destination for inc+reload */
} Channel;

static Channel s_ch[4];

static inline volatile uint32_t *dma_reg(int ch, int word) {
    return (volatile uint32_t *)(uintptr_t)(PC_IO_BASE + 0xB0 + ch * 12 + word * 4);
}

static void transfer(Channel *c) {
    int unit = (c->cnt & DMA_32BIT_BIT) ? 4 : 2;
    int sstep = DMA_SRC_MODE(c->cnt) == 0 ? unit : DMA_SRC_MODE(c->cnt) == 1 ? -unit : 0;
    int dmode = DMA_DEST_MODE(c->cnt);
    int dstep = (dmode == 0 || dmode == 3) ? unit : dmode == 1 ? -unit : 0;
    uintptr_t s = c->src, d = c->dest;
    for (uint32_t i = 0; i < c->count; i++) {
        if (unit == 4) {
            uint32_t v;
            memcpy(&v, (const void *)s, 4);
            memcpy((void *)d, &v, 4);
        } else {
            uint16_t v;
            memcpy(&v, (const void *)s, 2);
            memcpy((void *)d, &v, 2);
        }
        s += (uintptr_t)(intptr_t)sstep;
        d += (uintptr_t)(intptr_t)dstep;
    }
    c->src = s;
    c->dest = dmode == 3 ? c->dest_reload : d;
}

static void finish(int ch) {
    Channel *c = &s_ch[ch];
    if (!(c->cnt & DMA_REPEAT_BIT) || DMA_TIMING(c->cnt) == 0) {
        c->armed = false;
        c->cnt &= (uint16_t)~DMA_ENABLE_BIT;
        *dma_reg(ch, 2) &= ~((uint32_t)DMA_ENABLE_BIT << 16);
    }
    if (c->cnt & 0x4000) {
        pc_irq_raise((uint16_t)(0x0100 << ch));   /* INTR_FLAG_DMA0..3 */
    }
}

void pc_dma_set(int ch, const volatile void *src, volatile void *dest, uint32_t control) {
    if (ch < 0 || ch > 3) {
        return;
    }
    Channel *c = &s_ch[ch];
    /* Registers as the game would see them (32-bit addresses). */
    *dma_reg(ch, 0) = (uint32_t)(uintptr_t)src;
    *dma_reg(ch, 1) = (uint32_t)(uintptr_t)dest;
    *dma_reg(ch, 2) = control;

    c->cnt = (uint16_t)(control >> 16);
    if (!(c->cnt & DMA_ENABLE_BIT)) {
        c->armed = false;
        return;
    }
    c->count = control & 0xFFFF;
    if (c->count == 0) {
        c->count = ch == 3 ? 0x10000 : 0x4000;
    }
    c->src = (uintptr_t)src;
    c->dest = c->dest_reload = (uintptr_t)dest;

    switch (DMA_TIMING(c->cnt)) {
    case 0:
        transfer(c);
        finish(ch);
        break;
    case 1:
    case 2:
        c->armed = true;
        break;
    default:
        /* Sound FIFO (ch 1/2) or video capture (ch 3): handled elsewhere. */
        c->armed = false;
        break;
    }
}

void pc_dma_stop(int ch) {
    if (ch < 0 || ch > 3) {
        return;
    }
    s_ch[ch].armed = false;
    s_ch[ch].cnt = 0;
    *dma_reg(ch, 2) &= 0x0000FFFFu;
}

static void run_timed(int timing) {
    for (int ch = 0; ch < 4; ch++) {
        Channel *c = &s_ch[ch];
        if (c->armed && DMA_TIMING(c->cnt) == timing) {
            transfer(c);
            finish(ch);
        }
    }
}

void pc_dma_on_hblank(void) { run_timed(2); }
void pc_dma_on_vblank(void) { run_timed(1); }
