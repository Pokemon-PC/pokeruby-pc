/*
 * nullread_pc.c - accesses through NULL behave as on the GBA.
 *
 * On the GBA, address 0 is the BIOS: reading a field through a NULL pointer
 * (a freed screen's data used by a sprite or VBlank callback one frame too
 * late, ...) returns a value, and writing there is ignored (it is ROM). Such
 * bugs are spread over the decompiled code; on PC the access faults. Rather
 * than patching every site, the crash handler calls pc_emulate_null_access():
 * if the faulting instruction is a simple x86 load, store, compare or
 * read-modify-write on an address in the GBA BIOS range (0x0000-0x3FFF), it
 * is emulated - memory reads as 0, writes are dropped, flags are computed as
 * the instruction would - and execution goes on after it. Every emulated
 * site is reported once (with a backtrace) so the bugs stay visible; any
 * other instruction is still a crash.
 *
 * Forms (memory operand only):
 *   loads   MOV r,m (8A/8B), MOVZX/MOVSX (0F B6/B7/BE/BF), MOVSXD (63)
 *   stores  MOV m,r (88/89), MOV m,imm (C6/C7 /0)
 *   compare CMP m,imm (80/81/83 /7), CMP m,r / r,m (38-3B),
 *           TEST m,imm (F6/F7 /0), TEST m,r (84/85)
 *   RMW     ADD/OR/AND/SUB/XOR m,imm (80/81/83) and m,r (00/01 08/09 20/21
 *           28/29 30/31), INC/DEC m (FE/FF /0 /1)
 *
 * Also here: integer division by zero (pc_emulate_divide_by_zero, end of file).
 */
#include "pc_hal.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define GBA_BIOS_END 0x4000u

#define FL_CF 0x0001u
#define FL_PF 0x0004u
#define FL_AF 0x0010u
#define FL_ZF 0x0040u
#define FL_SF 0x0080u
#define FL_OF 0x0800u
#define FL_ARITH (FL_CF | FL_PF | FL_AF | FL_ZF | FL_SF | FL_OF)

static uint64_t mask_of(int bits) {
    return bits == 64 ? ~0ull : ((1ull << bits) - 1);
}

static uint32_t parity_flag(uint64_t v) {
    uint8_t b = (uint8_t)v;
    b ^= b >> 4;
    b ^= b >> 2;
    b ^= b >> 1;
    return (b & 1) ? 0 : FL_PF;
}

static uint32_t result_flags(uint64_t r, int bits) {
    r &= mask_of(bits);
    uint32_t f = parity_flag(r);
    if (r == 0) f |= FL_ZF;
    if (r & (1ull << (bits - 1))) f |= FL_SF;
    return f;
}

/* Flags of a - b. */
static uint32_t sub_flags(uint64_t a, uint64_t b, int bits) {
    uint64_t m = mask_of(bits), sign = 1ull << (bits - 1);
    a &= m;
    b &= m;
    uint64_t r = (a - b) & m;
    uint32_t f = result_flags(r, bits);
    if (a < b) f |= FL_CF;
    if (((a ^ b) & (a ^ r)) & sign) f |= FL_OF;
    if ((a ^ b ^ r) & 0x10) f |= FL_AF;
    return f;
}

/* Flags of a + b. */
static uint32_t add_flags(uint64_t a, uint64_t b, int bits) {
    uint64_t m = mask_of(bits), sign = 1ull << (bits - 1);
    a &= m;
    b &= m;
    uint64_t r = (a + b) & m;
    uint32_t f = result_flags(r, bits);
    if (r < a) f |= FL_CF;
    if ((~(a ^ b) & (a ^ r)) & sign) f |= FL_OF;
    if ((a ^ b ^ r) & 0x10) f |= FL_AF;
    return f;
}

/* Flags of "0 op src" for the ALU group (reg field of 80/81/83). */
static bool alu_flags(int alu, uint64_t src, int bits, uint32_t *flags) {
    switch (alu) {
    case 0: *flags = add_flags(0, src, bits); return true;     /* ADD */
    case 1: *flags = result_flags(src, bits); return true;     /* OR  */
    case 4: *flags = result_flags(0, bits); return true;       /* AND */
    case 5: *flags = sub_flags(0, src, bits); return true;     /* SUB */
    case 6: *flags = result_flags(src, bits); return true;     /* XOR */
    case 7: *flags = sub_flags(0, src, bits); return true;     /* CMP */
    default: return false;                                     /* ADC/SBB */
    }
}

