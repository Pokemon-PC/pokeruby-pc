/*
 * bios_pc.c - the GBA BIOS calls used by pokeemerald (libagbsyscall), in C.
 *
 * Behaviour follows GBATEK / mGBA's HLE BIOS. Symbol names and arguments are
 * those of include/gba/syscall.h so the game links against them unchanged.
 */
#include "hw_pc.h"
#include "platform_pc.h"

#include <math.h>
#include <stdint.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

struct BgAffineSrcData { int32_t texX, texY; int16_t scrX, scrY, sx, sy; uint16_t alpha; };
struct BgAffineDstData { int16_t pa, pb, pc, pd; int32_t dx, dy; };
struct ObjAffineSrcData { int16_t xScale, yScale; uint16_t rotation; };

/* ---------------------------------------------------------------- copies */

void CpuSet(const void *src, void *dest, uint32_t control) {
    uint32_t count = control & 0x1FFFFF;
    int fixed = (control >> 24) & 1;
    if (control & (1u << 26)) {
        const uint32_t *s = src;
        uint32_t *d = dest;
        uint32_t v = *s;
        for (uint32_t i = 0; i < count; i++) {
            d[i] = fixed ? v : s[i];
        }
    } else {
        const uint16_t *s = src;
        uint16_t *d = dest;
        uint16_t v = *s;
        for (uint32_t i = 0; i < count; i++) {
            d[i] = fixed ? v : s[i];
        }
    }
}

void CpuFastSet(const void *src, void *dest, uint32_t control) {
    uint32_t count = ((control & 0x1FFFFF) + 7) & ~7u;   /* blocks of 8 words */
    int fixed = (control >> 24) & 1;
    const uint32_t *s = src;
    uint32_t *d = dest;
    uint32_t v = *s;
    for (uint32_t i = 0; i < count; i++) {
        d[i] = fixed ? v : s[i];
    }
}

/* --------------------------------------------------------- decompression */

static void lz77(const uint8_t *src, uint8_t *dest) {
    uint32_t size = src[1] | (src[2] << 8) | (src[3] << 16);
    const uint8_t *p = src + 4;
    uint32_t out = 0;
    while (out < size) {
        uint8_t flags = *p++;
        for (int bit = 7; bit >= 0 && out < size; bit--) {
            if (flags & (1 << bit)) {
                uint32_t len = (p[0] >> 4) + 3;
                uint32_t disp = (((p[0] & 0xF) << 8) | p[1]) + 1;
                p += 2;
                for (uint32_t i = 0; i < len && out < size; i++, out++) {
                    dest[out] = dest[out - disp];
                }
            } else {
                dest[out++] = *p++;
            }
        }
    }
}

static void rl(const uint8_t *src, uint8_t *dest) {
    uint32_t size = src[1] | (src[2] << 8) | (src[3] << 16);
    const uint8_t *p = src + 4;
    uint32_t out = 0;
    while (out < size) {
        uint8_t flag = *p++;
        if (flag & 0x80) {
            uint32_t len = (flag & 0x7F) + 3;
            uint8_t v = *p++;
            for (uint32_t i = 0; i < len && out < size; i++) {
                dest[out++] = v;
            }
        } else {
            uint32_t len = (flag & 0x7F) + 1;
            for (uint32_t i = 0; i < len && out < size; i++) {
                dest[out++] = *p++;
            }
        }
    }
}

/* The Vram variants write 16 bits at a time on hardware; the result in
 * memory is the same (an odd final byte is padded by the encoder). */
void LZ77UnCompWram(const uint32_t *src, void *dest) { lz77((const uint8_t *)src, dest); }
void LZ77UnCompVram(const uint32_t *src, void *dest) { lz77((const uint8_t *)src, dest); }
void RLUnCompWram(const uint32_t *src, void *dest) { rl((const uint8_t *)src, dest); }
void RLUnCompVram(const uint32_t *src, void *dest) { rl((const uint8_t *)src, dest); }

/* ------------------------------------------------------------------ math */

int32_t Div(int32_t num, int32_t denom) {
    if (denom == 0) {
        return num < 0 ? -1 : 1;
    }
    return num / denom;
}

uint16_t Sqrt(uint32_t num) {
    uint32_t res = 0, bit = 1u << 30;
    while (bit > num) {
        bit >>= 2;
    }
    while (bit != 0) {
        if (num >= res + bit) {
            num -= res + bit;
            res = (res >> 1) + bit;
        } else {
            res >>= 1;
        }
        bit >>= 2;
    }
    return (uint16_t)res;
}

static int32_t arctan(int32_t i) {
    int32_t a = -((i * i) >> 14);
    int32_t b = ((0xA9 * a) >> 14) + 0x390;
    b = ((b * a) >> 14) + 0x91C;
    b = ((b * a) >> 14) + 0xFB6;
    b = ((b * a) >> 14) + 0x16AA;
    b = ((b * a) >> 14) + 0x2081;
    b = ((b * a) >> 14) + 0x3651;
    b = ((b * a) >> 14) + 0xA2F9;
    return (i * b) >> 16;
}

