/*
 * sound_pc.c - the ARM half of the MP2K sound engine (src/m4a_1.s) in C.
 *
 * src/m4a.c (players, songs, fades, cries, CGB channel control) is compiled
 * as-is; this file is a transcription of what m4a_1.s does, routine by
 * routine (same names, same order of operations, same 8/16/32-bit
 * arithmetic), so the music plays and times exactly as on the GBA:
 *   - MPlayMain: the sequencer (tempo, track commands, gate times, LFO,
 *     volume/pitch updates), chained over all music players;
 *   - ply_*: the track commands of gMPlayJumpTableTemplate (m4a_tables.c);
 *   - ply_note / ply_endtie: note on/off, channel allocation by priority;
 *   - SoundMain: once per VBlank, runs the sequencer and the CGB (PSG)
 *     control, then mixes the DirectSound channels into this frame's slice
 *     of the PCM buffer (envelope, master volume, linear-interpolated
 *     resampling, DPCM-compressed and reversed samples, loops, reverb), and
 *     hands that slice to the host audio output (audio_pc.c).
 *
 * The channel position is kept as a sample index (count = samples left;
 * forward index = size - count, reverse index = count) instead of the ARM
 * code's mix of pointers and indices; the results are the same.
 */
#include <string.h>

#include "pc_hal.h"
#include "platform_pc.h"

#include "global.h"
#include "gba/m4a_internal.h"
#include "m4a.h"

#define SF_ON      SOUND_CHANNEL_SF_ON
#define SF_START   SOUND_CHANNEL_SF_START
#define SF_STOP    SOUND_CHANNEL_SF_STOP
#define SF_LOOP    SOUND_CHANNEL_SF_LOOP
#define SF_IEC     SOUND_CHANNEL_SF_IEC
#define SF_ENV     SOUND_CHANNEL_SF_ENV

#define TONEDATA_TYPE_REV 0x10   /* m4a_constants.inc */
#define TONEDATA_TYPE_CMP 0x20
#define WAVE_DATA_FLAG_LOOP 0xC0

extern const u8 gClockTable[];
extern const s8 gDeltaEncodingTable[];
extern void *const gMPlayJumpTableTemplate[];
void ClearChain(void *x);
void Clear64byte(void *x);
void TrkVolPitSet(struct MusicPlayerInfo *mplayInfo, struct MusicPlayerTrack *track);
void FadeOutBody(struct MusicPlayerInfo *mplayInfo);
u32 MidiKeyToFreq(struct WaveData *wav, u8 key, u8 fineAdjust);

/* m4a.c copies 0x800 bytes from here into IWRAM (code on the GBA); on PC
 * nothing executes it, it only needs to be readable. */
__attribute__((aligned(4))) char SoundMainRAM[0x800];

static inline struct SoundInfo *sound_info(void) {
    return SOUND_INFO_PTR;
}

/* ---- small helpers of m4a_1.s ------------------------------------------- */

u32 umul3232H32(u32 multiplier, u32 multiplicand) {
    return (u32)(((u64)multiplier * multiplicand) >> 32);
}

/* Template entry 35 is SoundMainBTM: on the GBA, the "clear 64 bytes"
 * routine behind Clear64byte(). */
static void pc_clear64(void *x) {
    memset(x, 0, 64);
}

void SoundMainBTM(void) {}

void MPlayJumpTableCopy(MPlayFunc *mplayJumpTable) {
    for (int i = 0; i < 36; i++) {
        mplayJumpTable[i] = (MPlayFunc)gMPlayJumpTableTemplate[i];
    }
    mplayJumpTable[35] = (MPlayFunc)pc_clear64;
}

/* Reads the next command byte (ld_r3_tp_adr_i). */
static inline u8 read_byte(struct MusicPlayerTrack *track) {
    u8 *p = track->cmdPtr;
    track->cmdPtr = p + 1;
    return *p;
}

