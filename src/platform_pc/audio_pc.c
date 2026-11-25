/*
 * audio_pc.c - the GBA sound hardware on the host.
 *
 *  - DirectSound: SoundMain (sound_pc.c) mixes one slice of signed 8-bit
 *    samples per VBlank for FIFO A and FIFO B; here it is resampled from the
 *    engine rate (pcmSamplesPerVBlank * 59.7275 Hz) to the host rate.
 *  - PSG (the four Game Boy channels): m4a.c's CgbSound programs them
 *    through NR10-NR52 and wave RAM, exactly as on hardware. They are
 *    synthesised here from those registers: square 1 (with sweep), square 2,
 *    wave, noise, with the 512 Hz frame sequencer (length 256 Hz, sweep
 *    128 Hz, envelope 64 Hz). A write with bit 7 of NRx4 set is a trigger;
 *    the bit is write-only, so it is cleared once seen.
 *  - Mixing follows SOUNDCNT_L/H (PSG volume and panning, DMA A/B volume and
 *    routing), then goes to SDL (queued audio, latency kept under ~8 frames).
 *
 * Environment: PRS_MUTE=1 (no audio device), PRS_WAV=out.wav (record what is
 * played, for checking), PRS_FAST disables queueing (no pacing to follow).
 */
#include "pc_hal.h"

#include "hw_pc.h"
#include "platform_pc.h"

#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define OUT_RATE  48000
#define FRAME_HZ  (16777216.0 / 280896.0)

#define IO8(off)  (*(volatile uint8_t *)(uintptr_t)(PC_IO_BASE + (off)))
#define IO16(off) (*(volatile uint16_t *)(uintptr_t)(PC_IO_BASE + (off)))

/* NRxx byte offsets in the GBA I/O map */
enum {
    NR10 = 0x60, NR11 = 0x62, NR12 = 0x63, NR13 = 0x64, NR14 = 0x65,
    NR21 = 0x68, NR22 = 0x69, NR23 = 0x6C, NR24 = 0x6D,
    NR30 = 0x70, NR31 = 0x72, NR32 = 0x73, NR33 = 0x74, NR34 = 0x75,
    NR41 = 0x78, NR42 = 0x79, NR43 = 0x7C, NR44 = 0x7D,
    NR50 = 0x80, NR51 = 0x81, SOUNDCNT_H = 0x82, NR52 = 0x84, WAVE_RAM = 0x90,
};

typedef struct {
    bool     on;
    int      volume;        /* 0..15 */
    int      env_dir;       /* +1 / -1 */
    int      env_period;    /* 0 = off */
    int      env_timer;
    int      length;        /* remaining length ticks */
    double   phase;         /* 0..1 over one waveform period */
    /* square 1 sweep */
    int      sweep_period, sweep_timer, sweep_shift, sweep_neg;
    int      shadow_freq;
    /* noise */
    uint16_t lfsr;
    double   noise_acc;
} Psg;

static Psg s_psg[4];
static double s_seq_acc;    /* frame sequencer, in 512 Hz steps */
static int s_seq_step;

static SDL_AudioDeviceID s_dev;
static bool s_fast;
static double s_out_acc;
static int16_t s_out[4096 * 2];
static int s_prev_r, s_prev_l;   /* last DirectSound sample of the previous frame */

static int s_volume = 100;   /* config.ini [audio] volume, 0..100 */
static FILE *s_wav;
static uint32_t s_wav_bytes;

/* ---- WAV recording ----------------------------------------------------------- */

static void wav_header(FILE *f, uint32_t data_bytes) {
    uint32_t v32;
    uint16_t v16;
    fwrite("RIFF", 1, 4, f);
    v32 = 36 + data_bytes; fwrite(&v32, 4, 1, f);
    fwrite("WAVEfmt ", 1, 8, f);
    v32 = 16; fwrite(&v32, 4, 1, f);
    v16 = 1; fwrite(&v16, 2, 1, f);              /* PCM */
    v16 = 2; fwrite(&v16, 2, 1, f);              /* stereo */
    v32 = OUT_RATE; fwrite(&v32, 4, 1, f);
    v32 = OUT_RATE * 4; fwrite(&v32, 4, 1, f);
    v16 = 4; fwrite(&v16, 2, 1, f);
    v16 = 16; fwrite(&v16, 2, 1, f);
    fwrite("data", 1, 4, f);
    fwrite(&data_bytes, 4, 1, f);
}

