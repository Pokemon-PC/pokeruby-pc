/*
 * video_pc.c - SDL window, keyboard and gamepad.
 *
 * Default keys (same as mGBA): arrows, X = A, Z = B, A = L, S = R,
 * Enter = Start, Backspace = Select. F11 or Alt+Enter = fullscreen.
 * Any SDL game controller works too (Nintendo layout).
 * PRS_HEADLESS=1: no window (tests). PRS_SCALE=n: window size.
 */
#include "pc_hal.h"

#include "hw_pc.h"
#include "platform_pc.h"

#include <SDL.h>
#include <stdlib.h>
#include <string.h>

enum {
    KEY_A = 0x001, KEY_B = 0x002, KEY_SELECT = 0x004, KEY_START = 0x008,
    KEY_RIGHT = 0x010, KEY_LEFT = 0x020, KEY_UP = 0x040, KEY_DOWN = 0x080,
    KEY_R = 0x100, KEY_L = 0x200,
};

/* Keyboard bindings, from the [keys] section of config.ini. */
static struct { SDL_Scancode code; uint16_t gba; } s_key_map[64];
static int s_key_count;

static void load_key_map(void) {
    static const struct { const char *name; uint16_t gba; const char *def; } kButtons[] = {
        { "A", KEY_A, "X" }, { "B", KEY_B, "Z" }, { "Start", KEY_START, "Return" },
        { "Select", KEY_SELECT, "Backspace" }, { "Up", KEY_UP, "Up" }, { "Down", KEY_DOWN, "Down" },
        { "Left", KEY_LEFT, "Left" }, { "Right", KEY_RIGHT, "Right" }, { "L", KEY_L, "A" },
        { "R", KEY_R, "S" },
    };
    s_key_count = 0;
    for (unsigned b = 0; b < sizeof kButtons / sizeof kButtons[0]; b++) {
        char list[128];
        snprintf(list, sizeof list, "%s", pc_config_get("keys", kButtons[b].name, kButtons[b].def));
        for (char *tok = strtok(list, ","); tok; tok = strtok(NULL, ",")) {
            while (*tok == ' ') tok++;
            char *end = tok + strlen(tok);
            while (end > tok && end[-1] == ' ') *--end = '\0';
            SDL_Scancode code = SDL_GetScancodeFromName(tok);
            if (code == SDL_SCANCODE_UNKNOWN) {
                pc_warn("config: unknown key name '%s' for %s", tok, kButtons[b].name);
                continue;
            }
            if (s_key_count < (int)(sizeof s_key_map / sizeof s_key_map[0])) {
                s_key_map[s_key_count].code = code;
                s_key_map[s_key_count].gba = kButtons[b].gba;
                s_key_count++;
            }
        }
    }
}

static SDL_Window *s_win;
static SDL_Renderer *s_ren;
static SDL_Texture *s_tex;
static SDL_GameController *s_pad;
static bool s_headless, s_quit;
static uint16_t (*s_override)(uint64_t);
static void (*s_frame_hook)(uint64_t, const uint32_t *);

void pc_input_set_override(uint16_t (*fn)(uint64_t)) { s_override = fn; }
void pc_video_set_frame_hook(void (*fn)(uint64_t, const uint32_t *)) { s_frame_hook = fn; }
bool pc_quit_requested(void) { return s_quit; }

bool pc_video_init(void) {
    const char *h = getenv("PRS_HEADLESS");
    s_headless = h && *h && *h != '0';
    if (s_headless) {
        return true;
    }
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) != 0) {
        pc_warn("SDL_Init: %s", SDL_GetError());
        return false;
    }
    int scale = pc_config_int("video", "scale", 3);
    const char *s = getenv("PRS_SCALE");
    if (s && atoi(s) > 0) {
        scale = atoi(s);
    }
    if (scale < 1) {
        scale = 1;
    }
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "nearest");
    s_win = SDL_CreateWindow(PRS_GAME_TITLE, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                             PC_GBA_WIDTH * scale, PC_GBA_HEIGHT * scale, SDL_WINDOW_RESIZABLE);
    if (!s_win) {
        pc_warn("SDL_CreateWindow: %s", SDL_GetError());
        return false;
    }
    s_ren = SDL_CreateRenderer(s_win, -1, SDL_RENDERER_ACCELERATED);
    if (!s_ren) {
        s_ren = SDL_CreateRenderer(s_win, -1, SDL_RENDERER_SOFTWARE);
    }
    SDL_RenderSetLogicalSize(s_ren, PC_GBA_WIDTH, PC_GBA_HEIGHT);
    SDL_RenderSetIntegerScale(s_ren, SDL_FALSE);
    s_tex = SDL_CreateTexture(s_ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING,
                              PC_GBA_WIDTH, PC_GBA_HEIGHT);
    if (pc_config_int("video", "fullscreen", 0)) {
        SDL_SetWindowFullscreen(s_win, SDL_WINDOW_FULLSCREEN_DESKTOP);
    }
    load_key_map();
    return s_tex != NULL;
}