void RealClearChain(void *x) {
    struct SoundChannel *chan = x;
    struct MusicPlayerTrack *track = chan->track;
    if (track == NULL) {
        return;
    }
    struct SoundChannel *next = chan->nextChannelPointer;
    struct SoundChannel *prev = chan->prevChannelPointer;
    if (prev != NULL) {
        prev->nextChannelPointer = next;
    } else {
        track->chan = next;
    }
    if (next != NULL) {
        next->prevChannelPointer = prev;
    }
    chan->track = NULL;
}

static void clear_modM(struct MusicPlayerTrack *track) {
    track->modM = 0;
    track->lfoSpeedC = 0;
    track->flags |= (track->modT == 0) ? MPT_FLG_PITCHG : MPT_FLG_VOLCHG;
}

/* ChnVolSetAsm: channel volumes from velocity, rhythm pan and track volume. */
static void chn_vol_set(struct SoundChannel *chan, struct MusicPlayerTrack *track) {
    u32 velocity = chan->velocity;
    s32 pan = (s8)chan->rhythmPan;
    u32 r = ((u32)((0x80 + pan) * (s32)velocity) * track->volMR) >> 14;
    chan->rightVolume = (u8)(r > 0xFF ? 0xFF : r);
    u32 l = ((u32)((0x7F - pan) * (s32)velocity) * track->volML) >> 14;
    chan->leftVolume = (u8)(l > 0xFF ? 0xFF : l);
}

/* ---- track commands ------------------------------------------------------- */

void ply_fine(struct MusicPlayerInfo *mplayInfo, struct MusicPlayerTrack *track) {
    (void)mplayInfo;
    for (struct SoundChannel *chan = track->chan; chan != NULL; chan = chan->nextChannelPointer) {
        if (chan->statusFlags & SF_ON) {
            chan->statusFlags |= SF_STOP;
        }
        RealClearChain(chan);
    }
    track->flags = 0;
}

static void goto_here(struct MusicPlayerTrack *track) {
    u8 *p = track->cmdPtr;
    u32 addr = p[0] | (p[1] << 8) | (p[2] << 16) | ((u32)p[3] << 24);
    track->cmdPtr = (u8 *)(uintptr_t)addr;
}

void ply_goto(struct MusicPlayerInfo *mplayInfo, struct MusicPlayerTrack *track) {
    (void)mplayInfo;
    goto_here(track);
}

void ply_patt(struct MusicPlayerInfo *mplayInfo, struct MusicPlayerTrack *track) {
    if (track->patternLevel >= 3) {
        ply_fine(mplayInfo, track);
        return;
    }
    track->patternStack[track->patternLevel] = track->cmdPtr + 4;
    track->patternLevel++;
    goto_here(track);
}

void ply_pend(struct MusicPlayerInfo *mplayInfo, struct MusicPlayerTrack *track) {
    (void)mplayInfo;
    if (track->patternLevel != 0) {
        track->patternLevel--;
        track->cmdPtr = track->patternStack[track->patternLevel];
    }
}

void ply_rept(struct MusicPlayerInfo *mplayInfo, struct MusicPlayerTrack *track) {
    (void)mplayInfo;
    u8 *p = track->cmdPtr;
    if (*p == 0) {                       /* infinite repeat */
        track->cmdPtr = p + 1;
        goto_here(track);
        return;
    }
    u8 n = ++track->repN;
    u8 count = read_byte(track);
    if (n < count) {
        goto_here(track);
    } else {
        track->repN = 0;
        track->cmdPtr = p + 5;
    }
}

void ply_prio(struct MusicPlayerInfo *mplayInfo, struct MusicPlayerTrack *track) {
    (void)mplayInfo;
    track->priority = read_byte(track);
}

void ply_tempo(struct MusicPlayerInfo *mplayInfo, struct MusicPlayerTrack *track) {
    u32 t = read_byte(track) << 1;
    mplayInfo->tempoD = (u16)t;
    mplayInfo->tempoI = (u16)((t * mplayInfo->tempoU) >> 8);
}