static void wav_close(void) {
    if (!s_wav) {
        return;
    }
    fseek(s_wav, 0, SEEK_SET);
    wav_header(s_wav, s_wav_bytes);
    fclose(s_wav);
    s_wav = NULL;
}

/* ---- init ----------------------------------------------------------------------- */

void pc_audio_init(void) {
    const char *e = getenv("PRS_FAST");
    s_fast = e && *e && *e != '0';
    e = getenv("PRS_WAV");
    if (e && *e) {
        s_wav = fopen(e, "wb");
        if (s_wav) {
            wav_header(s_wav, 0);
            atexit(wav_close);
            pc_log("audio: recording to %s", e);
        }
    }
    for (int i = 0; i < 4; i++) {
        s_psg[i].lfsr = 0x7FFF;
    }
    s_volume = pc_config_int("audio", "volume", 100);
    if (s_volume < 0) s_volume = 0;
    if (s_volume > 100) s_volume = 100;
    const char *mute = getenv("PRS_MUTE");
    const char *headless = getenv("PRS_HEADLESS");
    if ((mute && *mute && *mute != '0') || (headless && *headless && *headless != '0')
        || pc_config_int("audio", "mute", 0)) {
        return;
    }
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
        pc_warn("audio: SDL_INIT_AUDIO failed: %s (no sound)", SDL_GetError());
        return;
    }
    SDL_AudioSpec want, have;
    SDL_zero(want);
    want.freq = OUT_RATE;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = 1024;
    s_dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (s_dev == 0) {
        pc_warn("audio: cannot open a device: %s (no sound)", SDL_GetError());
        return;
    }
    SDL_PauseAudioDevice(s_dev, 0);
    pc_log("audio: %d Hz stereo", have.freq);
}

/* ---- PSG ---------------------------------------------------------------------- */

static int square_freq_reg(int ch) {
    int lo = ch == 0 ? NR13 : NR23, hi = ch == 0 ? NR14 : NR24;
    return IO8(lo) | ((IO8(hi) & 7) << 8);
}

static void trigger(int ch) {
    Psg *p = &s_psg[ch];
    p->on = true;
    p->phase = 0;
    int nrx2 = ch == 0 ? NR12 : ch == 1 ? NR22 : ch == 3 ? NR42 : 0;
    if (ch != 2) {
        uint8_t env = IO8(nrx2);
        p->volume = env >> 4;
        p->env_dir = (env & 8) ? 1 : -1;
        p->env_period = env & 7;
        p->env_timer = p->env_period;
        if ((env & 0xF8) == 0) {
            p->on = false;          /* DAC off */
        }
    }
    if (p->length == 0) {
        p->length = ch == 2 ? 256 : 64;
    }
    if (ch == 0) {
        uint8_t sw = IO8(NR10);
        p->shadow_freq = square_freq_reg(0);
        p->sweep_period = (sw >> 4) & 7;
        p->sweep_neg = (sw & 8) != 0;
        p->sweep_shift = sw & 7;
        p->sweep_timer = p->sweep_period ? p->sweep_period : 8;
    }
    if (ch == 3) {
        p->lfsr = 0x7FFF;
        p->noise_acc = 0;
    }
}

/* Registers -> channel state, once per frame (after CgbSound has run). */
static void psg_read_registers(void) {
    static const int nrx1[4] = { NR11, NR21, NR31, NR41 };
    static const int nrx4[4] = { NR14, NR24, NR34, NR44 };
    for (int ch = 0; ch < 4; ch++) {
        uint8_t v4 = IO8(nrx4[ch]);
        if (v4 & 0x80) {
            /* length load at trigger time */
            uint8_t v1 = IO8(nrx1[ch]);
            s_psg[ch].length = ch == 2 ? 256 - v1 : 64 - (v1 & 63);
            trigger(ch);
            IO8(nrx4[ch]) = v4 & 0x7F;   /* write-only bit */
        }
    }
    if (!(IO8(NR30) & 0x80)) {
        s_psg[2].on = false;              /* wave DAC off */
    }
}