void pc_video_present(const uint32_t *argb) {
    if (s_frame_hook) {
        s_frame_hook(pc_frame_count(), argb);
    }
    if (s_headless || !s_tex) {
        return;
    }
    SDL_UpdateTexture(s_tex, NULL, argb, PC_GBA_WIDTH * 4);
    SDL_RenderClear(s_ren);
    SDL_RenderCopy(s_ren, s_tex, NULL, NULL);
    SDL_RenderPresent(s_ren);
}

static uint16_t keyboard_keys(void) {
    const uint8_t *k = SDL_GetKeyboardState(NULL);
    uint16_t keys = 0;
    for (int i = 0; i < s_key_count; i++) {
        if (!k[s_key_map[i].code]) {
            continue;
        }
        /* Alt+Enter is the full-screen toggle, not Start. */
        if (s_key_map[i].code == SDL_SCANCODE_RETURN && (SDL_GetModState() & KMOD_ALT)) {
            continue;
        }
        keys |= s_key_map[i].gba;
    }
    return keys;
}

static uint16_t pad_keys(void) {
    if (!s_pad) {
        return 0;
    }
    uint16_t keys = 0;
    /* Nintendo layout: east = A, south = B. */
    if (SDL_GameControllerGetButton(s_pad, SDL_CONTROLLER_BUTTON_B)) keys |= KEY_A;
    if (SDL_GameControllerGetButton(s_pad, SDL_CONTROLLER_BUTTON_A)) keys |= KEY_B;
    if (SDL_GameControllerGetButton(s_pad, SDL_CONTROLLER_BUTTON_BACK)) keys |= KEY_SELECT;
    if (SDL_GameControllerGetButton(s_pad, SDL_CONTROLLER_BUTTON_START)) keys |= KEY_START;
    if (SDL_GameControllerGetButton(s_pad, SDL_CONTROLLER_BUTTON_DPAD_RIGHT)) keys |= KEY_RIGHT;
    if (SDL_GameControllerGetButton(s_pad, SDL_CONTROLLER_BUTTON_DPAD_LEFT)) keys |= KEY_LEFT;
    if (SDL_GameControllerGetButton(s_pad, SDL_CONTROLLER_BUTTON_DPAD_UP)) keys |= KEY_UP;
    if (SDL_GameControllerGetButton(s_pad, SDL_CONTROLLER_BUTTON_DPAD_DOWN)) keys |= KEY_DOWN;
    if (SDL_GameControllerGetButton(s_pad, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER)) keys |= KEY_R;
    if (SDL_GameControllerGetButton(s_pad, SDL_CONTROLLER_BUTTON_LEFTSHOULDER)) keys |= KEY_L;
    int lx = SDL_GameControllerGetAxis(s_pad, SDL_CONTROLLER_AXIS_LEFTX);
    int ly = SDL_GameControllerGetAxis(s_pad, SDL_CONTROLLER_AXIS_LEFTY);
    if (lx > 16000) keys |= KEY_RIGHT;
    if (lx < -16000) keys |= KEY_LEFT;
    if (ly > 16000) keys |= KEY_DOWN;
    if (ly < -16000) keys |= KEY_UP;
    return keys;
}

uint16_t pc_input_poll(void) {
    if (!s_headless) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            switch (e.type) {
            case SDL_QUIT:
                s_quit = true;
                break;
            case SDL_KEYDOWN:
                if (e.key.keysym.sym == SDLK_F11 ||
                    (e.key.keysym.sym == SDLK_RETURN && (e.key.keysym.mod & KMOD_ALT))) {
                    bool fs = SDL_GetWindowFlags(s_win) & SDL_WINDOW_FULLSCREEN_DESKTOP;
                    SDL_SetWindowFullscreen(s_win, fs ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
                }
                break;
            case SDL_CONTROLLERDEVICEADDED:
                if (!s_pad) {
                    s_pad = SDL_GameControllerOpen(e.cdevice.which);
                }
                break;
            case SDL_CONTROLLERDEVICEREMOVED:
                if (s_pad && e.cdevice.which == SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(s_pad))) {
                    SDL_GameControllerClose(s_pad);
                    s_pad = NULL;
                }
                break;
            default:
                break;
            }
        }
    }
    if (s_override) {
        return s_override(pc_frame_count());
    }
    if (s_headless) {
        return 0;
    }
    uint16_t keys = keyboard_keys() | pad_keys();
    /* The GBA d-pad cannot report opposite directions at once. */
    if ((keys & (KEY_LEFT | KEY_RIGHT)) == (KEY_LEFT | KEY_RIGHT)) keys &= (uint16_t)~(KEY_LEFT | KEY_RIGHT);
    if ((keys & (KEY_UP | KEY_DOWN)) == (KEY_UP | KEY_DOWN)) keys &= (uint16_t)~(KEY_UP | KEY_DOWN);
    return keys;
}

void pc_video_shutdown(void) {
    if (s_pad) SDL_GameControllerClose(s_pad);
    if (s_tex) SDL_DestroyTexture(s_tex);
    if (s_ren) SDL_DestroyRenderer(s_ren);
    if (s_win) SDL_DestroyWindow(s_win);
    s_pad = NULL;
    s_tex = NULL;
    s_ren = NULL;
    s_win = NULL;
    if (!s_headless) {
        SDL_QuitSubSystem(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER);
    }
}
