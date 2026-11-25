/*
 * flash_pc.c - the 128 KiB cartridge flash (Macronix MX29L010), as a file.
 *
 * Replaces src/agb_flash.c, agb_flash_1m.c and agb_flash_mx.c in the PC
 * build: those drive the chip through command writes to 0x0E005555 etc.,
 * which plain memory cannot react to. Same symbols, same return values
 * (0 = success), backed by save.bin. Writes are flushed at the next VBlank
 * and at exit, through a temporary file + rename so a crash cannot leave a
 * half-written save.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pc_hal.h"
#include "platform_pc.h"

#include "global.h"
#include "gba/flash_internal.h"

#define FLASH_SIZE   0x20000
#define SECTOR_SIZE  0x1000
#define SECTOR_COUNT (FLASH_SIZE / SECTOR_SIZE)

static uint8_t s_flash[FLASH_SIZE];
static char s_path[1024];
static bool s_loaded, s_dirty;

/* ---- the variables agb_flash.c exports ------------------------------------ */
u8 gFlashTimeoutFlag;
u8 (*PollFlashStatus)(u8 *);
u16 (*WaitForFlashWrite)(u8 phase, u8 *addr, u8 lastData);
u16 (*ProgramFlashSector)(u16 sectorNum, void *src);
const struct FlashType *gFlash;
u16 (*ProgramFlashByte)(u16 sectorNum, u32 offset, u8 data);
u16 gFlashNumRemainingBytes;
u16 (*EraseFlashChip)(void);
u16 (*EraseFlashSector)(u16 sectorNum);
const u16 *gFlashMaxTime;

static const u16 sMaxTime[] = { 10, 65469, 4, 2000, 65469, 4, 2000, 65469, 4 };

static const struct FlashType sType = {
    .romSize = FLASH_SIZE,
    .sector = { .size = SECTOR_SIZE, .shift = 12, .count = SECTOR_COUNT, .top = 0 },
    .wait = { 3, 1 },
    .ids = { .separate = { .makerId = 0xC2, .deviceId = 0x09 } },
};

/* ---- file backing --------------------------------------------------------- */

void pc_flash_set_path(const char *path) {
    snprintf(s_path, sizeof s_path, "%s", path);
    s_loaded = false;
}

static void flush(void);

static void load(void) {
    if (s_loaded) {
        return;
    }
    s_loaded = true;
    if (!s_path[0]) {
        const char *e = getenv("PRS_SAVE");
        snprintf(s_path, sizeof s_path, "%s", e && *e ? e : PRS_SAVE_DEFAULT);
    }
    memset(s_flash, 0xFF, sizeof s_flash);   /* erased flash reads 0xFF */
    FILE *f = fopen(s_path, "rb");
    if (f) {
        size_t n = fread(s_flash, 1, sizeof s_flash, f);
        fclose(f);
        pc_log("flash: loaded %s (%zu bytes)", s_path, n);
    } else {
        pc_log("flash: no %s yet, starting with an erased chip", s_path);
    }
    static bool registered;
    if (!registered) {
        registered = true;
        atexit(flush);
    }
}

static void flush(void) {
    if (!s_dirty) {
        return;
    }
    char tmp[1100];
    snprintf(tmp, sizeof tmp, "%s.tmp", s_path);
    FILE *f = fopen(tmp, "wb");
    if (!f) {
        pc_warn("flash: cannot write %s", tmp);
        return;
    }
    bool ok = fwrite(s_flash, 1, sizeof s_flash, f) == sizeof s_flash;
    ok = fclose(f) == 0 && ok;
    if (!ok) {
        pc_warn("flash: short write to %s", tmp);
        remove(tmp);
        return;
    }
    remove(s_path);   /* rename() does not replace on Windows */
    if (rename(tmp, s_path) != 0) {
        pc_warn("flash: cannot rename %s to %s", tmp, s_path);
        return;
    }
    s_dirty = false;
}

void pc_flash_flush(void) {
    flush();
}

const uint8_t *pc_flash_data(void) {
    load();
    return s_flash;
}

/* ---- chip operations -------------------------------------------------------- */

static u16 PcProgramFlashByte(u16 sectorNum, u32 offset, u8 data) {
    if (sectorNum >= SECTOR_COUNT || offset >= SECTOR_SIZE) {
        return 0x80FF;
    }
    load();
    /* Flash programming can only clear bits. */
    s_flash[sectorNum * SECTOR_SIZE + offset] &= data;
    s_dirty = true;
    return 0;
}

