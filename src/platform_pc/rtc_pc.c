/*
 * rtc_pc.c - the cartridge's Seiko S-3511 real-time clock.
 *
 * Replaces src/siirtc.c in the PC build: that file bit-bangs the chip
 * through the GPIO port at 0x080000C4 (inside the ROM area), which plain
 * memory cannot emulate. Same functions, same values (BCD), over the host's
 * local clock plus an offset. The offset and the status byte persist in a
 * small text file next to the save (PRS_RTC, default PRS_RTC_DEFAULT), like the
 * cartridge battery: resetting or setting the clock in game survives restarts.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "pc_hal.h"
#include "platform_pc.h"

#include "global.h"
#include "siirtc.h"

static char s_path[1024];
static bool s_loaded;
static long long s_offset;           /* emulated - host, in seconds */
static u8 s_status = SIIRTCINFO_24HOUR;

void pc_rtc_set_path(const char *path) {
    snprintf(s_path, sizeof s_path, "%s", path);
    s_loaded = false;
}

static void load(void) {
    if (s_loaded) {
        return;
    }
    s_loaded = true;
    if (!s_path[0]) {
        const char *e = getenv("PRS_RTC");
        snprintf(s_path, sizeof s_path, "%s", e && *e ? e : PRS_RTC_DEFAULT);
    }
    s_offset = 0;
    s_status = SIIRTCINFO_24HOUR;
    FILE *f = fopen(s_path, "r");
    if (f) {
        unsigned st = SIIRTCINFO_24HOUR;
        if (fscanf(f, "offset=%lld status=%u", &s_offset, &st) >= 1) {
            s_status = (u8)st;
        }
        fclose(f);
    }
}

static void store(void) {
    FILE *f = fopen(s_path, "w");
    if (f) {
        fprintf(f, "offset=%lld status=%u\n", s_offset, (unsigned)s_status);
        fclose(f);
    }
}

static u8 to_bcd(int v) { return (u8)(((v / 10) << 4) | (v % 10)); }
static int from_bcd(u8 v) { return (v >> 4) * 10 + (v & 0xF); }

static struct tm now_tm(void) {
    load();
    time_t t = time(NULL) + (time_t)s_offset;
    struct tm tm;
#if defined(_WIN32)
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    return tm;
}

/* Makes the clock read `tm` from now on. */
static void set_tm(struct tm *tm) {
    load();
    tm->tm_isdst = -1;
    time_t target = mktime(tm);
    if (target != (time_t)-1) {
        s_offset = (long long)(target - time(NULL));
        store();
    }
}

void SiiRtcUnprotect(void) {}
void SiiRtcProtect(void) {}

u8 SiiRtcProbe(void) {
    load();
    return 1;   /* chip present, no reset needed (24-hour mode, no power loss) */
}

bool8 SiiRtcReset(void) {
    /* The chip resets to 2000-01-01 00:00:00; siirtc.c then sets 24-hour mode. */
    struct tm tm;
    memset(&tm, 0, sizeof tm);
    tm.tm_year = 100;
    tm.tm_mday = 1;
    s_status = SIIRTCINFO_24HOUR;
    set_tm(&tm);
    return TRUE;
}

bool8 SiiRtcGetStatus(struct SiiRtcInfo *rtc) {
    load();
    rtc->status = s_status;
    return TRUE;
}

bool8 SiiRtcSetStatus(struct SiiRtcInfo *rtc) {
    load();
    /* The driver always writes 24-hour mode; the power flag is read-only. */
    s_status = (u8)(SIIRTCINFO_24HOUR | (rtc->status & (SIIRTCINFO_INTAE | SIIRTCINFO_INTME | SIIRTCINFO_INTFE)));
    store();
    return TRUE;
}

bool8 SiiRtcGetDateTime(struct SiiRtcInfo *rtc) {
    struct tm tm = now_tm();
    int year = tm.tm_year - 100;   /* the chip counts 2000-2099 */
    if (year < 0) year = 0;
    if (year > 99) year = 99;
    rtc->year = to_bcd(year);
    rtc->month = to_bcd(tm.tm_mon + 1);
    rtc->day = to_bcd(tm.tm_mday);
    rtc->dayOfWeek = to_bcd(tm.tm_wday);
    rtc->hour = to_bcd(tm.tm_hour);
    rtc->minute = to_bcd(tm.tm_min);
    rtc->second = to_bcd(tm.tm_sec);
    return TRUE;
}

bool8 SiiRtcGetTime(struct SiiRtcInfo *rtc) {
    struct tm tm = now_tm();
    rtc->hour = to_bcd(tm.tm_hour);
    rtc->minute = to_bcd(tm.tm_min);
    rtc->second = to_bcd(tm.tm_sec);
    return TRUE;
}

bool8 SiiRtcSetDateTime(struct SiiRtcInfo *rtc) {
    struct tm tm;
    memset(&tm, 0, sizeof tm);
    tm.tm_year = 100 + from_bcd(rtc->year);
    tm.tm_mon = from_bcd(rtc->month) - 1;
    tm.tm_mday = from_bcd(rtc->day);
    tm.tm_hour = from_bcd(rtc->hour & 0x3F);
    tm.tm_min = from_bcd(rtc->minute);
    tm.tm_sec = from_bcd(rtc->second);
    set_tm(&tm);
    return TRUE;
}

bool8 SiiRtcSetTime(struct SiiRtcInfo *rtc) {
    struct tm tm = now_tm();
    tm.tm_hour = from_bcd(rtc->hour & 0x3F);
    tm.tm_min = from_bcd(rtc->minute);
    tm.tm_sec = from_bcd(rtc->second);
    set_tm(&tm);
    return TRUE;
}
