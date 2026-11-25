/*
 * irq_pc.c - interrupts and the display timeline.
 *
 * The GBA draws 228 lines per frame (160 visible + 68 of VBlank) at
 * 59.7275 Hz. The game's main loop busy-waits for the VBlank interrupt
 * (WaitForVBlank in src/main.c). On PC that wait is where time advances:
 * pc_wait_vblank() walks the lines from where the previous call stopped up
 * to the start of the next VBlank, firing exactly what the hardware would on
 * each line - VCount match IRQ, line rendering, HBlank DMA and HBlank IRQ -
 * then VBlank DMA and the VBlank IRQ. Game code between two waits thus runs
 * "during VBlank", which is where a real GBA game does most of its work.
 */
#include "pc_hal.h"

#include "gpu_pc.h"
#include "hw_pc.h"
#include "platform_pc.h"

#include <SDL.h>
#include <stdlib.h>

#define REG16(off) (*(volatile uint16_t *)(uintptr_t)(PC_IO_BASE + (off)))
#define REG_DISPSTAT REG16(0x004)
#define REG_IE       REG16(0x200)
#define REG_IF       REG16(0x202)
#define REG_IME      REG16(0x208)

#define F_VBLANK 0x0001
#define F_HBLANK 0x0002
#define F_VCOUNT 0x0004

#define LINES_PER_FRAME 228
#define FRAME_HZ        (16777216.0 / 280896.0)   /* 59.7275 */

static PcIntrFunc *s_table;
static int s_table_count;
static int s_line;             /* next line to process */
static uint64_t s_frames;
static uint32_t s_frame[PC_GBA_WIDTH * PC_GBA_HEIGHT];
static bool s_in_handler;

/* IntrMain's priority order (pokeruby src/crt0.s: Serial, Timer3, HBlank,
 * VBlank, VCount, ... - pokeemerald tests VCount first), as a table index
 * for each IF bit: gIntrTable (main.c) is laid out in that order. */
static const struct { uint16_t flag; int index; } kOrder[] = {
    { 0x0080, 0 },  /* Serial  */
    { 0x0040, 1 },  /* Timer3  */
    { 0x0002, 2 },  /* HBlank  */
    { 0x0001, 3 },  /* VBlank  */
    { 0x0004, 4 },  /* VCount  */
    { 0x0008, 5 },  /* Timer0  */
    { 0x0010, 6 },  /* Timer1  */
    { 0x0020, 7 },  /* Timer2  */
    { 0x0100, 8 },  /* DMA0    */
    { 0x0200, 9 },  /* DMA1    */
    { 0x0400, 10 }, /* DMA2    */
    { 0x0800, 11 }, /* DMA3    */
    { 0x1000, 12 }, /* Keypad  */
    { 0x2000, 13 }, /* GamePak */
};

void pc_irq_set_table(PcIntrFunc *table, int count) {
    s_table = table;
    s_table_count = count;
}

/* Runs pending, enabled interrupts one at a time, highest priority first. */
static void dispatch(void) {
    if (s_in_handler) {
        return;   /* IntrMain masks the source being serviced; keep it simple */
    }
    for (;;) {
        if (!(REG_IME & 1)) {
            return;
        }
        uint16_t pending = REG_IE & REG_IF;
        if (!pending) {
            return;
        }
        int idx = -1;
        uint16_t flag = 0;
        for (unsigned i = 0; i < sizeof kOrder / sizeof kOrder[0]; i++) {
            if (pending & kOrder[i].flag) {
                idx = kOrder[i].index;
                flag = kOrder[i].flag;
                break;
            }
        }
        REG_IF &= (uint16_t)~flag;           /* acknowledge, as IntrMain does */
        if (idx < 0 || !s_table || idx >= s_table_count || !s_table[idx]) {
            continue;
        }
        s_in_handler = true;
        s_table[idx]();
        s_in_handler = false;
    }
}

void pc_irq_raise(uint16_t flag) {
    REG_IF |= flag;
    dispatch();
}

