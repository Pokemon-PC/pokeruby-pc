/*
 * main_pc.c - entry point of the PC build: hardware, then AgbMain().
 *
 * Soft reset (A+B+Start+Select, end of the credits, ...): on the GBA the
 * BIOS clears the RAM and restarts the ROM, so every game variable starts
 * over. Here the game's variables all live in one section (pp_c.py places
 * them there); it is saved before the first AgbMain() and restored by
 * SoftReset(), which then longjmps back to the start of the game.
 *
 * Test hooks (environment):
 *   PRS_EXIT_AFTER=N        quit after N frames
 *   PRS_SHOT=F:path.bmp     save frame F (repeatable: F1:a.bmp,F2:b.bmp)
 *   PRS_INPUT=F1-F2:A+UP,.. scripted keys (see scripted_input)
 */
#include <SDL.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hw_pc.h"
#include "pc_hal.h"
#include "platform_pc.h"

void AgbMain(void);
void RegisterRamReset(uint32_t flags);

/* ---- the game's variables ------------------------------------------------ */
#if defined(_WIN32)
/* lld sorts grouped COFF sections by name: $prs0 < $prs1 (game) < $prs2. */
__attribute__((section(".data$prs0"), used)) static char s_game_begin[16];
__attribute__((section(".data$prs2"), used)) static char s_game_end[16];
#define GAME_DATA_BEGIN ((char *)(s_game_begin + sizeof s_game_begin))
#define GAME_DATA_END   ((char *)s_game_end)
#elif defined(__APPLE__)
/* Mach-O: ld64 gives the bounds of any section as section$start/end$SEG$SECT. */
extern char s_prs_game_start[] __asm("section$start$__DATA$__prsgame");
extern char s_prs_game_end[] __asm("section$end$__DATA$__prsgame");
__attribute__((section("__DATA,__prsgame"), used)) static char s_game_anchor = 1;
#define GAME_DATA_BEGIN s_prs_game_start
#define GAME_DATA_END   s_prs_game_end
#else
/* The linker defines __start_/__stop_ for sections named like C identifiers. */
extern char __start_prs_game[], __stop_prs_game[];
__attribute__((section("prs_game"), used)) static char s_game_anchor = 1;
#define GAME_DATA_BEGIN __start_prs_game
#define GAME_DATA_END   __stop_prs_game
#endif

static void *s_snapshot;
static size_t s_snapshot_size;
static jmp_buf s_reset_jump;

static void snapshot_game_data(void) {
    s_snapshot_size = (size_t)(GAME_DATA_END - GAME_DATA_BEGIN);
    s_snapshot = malloc(s_snapshot_size);
    if (!s_snapshot) {
        pc_fatal("out of memory for the reset snapshot (%zu bytes)", s_snapshot_size);
    }
    memcpy(s_snapshot, GAME_DATA_BEGIN, s_snapshot_size);
    pc_log("game variables: %zu KiB at %p", s_snapshot_size / 1024, (void *)GAME_DATA_BEGIN);
}

/* BIOS SoftReset, as libagbsyscall calls it: RegisterRamReset(flags), then
 * restart from the ROM entry point (crt0 clears all RAM again). */
void SoftReset(uint32_t flags) {
    pc_log("soft reset");
    pc_flash_flush();
    RegisterRamReset(flags | 0xFF);
    longjmp(s_reset_jump, 1);
}

/* Same entry the ROM header jumps to when SoftResetRom is used. */
void SoftResetRom(uint32_t flags) {
    SoftReset(flags);
}

/* ---- test hooks ---------------------------------------------------------- */
static long s_exit_after = -1;
static char *s_shots;   /* PRS_SHOT, any length */

static void save_bmp(const char *path, const uint32_t *argb) {
    SDL_Surface *s = SDL_CreateRGBSurfaceWithFormatFrom((void *)argb, PC_GBA_WIDTH, PC_GBA_HEIGHT, 32,
                                                        PC_GBA_WIDTH * 4, SDL_PIXELFORMAT_ARGB8888);
    if (!s || SDL_SaveBMP(s, path) != 0) {
        pc_warn("cannot save %s: %s", path, SDL_GetError());
    } else {
        pc_log("saved %s", path);
    }
    if (s) {
        SDL_FreeSurface(s);
    }
}

/* PRS_REGS=F1,F2,...: log the display registers and palette at those frames. */
static char *s_regs;    /* PRS_REGS */

void pc_debug_dump_game(unsigned long long frame);