void ply_keysh(struct MusicPlayerInfo *mplayInfo, struct MusicPlayerTrack *track) {
    (void)mplayInfo;
    track->keyShift = (s8)read_byte(track);
    track->flags |= 0xC;
}

void ply_voice(struct MusicPlayerInfo *mplayInfo, struct MusicPlayerTrack *track) {
    u8 idx = read_byte(track);
    memcpy(&track->tone, &mplayInfo->tone[idx], sizeof(struct ToneData));
}

void ply_vol(struct MusicPlayerInfo *mplayInfo, struct MusicPlayerTrack *track) {
    (void)mplayInfo;
    track->vol = read_byte(track);
    track->flags |= MPT_FLG_VOLCHG;
}

void ply_pan(struct MusicPlayerInfo *mplayInfo, struct MusicPlayerTrack *track) {
    (void)mplayInfo;
    track->pan = (s8)(read_byte(track) - C_V);
    track->flags |= MPT_FLG_VOLCHG;
}

void ply_bend(struct MusicPlayerInfo *mplayInfo, struct MusicPlayerTrack *track) {
    (void)mplayInfo;
    track->bend = (s8)(read_byte(track) - C_V);
    track->flags |= MPT_FLG_PITCHG;
}

void ply_bendr(struct MusicPlayerInfo *mplayInfo, struct MusicPlayerTrack *track) {
    (void)mplayInfo;
    track->bendRange = read_byte(track);
    track->flags |= MPT_FLG_PITCHG;
}

void ply_lfodl(struct MusicPlayerInfo *mplayInfo, struct MusicPlayerTrack *track) {
    (void)mplayInfo;
    track->lfoDelay = read_byte(track);
}

void ply_modt(struct MusicPlayerInfo *mplayInfo, struct MusicPlayerTrack *track) {
    (void)mplayInfo;
    u8 v = read_byte(track);
    if (track->modT != v) {
        track->modT = v;
        track->flags |= MPT_FLG_VOLCHG | MPT_FLG_PITCHG;
    }
}

void ply_tune(struct MusicPlayerInfo *mplayInfo, struct MusicPlayerTrack *track) {
    (void)mplayInfo;
    track->tune = (s8)(read_byte(track) - C_V);
    track->flags |= MPT_FLG_PITCHG;
}

void ply_port(struct MusicPlayerInfo *mplayInfo, struct MusicPlayerTrack *track) {
    (void)mplayInfo;
    u8 reg = read_byte(track);
    u8 value = read_byte(track);
    *(vu8 *)(uintptr_t)(REG_ADDR_NR10 + reg) = value;
}

void ply_lfos(struct MusicPlayerInfo *mplayInfo, struct MusicPlayerTrack *track) {
    (void)mplayInfo;
    track->lfoSpeed = read_byte(track);
    if (track->lfoSpeed == 0) {
        clear_modM(track);
    }
}

void ply_mod(struct MusicPlayerInfo *mplayInfo, struct MusicPlayerTrack *track) {
    (void)mplayInfo;
    track->mod = read_byte(track);
    if (track->mod == 0) {
        clear_modM(track);
    }
}

void ply_endtie(struct MusicPlayerInfo *mplayInfo, struct MusicPlayerTrack *track) {
    (void)mplayInfo;
    u8 key;
    if (*track->cmdPtr < 0x80) {
        key = read_byte(track);
        track->key = key;
    } else {
        key = track->key;
    }
    for (struct SoundChannel *chan = track->chan; chan != NULL; chan = chan->nextChannelPointer) {
        u8 f = chan->statusFlags;
        if ((f & (SF_START | SF_ENV)) && !(f & SF_STOP) && chan->midiKey == key) {
            chan->statusFlags = f | SF_STOP;
            return;
        }
    }
}