uint64_t pc_frame_count(void) {
    return s_frames;
}

/* ---- pacing ------------------------------------------------------------ */

static void pace(void) {
    static uint64_t next;
    static int fast = -1;
    if (fast < 0) {
        const char *e = getenv("PRS_FAST");
        fast = e && *e && *e != '0';
    }
    if (fast) {
        return;
    }
    uint64_t freq = SDL_GetPerformanceFrequency();
    uint64_t period = (uint64_t)(freq / FRAME_HZ);
    uint64_t now = SDL_GetPerformanceCounter();
    if (next == 0 || now > next + period * 4) {
        next = now;   /* first frame, or we fell far behind: resync */
    }
    next += period;
    while ((now = SDL_GetPerformanceCounter()) < next) {
        uint64_t left_ms = (next - now) * 1000 / freq;
        if (left_ms > 2) {
            SDL_Delay((uint32_t)(left_ms - 1));
        }
    }
}

/* ---- the line loop ----------------------------------------------------- */

#define CYCLES_PER_LINE 1232

static void do_line(int line) {
    pc_timers_step(CYCLES_PER_LINE);
    uint16_t stat = REG_DISPSTAT;
    pc_hw_set_vcount((uint16_t)line, false);
    if ((stat >> 8) == line && (stat & 0x20)) {
        pc_irq_raise(F_VCOUNT);
    }
    if (line == 0) {
        pc_gpu_frame_start();
    }
    if (line < PC_GBA_HEIGHT) {
        pc_gpu_render_line(line, s_frame + line * PC_GBA_WIDTH);
    }
    /* HBlank: DMA first (it stalls the CPU), then the IRQ. */
    pc_hw_set_vcount((uint16_t)line, true);
    if (line < PC_GBA_HEIGHT) {
        pc_dma_on_hblank();
    }
    if (REG_DISPSTAT & 0x10) {
        pc_irq_raise(F_HBLANK);
    }
    dispatch();   /* something enabled IME/IE with a flag pending */
}

/* Line 160: the frame is complete, VBlank starts. */
static void do_vblank_line(int line) {
    pc_timers_step(CYCLES_PER_LINE);
    pc_hw_set_vcount((uint16_t)line, false);
    s_frames++;
    pc_watchdog_kick();
    pc_video_present(s_frame);
    pc_flash_flush();
    pace();
    pc_hw_set_keys(pc_input_poll());
    if (pc_quit_requested()) {
        pc_log("quit requested");
        exit(0);
    }
    pc_dma_on_vblank();
    if (REG_DISPSTAT & 0x08) {
        pc_irq_raise(F_VBLANK);
    }
    dispatch();
    /* Line 160 also has its VCount check and HBlank. */
    uint16_t stat = REG_DISPSTAT;
    if ((stat >> 8) == line && (stat & 0x20)) {
        pc_irq_raise(F_VCOUNT);
    }
    pc_hw_set_vcount((uint16_t)line, true);
    if (REG_DISPSTAT & 0x10) {
        pc_irq_raise(F_HBLANK);
    }
}

/* One scanline of time. Returns true when it was the start of VBlank. */
static bool step_line(void) {
    int line = s_line;
    s_line = (s_line + 1) % LINES_PER_FRAME;
    if (line == PC_GBA_HEIGHT) {
        do_vblank_line(line);
        return true;
    }
    do_line(line);
    return false;
}

void pc_wait_vblank(void) {
    while (!step_line()) {
    }
    /* On hardware the VBlank interrupt handler (register copies, DMA queue,
     * sound) takes time: the main loop resumes on a later line, and code
     * such as SetGpuReg() relies on VCOUNT being in 161..225 then (it writes
     * registers directly instead of queueing them for the next VBlank). */
    step_line();
}

void pc_advance_line(void) {
    step_line();
}

uint16_t pc_vcount_for_game_code(void) {
    return s_in_handler ? REG16(0x006) : (uint16_t)(PC_GBA_HEIGHT + 1);
}