static void dump_regs(uint64_t frame) {
    pc_debug_dump_game(frame);
    const volatile uint16_t *io = (const volatile uint16_t *)(uintptr_t)PC_IO_BASE;
    const volatile uint16_t *pal = (const volatile uint16_t *)(uintptr_t)PC_PLTT_BASE;
    pc_log("regs@%llu DISPCNT=%04X BG0CNT=%04X BG1CNT=%04X BG2CNT=%04X BG3CNT=%04X "
           "BLDCNT=%04X BLDALPHA=%04X BLDY=%04X WININ=%04X WINOUT=%04X IE=%04X IME=%04X DISPSTAT=%04X",
           (unsigned long long)frame, io[0], io[4], io[5], io[6], io[7], io[0x28], io[0x29], io[0x2A],
           io[0x24], io[0x25], io[0x100], io[0x104], io[2]);
    char line[512];
    int n = 0;
    for (int i = 0; i < 32; i++) {
        n += snprintf(line + n, sizeof line - (size_t)n, "%04X ", pal[i]);
    }
    pc_log("regs@%llu BGPAL[0..31] %s", (unsigned long long)frame, line);
    n = 0;
    for (int i = 256; i < 272; i++) {
        n += snprintf(line + n, sizeof line - (size_t)n, "%04X ", pal[i]);
    }
    pc_log("regs@%llu OBJPAL[0..15] %s", (unsigned long long)frame, line);
    /* How much of each VRAM area is non-zero: 4 BG char blocks (16 KiB),
     * the 32 screen blocks (2 KiB, in percent), OBJ tiles. */
    const volatile uint8_t *vram = (const volatile uint8_t *)(uintptr_t)PC_VRAM_BASE;
    n = 0;
    for (int b = 0; b < 4; b++) {
        int nz = 0;
        for (int i = 0; i < 0x4000; i++) nz += vram[b * 0x4000 + i] != 0;
        n += snprintf(line + n, sizeof line - (size_t)n, "char%d=%d%% ", b, nz * 100 / 0x4000);
    }
    int nzobj = 0;
    for (int i = 0x10000; i < 0x18000; i++) nzobj += vram[i] != 0;
    n += snprintf(line + n, sizeof line - (size_t)n, "obj=%d%% | screen:", nzobj * 100 / 0x8000);
    for (int s = 0; s < 32; s++) {
        int nz = 0;
        for (int i = 0; i < 0x800; i++) nz += vram[s * 0x800 + i] != 0;
        n += snprintf(line + n, sizeof line - (size_t)n, " %d", nz * 100 / 0x800);
    }
    pc_log("regs@%llu VRAM %s", (unsigned long long)frame, line);

    /* PRS_PEEK=ADDR:LEN,...: raw memory (addresses from `nm pokeruby.exe`,
     * statics included), 16 bytes per line. */
    const char *peek = getenv("PRS_PEEK");
    if (peek && *peek) {
        char *list = strdup(peek), *save = NULL;
        for (char *tok = strtok_r(list, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
            char *colon = strchr(tok, ':');
            uintptr_t addr = (uintptr_t)strtoull(tok, NULL, 0);
            size_t len = colon ? (size_t)strtoull(colon + 1, NULL, 0) : 16;
            for (size_t off = 0; off < len; off += 16) {
                n = 0;
                for (size_t i = off; i < off + 16 && i < len; i++) {
                    n += snprintf(line + n, sizeof line - (size_t)n, "%02X ",
                                  ((const volatile uint8_t *)addr)[i]);
                }
                pc_log("peek@%llu %08llX: %s", (unsigned long long)frame, (unsigned long long)(addr + off), line);
            }
        }
        free(list);
    }
}

void pc_debug_frame(unsigned long long frame);

static void frame_hook(uint64_t frame, const uint32_t *argb) {
    pc_debug_frame(frame);
    if (s_shots) {
        char *list = strdup(s_shots), *save = NULL;
        for (char *tok = strtok_r(list, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
            char *colon = strchr(tok, ':');
            if (colon && strtoull(tok, NULL, 10) == frame) {
                save_bmp(colon + 1, argb);
            }
        }
        free(list);
    }
    if (s_regs) {
        char *list = strdup(s_regs), *save = NULL;
        for (char *tok = strtok_r(list, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
            if (strtoull(tok, NULL, 10) == frame) {
                dump_regs(frame);
            }
        }
        free(list);
    }
    if (s_exit_after >= 0 && (long)frame >= s_exit_after) {
        pc_log("PRS_EXIT_AFTER reached (%ld frames)", s_exit_after);
        pc_flash_flush();
        exit(0);
    }
}

/* PRS_INPUT="F1-F2:KEYS,F3:KEYS,F4-F5/P:KEYS,..." holds KEYS (A,B,START,
 * SELECT,UP,DOWN,LEFT,RIGHT,L,R joined by '+') during frames F1..F2 (or
 * frame F3 only); "/P" presses them for 4 frames every P frames instead.
 * Replaces keyboard/gamepad input for the whole run. */
static char *s_input;

static uint16_t parse_keys(const char *s) {
    static const struct { const char *name; uint16_t bit; } kKeys[] = {
        { "A", 0x001 }, { "B", 0x002 }, { "SELECT", 0x004 }, { "START", 0x008 },
        { "RIGHT", 0x010 }, { "LEFT", 0x020 }, { "UP", 0x040 }, { "DOWN", 0x080 },
        { "R", 0x100 }, { "L", 0x200 },
    };
    uint16_t keys = 0;
    char buf[128];
    snprintf(buf, sizeof buf, "%s", s);
    for (char *tok = strtok(buf, "+"); tok; tok = strtok(NULL, "+")) {
        for (unsigned i = 0; i < sizeof kKeys / sizeof kKeys[0]; i++) {
            if (strcmp(tok, kKeys[i].name) == 0) {
                keys |= kKeys[i].bit;
            }
        }
    }
    return keys;
}

static uint16_t scripted_input(uint64_t frame) {
    uint16_t keys = 0;
    char *list = strdup(s_input);
    char *save = NULL;
    for (char *tok = strtok_r(list, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
        char *colon = strchr(tok, ':');
        if (!colon) {
            continue;
        }
        *colon = '\0';
        char *dash = strchr(tok, '-');
        char *slash = strchr(tok, '/');
        uint64_t f1 = strtoull(tok, NULL, 10);
        uint64_t f2 = dash ? strtoull(dash + 1, NULL, 10) : f1;
        uint64_t period = slash ? strtoull(slash + 1, NULL, 10) : 0;
        if (frame >= f1 && frame <= f2 && (period == 0 || (frame - f1) % period < 4)) {
            keys |= parse_keys(colon + 1);
        }
    }
    free(list);
    return keys;
}

/* ---- entry ----------------------------------------------------------------- */
static void run_game(void *arg) {
    (void)arg;
    pc_crash_install();
    pc_lang_apply();          /* French strings, before the snapshot */
    snapshot_game_data();
    if (setjmp(s_reset_jump) != 0) {
        memcpy(GAME_DATA_BEGIN, s_snapshot, s_snapshot_size);
        for (int ch = 0; ch < 4; ch++) {
            pc_dma_stop(ch);
        }
        pc_timers_reset();
    }
    RegisterRamReset(0xFF);   /* crt0: clear RAM and registers before AgbMain */
    AgbMain();
    pc_warn("AgbMain returned");
}

/* Command line (for a launcher): --lang en|fr|auto, --rom PATH (the
 * player's French ROM). Same as PRS_LANG / PRS_ROM, which they set. */
static void parse_args(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        const char *name = NULL, *value = NULL;
        if (strncmp(argv[i], "--lang", 6) == 0) {
            name = "PRS_LANG";
            value = argv[i][6] == '=' ? argv[i] + 7 : (argv[i][6] == '\0' && i + 1 < argc ? argv[++i] : NULL);
        } else if (strncmp(argv[i], "--rom", 5) == 0) {
            name = "PRS_ROM";
            value = argv[i][5] == '=' ? argv[i] + 6 : (argv[i][5] == '\0' && i + 1 < argc ? argv[++i] : NULL);
        }
        if (name == NULL || value == NULL) {
            pc_warn("unknown argument '%s' (known: --lang en|fr|auto, --rom PATH)", argv[i]);
            continue;
        }
#if defined(_WIN32)
        _putenv_s(name, value);
#else
        setenv(name, value, 1);
#endif
    }
}

int main(int argc, char **argv) {
    parse_args(argc, argv);
    const char *e = getenv("PRS_EXIT_AFTER");
    if (e && *e) {
        s_exit_after = strtol(e, NULL, 10);
    }
    e = getenv("PRS_SHOT");
    if (e && *e) {
        s_shots = strdup(e);
    }
    if (!pc_hw_init()) {
        pc_fatal("cannot reserve the GBA memory map");
    }
    if (!pc_video_init()) {
        pc_fatal("cannot open the window");
    }
    pc_audio_init();
    pc_video_set_frame_hook(frame_hook);
    e = getenv("PRS_REGS");
    if (e && *e) {
        s_regs = strdup(e);
    }
    e = getenv("PRS_INPUT");
    if (e && *e) {
        s_input = strdup(e);
        pc_input_set_override(scripted_input);
    }
    pc_run_on_low_stack(run_game, NULL);
    pc_flash_flush();
    pc_audio_shutdown();
    pc_video_shutdown();
    SDL_Quit();
    return 0;
}
