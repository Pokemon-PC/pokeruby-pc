#include "gpu_pc.h"

#include "hw_pc.h"

#include <stdbool.h>
#include <string.h>

#define W PC_GBA_WIDTH
#define H PC_GBA_HEIGHT

#define IO16(off) (*(volatile uint16_t *)(uintptr_t)(PC_IO_BASE + (off)))
#define IO32(off) (*(volatile uint32_t *)(uintptr_t)(PC_IO_BASE + (off)))

static const uint8_t *const VRAM8 = (const uint8_t *)(uintptr_t)PC_VRAM_BASE;
static const uint16_t *const PAL = (const uint16_t *)(uintptr_t)PC_PLTT_BASE;
static const uint16_t *const OAM16 = (const uint16_t *)(uintptr_t)PC_OAM_BASE;

/* Layer pixel: bit 15 set = opaque, bits 0-14 = BGR555. */
#define OPAQUE 0x8000u

/* Affine reference points (BG2, BG3): internal counters + last raw value. */
static int32_t s_refx[2], s_refy[2];
static uint32_t s_rawx[2], s_rawy[2];

static int32_t sext28(uint32_t v) { return (int32_t)(v << 4) >> 4; }

static void latch_ref(int i, bool force) {
    uint32_t rx = IO32(0x28 + i * 0x10), ry = IO32(0x2C + i * 0x10);
    /* A write by the game (detected as a changed value) reloads the counter. */
    if (force || rx != s_rawx[i]) {
        s_rawx[i] = rx;
        s_refx[i] = sext28(rx);
    }
    if (force || ry != s_rawy[i]) {
        s_rawy[i] = ry;
        s_refy[i] = sext28(ry);
    }
}

void pc_gpu_frame_start(void) {
    latch_ref(0, true);
    latch_ref(1, true);
}

/* ------------------------------------------------------------ backgrounds */

static void render_text_bg(int bg, int y, uint16_t *line) {
    uint16_t cnt = IO16(0x08 + bg * 2);
    uint32_t charbase = ((cnt >> 2) & 3) * 0x4000u;
    uint32_t screenbase = ((cnt >> 8) & 0x1F) * 0x800u;
    bool bpp8 = (cnt >> 7) & 1;
    int size = cnt >> 14;
    int wpx = (size & 1) ? 512 : 256, hpx = (size & 2) ? 512 : 256;
    int hofs = IO16(0x10 + bg * 4) & 0x1FF, vofs = IO16(0x12 + bg * 4) & 0x1FF;
    int sy = (y + vofs) & (hpx - 1);

    for (int x = 0; x < W; x++) {
        int sx = (x + hofs) & (wpx - 1);
        uint32_t block = (uint32_t)((sy >> 8) * (wpx >> 8) + (sx >> 8));
        uint32_t mapaddr = screenbase + block * 0x800u + (uint32_t)(((sy & 255) >> 3) * 32 + ((sx & 255) >> 3)) * 2;
        if (mapaddr >= 0x10000) {
            continue;
        }
        uint16_t e = (uint16_t)(VRAM8[mapaddr] | (VRAM8[mapaddr + 1] << 8));
        int px = sx & 7, py = sy & 7;
        if (e & 0x400) px = 7 - px;
        if (e & 0x800) py = 7 - py;
        uint32_t tile = e & 0x3FF;
        uint8_t ci;
        if (bpp8) {
            uint32_t a = charbase + tile * 64 + (uint32_t)(py * 8 + px);
            if (a >= 0x10000) continue;
            ci = VRAM8[a];
            if (ci) line[x] = OPAQUE | (PAL[ci] & 0x7FFF);
        } else {
            uint32_t a = charbase + tile * 32 + (uint32_t)(py * 4 + (px >> 1));
            if (a >= 0x10000) continue;
            ci = (VRAM8[a] >> ((px & 1) * 4)) & 0xF;
            if (ci) line[x] = OPAQUE | (PAL[(e >> 12) * 16 + ci] & 0x7FFF);
        }
    }
}