void TrackStop(struct MusicPlayerInfo *mplayInfo, struct MusicPlayerTrack *track) {
    (void)mplayInfo;
    if (!(track->flags & MPT_FLG_EXIST)) {
        return;
    }
    for (struct SoundChannel *chan = track->chan; chan != NULL; chan = chan->nextChannelPointer) {
        if (chan->statusFlags != 0) {
            u8 cgb = chan->type & TONEDATA_TYPE_CGB;
            if (cgb) {
                sound_info()->CgbOscOff(cgb);
            }
            chan->statusFlags = 0;
        }
        chan->track = NULL;
    }
    track->chan = NULL;
}

/* ---- notes ----------------------------------------------------------------- */

void ply_note(u32 note_cmd, struct MusicPlayerInfo *mplayInfo, struct MusicPlayerTrack *track) {
    struct SoundInfo *si = sound_info();

    track->gateTime = gClockTable[note_cmd];
    u8 *p = track->cmdPtr;
    if (*p < 0x80) {
        track->key = *p++;
        if (*p < 0x80) {
            track->velocity = *p++;
            if (*p < 0x80) {
                track->gateTime += *p++;
            }
        }
        track->cmdPtr = p;
    }

    s32 rhythmPan = 0;
    struct ToneData *tone = &track->tone;
    u8 type = tone->type;
    u8 key = track->key;
    if (type & (TONEDATA_TYPE_RHY | TONEDATA_TYPE_SPL)) {
        u8 idx = key;
        if (type & TONEDATA_TYPE_SPL) {
            u32 table;
            memcpy(&table, &tone->attack, 4);   /* key split table pointer */
            idx = ((const u8 *)(uintptr_t)table)[key];
        }
        tone = &((struct ToneData *)tone->wav)[idx];
        if (tone->type & (TONEDATA_TYPE_SPL | TONEDATA_TYPE_RHY)) {
            return;
        }
        if (type & TONEDATA_TYPE_RHY) {
            if (tone->pan_sweep & 0x80) {
                rhythmPan = ((s32)tone->pan_sweep - TONEDATA_P_S_PAN) << 1;
            }
            key = tone->key;
        }
    }

    u32 priority = mplayInfo->priority + track->priority;
    if (priority > 0xFF) {
        priority = 0xFF;
    }

    u8 cgbType = tone->type & TONEDATA_TYPE_CGB;
    struct SoundChannel *chan;
    if (cgbType) {
        if (si->cgbChans == NULL) {
            return;
        }
        struct CgbChannel *c = &si->cgbChans[cgbType - 1];
        if ((c->statusFlags & SF_ON) && !(c->statusFlags & SF_STOP)) {
            if (c->priority > priority) {
                return;
            }
            if (c->priority == priority && (uintptr_t)c->track < (uintptr_t)track) {
                return;
            }
        }
        chan = (struct SoundChannel *)c;
    } else {
        u32 bestPrio = priority;
        struct MusicPlayerTrack *bestTrack = track;
        bool8 foundStopped = FALSE;
        chan = NULL;
        for (int i = 0; i < si->maxChans; i++) {
            struct SoundChannel *c = &si->chans[i];
            u8 f = c->statusFlags;
            if (!(f & SF_ON)) {
                chan = c;
                break;
            }
            if (f & SF_STOP) {
                if (!foundStopped) {
                    foundStopped = TRUE;
                    bestPrio = c->priority;
                    bestTrack = c->track;
                    chan = c;
                    continue;
                }
            } else if (foundStopped) {
                continue;
            }
            if (c->priority < bestPrio) {
                bestPrio = c->priority;
                bestTrack = c->track;
                chan = c;
            } else if (c->priority == bestPrio) {
                if ((uintptr_t)c->track > (uintptr_t)bestTrack) {
                    bestTrack = c->track;
                    chan = c;
                } else if (c->track == bestTrack) {
                    chan = c;
                }
            }
        }
        if (chan == NULL) {
            return;
        }
    }

    ClearChain(chan);
    chan->prevChannelPointer = NULL;
    chan->nextChannelPointer = track->chan;
    if (track->chan != NULL) {
        track->chan->prevChannelPointer = chan;
    }
    track->chan = chan;
    chan->track = track;
    track->lfoDelayC = track->lfoDelay;
    if (track->lfoDelay != 0) {
        clear_modM(track);
    }
    TrkVolPitSet(mplayInfo, track);
    chan->gateTime = track->gateTime;
    chan->midiKey = track->key;
    chan->velocity = track->velocity;
    chan->priority = (u8)priority;
    chan->key = key;
    chan->rhythmPan = (u8)rhythmPan;
    chan->type = tone->type;
    chan->wav = tone->wav;
    chan->attack = tone->attack;
    chan->decay = tone->decay;
    chan->sustain = tone->sustain;
    chan->release = tone->release;
    chan->pseudoEchoVolume = track->pseudoEchoVolume;
    chan->pseudoEchoLength = track->pseudoEchoLength;
    chn_vol_set(chan, track);

    s32 k = chan->key + (s8)track->keyM;
    if (k < 0) {
        k = 0;
    }
    if (cgbType) {
        struct CgbChannel *c = (struct CgbChannel *)chan;
        c->length = tone->length;
        u8 ps = tone->pan_sweep;
        if ((ps & 0x80) || !(ps & 0x70)) {
            ps = 8;
        }
        c->sweep = ps;
        c->frequency = si->MidiKeyToCgbFreq(cgbType, (u8)k, track->pitM);
    } else {
        chan->count = track->unk_3C;
        chan->frequency = MidiKeyToFreq(chan->wav, (u8)k, track->pitM);
    }
    chan->statusFlags = SF_START;
    track->flags &= 0xF0;
}

