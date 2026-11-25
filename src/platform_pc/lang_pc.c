/*
 * lang_pc.c - choice of language: English (built in) or French (from the
 * player's own French ROM, never shipped).
 *
 * Each English string listed in src/platform_pc/lang/fr_axvf.tsv (Ruby) or
 * fr_axpf.tsv (Sapphire) was given
 * room for its French version at build time, and an entry in the registry
 * section { string, French offset, French length } (scripts/lang/
 * lang_build.py). With French chosen, the player's ROM "Pokemon - Version
 * Rubis (France)" (AXVF) or "Version Saphir" (AXPF), revision 1 - the one of
 * the game this executable is - is looked for, checked (CRC-32), and the
 * French strings are copied over the English ones before the game starts.
 *
 * config.ini [game] language = auto | en | fr   (auto: French if the ROM is
 * found). The ROM: [game] rom = its path, if given; else the *.gba files of
 * the executable's folder, then of [game] rom_dir (a common ROM folder),
 * then of the working folder - chosen by header and checksum, not by name.
 * PRS_LANG / PRS_ROM / PRS_ROM_DIR (or --lang / --rom / --rom-dir) override
 * the config.
 */
#include "pc_hal.h"
#include "platform_pc.h"

#include <SDL.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <dirent.h>
#include <sys/mman.h>
#include <unistd.h>
#endif

/* The French ROM of this game (dumps are 16 MiB, the game 8 MiB of it). */
#define FR_ROM_SIZE  0x1000000u
#if defined(SAPPHIRE)
#define FR_ROM_CODE  "AXPF"
#define FR_ROM_CRC32 0x2AE49146u    /* Pokemon - Version Saphir (France), rev 1 */
#define FR_ROM_NAME  "French Sapphire"
#else
#define FR_ROM_CODE  "AXVF"
#define FR_ROM_CRC32 0x9F981F72u    /* Pokemon - Version Rubis (France), rev 1 */
#define FR_ROM_NAME  "French Ruby"
#endif

struct PemLangText {
    const unsigned char *text;
    unsigned int frOffset, frLength;
};

/* Registry bounds: one section, entries from every game file in between. */
#if defined(_WIN32)
__attribute__((section(".rdata$prslang0"), used, aligned(8))) static const struct PemLangText s_lang_begin[1];
__attribute__((section(".rdata$prslang2"), used, aligned(8))) static const struct PemLangText s_lang_end[1];
#define LANG_BEGIN (s_lang_begin + 1)
#define LANG_END   (s_lang_end)
#elif defined(__APPLE__)
extern const struct PemLangText s_lang_start[] __asm("section$start$__DATA$__prslang");
extern const struct PemLangText s_lang_stop[] __asm("section$end$__DATA$__prslang");
#define LANG_BEGIN s_lang_start
#define LANG_END   s_lang_stop
#else
extern const struct PemLangText __start_prs_lang[] __attribute__((weak));
extern const struct PemLangText __stop_prs_lang[] __attribute__((weak));
#define LANG_BEGIN __start_prs_lang
#define LANG_END   __stop_prs_lang
#endif

static char s_lang[8] = "en";

const char *pc_lang_current(void) {
    return s_lang;
}

static uint32_t crc32_of(const uint8_t *p, size_t n) {
    static uint32_t table[256];
    if (table[1] == 0) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) {
                c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            }
            table[i] = c;
        }
    }
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) {
        c = table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    }
    return c ^ 0xFFFFFFFFu;
}

/* Reads path if it is the French ROM of this game (header code FR_ROM_CODE). */
static uint8_t *load_if_french(const char *path) {
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return NULL;
    }
    uint8_t head[0xB0];
    uint8_t *rom = NULL;
    if (fread(head, 1, sizeof head, f) == sizeof head && memcmp(head + 0xAC, FR_ROM_CODE, 4) == 0) {
        rom = malloc(FR_ROM_SIZE);
        fseek(f, 0, SEEK_END);
        long size = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (rom == NULL || size != (long)FR_ROM_SIZE || fread(rom, 1, FR_ROM_SIZE, f) != FR_ROM_SIZE) {
            free(rom);
            rom = NULL;
        }
    }
    fclose(f);
    return rom;
}

/* In a folder of many ROMs (rom_dir): the right one is chosen by its header
 * (game code) and its checksum, never by its file name; another revision of
 * the same game is skipped, the search goes on. */
static uint8_t *load_if_expected(const char *path) {
    uint8_t *rom = load_if_french(path);
    if (rom != NULL && crc32_of(rom, FR_ROM_SIZE) != FR_ROM_CRC32) {
        free(rom);
        rom = NULL;
    }
    return rom;
}