/* mode: 0 = affine tiled, 3/4/5 = bitmap modes (BG2 only). */
static void render_affine_bg(int bg, int y, uint16_t *line, int mode) {
    int i = bg - 2;
    uint16_t cnt = IO16(0x08 + bg * 2);
    int16_t pa = (int16_t)IO16(0x20 + i * 0x10), pc = (int16_t)IO16(0x24 + i * 0x10);
    int32_t cx = s_refx[i], cy = s_refy[i];
    (void)y;

    if (mode == 0) {
        uint32_t charbase = ((cnt >> 2) & 3) * 0x4000u;
        uint32_t screenbase = ((cnt >> 8) & 0x1F) * 0x800u;
        int sizepx = 128 << (cnt >> 14);
        bool wrap = (cnt >> 13) & 1;
        for (int x = 0; x < W; x++, cx += pa, cy += pc) {
            int tx = cx >> 8, ty = cy >> 8;
            if (wrap) {
                tx &= sizepx - 1;
                ty &= sizepx - 1;
            } else if (tx < 0 || ty < 0 || tx >= sizepx || ty >= sizepx) {
                continue;
            }
            uint32_t mapaddr = screenbase + (uint32_t)((ty >> 3) * (sizepx >> 3) + (tx >> 3));
            if (mapaddr >= 0x10000) continue;
            uint32_t a = charbase + VRAM8[mapaddr] * 64u + (uint32_t)((ty & 7) * 8 + (tx & 7));
            if (a >= 0x10000) continue;
            uint8_t ci = VRAM8[a];
            if (ci) line[x] = OPAQUE | (PAL[ci] & 0x7FFF);
        }
        return;
    }

    int bw = mode == 5 ? 160 : 240, bh = mode == 5 ? 128 : 160;
    uint32_t page = (mode != 3 && (IO16(0) & 0x10)) ? 0xA000u : 0;
    for (int x = 0; x < W; x++, cx += pa, cy += pc) {
        int tx = cx >> 8, ty = cy >> 8;
        if (tx < 0 || ty < 0 || tx >= bw || ty >= bh) continue;
        if (mode == 4) {
            uint8_t ci = VRAM8[page + (uint32_t)(ty * 240 + tx)];
            if (ci) line[x] = OPAQUE | (PAL[ci] & 0x7FFF);
        } else {
            uint32_t a = page + (uint32_t)(ty * bw + tx) * 2;
            line[x] = OPAQUE | ((VRAM8[a] | (VRAM8[a + 1] << 8)) & 0x7FFF);
        }
    }
}

/* ---------------------------------------------------------------- sprites */

static const uint8_t kObjSize[3][4][2] = {
    { { 8, 8 }, { 16, 16 }, { 32, 32 }, { 64, 64 } },   /* square */
    { { 16, 8 }, { 32, 8 }, { 32, 16 }, { 64, 32 } },   /* horizontal */
    { { 8, 16 }, { 8, 32 }, { 16, 32 }, { 32, 64 } },   /* vertical */
};

typedef struct {
    uint16_t color[W];   /* OPAQUE | BGR555 */
    uint8_t  prio[W];
    uint8_t  semi[W];    /* semi-transparent (mode 1) */
    uint8_t  win[W];     /* inside the OBJ window */
} ObjLine;