/* ---- the sequencer ------------------------------------------------------------ */

void MPlayMain(struct MusicPlayerInfo *mplayInfo) {
    if (mplayInfo->ident != ID_NUMBER) {
        return;
    }
    mplayInfo->ident++;
    if (mplayInfo->MPlayMainNext != NULL) {
        mplayInfo->MPlayMainNext(mplayInfo->musicPlayerNext);
    }

    struct SoundInfo *si = sound_info();
    if ((s32)mplayInfo->status < 0) {
        goto done;
    }
    FadeOutBody(mplayInfo);
    if ((s32)mplayInfo->status < 0) {
        goto done;
    }

    u32 tempoC = (u16)(mplayInfo->tempoC + mplayInfo->tempoI);
    while (tempoC >= 150) {
        u32 trackBits = 0;
        u32 bit = 1;
        struct MusicPlayerTrack *track = mplayInfo->tracks;
        for (int i = 0; i < mplayInfo->trackCount; i++, track++, bit <<= 1) {
            if (!(track->flags & MPT_FLG_EXIST)) {
                continue;
            }
            trackBits |= bit;

            for (struct SoundChannel *chan = track->chan; chan != NULL; chan = chan->nextChannelPointer) {
                if (chan->statusFlags & SF_ON) {
                    if (chan->gateTime != 0 && --chan->gateTime == 0) {
                        chan->statusFlags |= SF_STOP;
                    }
                } else {
                    ClearChain(chan);
                }
            }

            if (track->flags & MPT_FLG_START) {
                Clear64byte(track);
                track->flags = MPT_FLG_EXIST;
                track->bendRange = 2;
                track->volX = 0x40;
                track->lfoSpeed = 0x16;
                track->tone.type = 1;
            }

            bool8 trackEnded = FALSE;
            while (track->wait == 0) {
                u8 cmd = *track->cmdPtr;
                if (cmd < 0x80) {
                    cmd = track->runningStatus;
                } else {
                    track->cmdPtr++;
                    if (cmd >= 0xBD) {
                        track->runningStatus = cmd;
                    }
                }
                if (cmd >= 0xCF) {
                    si->plynote(cmd - 0xCF, mplayInfo, track);
                } else if (cmd > 0xB0) {
                    mplayInfo->cmd = cmd - 0xB1;
                    ((void (*)(struct MusicPlayerInfo *, struct MusicPlayerTrack *))
                         si->MPlayJumpTable[mplayInfo->cmd])(mplayInfo, track);
                    if (track->flags == 0) {
                        trackEnded = TRUE;
                        break;
                    }
                } else {
                    track->wait = gClockTable[cmd - 0x80];
                }
            }
            if (trackEnded) {
                continue;
            }

            track->wait--;
            if (track->lfoSpeed != 0 && track->mod != 0) {
                if (track->lfoDelayC != 0) {
                    track->lfoDelayC--;
                } else {
                    u32 sum = track->lfoSpeedC + track->lfoSpeed;   /* not truncated, as in the asm */
                    track->lfoSpeedC = (u8)sum;
                    s32 r;
                    if ((u8)(sum - 0x40) & 0x80) {
                        r = (s8)sum;
                    } else {
                        r = 0x80 - (s32)sum;
                    }
                    r = (track->mod * r) >> 6;
                    if ((u8)(track->modM ^ r) != 0) {
                        track->modM = (s8)r;
                        track->flags |= (track->modT == 0) ? MPT_FLG_PITCHG : MPT_FLG_VOLCHG;
                    }
                }
            }
        }

        mplayInfo->clock++;
        if (trackBits == 0) {
            mplayInfo->status = MUSICPLAYER_STATUS_PAUSE;
            goto done;
        }
        mplayInfo->status = trackBits;
        tempoC = (u16)(tempoC - 150);
    }
    mplayInfo->tempoC = (u16)tempoC;

    {
        struct MusicPlayerTrack *track = mplayInfo->tracks;
        for (int i = 0; i < mplayInfo->trackCount; i++, track++) {
            if (!(track->flags & MPT_FLG_EXIST) || !(track->flags & (MPT_FLG_VOLCHG | MPT_FLG_PITCHG))) {
                continue;
            }
            TrkVolPitSet(mplayInfo, track);
            for (struct SoundChannel *chan = track->chan; chan != NULL; chan = chan->nextChannelPointer) {
                if (!(chan->statusFlags & SF_ON)) {
                    ClearChain(chan);
                    continue;
                }
                u8 cgb = chan->type & TONEDATA_TYPE_CGB;
                if (track->flags & MPT_FLG_VOLCHG) {
                    chn_vol_set(chan, track);
                    if (cgb) {
                        ((struct CgbChannel *)chan)->modify |= CGB_CHANNEL_MO_VOL;
                    }
                }
                if (track->flags & MPT_FLG_PITCHG) {
                    s32 k = chan->key + (s8)track->keyM;
                    if (k < 0) {
                        k = 0;
                    }
                    if (cgb) {
                        struct CgbChannel *c = (struct CgbChannel *)chan;
                        c->frequency = si->MidiKeyToCgbFreq(cgb, (u8)k, track->pitM);
                        c->modify |= CGB_CHANNEL_MO_PIT;
                    } else {
                        chan->frequency = MidiKeyToFreq(chan->wav, (u8)k, track->pitM);
                    }
                }
            }
            track->flags &= 0xF0;
        }
    }
done:
    mplayInfo->ident = ID_NUMBER;
}