/* First French ROM of this game among the *.gba files of dir. */
static uint8_t *find_in_dir(const char *dir, char *found, size_t n) {
#if defined(_WIN32)
    char pattern[1100];
    snprintf(pattern, sizeof pattern, "%s\\*.gba", dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) {
        return NULL;
    }
    uint8_t *rom = NULL;
    do {
        snprintf(found, n, "%s\\%s", dir, fd.cFileName);
        rom = load_if_expected(found);
    } while (rom == NULL && FindNextFileA(h, &fd));
    FindClose(h);
    return rom;
#else
    DIR *d = opendir(dir);
    if (d == NULL) {
        return NULL;
    }
    uint8_t *rom = NULL;
    struct dirent *e;
    while (rom == NULL && (e = readdir(d)) != NULL) {
        size_t l = strlen(e->d_name);
        if (l > 4 && (strcmp(e->d_name + l - 4, ".gba") == 0 || strcmp(e->d_name + l - 4, ".GBA") == 0)) {
            snprintf(found, n, "%s/%s", dir, e->d_name);
            rom = load_if_expected(found);
        }
    }
    closedir(d);
    return rom;
#endif
}

static uint8_t *find_french_rom(char *path, size_t n) {
    const char *forced = getenv("PRS_ROM");
    if (forced == NULL || *forced == '\0') {
        forced = pc_config_get("game", "rom", NULL);
    }
    if (forced != NULL && *forced != '\0') {
        snprintf(path, n, "%s", forced);
        return load_if_french(path);
    }
    /* Search order: explicit path (above), the executable's folder, the
     * common ROM folder (rom_dir), the working folder. */
    uint8_t *rom = NULL;
    char *base = SDL_GetBasePath();
    if (base != NULL) {
        size_t l = strlen(base);
        while (l > 1 && (base[l - 1] == '/' || base[l - 1] == '\\')) {
            base[--l] = '\0';
        }
        rom = find_in_dir(base, path, n);
        SDL_free(base);
    }
    if (rom == NULL) {
        const char *dir = getenv("PRS_ROM_DIR");
        if (dir == NULL || *dir == '\0') {
            dir = pc_config_get("game", "rom_dir", NULL);
        }
        if (dir != NULL && *dir != '\0') {
            char d[1024];
            snprintf(d, sizeof d, "%s", dir);
            size_t l = strlen(d);
            while (l > 1 && (d[l - 1] == '/' || d[l - 1] == '\\')) {
                d[--l] = '\0';
            }
            rom = find_in_dir(d, path, n);
        }
    }
    if (rom == NULL) {
        rom = find_in_dir(".", path, n);
    }
    return rom;
}

/* Makes the page(s) of [p, p+n) writable (strings live in read-only data). */
void pc_lang_make_writable(const void *p, size_t n) {
    static uintptr_t last_page = 0;
#if defined(_WIN32)
    const uintptr_t page = 4096;
#else
    const uintptr_t page = (uintptr_t)sysconf(_SC_PAGESIZE);
#endif
    uintptr_t a = (uintptr_t)p & ~(page - 1);
    uintptr_t end = ((uintptr_t)p + n + page - 1) & ~(page - 1);
    for (; a < end; a += page) {
        if (a == last_page) {
            continue;
        }
#if defined(_WIN32)
        DWORD old;
        VirtualProtect((void *)a, page, PAGE_READWRITE, &old);
#else
        mprotect((void *)a, page, PROT_READ | PROT_WRITE);
#endif
        last_page = a;
    }
}

void pc_lang_tables_apply(const uint8_t *rom);   /* lang_tables_pc.c */

void pc_lang_apply(void) {
    const char *want = getenv("PRS_LANG");
    if (want == NULL || *want == '\0') {
        want = pc_config_get("game", "language", "auto");
    }
    if (SDL_strcasecmp(want, "en") == 0) {
        pc_log("lang: English");
        return;
    }
    char path[1100];
    uint8_t *rom = find_french_rom(path, sizeof path);
    if (rom == NULL) {
        if (SDL_strcasecmp(want, "fr") == 0) {
            pc_log("lang: French asked, but no " FR_ROM_NAME " ROM (" FR_ROM_CODE ") found next to the game"
                   " or in rom_dir: English. Put your own ROM (*.gba) next to the executable, or set"
                   " [game] rom_dir (or rom) in config.ini.");
        } else {
            pc_log("lang: English (no French ROM found)");
        }
        return;
    }
    uint32_t crc = crc32_of(rom, FR_ROM_SIZE);
    if (crc != FR_ROM_CRC32) {
        pc_log("lang: %s is a " FR_ROM_NAME " ROM but not the expected one (CRC %08X, want %08X): English",
               path, (unsigned)crc, (unsigned)FR_ROM_CRC32);
        free(rom);
        return;
    }
    int applied = 0, skipped = 0;
    for (const struct PemLangText *e = LANG_BEGIN; e < LANG_END; e++) {
        if (e->text == NULL) {
            continue;                      /* section padding */
        }
        if (e->frOffset + e->frLength > FR_ROM_SIZE || e->frLength == 0
            || rom[e->frOffset + e->frLength - 1] != 0xFF) {
            skipped++;
            continue;
        }
        pc_lang_make_writable(e->text, e->frLength);
        memcpy((void *)e->text, rom + e->frOffset, e->frLength);
        applied++;
    }
    pc_lang_tables_apply(rom);
    free(rom);
    snprintf(s_lang, sizeof s_lang, "fr");
    pc_log("lang: French from %s (%d strings%s)", path, applied, skipped ? ", some skipped" : "");
}

int pc_lang_is_french(void) {
    return s_lang[0] == 'f';
}