static void sequencer_step(void) {
    static const int nrx4[4] = { NR14, NR24, NR34, NR44 };
    int step = s_seq_step;
    s_seq_step = (s_seq_step + 1) & 7;
    if ((step & 1) == 0) {                /* length, 256 Hz */
        for (int ch = 0; ch < 4; ch++) {
            Psg *p = &s_psg[ch];
            if ((IO8(nrx4[ch]) & 0x40) && p->length > 0 && --p->length == 0) {
                p->on = false;
            }
        }
    }
    if (step == 2 || step == 6) {         /* sweep, 128 Hz */
        Psg *p = &s_psg[0];
        if (p->on && --p->sweep_timer <= 0) {
            p->sweep_timer = p->sweep_period ? p->sweep_period : 8;
            if (p->sweep_period && p->sweep_shift) {
                int d = p->shadow_freq >> p->sweep_shift;
                int f = p->sweep_neg ? p->shadow_freq - d : p->shadow_freq + d;
                if (f > 2047) {
                    p->on = false;
                } else if (f >= 0) {
                    p->shadow_freq = f;
                    IO8(NR13) = (uint8_t)f;
                    IO8(NR14) = (uint8_t)((IO8(NR14) & 0xF8) | (f >> 8));
                }
            }
        }
    }
    if (step == 7) {                      /* envelope, 64 Hz */
        for (int ch = 0; ch < 4; ch++) {
            Psg *p = &s_psg[ch];
            if (ch == 2 || !p->on || p->env_period == 0) {
                continue;
            }
            if (--p->env_timer <= 0) {
                p->env_timer = p->env_period;
                int v = p->volume + p->env_dir;
                if (v >= 0 && v <= 15) {
                    p->volume = v;
                }
            }
        }
    }
}

static const uint8_t kDuty[4] = { 0x01, 0x81, 0x87, 0x7E };

/* One output sample of each channel, in -15..15. */
static void psg_sample(int out[4]) {
    for (int ch = 0; ch < 2; ch++) {
        Psg *p = &s_psg[ch];
        out[ch] = 0;
        if (!p->on) {
            continue;
        }
        int x = square_freq_reg(ch);
        double hz = 131072.0 / (2048 - x);
        p->phase += hz / OUT_RATE;
        p->phase -= (int)p->phase;
        int duty = (IO8(ch == 0 ? NR11 : NR21) >> 6) & 3;
        int bit = (kDuty[duty] >> (7 - (int)(p->phase * 8))) & 1;
        out[ch] = bit ? p->volume : -p->volume;
    }
    {
        Psg *p = &s_psg[2];
        out[2] = 0;
        if (p->on) {
            int x = IO8(NR33) | ((IO8(NR34) & 7) << 8);
            double rate = 2097152.0 / (2048 - x);          /* samples per second */
            p->phase += rate / OUT_RATE / 32.0;
            p->phase -= (int)p->phase;
            int idx = (int)(p->phase * 32);
            uint8_t b = IO8(WAVE_RAM + idx / 2);
            int s = ((idx & 1) ? (b & 0xF) : (b >> 4)) - 8;  /* centred: -8..7 */
            uint8_t vol = IO8(NR32);
            int v;
            if (vol & 0x80) {
                v = (s * 3) / 4;                              /* forced 75% */
            } else {
                static const int shift[4] = { -1, 0, 1, 2 };  /* mute, 100, 50, 25% */
                int sh = shift[(vol >> 5) & 3];
                v = sh < 0 ? 0 : s >> sh;
            }
            out[2] = v * 2;
        }
    }
    {
        Psg *p = &s_psg[3];
        out[3] = 0;
        if (p->on) {
            uint8_t nr43 = IO8(NR43);
            int s = nr43 >> 4, width7 = (nr43 >> 3) & 1, r = nr43 & 7;
            double hz = 524288.0 / (r ? r : 0.5) / (double)(2 << s);
            p->noise_acc += hz / OUT_RATE;
            while (p->noise_acc >= 1.0) {
                p->noise_acc -= 1.0;
                int x = (p->lfsr ^ (p->lfsr >> 1)) & 1;
                p->lfsr = (uint16_t)((p->lfsr >> 1) | (x << 14));
                if (width7) {
                    p->lfsr = (uint16_t)((p->lfsr & ~0x40) | (x << 6));
                }
            }
            out[3] = (p->lfsr & 1) ? -p->volume : p->volume;
        }
    }
}