/* ---- the PCM mixer ------------------------------------------------------------- */

/* DPCM-compressed waves: blocks of 64 samples in 33 bytes (SoundMainRAM_Unk2). */
typedef struct {
    const struct WaveData *wav;
    s32 block;
    s8 samples[64];
} DecodeCache;

static s8 sample_at(const struct WaveData *wav, bool8 compressed, DecodeCache *cache, s32 i) {
    if (i < 0) {
        i = 0;
    }
    if (!compressed) {
        return wav->data[i];
    }
    s32 block = i >> 6;
    if (cache->wav != wav || cache->block != block) {
        const u8 *src = (const u8 *)wav + 0x10 + block * 0x21;
        u8 acc = src[0];
        cache->samples[0] = (s8)acc;
        acc = (u8)(acc + gDeltaEncodingTable[src[1] & 0xF]);
        cache->samples[1] = (s8)acc;
        for (int k = 2, j = 2; k < 64; k += 2, j++) {
            acc = (u8)(acc + gDeltaEncodingTable[src[j] >> 4]);
            cache->samples[k] = (s8)acc;
            acc = (u8)(acc + gDeltaEncodingTable[src[j] & 0xF]);
            cache->samples[k + 1] = (s8)acc;
        }
        cache->wav = wav;
        cache->block = block;
    }
    return cache->samples[i & 63];
}