static void render_objs(int y, ObjLine *ol) {
    uint16_t dispcnt = IO16(0);
    bool map1d = (dispcnt >> 6) & 1;
    bool bitmap = (dispcnt & 7) >= 3;
    const uint8_t *objvram = VRAM8 + 0x10000;

    memset(ol->color, 0, sizeof ol->color);
    memset(ol->prio, 4, sizeof ol->prio);
    memset(ol->semi, 0, sizeof ol->semi);
    memset(ol->win, 0, sizeof ol->win);

    for (int n = 0; n < 128; n++) {
        uint16_t a0 = OAM16[n * 4], a1 = OAM16[n * 4 + 1], a2 = OAM16[n * 4 + 2];
        bool affine = (a0 >> 8) & 1;
        if (!affine && ((a0 >> 9) & 1)) continue;        /* disabled */
        int shape = (a0 >> 14) & 3;
        if (shape == 3) continue;
        int sz = (a1 >> 14) & 3;
        int w = kObjSize[shape][sz][0], h = kObjSize[shape][sz][1];
        bool dbl = affine && ((a0 >> 9) & 1);
        int bw = dbl ? w * 2 : w, bh = dbl ? h * 2 : h;
        int oy = a0 & 0xFF;
        if (oy >= H) oy -= 256;
        int ox = a1 & 0x1FF;
        if (ox >= 256) ox -= 512;
        if (y < oy || y >= oy + bh) continue;

        int mode = (a0 >> 10) & 3;
        if (mode == 3) continue;
        bool bpp8 = (a0 >> 13) & 1;
        uint32_t tile = a2 & 0x3FF;
        if (bitmap && tile < 512) continue;
        int prio = (a2 >> 10) & 3;
        int palbank = a2 >> 12;
        int rowtiles = map1d ? (w / 8) * (bpp8 ? 2 : 1) : 32;

        int16_t pa = 0x100, pb = 0, pc = 0, pd = 0x100;
        if (affine) {
            int g = (a1 >> 9) & 0x1F;
            pa = (int16_t)OAM16[g * 16 + 3];
            pb = (int16_t)OAM16[g * 16 + 7];
            pc = (int16_t)OAM16[g * 16 + 11];
            pd = (int16_t)OAM16[g * 16 + 15];
        }
        int ly = y - oy;
        for (int lx = 0; lx < bw; lx++) {
            int sx = ox + lx;
            if (sx < 0 || sx >= W) continue;
            int tx, ty;
            if (affine) {
                int dx = lx - bw / 2, dy = ly - bh / 2;
                tx = ((pa * dx + pb * dy) >> 8) + w / 2;
                ty = ((pc * dx + pd * dy) >> 8) + h / 2;
                if (tx < 0 || ty < 0 || tx >= w || ty >= h) continue;
            } else {
                tx = (a1 & 0x1000) ? w - 1 - lx : lx;
                ty = (a1 & 0x2000) ? h - 1 - ly : ly;
            }
            uint32_t t = tile + (uint32_t)((ty >> 3) * rowtiles) + (uint32_t)((tx >> 3) * (bpp8 ? 2 : 1));
            uint8_t ci;
            uint16_t col;
            if (bpp8) {
                uint32_t addr = (t & 0x3FF) * 32 + (uint32_t)((ty & 7) * 8 + (tx & 7));
                ci = objvram[addr & 0x7FFF];
                col = PAL[256 + ci];
            } else {
                uint32_t addr = (t & 0x3FF) * 32 + (uint32_t)((ty & 7) * 4 + ((tx & 7) >> 1));
                ci = (objvram[addr & 0x7FFF] >> ((tx & 1) * 4)) & 0xF;
                col = PAL[256 + palbank * 16 + ci];
            }
            if (!ci) continue;
            if (mode == 2) {
                ol->win[sx] = 1;
                continue;
            }
            /* Lower OAM index wins among sprites; then by priority. */
            if (prio < ol->prio[sx]) {
                ol->color[sx] = OPAQUE | (col & 0x7FFF);
                ol->prio[sx] = (uint8_t)prio;
                ol->semi[sx] = mode == 1;
            }
        }
    }
}

/* ------------------------------------------------------------- composing */

static inline uint32_t to_argb(uint16_t c) {
    uint32_t r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31;
    r = (r << 3) | (r >> 2);
    g = (g << 3) | (g >> 2);
    b = (b << 3) | (b >> 2);
    return 0xFF000000u | (r << 16) | (g << 8) | b;
}

static inline uint16_t blend_alpha(uint16_t a, uint16_t b, int eva, int evb) {
    int r = ((a & 31) * eva + (b & 31) * evb) >> 4;
    int g = (((a >> 5) & 31) * eva + ((b >> 5) & 31) * evb) >> 4;
    int bl = (((a >> 10) & 31) * eva + ((b >> 10) & 31) * evb) >> 4;
    if (r > 31) r = 31;
    if (g > 31) g = 31;
    if (bl > 31) bl = 31;
    return (uint16_t)(r | (g << 5) | (bl << 10));
}

static inline uint16_t brighten(uint16_t c, int evy, bool up) {
    int ch[3] = { c & 31, (c >> 5) & 31, (c >> 10) & 31 };
    for (int k = 0; k < 3; k++) {
        ch[k] = up ? ch[k] + (((31 - ch[k]) * evy) >> 4) : ch[k] - ((ch[k] * evy) >> 4);
    }
    return (uint16_t)(ch[0] | (ch[1] << 5) | (ch[2] << 10));
}

static bool in_range(int v, int lo, int hi, int limit) {
    if (lo <= hi) {
        if (hi > limit) hi = limit;        /* garbage right/bottom edge */
        return v >= lo && v < hi;
    }
    return v >= lo || v < hi;              /* left > right: wraps around */
}

