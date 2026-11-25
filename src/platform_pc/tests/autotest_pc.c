/*
 * autotest_pc.c - milestone M2 check of the hardware layer (run by CTest).
 *
 * Uses the game's own headers and macros (REG_*, DmaCopy16, DmaSet, the
 * flash and RTC APIs of save.c / rtc.c) against the PC HAL:
 *   memory map, BIOS, DMA (immediate + HBlank repeat as in scanline_effect.c),
 *   interrupt dispatch and the line timeline, keys, flash (save.bin), RTC.
 * Optional argument: repository root, to also LZ77-check a generated asset.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gpu_pc.h"
#include "hw_pc.h"
#include "pc_hal.h"
#include "platform_pc.h"

#include "global.h"
#include "main.h"
#include "constants/rgb.h"
#include "siirtc.h"
#include "gba/flash_internal.h"

/* pokeruby names (pokeemerald: EWRAM_START, IWRAM_START, BG_SCREEN_SIZE). */
#ifndef EWRAM_START
#define EWRAM_START EWRAM
#endif
#ifndef IWRAM_START
#define IWRAM_START IWRAM
#endif
#ifndef BG_SCREEN_SIZE
#define BG_SCREEN_SIZE 0x800
#endif
s32 Div(s32 num, s32 denom);   /* BIOS call (bios_pc.c), not in pokeruby's syscall.h */

static int s_failures, s_checks;