static void mix_channel(struct SoundInfo *si, struct SoundChannel *chan, s8 *right, s8 *left, s32 n) {
    const struct WaveData *wav = chan->wav;
    bool8 compressed = wav->type != 0;
    bool8 reverse = (chan->type & TONEDATA_TYPE_REV) != 0;
    bool8 loop = (chan->statusFlags & SF_LOOP) && !reverse;
    s32 size = (s32)wav->size;
    s32 loopStart = (s32)wav->loopStart;
    s32 loopLen = size - loopStart;
    u32 step = (chan->type & TONEDATA_TYPE_FIX) ? 0x800000u : si->divFreq * chan->frequency;
    u32 fw = chan->fw;
    s32 rem = (s32)chan->count;
    s32 envR = chan->envelopeVolumeRight, envL = chan->envelopeVolumeLeft;
    DecodeCache cache = { NULL, -1, { 0 } };

    if (rem <= 0) {
        chan->statusFlags = 0;
        return;
    }
    for (s32 i = 0; i < n; i++) {
        s32 s0, s1;
        if (reverse) {
            s0 = sample_at(wav, compressed, &cache, rem - 1);
            s1 = sample_at(wav, compressed, &cache, rem - 2);
        } else {
            s32 idx = size - rem;
            s0 = sample_at(wav, compressed, &cache, idx);
            s32 nxt = idx + 1 < size ? idx + 1 : (loop ? loopStart : idx);
            s1 = sample_at(wav, compressed, &cache, nxt);
        }
        s32 v = s0 + (((s1 - s0) * (s32)fw) >> 23);
        right[i] = (s8)(right[i] + ((envR * v) >> 8));   /* 8-bit wrap, as the GBA */
        left[i] = (s8)(left[i] + ((envL * v) >> 8));

        fw += step;
        u32 adv = fw >> 23;
        if (adv != 0) {
            fw &= 0x7FFFFF;
            rem -= (s32)adv;
            if (rem <= 0) {
                if (loop && loopLen > 0) {
                    do {
                        rem += loopLen;
                    } while (rem <= 0);
                } else {
                    chan->statusFlags = 0;
                    break;
                }
            }
        }
    }
    chan->fw = fw;
    chan->count = (u32)rem;
}