void pc_gpu_render_line(int y, uint32_t *out) {
    uint16_t dispcnt = IO16(0);
    if (dispcnt & 0x80) {                 /* forced blank: white */
        for (int x = 0; x < W; x++) out[x] = 0xFFFFFFFFu;
        return;
    }
    latch_ref(0, false);
    latch_ref(1, false);

    int mode = dispcnt & 7;
    static uint16_t bgline[4][W];
    bool bg_on[4] = { false, false, false, false };
    for (int bg = 0; bg < 4; bg++) {
        if (!((dispcnt >> (8 + bg)) & 1)) continue;
        bool text = mode == 0 || (mode == 1 && bg < 2);
        bool aff = (mode == 1 && bg == 2) || (mode == 2 && bg >= 2);
        bool bmp = mode >= 3 && mode <= 5 && bg == 2;
        if (!text && !aff && !bmp) continue;
        memset(bgline[bg], 0, sizeof bgline[bg]);
        if (text) render_text_bg(bg, y, bgline[bg]);
        else render_affine_bg(bg, y, bgline[bg], bmp ? mode : 0);
        bg_on[bg] = true;
    }
    static ObjLine ol;
    bool obj_on = (dispcnt >> 12) & 1;
    if (obj_on || (dispcnt & 0x8000)) render_objs(y, &ol);

    /* Window masks: bit 0-3 BG, 4 OBJ, 5 color effect. */
    bool any_win = (dispcnt & 0xE000) != 0;
    uint16_t winin = IO16(0x48), winout = IO16(0x4A);
    uint8_t wmask[W];
    for (int x = 0; x < W; x++) {
        uint8_t m = 0x3F;
        if (any_win) {
            m = winout & 0x3F;
            if ((dispcnt & 0x8000) && obj_on && ol.win[x]) m = (winout >> 8) & 0x3F;
            for (int wi = 1; wi >= 0; wi--) {
                if (!((dispcnt >> (13 + wi)) & 1)) continue;
                uint16_t wh = IO16(0x40 + wi * 2), wv = IO16(0x44 + wi * 2);
                int x1 = wh >> 8, x2 = wh & 0xFF, y1 = wv >> 8, y2 = wv & 0xFF;
                if (in_range(x, x1, x2, W) && in_range(y, y1, y2, H)) {
                    m = (winin >> (wi * 8)) & 0x3F;
                }
            }
        }
        wmask[x] = m;
    }

    uint16_t bldcnt = IO16(0x50), bldalpha = IO16(0x52);
    int eva = bldalpha & 31, evb = (bldalpha >> 8) & 31, evy = IO16(0x54) & 31;
    if (eva > 16) eva = 16;
    if (evb > 16) evb = 16;
    if (evy > 16) evy = 16;
    int effect = (bldcnt >> 6) & 3;
    uint16_t backdrop = PAL[0] & 0x7FFF;

    for (int x = 0; x < W; x++) {
        /* Find the two front-most layers: 0-3 BG, 4 OBJ, 5 backdrop. */
        int top = 5, second = 5, topprio = 4, secprio = 4;
        uint16_t topc = backdrop, secc = backdrop;
        for (int prio = 0; prio < 4 && second == 5; prio++) {
            if (obj_on && (wmask[x] & 0x10) && (ol.color[x] & OPAQUE) && ol.prio[x] == prio) {
                if (top == 5) { top = 4; topc = ol.color[x]; topprio = prio; }
                else if (second == 5) { second = 4; secc = ol.color[x]; secprio = prio; }
            }
            for (int bg = 0; bg < 4; bg++) {
                if (!bg_on[bg] || !(wmask[x] & (1 << bg)) || !(bgline[bg][x] & OPAQUE)) continue;
                if ((IO16(0x08 + bg * 2) & 3) != prio) continue;
                if (top == 5) { top = bg; topc = bgline[bg][x]; topprio = prio; }
                else if (second == 5) { second = bg; secc = bgline[bg][x]; secprio = prio; break; }
            }
        }
        (void)topprio;
        (void)secprio;
        uint16_t c = topc & 0x7FFF;
        bool t1 = (bldcnt >> top) & 1, t2 = (bldcnt >> (8 + second)) & 1;
        if (top == 4 && ol.semi[x] && t2) {
            c = blend_alpha(c, secc & 0x7FFF, eva, evb);   /* forced alpha */
        } else if (wmask[x] & 0x20) {
            if (effect == 1 && t1 && t2) c = blend_alpha(c, secc & 0x7FFF, eva, evb);
            else if (effect == 2 && t1) c = brighten(c, evy, true);
            else if (effect == 3 && t1) c = brighten(c, evy, false);
        }
        out[x] = to_argb(c);
    }

    /* Affine counters advance by (pb, pd) after each line. */
    for (int i = 0; i < 2; i++) {
        s_refx[i] += (int16_t)IO16(0x22 + i * 0x10);
        s_refy[i] += (int16_t)IO16(0x26 + i * 0x10);
    }
}

void pc_gpu_render_frame(uint32_t *out) {
    pc_gpu_frame_start();
    for (int y = 0; y < H; y++) {
        pc_gpu_render_line(y, out + y * W);
    }
}
