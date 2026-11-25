/*
 * timer_pc.c - the four GBA timers (TM0-TM3), advanced with the display
 * timeline (1232 CPU cycles per scanline).
 *
 * TMxCNT_L is one address with two meanings: writing sets the reload value,
 * reading gives the running counter. Plain memory cannot tell them apart, so
 * the counter is published into the register after each step, and a value
 * that differs from the one published means the game wrote a new reload
 * (applied at the next overflow, as on hardware). Starting a timer (enable
 * bit 0 -> 1) loads the counter from the reload value.
 */
#include "pc_hal.h"

#include "hw_pc.h"

#define TMCNT_L(i) (*(volatile uint16_t *)(uintptr_t)(PC_IO_BASE + 0x100 + (i) * 4))
#define TMCNT_H(i) (*(volatile uint16_t *)(uintptr_t)(PC_IO_BASE + 0x102 + (i) * 4))

#define TM_ENABLE  0x80
#define TM_IRQ     0x40
#define TM_CASCADE 0x04

static const uint32_t kPrescaleShift[4] = { 0, 6, 8, 10 };   /* 1, 64, 256, 1024 */

typedef struct {
    bool     running;
    uint16_t reload;
    uint16_t published;   /* last counter value written to TMxCNT_L */
    uint32_t counter;     /* 16-bit counter, kept wider for the arithmetic */
    uint32_t sub;         /* cycles not yet turned into ticks */
} Timer;

static Timer s_tm[4];

/* Adds `ticks` to timer i; returns the number of overflows. */
static uint32_t tick(int i, uint32_t ticks) {
    Timer *t = &s_tm[i];
    uint32_t overflows = 0;
    t->counter += ticks;
    while (t->counter >= 0x10000) {
        overflows++;
        t->counter = t->counter - 0x10000 + t->reload;
        if (t->reload == 0xFFFF) {     /* would overflow every tick: count once */
            t->counter = t->reload;
            break;
        }
    }
    return overflows;
}

void pc_timers_step(uint32_t cycles) {
    uint32_t prev_overflows = 0;
    for (int i = 0; i < 4; i++) {
        Timer *t = &s_tm[i];
        uint16_t cnt = TMCNT_H(i);
        uint16_t reg = TMCNT_L(i);

        if (!(cnt & TM_ENABLE)) {
            /* Stopped: what the game writes is the reload value. */
            t->running = false;
            t->reload = reg;
            t->published = reg;
            prev_overflows = 0;
            continue;
        }
        if (!t->running) {
            t->running = true;
            t->reload = reg;
            t->counter = reg;
            t->sub = 0;
        } else if (reg != t->published) {
            t->reload = reg;   /* written while running: next reload */
        }

        uint32_t overflows;
        if ((cnt & TM_CASCADE) && i > 0) {
            overflows = tick(i, prev_overflows);
        } else {
            uint32_t shift = kPrescaleShift[cnt & 3];
            t->sub += cycles;
            uint32_t ticks = t->sub >> shift;
            t->sub &= (1u << shift) - 1;
            overflows = tick(i, ticks);
        }
        if (overflows && (cnt & TM_IRQ)) {
            pc_irq_raise((uint16_t)(0x0008 << i));   /* INTR_FLAG_TIMER0..3 */
        }
        t->published = (uint16_t)t->counter;
        TMCNT_L(i) = t->published;
        prev_overflows = overflows;
    }
}

void pc_timers_reset(void) {
    for (int i = 0; i < 4; i++) {
        s_tm[i] = (Timer){ 0 };
    }
}