static void write_reg(uint64_t *regs[16], int reg, int bits, bool legacy_high8, uint64_t value) {
    if (bits == 8 && legacy_high8) {             /* AH, CH, DH, BH */
        uint64_t *r = regs[reg - 4];
        *r = (*r & ~0xFF00ull) | ((value & 0xFF) << 8);
    } else if (bits == 8) {
        *regs[reg] = (*regs[reg] & ~0xFFull) | (value & 0xFF);
    } else if (bits == 16) {
        *regs[reg] = (*regs[reg] & ~0xFFFFull) | (value & 0xFFFF);
    } else if (bits == 32) {
        *regs[reg] = value & 0xFFFFFFFFull;      /* zero-extends */
    } else {
        *regs[reg] = value;
    }
}

static uint64_t read_reg(uint64_t *regs[16], int reg, int bits, bool legacy_high8) {
    if (bits == 8 && legacy_high8) {
        return (*regs[reg - 4] >> 8) & 0xFF;
    }
    return *regs[reg] & mask_of(bits);
}

bool pc_emulate_null_access(uint64_t *regs[16], uint64_t *rip, uint32_t *eflags, uintptr_t fault, bool write) {
    if (fault >= GBA_BIOS_END) {
        return false;
    }
    const uint8_t *p = (const uint8_t *)(uintptr_t)*rip;
    const uint8_t *start = p;
    bool opsize16 = false;
    uint8_t rex = 0;
    for (;;) {
        if (*p == 0x66) { opsize16 = true; p++; }
        else if (*p == 0x2E || *p == 0x3E || *p == 0x26 || *p == 0x36) { p++; }
        else break;
    }
    if ((*p & 0xF0) == 0x40) {
        rex = *p++;
    }
    bool rexw = rex & 8, rexr = rex & 4;

    int op = *p++;
    int op2 = -1;
    if (op == 0x0F) {
        op2 = *p++;
    }

    uint8_t modrm = *p++;
    int mod = modrm >> 6, sub = (modrm >> 3) & 7, reg = sub | (rexr ? 8 : 0), rm = modrm & 7;
    if (mod == 3) {
        return false;                      /* register operand: not the faulting access */
    }
    if (rm == 4) {
        uint8_t sib = *p++;
        if (mod == 0 && (sib & 7) == 5) p += 4;
    } else if (mod == 0 && rm == 5) {
        p += 4;                            /* RIP-relative */
    }
    if (mod == 1) p += 1;
    if (mod == 2) p += 4;

    int opbits = rexw ? 64 : opsize16 ? 16 : 32;
    bool high8 = rex == 0 && (reg & 7) >= 4 && reg < 8;
    uint32_t newflags = 0;
    bool setflags = false;

    if (op2 >= 0) {
        if (write) return false;
        switch (op2) {
        case 0xB6: case 0xB7: case 0xBE: case 0xBF:        /* MOVZX / MOVSX */
            write_reg(regs, reg, opbits, false, 0);
            break;
        default:
            return false;
        }
    } else {
        switch (op) {
        /* ---- loads ---- */
        case 0x8A:
            if (write) return false;
            write_reg(regs, reg, 8, high8, 0);
            break;
        case 0x8B:
            if (write) return false;
            write_reg(regs, reg, opbits, false, 0);
            break;
        case 0x63:
            if (write) return false;
            write_reg(regs, reg, rexw ? 64 : 32, false, 0);
            break;
        /* ---- stores: dropped ---- */
        case 0x88: case 0x89:
            break;
        case 0xC6: case 0xC7:
            if (sub != 0) return false;
            p += op == 0xC6 ? 1 : (opbits == 16 ? 2 : 4);
            break;
        /* ---- ALU with immediate (CMP reads; others read-modify-write) ---- */
        case 0x80: case 0x81: case 0x83: {
            int bits = op == 0x80 ? 8 : opbits;
            int64_t imm;
            if (op == 0x81) {
                if (bits == 16) { imm = (int16_t)(p[0] | (p[1] << 8)); p += 2; }
                else { int32_t v; memcpy(&v, p, 4); imm = v; p += 4; }
            } else {
                imm = (int8_t)*p++;
            }
            if (!alu_flags(sub, (uint64_t)imm, bits, &newflags)) return false;
            setflags = true;
            break;
        }
        /* ---- ALU m, r (read-modify-write) ---- */
        case 0x00: case 0x01: case 0x08: case 0x09: case 0x20: case 0x21:
        case 0x28: case 0x29: case 0x30: case 0x31: {
            int bits = (op & 1) ? opbits : 8;
            uint64_t src = read_reg(regs, reg, bits, !(op & 1) && high8);
            if (!alu_flags(op >> 3, src, bits, &newflags)) return false;
            setflags = true;
            break;
        }
        /* ---- compares ---- */
        case 0x38: case 0x39: {
            int bits = op == 0x38 ? 8 : opbits;
            newflags = sub_flags(0, read_reg(regs, reg, bits, op == 0x38 && high8), bits);
            setflags = true;
            break;
        }
        case 0x3A: case 0x3B: {
            int bits = op == 0x3A ? 8 : opbits;
            newflags = sub_flags(read_reg(regs, reg, bits, op == 0x3A && high8), 0, bits);
            setflags = true;
            break;
        }
        case 0x84: case 0x85:
            newflags = result_flags(0, op == 0x84 ? 8 : opbits);
            setflags = true;
            break;
        case 0xF6: case 0xF7: {
            if (sub != 0) return false;
            int bits = op == 0xF6 ? 8 : opbits;
            p += op == 0xF6 ? 1 : (bits == 16 ? 2 : 4);
            newflags = result_flags(0, bits);
            setflags = true;
            break;
        }
        /* ---- INC / DEC m (CF unchanged) ---- */
        case 0xFE: case 0xFF: {
            if (sub > 1) return false;
            int bits = op == 0xFE ? 8 : opbits;
            uint32_t f = sub == 0 ? add_flags(0, 1, bits) : sub_flags(0, 1, bits);
            *eflags = (*eflags & ~(FL_ARITH & ~FL_CF)) | (f & ~FL_CF);
            break;
        }
        default:
            return false;
        }
    }
    if (setflags) {
        *eflags = (*eflags & ~FL_ARITH) | newflags;
    }
    *rip += (uint64_t)(p - start);
    return true;
}