void SoundMain(void) {
    struct SoundInfo *si = sound_info();
    if (si->ident != ID_NUMBER) {
        return;
    }
    si->ident++;

    if (si->MPlayMainHead != NULL) {
        si->MPlayMainHead(si->musicPlayerHead);
    }
    si->CgbSound();

    s32 n = si->pcmSamplesPerVBlank;
    s8 *base = si->pcmBuffer;
    s32 counter = si->pcmDmaCounter;
    if (counter - 1 > 0) {
        base += (si->pcmDmaPeriod - (counter - 1)) * n;
    }
    s8 *right = base;                    /* FIFO A */
    s8 *left = base + PCM_DMA_BUF_SIZE;  /* FIFO B */

    if (si->reverb != 0) {
        s8 *prev = (counter == 2) ? si->pcmBuffer : base + n;
        for (s32 i = 0; i < n; i++) {
            s32 sum = left[i] + right[i] + prev[i + PCM_DMA_BUF_SIZE] + prev[i];
            s32 v = (sum * si->reverb) >> 9;
            if (v & 0x80) {
                v++;
            }
            left[i] = (s8)v;
            right[i] = (s8)v;
        }
    } else {
        memset(right, 0, (size_t)n);
        memset(left, 0, (size_t)n);
    }

    for (int c = 0; c < si->maxChans; c++) {
        struct SoundChannel *chan = &si->chans[c];
        u8 f = chan->statusFlags;
        if (!(f & SF_ON)) {
            continue;
        }
        const struct WaveData *wav = chan->wav;
        s32 env;
        if (f & SF_START) {
            if (f & SF_STOP) {
                chan->statusFlags = 0;
                continue;
            }
            f = SOUND_CHANNEL_SF_ENV_ATTACK;
            u32 offset = chan->count;
            chan->count = wav->size - offset;   /* samples left from the start offset */
            env = 0;
            chan->envelopeVolume = 0;
            chan->fw = 0;
            if ((wav->status >> 8) & WAVE_DATA_FLAG_LOOP) {   /* o_WaveData_flags = byte 3 */
                f |= SF_LOOP;
            }
            chan->statusFlags = f;
            goto attack;
        }
        env = chan->envelopeVolume;
        if (f & SF_IEC) {
            u8 l = chan->pseudoEchoLength;
            chan->pseudoEchoLength = (u8)(l - 1);
            if (l > 1) {
                goto set_env;
            }
            chan->statusFlags = 0;
            continue;
        }
        if (f & SF_STOP) {
            env = (env * chan->release) >> 8;
            if (env > chan->pseudoEchoVolume) {
                goto set_env;
            }
            goto echo;
        }
        if ((f & SF_ENV) == SOUND_CHANNEL_SF_ENV_DECAY) {
            env = (env * chan->decay) >> 8;
            if (env > chan->sustain) {
                goto set_env;
            }
            env = chan->sustain;
            if (env == 0) {
                goto echo;
            }
            f--;
            chan->statusFlags = f;
            goto set_env;
        }
        if ((f & SF_ENV) != SOUND_CHANNEL_SF_ENV_ATTACK) {
            goto set_env;
        }
    attack:
        env += chan->attack;
        if (env >= 0xFF) {
            env = 0xFF;
            f--;
            chan->statusFlags = f;
        }
        goto set_env;
    echo:
        env = chan->pseudoEchoVolume;
        if (env == 0) {
            chan->statusFlags = 0;
            continue;
        }
        f |= SF_IEC;
        chan->statusFlags = f;
    set_env:
        chan->envelopeVolume = (u8)env;
        env = (env * (si->masterVolume + 1)) >> 4;
        chan->envelopeVolumeRight = (u8)((chan->rightVolume * env) >> 8);
        chan->envelopeVolumeLeft = (u8)((chan->leftVolume * env) >> 8);
        mix_channel(si, chan, right, left, n);
    }

    si->ident = ID_NUMBER;
    pc_audio_frame(right, left, n);
}

/* m4aSoundVSync (VCount IRQ): keeps the PCM DMA buffer position in step. */
void m4aSoundVSync(void) {
    struct SoundInfo *si = sound_info();
    if (si->ident - ID_NUMBER > 1) {
        return;
    }
    s32 c = (s32)si->pcmDmaCounter - 1;
    si->pcmDmaCounter = (u8)c;
    if (c > 0) {
        return;
    }
    si->pcmDmaCounter = si->pcmDmaPeriod;
}

/* Linker-script symbols of ld_script.txt: their *address* is the value. */
__attribute__((used)) static void prs_define_sound_constants(void) {
    __asm__ volatile(".globl gNumMusicPlayers\n.set gNumMusicPlayers, 4\n"
                     ".globl gMaxLines\n.set gMaxLines, 60\n");
}

/* m4a_1.s .bss: the tracks of the four music players (sound/
 * music_player_table.inc points into it: BGM +0x40, SE1 +0x360...). */
__attribute__((aligned(4))) u8 gUnknown_030007B8[0x770];