uint16_t ArcTan2(int16_t x16, int16_t y16) {
    int32_t x = x16, y = y16;
    if (y == 0) {
        return x >= 0 ? 0 : 0x8000;
    }
    if (x == 0) {
        return y >= 0 ? 0x4000 : 0xC000;
    }
    if (y >= 0) {
        if (x >= 0) {
            if (x >= y) {
                return (uint16_t)arctan((y << 14) / x);
            }
        } else if (-x >= y) {
            return (uint16_t)(arctan((y << 14) / x) + 0x8000);
        }
        return (uint16_t)(0x4000 - arctan((x << 14) / y));
    }
    if (x <= 0) {
        if (-x > -y) {
            return (uint16_t)(arctan((y << 14) / x) + 0x8000);
        }
    } else if (x >= -y) {
        return (uint16_t)(arctan((y << 14) / x) + 0x10000);
    }
    return (uint16_t)(0xC000 - arctan((x << 14) / y));
}

void BgAffineSet(struct BgAffineSrcData *src, struct BgAffineDstData *dest, int32_t count) {
    for (int32_t i = 0; i < count; i++, src++, dest++) {
        double ox = src->texX / 256.0, oy = src->texY / 256.0;
        double cx = src->scrX, cy = src->scrY;
        double sx = src->sx / 256.0, sy = src->sy / 256.0;
        double theta = (src->alpha >> 8) / 128.0 * M_PI;
        double a = cos(theta) * sx, b = -sin(theta) * sx;
        double c = sin(theta) * sy, d = cos(theta) * sy;
        double rx = ox - (a * cx + b * cy);
        double ry = oy - (c * cx + d * cy);
        dest->pa = (int16_t)(a * 256);
        dest->pb = (int16_t)(b * 256);
        dest->pc = (int16_t)(c * 256);
        dest->pd = (int16_t)(d * 256);
        dest->dx = (int32_t)(rx * 256);
        dest->dy = (int32_t)(ry * 256);
    }
}

void ObjAffineSet(struct ObjAffineSrcData *src, void *dest, int32_t count, int32_t offset) {
    uint8_t *d = dest;
    for (int32_t i = 0; i < count; i++, src++) {
        double sx = src->xScale / 256.0, sy = src->yScale / 256.0;
        double theta = (src->rotation >> 8) / 128.0 * M_PI;
        int16_t m[4] = {
            (int16_t)(cos(theta) * sx * 256), (int16_t)(-sin(theta) * sx * 256),
            (int16_t)(sin(theta) * sy * 256), (int16_t)(cos(theta) * sy * 256),
        };
        for (int k = 0; k < 4; k++) {
            memcpy(d + k * offset, &m[k], 2);
        }
        d += 4 * offset;
    }
}

/* ----------------------------------------------------------------- reset */

void RegisterRamReset(uint32_t flags) {
    /* EWRAM/IWRAM hold no static data on PC (globals live in the executable);
     * they are cleared like on hardware, minus IWRAM's top 0x200 bytes. */
    if (flags & 0x01) memset((void *)(uintptr_t)PC_EWRAM_BASE, 0, PC_EWRAM_SIZE);
    if (flags & 0x02) memset((void *)(uintptr_t)PC_IWRAM_BASE, 0, PC_IWRAM_SIZE - 0x200);
    if (flags & 0x04) memset((void *)(uintptr_t)PC_PLTT_BASE, 0, PC_PLTT_SIZE);
    if (flags & 0x08) memset((void *)(uintptr_t)PC_VRAM_BASE, 0, PC_VRAM_SIZE);
    if (flags & 0x10) memset((void *)(uintptr_t)PC_OAM_BASE, 0, PC_OAM_SIZE);
    if (flags & 0x80) {
        /* Display/DMA/timer/IRQ registers, keeping the power-on defaults. */
        memset((void *)(uintptr_t)PC_IO_BASE, 0, 0x60);
        memset((void *)(uintptr_t)(PC_IO_BASE + 0xB0), 0, 0x60);
        memset((void *)(uintptr_t)(PC_IO_BASE + 0x200), 0, 0x10);
        *pc_hw_reg16(0x04000000u) = 0x0080;
        *pc_hw_reg16(0x04000020u) = 0x0100;
        *pc_hw_reg16(0x04000026u) = 0x0100;
        *pc_hw_reg16(0x04000030u) = 0x0100;
        *pc_hw_reg16(0x04000036u) = 0x0100;
    }
}

/* ------------------------------------------------------------ waiting */

#include "pc_hal.h"

/* BIOS VBlankIntrWait: halt until the next VBlank interrupt. On PC, running
 * the display up to the next VBlank is exactly that. */
void VBlankIntrWait(void) {
    pc_wait_vblank();
}

/* BIOS IntrWait(discard, flags): only the VBlank form is used by the game. */
void IntrWait(uint32_t discard, uint32_t flags) {
    (void)discard;
    (void)flags;
    pc_wait_vblank();
}
