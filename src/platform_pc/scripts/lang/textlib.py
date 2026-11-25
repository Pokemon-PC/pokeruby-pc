"""Shared helpers for the French text mapping (scripts/lang/).

Gen 3 text: bytes ending with 0xFF. Control bytes:
  0xFC xx [args]  text command (colour, pause, font...)
  0xFD xx         placeholder ({PLAYER}, {STR_VAR_1}...)
  0xFE            new line,  0xFA / 0xFB  scroll / new page
They do not depend on the language, so their sequence (the "skeleton") is
what matches an English string to its French counterpart.
"""
import re

# Argument count of each 0xFC command (include/characters.h, EXT_CTRL_CODE_*).
FC_ARGS = {0x00: 0, 0x01: 1, 0x02: 1, 0x03: 1, 0x04: 3, 0x05: 1, 0x06: 1, 0x07: 0, 0x08: 1,
           0x09: 0, 0x0A: 0, 0x0B: 2, 0x0C: 1, 0x0D: 1, 0x0E: 1, 0x0F: 0, 0x10: 2, 0x11: 1,
           0x12: 1, 0x13: 1, 0x14: 1, 0x15: 0, 0x16: 0, 0x17: 0, 0x18: 0}


def units(data, start, end):
    """Yields (offset, bytes) of 0xFF-terminated units in data[start:end]."""
    i = start
    while i < end:
        j = data.find(b"\xff", i, end)
        if j < 0:
            return
        yield i, data[i:j + 1]
        i = j + 1


def skeleton(s):
    """Language-independent part of a string: the placeholders it prints
    ({PLAYER}, {STR_VAR_1}...), as a sorted tuple (a translation may reorder
    them). Line and page breaks are not part of it: a translation spreads
    its text over its own number of lines and pages."""
    out = []
    i = 0
    n = len(s)
    while i < n:
        b = s[i]
        if b == 0xFC and i + 1 < n:
            i += 2 + FC_ARGS.get(s[i + 1], 0)
        elif b == 0xFD and i + 1 < n:
            out.append(s[i + 1])
            i += 2
        else:
            i += 1
    return tuple(sorted(out))


def pages(s):
    """Number of page / scroll breaks (soft similarity only)."""
    return sum(1 for b in s if b in (0xFA, 0xFB))


C_DEF = re.compile(r"^(?:static\s+)?(?:const\s+)?u8\s+(\w+)\s*\[\s*\]\s*=\s*\{\s*([^}]*)\}\s*;", re.M)


def c_strings(pp_text):
    """Top-level u8 string arrays of a preprocessed C file, in order:
    [(name, bytes)]. Only arrays that are exactly one 0xFF-terminated string."""
    out = []
    for m in C_DEF.finditer(pp_text):
        try:
            vals = bytes(int(v, 0) for v in m.group(2).replace("\n", " ").split(",") if v.strip())
        except ValueError:
            continue
        if vals and vals[-1] == 0xFF:
            first = vals.index(0xFF)
            # one string, possibly followed by the French-room padding (0xFF...)
            if all(v == 0xFF for v in vals[first:]):
                out.append((m.group(1), vals[:first + 1]))
    return out