/* ---- per frame ---------------------------------------------------------------- */

void pc_audio_frame(const int8_t *right, const int8_t *left, int n) {
    if (n <= 0) {
        return;
    }
    psg_read_registers();

    s_out_acc += OUT_RATE / FRAME_HZ;
    int m = (int)s_out_acc;
    s_out_acc -= m;
    if (m > 4096) {
        m = 4096;
    }

    uint16_t cnth = IO16(SOUNDCNT_H);
    uint8_t nr50 = IO8(NR50), nr51 = IO8(NR51);
    bool master = (IO8(NR52) & 0x80) != 0;
    static const int kPsgRatio[4] = { 1, 2, 4, 4 };    /* 25%, 50%, 100% (in quarters) */
    int psg_ratio = kPsgRatio[cnth & 3];
    int a_mul = (cnth & 4) ? 4 : 2, b_mul = (cnth & 8) ? 4 : 2;
    int vol_r = (nr50 & 7) + 1, vol_l = ((nr50 >> 4) & 7) + 1;

    for (int j = 0; j < m; j++) {
        /* 512 Hz frame sequencer */
        s_seq_acc += 512.0 / OUT_RATE;
        while (s_seq_acc >= 1.0) {
            s_seq_acc -= 1.0;
            sequencer_step();
        }

        /* DirectSound: linear interpolation over this frame's slice. */
        double pos = (double)j * n / m;
        int i = (int)pos;
        double fr = pos - i;
        int r0 = i == 0 ? s_prev_r : right[i - 1], r1 = right[i < n ? i : n - 1];
        int l0 = i == 0 ? s_prev_l : left[i - 1], l1 = left[i < n ? i : n - 1];
        double dr = r0 + (r1 - r0) * fr, dl = l0 + (l1 - l0) * fr;

        /* FIFO A = right buffer, FIFO B = left buffer; routed by SOUNDCNT_H. */
        double out_r = 0, out_l = 0;
        if (cnth & 0x0100) out_r += dr * a_mul;
        if (cnth & 0x0200) out_l += dr * a_mul;
        if (cnth & 0x1000) out_r += dl * b_mul;
        if (cnth & 0x2000) out_l += dl * b_mul;

        if (master) {
            int ps[4];
            psg_sample(ps);
            int pr = 0, pl = 0;
            for (int ch = 0; ch < 4; ch++) {
                if (nr51 & (1 << ch)) pr += ps[ch];
                if (nr51 & (0x10 << ch)) pl += ps[ch];
            }
            out_r += (double)pr * vol_r * psg_ratio / 4.0;
            out_l += (double)pl * vol_l * psg_ratio / 4.0;
        }

        int sr = (int)(out_r * 32 * s_volume / 100), sl = (int)(out_l * 32 * s_volume / 100);
        if (sr > 32767) sr = 32767;
        if (sr < -32768) sr = -32768;
        if (sl > 32767) sl = 32767;
        if (sl < -32768) sl = -32768;
        s_out[j * 2] = (int16_t)sl;
        s_out[j * 2 + 1] = (int16_t)sr;
    }
    s_prev_r = right[n - 1];
    s_prev_l = left[n - 1];

    uint32_t bytes = (uint32_t)m * 4;
    if (s_wav) {
        s_wav_bytes += (uint32_t)fwrite(s_out, 1, bytes, s_wav);
    }
    if (s_dev && !s_fast) {
        /* Keep latency bounded if the game ran ahead (window dragged...). */
        if (SDL_GetQueuedAudioSize(s_dev) < bytes * 8) {
            SDL_QueueAudio(s_dev, s_out, bytes);
        }
    }
}

void pc_audio_shutdown(void) {
    if (s_dev) {
        SDL_CloseAudioDevice(s_dev);
        s_dev = 0;
    }
    wav_close();
}
