#!/usr/bin/env python3
"""Finds the fixed-size name tables in the French ROM.

    python src/platform_pc/scripts/lang/find_fr_tables.py ROM.gba

Names stored inside tables (gSpeciesNames[][11], gItems[].name...) are not
separate strings: lang_tables_pc.c copies them table by table. For each
table, the English names (from the sources, in order) vote: a name that is
the same in French at index k found at offset p gives the table base
p - k * stride - field. Prints the C table for lang_tables_pc.c.
"""
import collections
import os
import re
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", "..", ".."))

# symbol, source file, regex of the names in order, GBA stride, field offset, field size
TABLES = [
    ("gSpeciesNames", "src/data/text/species_names_en.h", r'\]\s*=\s*_\("([^"]*)"\)', 11, 0, 11),
    ("gMoveNames", "src/data/text/move_names_en.h", r'\]\s*=\s*_\("([^"]*)"\)', 13, 0, 13),
    ("gAbilityNames", "data/text/ability_names.inc", r'\.string\s+"([^"$]*)\$?"\s*,\s*13', 13, 0, 13),
    ("gTypeNames", "data/text/type_names.inc", r'\.string\s+"([^"$]*)\$?"\s*,\s*7', 7, 0, 7),
    ("gTrainerClassNames", "src/data/text/trainer_class_names_en.h", r'_\("([^"]*)"\)', 13, 0, 13),
    ("gItems", "src/data/items_en.h", r'\.name\s*=\s*_\("([^"]*)"\)', 44, 0, 14),
    ("gTrainers", "src/data/trainers_en.h", r'\.trainerName\s*=\s*_\("([^"]*)"\)', 40, 4, 12),
    ("gDecorations", "src/decoration.c", r'\{DECOR_\w+,\s*_\("([^"]*)"\)', 32, 1, 16),
    ("gBerries", "src/berry.c", r'#define NAME_\w+_BERRY\s+_\("([^"]*)"\)', 28, 0, 7),
    ("gPokedexEntries", "src/data/pokedex_entries_en.h", r'\.categoryName\s*=\s*_\("([^"]*)"\)', 36, 0, 12),
]


def encoder():
    enc = {}
    for line in open(os.path.join(ROOT, "charmap.txt"), encoding="utf-8"):
        m = re.match(r"^'(.)'\s*=\s*([0-9A-Fa-f]{2})\s*$", line.strip())
        if m and m.group(1) not in enc:
            enc[m.group(1)] = int(m.group(2), 16)
    multi = {"{PKMN}": None}

    def e(s):
        out = bytearray()
        for c in s:
            if c not in enc:
                return None
            out.append(enc[c])
        return bytes(out) + b"\xff"
    return e


def find_base(rom, sym):
    """French ROM offset of table `sym` (one of TABLES), or None."""
    enc = encoder()
    for name, src, rx, stride, field, size in TABLES:
        if name != sym:
            continue
        text = open(os.path.join(ROOT, src), encoding="utf-8").read()
        if sym == "gBerries":
            text = text[:text.index("#elif defined(GERMAN)")]
        votes = collections.Counter()
        for k, n in enumerate(re.findall(rx, text)):
            b = enc(n)
            if b is None or len(b) < 5:
                continue
            for m in re.finditer(re.escape(b), rom):
                if m.start() - k * stride - field >= 0:
                    votes[m.start() - k * stride - field] += 1
        return votes.most_common(1)[0][0] if votes else None
    return None


def main():
    rom = open(sys.argv[1], "rb").read()
    enc = encoder()
    rows = []
    for sym, src, rx, stride, field, size in TABLES:
        text = open(os.path.join(ROOT, src), encoding="utf-8").read()
        if sym == "gBerries":
            text = text[:text.index("#elif defined(GERMAN)")]   # the English names only
        names = re.findall(rx, text)
        votes = collections.Counter()
        for k, n in enumerate(names):
            b = enc(n)
            if b is None or len(b) < 5:
                continue
            for m in re.finditer(re.escape(b), rom):
                base = m.start() - k * stride - field
                if base >= 0:
                    votes[base] += 1
        if not votes:
            print(f"{sym}: no vote", file=sys.stderr)
            continue
        base, v = votes.most_common(1)[0]
        second = votes.most_common(2)[1][1] if len(votes) > 1 else 0
        # sanity: every French entry ends with 0xFF inside its field
        ok = sum(1 for k in range(len(names))
                 if 0xFF in rom[base + k * stride + field: base + k * stride + field + size])
        print(f"{sym}: base 0x{base:06X} votes {v} (next {second}), {ok}/{len(names)} entries end in field",
              file=sys.stderr)
        rows.append((sym, base, stride, field, size, len(names)))
    for sym, base, stride, field, size, n in rows:
        print(f"    {{ \"{sym}\", 0x{base:06X}, {stride}, {field}, {size}, {n} }},")


if __name__ == "__main__":
    main()