/* ---- integer division by zero -------------------------------------------
 * The GBA has no divide instruction: the code calls libgcc's __divsi3 /
 * __udivsi3 / __modsi3 / __umodsi3, which return 0 when the divisor is 0
 * (lib1funcs.asm: Ldiv0 -> __div0, then "mov r0, #0"). x86 DIV/IDIV raise
 * #DE instead. The game divides by zero in a few places (first frame of an
 * animation whose divisor starts at 0...): the crash handler calls this to
 * give quotient and remainder 0 and go on after the instruction.
 *
 * Forms: DIV/IDIV r/m (F6 /6 /7: AX <- AL, AH; F7 /6 /7: rAX, rDX). */
bool pc_emulate_divide_by_zero(uint64_t *regs[16], uint64_t *rip) {
    const uint8_t *p = (const uint8_t *)(uintptr_t)*rip;
    bool opsize16 = false;
    uint8_t rex = 0;
    for (;;) {
        if (*p == 0x66) { opsize16 = true; p++; }
        else if (*p == 0x2E || *p == 0x3E || *p == 0x26 || *p == 0x36) { p++; }
        else break;
    }
    if ((*p & 0xF0) == 0x40) {
        rex = *p++;
    }
    int op = *p++;
    if (op != 0xF6 && op != 0xF7) {
        return false;
    }
    uint8_t modrm = *p++;
    int mod = modrm >> 6, sub = (modrm >> 3) & 7, rm = modrm & 7;
    if (sub != 6 && sub != 7) {
        return false;
    }
    if (mod != 3) {
        if (rm == 4) {
            uint8_t sib = *p++;
            if (mod == 0 && (sib & 7) == 5) p += 4;
        } else if (mod == 0 && rm == 5) {
            p += 4;
        }
        if (mod == 1) p += 1;
        if (mod == 2) p += 4;
    }
    if (op == 0xF6) {
        write_reg(regs, 0, 16, false, 0);                 /* AL = AH = 0 */
    } else {
        int bits = (rex & 8) ? 64 : opsize16 ? 16 : 32;
        write_reg(regs, 0, bits, false, 0);               /* quotient */
        write_reg(regs, 2, bits, false, 0);               /* remainder */
    }
    *rip = (uint64_t)(uintptr_t)p;
    return true;
}
