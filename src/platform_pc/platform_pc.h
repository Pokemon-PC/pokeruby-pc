/* platform_pc.h - logging and small helpers shared by the PC HAL. */
#ifndef PRS_PLATFORM_PC_H
#define PRS_PLATFORM_PC_H

#ifdef __cplusplus
extern "C" {
#endif

#if defined(__GNUC__) || defined(__clang__)
#define PC_PRINTF(f, a) __attribute__((format(printf, f, a)))
#else
#define PC_PRINTF(f, a)
#endif

/* The game this executable is (CMake PRS_GAME_VERSION -> RUBY or SAPPHIRE):
 * window title, default save and clock files. Both games can share a folder. */
#if defined(SAPPHIRE)
#define PRS_GAME_TITLE "Pokemon Sapphire"
#define PRS_SAVE_DEFAULT "save_sapphire.bin"
#define PRS_RTC_DEFAULT "save_sapphire.rtc"
#else
#define PRS_GAME_TITLE "Pokemon Ruby"
#define PRS_SAVE_DEFAULT "save_ruby.bin"
#define PRS_RTC_DEFAULT "save_ruby.rtc"
#endif

void pc_log(const char *fmt, ...) PC_PRINTF(1, 2);
void pc_warn(const char *fmt, ...) PC_PRINTF(1, 2);
_Noreturn void pc_fatal(const char *fmt, ...) PC_PRINTF(1, 2);

#ifdef __cplusplus
}
#endif

#endif /* PRS_PLATFORM_PC_H */