#define CHECK(cond, ...)                                              \
    do {                                                              \
        s_checks++;                                                   \
        if (!(cond)) {                                                \
            s_failures++;                                             \
            printf("FAIL %s:%d: %s -- ", __FILE__, __LINE__, #cond);  \
            printf(__VA_ARGS__);                                      \
            printf("\n");                                             \
        }                                                             \
    } while (0)

static void set_env(const char *k, const char *v) {
#if defined(_WIN32)
    _putenv_s(k, v);
#else
    setenv(k, v, 1);
#endif
}

/* ------------------------------------------------------------ memory map */

static void test_memory_map(void) {
    CHECK(pc_hw_is_mapped(EWRAM_START, 0x40000), "EWRAM");
    CHECK(pc_hw_is_mapped(IWRAM_START, 0x8000), "IWRAM");
    CHECK(pc_hw_is_mapped(REG_BASE, 0x400), "I/O");
    CHECK(pc_hw_is_mapped(PLTT, PLTT_SIZE), "palettes");
    CHECK(pc_hw_is_mapped(VRAM, VRAM_SIZE), "VRAM");
    CHECK(pc_hw_is_mapped(OAM, OAM_SIZE), "OAM");
    CHECK(REG_DISPCNT == DISPCNT_FORCED_BLANK, "power-on DISPCNT=%04X", REG_DISPCNT);
    REG_BG0CNT = 0x1234;
    CHECK(*(vu16 *)0x04000008 == 0x1234, "REG_BG0CNT is at its GBA address");
    /* A heap pointer into EWRAM fits in 32 bits, as the game assumes. */
    u8 *p = (u8 *)EWRAM_START + 0x100;
    CHECK((uintptr_t)(u32)(uintptr_t)p == (uintptr_t)p, "EWRAM pointer survives a u32");
    /* The executable itself sits below 4 GiB (script .4byte pointers). */
    CHECK((uintptr_t)&test_memory_map < 0x100000000ull, "code at %p", (void *)&test_memory_map);
    CHECK((uintptr_t)&s_checks < 0x100000000ull, "data at %p", (void *)&s_checks);
}

/* ------------------------------------------------------------------ BIOS */

static void test_bios(const char *root) {
    /* LZ77: "ABCABCABCABC" = 3 literals + back-ref (len 9, disp 3). */
    static const u8 lz[] ALIGNED(4) = { 0x10, 12, 0, 0, 0x10, 'A', 'B', 'C', 0x60, 0x02 };
    u8 out[16] ALIGNED(4);
    memset(out, 0, sizeof out);
    LZ77UnCompWram((const u32 *)lz, out);
    CHECK(memcmp(out, "ABCABCABCABC", 12) == 0, "LZ77 got %.12s", out);

    /* RL: run of 5 'x', then literals "yz". */
    static const u8 rlv[] ALIGNED(4) = { 0x30, 7, 0, 0, 0x82, 'x', 0x01, 'y', 'z' };
    memset(out, 0, sizeof out);
    RLUnCompVram((const u32 *)rlv, out);
    CHECK(memcmp(out, "xxxxxyz", 7) == 0, "RL got %.7s", out);

    u32 words[16] ALIGNED(4);
    CpuFill32(0xDEADBEEF, words, sizeof words);
    CHECK(words[0] == 0xDEADBEEF && words[15] == 0xDEADBEEF, "CpuFill32");
    u16 halves[8] ALIGNED(4) = { 1, 2, 3, 4, 5, 6, 7, 8 };
    u16 copy[8] ALIGNED(4);
    CpuCopy16(halves, copy, sizeof halves);
    CHECK(memcmp(halves, copy, sizeof halves) == 0, "CpuCopy16");
    CpuFastFill(0, words, sizeof words);
    CHECK(words[0] == 0 && words[15] == 0, "CpuFastFill");

    CHECK(Sqrt(144) == 12 && Sqrt(150) == 12 && Sqrt(0xFFFFFFFF) == 0xFFFF, "Sqrt");
    CHECK(Div(-7, 2) == -3, "Div truncates toward zero");
    u16 a45 = ArcTan2(0x100, 0x100);
    CHECK(a45 > 0x1FF0 && a45 < 0x2010, "ArcTan2(1,1)=%04X", a45);
    CHECK(ArcTan2(0, 0x100) == 0x4000 && ArcTan2(-0x100, 0) == 0x8000, "ArcTan2 axes");

    struct ObjAffineSrcData src = { 0x100, 0x100, 0 };
    s16 m[4];
    ObjAffineSet(&src, m, 1, 2);
    CHECK(m[0] == 0x100 && m[1] == 0 && m[2] == 0 && m[3] == 0x100, "ObjAffineSet identity");
    struct ObjAffineSrcData rot = { 0x100, 0x100, 0x4000 };   /* 90 degrees */
    ObjAffineSet(&rot, m, 1, 2);
    CHECK(m[0] == 0 && m[1] == -0x100 && m[2] == 0x100 && m[3] == 0, "ObjAffineSet 90deg %d %d %d %d",
          m[0], m[1], m[2], m[3]);

    /* A real generated asset: the .lz must expand to its .4bpp source. */
    if (root) {
        char a[1024], b[1024];
        snprintf(a, sizeof a, "%s/graphics/berries/kelpsy.4bpp.lz", root);
        snprintf(b, sizeof b, "%s/graphics/berries/kelpsy.4bpp", root);
        FILE *fa = fopen(a, "rb"), *fb = fopen(b, "rb");
        if (fa && fb) {
            static u32 lzbuf[0x4000];
            static u8 raw[0x10000], dec[0x10000];
            size_t nl = fread(lzbuf, 1, sizeof lzbuf, fa);
            size_t nr = fread(raw, 1, sizeof raw, fb);
            (void)nl;
            LZ77UnCompVram(lzbuf, dec);
            CHECK(nr > 0 && memcmp(raw, dec, nr) == 0, "kelpsy.4bpp.lz expands to the .4bpp");
        } else {
            printf("skip: generated assets not found under %s\n", root);
        }
        if (fa) fclose(fa);
        if (fb) fclose(fb);
    }
}

/* ------------------------------------------------- interrupts and timeline */

static int s_vblanks, s_vcounts, s_hblanks;
static u16 s_hofs_seen[228];
static u16 s_line_seen[228];

static void VBlankHandler(void) { s_vblanks++; }
static void VCountHandler(void) { s_vcounts++; }
static void HBlankHandler(void) {
    u16 line = REG_VCOUNT;
    if (line < 228) {
        s_hofs_seen[line] = REG_BG0HOFS;
        s_line_seen[line] = 1;
    }
    s_hblanks++;
}
static void Dummy(void) {}

/* gIntrTable order of pokeruby's main.c / crt0.s IntrMain. */
static IntrFunc sTable[14] = {
    Dummy, Dummy, HBlankHandler, VBlankHandler, VCountHandler,
    Dummy, Dummy, Dummy, Dummy, Dummy, Dummy, Dummy, Dummy, Dummy,
};

static u16 KeysAPlusUp(uint64_t frame) {
    (void)frame;
    return A_BUTTON | DPAD_UP;
}

static void test_dma_and_irq(void) {
    /* Immediate DMA through the game's macros. */
    static u16 src[64] ALIGNED(4);
    for (int i = 0; i < 64; i++) src[i] = (u16)(i * 3);
    DmaCopy16(3, src, (void *)VRAM, sizeof src);
    CHECK(memcmp((void *)VRAM, src, sizeof src) == 0, "DmaCopy16 to VRAM");
    DmaFill32(3, 0x11223344, (void *)(VRAM + 0x100), 0x40);
    CHECK(*(vu32 *)(VRAM + 0x100) == 0x11223344 && *(vu32 *)(VRAM + 0x13C) == 0x11223344, "DmaFill32");
    DmaClear16(3, (void *)VRAM, sizeof src);
    CHECK(*(vu16 *)(VRAM + 2) == 0, "DmaClear16");

    pc_irq_set_table(sTable, 14);
    REG_IME = 1;
    REG_IE = INTR_FLAG_VBLANK | INTR_FLAG_VCOUNT | INTR_FLAG_HBLANK;
    REG_DISPSTAT = DISPSTAT_VBLANK_INTR | DISPSTAT_VCOUNT_INTR | DISPSTAT_HBLANK_INTR | (150 << 8);

    /* HBlank repeat DMA with reloading destination, exactly what
     * ScanlineEffect sets up: one value per line into BG0HOFS. */
    /* Repeat DMA keeps incrementing its source each frame until stopped:
     * leave room for two frames. */
    static u16 hofs[2 * 160] ALIGNED(4);
    for (int i = 0; i < 2 * 160; i++) hofs[i] = (u16)(1000 + i);
    pc_wait_vblank();   /* align on a VBlank first, as the game does */
    DmaSet(0, hofs, &REG_BG0HOFS,
           ((DMA_ENABLE | DMA_START_HBLANK | DMA_REPEAT | DMA_16BIT | DMA_SRC_INC | DMA_DEST_RELOAD) << 16) | 1);

    int v0 = s_vblanks, c0 = s_vcounts, h0 = s_hblanks;
    memset(s_line_seen, 0, sizeof s_line_seen);
    pc_input_set_override(KeysAPlusUp);
    pc_wait_vblank();
    CHECK(s_vblanks == v0 + 1, "one VBlank IRQ per wait (%d)", s_vblanks - v0);
    CHECK(s_vcounts == c0 + 1, "one VCount(150) IRQ per frame (%d)", s_vcounts - c0);
    CHECK(s_hblanks - h0 == 228, "one HBlank IRQ per line (%d)", s_hblanks - h0);
    CHECK(REG_VCOUNT == 161, "returns just after the VBlank IRQ, line 161 (VCOUNT=%d)", REG_VCOUNT);
    CHECK(REG_DISPSTAT & DISPSTAT_VBLANK, "VBlank flag set");

    /* Lines 161..227 of the previous frame came first, then 0..159. */
    int ok = 1, bad = -1;
    for (int y = 0; y < 160; y++) {
        if (!s_line_seen[y] || s_hofs_seen[y] != 1000 + y) { ok = 0; bad = y; break; }
    }
    CHECK(ok, "HBlank DMA value at line %d = %d", bad, bad >= 0 ? s_hofs_seen[bad] : 0);
    CHECK(REG_KEYINPUT == (u16)(~(A_BUTTON | DPAD_UP) & KEYS_MASK), "KEYINPUT=%03X", REG_KEYINPUT);

    /* VBlank lines get no HBlank DMA: line 200 still holds the last value. */
    pc_wait_vblank();
    DmaStop(0);
    CHECK(s_hofs_seen[200] == 1000 + 159, "no HBlank DMA during VBlank (%d)", s_hofs_seen[200]);

    /* IME off: nothing is dispatched, the flag stays pending. */
    REG_IME = 0;
    v0 = s_vblanks;
    pc_wait_vblank();
    CHECK(s_vblanks == v0, "IME=0 blocks interrupts");
    CHECK(REG_IF & INTR_FLAG_VBLANK, "VBlank stays pending in IF");
    REG_IME = 1;
    pc_wait_vblank();
    CHECK(s_vblanks >= v0 + 1, "pending VBlank runs once IME is back");

    v0 = s_vblanks;
    u64 f0 = pc_frame_count();
    for (int i = 0; i < 60; i++) pc_wait_vblank();
    CHECK(pc_frame_count() == f0 + 60 && s_vblanks == v0 + 60, "60 frames");
    pc_input_set_override(NULL);
    REG_IE = 0;
    REG_DISPSTAT = 0;
}

/* -------------------------------------------------------------- rendering */

static void test_gpu(void) {
    /* One 4bpp tile filled with colour 1 at the top-left of BG0. */
    DmaFill16(3, 0x1111, (void *)BG_CHAR_ADDR(0) + 32, 32);   /* tile 1 */
    DmaFill16(3, 0, (void *)BG_SCREEN_ADDR(31), BG_SCREEN_SIZE);
    *(vu16 *)BG_SCREEN_ADDR(31) = 1;                          /* entry: tile 1, pal 0 */
    ((vu16 *)BG_PLTT)[0] = RGB(0, 0, 31);
    ((vu16 *)BG_PLTT)[1] = RGB(31, 0, 0);
    REG_BG0CNT = BGCNT_PRIORITY(0) | BGCNT_CHARBASE(0) | BGCNT_SCREENBASE(31) | BGCNT_16COLOR;
    REG_BG0HOFS = 0;
    REG_BG0VOFS = 0;
    REG_BLDCNT = 0;
    REG_DISPCNT = DISPCNT_MODE_0 | DISPCNT_BG0_ON;
    static uint32_t frame[240 * 160];
    pc_gpu_render_frame(frame);
    CHECK(frame[0] == 0xFFFF0000u, "tile pixel red (%08X)", frame[0]);
    CHECK(frame[8] == 0xFF0000FFu, "backdrop blue (%08X)", frame[8]);
    REG_BLDCNT = BLDCNT_TGT1_BG0 | BLDCNT_EFFECT_DARKEN;
    REG_BLDY = 16;
    pc_gpu_render_frame(frame);
    CHECK(frame[0] == 0xFF000000u, "darken 16/16 gives black (%08X)", frame[0]);
    REG_BLDCNT = 0;
    REG_DISPCNT = DISPCNT_FORCED_BLANK;
}

/* ------------------------------------------------------------------ flash */

static void test_flash(const char *dir) {
    char path[1024];
    snprintf(path, sizeof path, "%s/autotest_save.bin", dir);
    remove(path);
    pc_flash_set_path(path);
    CHECK(IdentifyFlash() == 0, "IdentifyFlash");
    CHECK(gFlash && gFlash->sector.size == 0x1000 && gFlash->sector.count == 32, "MX29L010 geometry");

    static u8 sector[0x1000], back[0x1000];
    for (int i = 0; i < 0x1000; i++) sector[i] = (u8)(i * 7 + 1);
    CHECK(ProgramFlashSectorAndVerify(5, sector) == 0, "program + verify sector 5");
    ReadFlash(5, 0, back, sizeof back);
    CHECK(memcmp(sector, back, sizeof back) == 0, "ReadFlash sector 5");

    CHECK(EraseFlashSector(6) == 0, "erase sector 6");
    ReadFlash(6, 0x10, back, 4);
    CHECK(back[0] == 0xFF && back[3] == 0xFF, "erased reads 0xFF");
    CHECK(ProgramFlashByte(6, 0x10, 0x5A) == 0, "program byte");
    ProgramFlashByte(6, 0x10, 0xF0);   /* flash can only clear bits */
    ReadFlash(6, 0x10, back, 1);
    CHECK(back[0] == (0x5A & 0xF0), "byte programming ANDs (%02X)", back[0]);

    pc_flash_flush();
    FILE *f = fopen(path, "rb");
    long size = -1;
    if (f) {
        fseek(f, 0, SEEK_END);
        size = ftell(f);
        fclose(f);
    }
    CHECK(size == 0x20000, "save file is 128 KiB (%ld)", size);

    pc_flash_set_path(path);   /* reload from disk */
    IdentifyFlash();
    ReadFlash(5, 0, back, sizeof back);
    CHECK(memcmp(sector, back, sizeof back) == 0, "sector 5 survives a reload");
    remove(path);
}

/* -------------------------------------------------------------------- RTC */

static int bcd(u8 v) { return (v >> 4) * 10 + (v & 0xF); }

static void test_rtc(const char *dir) {
    char path[1024];
    snprintf(path, sizeof path, "%s/autotest_save.rtc", dir);
    remove(path);
    pc_rtc_set_path(path);

    struct SiiRtcInfo rtc;
    CHECK(SiiRtcProbe() == 1, "probe ok");
    CHECK(SiiRtcGetStatus(&rtc) && (rtc.status & SIIRTCINFO_24HOUR), "24-hour mode");
    CHECK(SiiRtcGetDateTime(&rtc), "get date/time");
    CHECK(bcd(rtc.month) >= 1 && bcd(rtc.month) <= 12 && bcd(rtc.day) >= 1 && bcd(rtc.hour) < 24,
          "valid BCD date %02X-%02X %02X:%02X", rtc.month, rtc.day, rtc.hour, rtc.minute);

    memset(&rtc, 0, sizeof rtc);
    rtc.year = 0x04; rtc.month = 0x02; rtc.day = 0x29;
    rtc.hour = 0x12; rtc.minute = 0x34; rtc.second = 0x56;
    SiiRtcSetDateTime(&rtc);
    memset(&rtc, 0, sizeof rtc);
    SiiRtcGetDateTime(&rtc);
    CHECK(rtc.year == 0x04 && rtc.month == 0x02 && rtc.day == 0x29 && rtc.hour == 0x12 && rtc.minute == 0x34,
          "set/get 2004-02-29 12:34 (%02X-%02X-%02X %02X:%02X)", rtc.year, rtc.month, rtc.day, rtc.hour, rtc.minute);
    CHECK(bcd(rtc.dayOfWeek) == 0, "2004-02-29 was a Sunday (%d)", bcd(rtc.dayOfWeek));

    pc_rtc_set_path(path);   /* the offset persists, like the battery */
    SiiRtcGetDateTime(&rtc);
    CHECK(rtc.year == 0x04 && rtc.day == 0x29, "clock offset survives a reload");

    SiiRtcReset();
    SiiRtcGetDateTime(&rtc);
    CHECK(rtc.year == 0 && rtc.month == 1 && rtc.day == 1 && rtc.hour == 0, "reset to 2000-01-01");
    remove(path);
}

/* ------------------------------------------------------- NULL reads (GBA BIOS) */

struct FarFields {
    u8 pad[0x1E20];
    u8 b;
    u16 h;
    u32 w;
    u64 q;
};

static int __attribute__((noinline)) null_read_cmp(volatile struct FarFields *p) {
    return p->b == 5 ? 1 : 2;   /* compiled as a CMP on memory */
}

static void test_null_reads(void) {
#if defined(_WIN32) || (defined(__x86_64__) && (defined(__linux__) || defined(__APPLE__)))
    volatile struct FarFields *p = NULL;
    u8 b = p->b;
    u16 h = p->h;
    u32 w = p->w;
    u64 q = p->q;
    s32 sx = (s8)p->b;
    CHECK(b == 0 && h == 0 && w == 0 && q == 0 && sx == 0, "loads through NULL read 0 (%u %u %u %llu)",
          b, h, w, (unsigned long long)q);
    CHECK(null_read_cmp(p) == 2, "compare through NULL sees 0");
    p->b = 7;                  /* writes are dropped (BIOS is ROM) */
    p->w = 0x12345678;
    p->h++;                    /* read-modify-write */
    CHECK(p->b == 0 && p->w == 0 && p->h == 0, "writes through NULL are ignored");
#else
    printf("skip: NULL-read emulation needs x86-64\n");
#endif
}

/* -------------------------------------------- division by zero (GBA libgcc: 0) */
static volatile int s_num = 1000, s_zero = 0;
static volatile unsigned s_unum = 77, s_uzero = 0;
static volatile short s_hnum = 50, s_hzero = 0;

/* Out of line, results through volatiles: division by zero is undefined
 * behaviour in C, the optimizer must not see it. */
static volatile int s_q, s_r, s_hq;
static volatile unsigned s_uq;
static volatile long long s_lq;
__attribute__((noinline)) static void do_divs(void) {
    s_q = s_num / s_zero;
    s_r = s_num % s_zero;
    s_uq = s_unum / s_uzero;
    s_lq = (long long)s_num / (long long)s_zero;
    s_hq = s_hnum / s_hzero;
}

static void test_div0(void) {
#if defined(__x86_64__) && (defined(_WIN32) || defined(__linux__))
    s_q = s_r = s_hq = -1;
    s_uq = 1;
    s_lq = -1;
    do_divs();
    int q = s_q, r = s_r, hq = s_hq;
    unsigned uq = s_uq;
    long long lq = s_lq;
    CHECK(q == 0 && r == 0 && uq == 0 && lq == 0 && hq == 0, "division by zero gives 0 (%d %d %u %lld %d)",
          q, r, uq, lq, hq);
    CHECK(s_num / 8 == 125, "normal division still works");
#else
    printf("skip: division-by-zero emulation needs x86-64 Windows/Linux\n");
#endif
}

int main(int argc, char **argv) {
    set_env("PRS_HEADLESS", "1");
    set_env("PRS_FAST", "1");
    const char *root = argc > 1 ? argv[1] : NULL;
    const char *dir = argc > 2 ? argv[2] : ".";

    if (!pc_hw_init()) {
        printf("FAIL: memory map\n");
        return 1;
    }
    pc_video_init();

    test_memory_map();
    test_bios(root);
    test_dma_and_irq();
    test_gpu();
    test_flash(dir);
    test_rtc(dir);
    pc_crash_install();
    test_null_reads();
    test_div0();

    printf("%d checks, %d failures\n", s_checks, s_failures);
    return s_failures ? 1 : 0;
}
