/*
 * pc_game_prefix.h - included first in every game C file of the PC build
 * (pp_c.py, -include).
 *
 * The decompiled code still carries ARM inline assembly meant for agbcc:
 * register pinning (`register u32 x asm("r4")`), compiler barriers
 * (`asm("":::"r5")`), padding between data (`asm(".space 32")`) and the
 * bodies of NAKED functions. None of it means anything on the host, and the
 * functions whose only version is ARM assembly are selected in C by
 * -DNONMATCHING. The few NAKED functions with no C version are marked
 * deprecated in include/gba/defines.h: calling one is a build error
 * (-Werror=deprecated-declarations), so none is silently empty.
 */
#ifndef PRS_PC_GAME_PREFIX_H
#define PRS_PC_GAME_PREFIX_H

#define asm(...)

#endif /* PRS_PC_GAME_PREFIX_H */
