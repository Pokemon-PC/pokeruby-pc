/*
 * config_pc.c - config.ini next to the executable.
 *
 * Created with the defaults (and comments) on first launch, so players can
 * edit it. Environment variables (PRS_SCALE, PRS_MUTE...) still win, for
 * tests. Format: [section] then key=value; ';' or '#' start a comment.
 */
#include "pc_hal.h"
#include "platform_pc.h"

#include <SDL.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_ENTRIES 64

typedef struct {
    char section[32];
    char key[32];
    char value[128];
} Entry;

static Entry s_entries[MAX_ENTRIES];
static int s_count;
static bool s_loaded;
static char s_path[1024];

static const char kDefault[] =
    "; Pokemon Ruby / Sapphire (PC) - configuration\n"
    "; Key names are SDL key names: X, Z, Return, Backspace, Left Shift, Space, Up...\n"
    "; Several keys for one button: separate them with commas (A = X, Space).\n"
    "\n"
    "[game]\n"
    "language = auto    ; en, fr, or auto (French if your own French ROM is found)\n"
    "rom =              ; your French Ruby / Sapphire ROM (default: a .gba next to the game)\n"
    "\n"
    "[video]\n"
    "scale = 3          ; window size: 240x160 times this\n"
    "fullscreen = 0     ; 1 = start in full screen (F11 or Alt+Enter toggles)\n"
    "\n"
    "[audio]\n"
    "mute = 0\n"
    "volume = 100       ; 0..100\n"
    "\n"
    "[keys]\n"
    "A = X\n"
    "B = Z\n"
    "Start = Return\n"
    "Select = Backspace\n"
    "Up = Up\n"
    "Down = Down\n"
    "Left = Left\n"
    "Right = Right\n"
    "L = A\n"
    "R = S\n";

static void trim(char *s) {
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) {
        *--e = '\0';
    }
    char *b = s;
    while (isspace((unsigned char)*b)) {
        b++;
    }
    memmove(s, b, strlen(b) + 1);
}

static void parse(const char *text) {
    char section[32] = "";
    const char *p = text;
    while (*p && s_count < MAX_ENTRIES) {
        char line[256];
        size_t n = strcspn(p, "\r\n");
        if (n >= sizeof line) {
            n = sizeof line - 1;
        }
        memcpy(line, p, n);
        line[n] = '\0';
        p += n;
        while (*p == '\r' || *p == '\n') {
            p++;
        }
        char *c = strpbrk(line, ";#");
        if (c) {
            *c = '\0';
        }
        trim(line);
        if (line[0] == '[') {
            char *end = strchr(line, ']');
            if (end) {
                *end = '\0';
                snprintf(section, sizeof section, "%s", line + 1);
                trim(section);
            }
            continue;
        }
        char *eq = strchr(line, '=');
        if (!eq) {
            continue;
        }
        *eq = '\0';
        Entry *e = &s_entries[s_count++];
        snprintf(e->section, sizeof e->section, "%s", section);
        snprintf(e->key, sizeof e->key, "%s", line);
        snprintf(e->value, sizeof e->value, "%s", eq + 1);
        trim(e->key);
        trim(e->value);
    }
}

static void load(void) {
    if (s_loaded) {
        return;
    }
    s_loaded = true;
    char *base = SDL_GetBasePath();
    snprintf(s_path, sizeof s_path, "%sconfig.ini", base ? base : "");
    SDL_free(base);
    FILE *f = fopen(s_path, "rb");
    if (!f) {
        f = fopen(s_path, "wb");
        if (f) {
            fputs(kDefault, f);
            fclose(f);
            pc_log("config: created %s", s_path);
        }
        parse(kDefault);
        return;
    }
    static char text[16384];
    size_t len = fread(text, 1, sizeof text - 1, f);
    fclose(f);
    text[len] = '\0';
    parse(text);
    pc_log("config: %s", s_path);
}

const char *pc_config_get(const char *section, const char *key, const char *def) {
    load();
    for (int i = 0; i < s_count; i++) {
        if (SDL_strcasecmp(s_entries[i].section, section) == 0 && SDL_strcasecmp(s_entries[i].key, key) == 0) {
            return s_entries[i].value;
        }
    }
    return def;
}

int pc_config_int(const char *section, const char *key, int def) {
    const char *v = pc_config_get(section, key, NULL);
    return v && *v ? atoi(v) : def;
}