static u16 PcProgramFlashSector(u16 sectorNum, void *src) {
    if (sectorNum >= SECTOR_COUNT) {
        return 0x80FF;
    }
    load();
    /* The real driver erases the sector first. */
    memcpy(&s_flash[sectorNum * SECTOR_SIZE], src, SECTOR_SIZE);
    gFlashNumRemainingBytes = 0;
    s_dirty = true;
    return 0;
}

static u16 PcEraseFlashChip(void) {
    load();
    memset(s_flash, 0xFF, sizeof s_flash);
    s_dirty = true;
    return 0;
}

static u16 PcEraseFlashSector(u16 sectorNum) {
    if (sectorNum >= SECTOR_COUNT) {
        return 0x80FF;
    }
    load();
    memset(&s_flash[sectorNum * SECTOR_SIZE], 0xFF, SECTOR_SIZE);
    s_dirty = true;
    return 0;
}

static u16 PcWaitForFlashWrite(u8 phase, u8 *addr, u8 lastData) {
    (void)phase;
    (void)addr;
    (void)lastData;
    return 0;
}

static u8 PcPollFlashStatus(u8 *addr) {
    return *addr;
}

u16 IdentifyFlash(void) {
    load();
    ProgramFlashByte = PcProgramFlashByte;
    ProgramFlashSector = PcProgramFlashSector;
    EraseFlashChip = PcEraseFlashChip;
    EraseFlashSector = PcEraseFlashSector;
    WaitForFlashWrite = PcWaitForFlashWrite;
    PollFlashStatus = PcPollFlashStatus;
    gFlashMaxTime = sMaxTime;
    gFlash = &sType;
    return 0;
}

u16 ReadFlashId(void) {
    return sType.ids.joined;
}

u16 SetFlashTimerIntr(u8 timerNum, void (**intrFunc)(void)) {
    /* No timeouts on PC: the flash never stays busy. */
    (void)intrFunc;
    return timerNum > 3 ? 1 : 0;
}

void SwitchFlashBank(u8 bankNum) {
    (void)bankNum;   /* sectors are numbered across both banks */
}

void ReadFlash(u16 sectorNum, u32 offset, void *dest, u32 size) {
    load();
    u32 start = (u32)sectorNum * SECTOR_SIZE + offset;
    if (start >= FLASH_SIZE) {
        return;
    }
    if (size > FLASH_SIZE - start) {
        size = FLASH_SIZE - start;
    }
    memcpy(dest, &s_flash[start], size);
}

u32 VerifyFlashSector(u16 sectorNum, u8 *src) {
    load();
    if (sectorNum >= SECTOR_COUNT) {
        return 1;
    }
    const u8 *p = &s_flash[sectorNum * SECTOR_SIZE];
    for (u32 i = 0; i < SECTOR_SIZE; i++) {
        if (p[i] != src[i]) {
            return 0x0E000000u + sectorNum * SECTOR_SIZE + i;   /* failing address */
        }
    }
    return 0;
}

u32 VerifyFlashSectorNBytes(u16 sectorNum, u8 *src, u32 n) {
    load();
    if (sectorNum >= SECTOR_COUNT) {
        return 1;
    }
    if (n > SECTOR_SIZE) {
        n = SECTOR_SIZE;
    }
    const u8 *p = &s_flash[sectorNum * SECTOR_SIZE];
    for (u32 i = 0; i < n; i++) {
        if (p[i] != src[i]) {
            return 0x0E000000u + sectorNum * SECTOR_SIZE + i;
        }
    }
    return 0;
}

u32 ProgramFlashSectorAndVerify(u16 sectorNum, u8 *src) {
    for (int tries = 0; tries < 3; tries++) {
        u32 r = PcProgramFlashSector(sectorNum, src);
        if (r == 0 && (r = VerifyFlashSector(sectorNum, src)) == 0) {
            return 0;
        }
    }
    return 1;
}

u32 ProgramFlashSectorAndVerifyNBytes(u16 sectorNum, void *src, u32 n) {
    if (sectorNum >= SECTOR_COUNT) {
        return 0x80FF;
    }
    load();
    if (n > SECTOR_SIZE) {
        n = SECTOR_SIZE;
    }
    memset(&s_flash[sectorNum * SECTOR_SIZE], 0xFF, SECTOR_SIZE);
    memcpy(&s_flash[sectorNum * SECTOR_SIZE], src, n);
    s_dirty = true;
    return VerifyFlashSectorNBytes(sectorNum, src, n);
}
