#!/usr/bin/env python3
"""Builds the English -> French text map from the player's French ROM.

    python src/platform_pc/scripts/lang/build_fr_map.py ROM.gba [build-dir] [out.tsv]

ROM.gba: the player's French Ruby (AXVF) or Sapphire (AXPF), revision 1;
build-dir: the matching English build (build-pc for Ruby, build-sapphire
for Sapphire: its pp/ folder holds the preprocessed sources); out.tsv:
default src/platform_pc/lang/fr_<code>.tsv (fr_axvf.tsv, fr_axpf.tsv).

Output (no game text, only positions): one line per English string symbol
    key  fr_offset  fr_length  en_length  how
key = "s:<label>" (data .s) or "c:<file>:<symbol>" (C array).
how = anchor | dp | gap (how the pair was found, for review).

Method: the French ROM was built from the same sources in the same order,
only the strings differ. English strings are listed in link order
(ld_script.txt: script_data, then .rodata object by object). French
candidates are the 0xFF-terminated units that some ROM pointer points to
(every string is reached through a pointer: tables, scripts, literal pools).
Pairs with a rare, identical control-code skeleton are anchors (longest
increasing chain); between anchors a Needleman-Wunsch alignment pairs the
rest; equal-sized leftovers between two pairs are paired in order.
"""
import bisect
import hashlib
import os
import re
import struct
import sys
from collections import defaultdict

sys.path.insert(0, os.path.dirname(__file__))
from textlib import skeleton, c_strings, FC_ARGS  # noqa: E402
from align import align, match_score         # noqa: E402

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", "..", ".."))
# The French ROMs the maps are made for (game code -> sha1, name).
ROMS = {
    "AXVF": ("ba888dfba231a231cbd60fe228e894b54fb1ed79", "Pokemon - Version Rubis (France) rev 1"),
    "AXPF": ("860e93f5ea44f4278132f6c1ee5650d07b852fd8", "Pokemon - Version Saphir (France) rev 1"),
}

# C tables of structs with a known French base (find_fr_tables.py TABLES):
# (table, {pointer field: offset in the GBA struct}, GBA stride).
KNOWN_TABLES = [
    ("gPokedexEntries", {"descriptionPage1": 0x10, "descriptionPage2": 0x14}, 36),
]

# "Name:" (local) or "Name: ; .global Name" (preproc's form of "Name::")
LABEL = re.compile(r"^([A-Za-z_]\w*)::?\s*(?:;\s*\.global\s+\w+\s*)?$")
BYTES = re.compile(r"^\s*\.byte\s+(.*)$")


def s_strings(path):
    """Labeled text units of a preprocessed data .s file, in order."""
    out = []
    label, buf = None, None
    for line in open(path, encoding="utf-8", errors="replace"):
        m = LABEL.match(line)
        if m:
            label, buf = m.group(1), bytearray()
            continue
        if label is None:
            continue
        b = BYTES.match(line)
        if not b:
            label = None          # something else than text follows the label
            continue
        try:
            buf += bytes(int(v, 0) & 0xFF for v in b.group(1).split(",") if v.strip())
        except ValueError:
            label = None
            continue
        if 0xFF in buf:
            if buf.index(0xFF) == len(buf) - 1:
                out.append((label, bytes(buf)))
            label = None
    return out


def link_order():
    """[(kind, path)] objects with text, in ROM order."""
    ld = open(os.path.join(ROOT, "ld_script.txt"), encoding="utf-8").read()
    objs = []
    for m in re.finditer(r"^\s*(src|data)/([\w/]+)\.o\((script_data|\.rodata)\)", ld, re.M):
        objs.append((m.group(1), m.group(2)))
    return objs


def english(build):
    pp = os.path.join(build, "pp")
    seq, seen = [], set()
    for d, name in link_order():
        flat = f"{d}__{name.replace('/', '__')}"
        if d == "data" and os.path.exists(os.path.join(pp, flat + ".s")):
            for label, b in s_strings(os.path.join(pp, flat + ".s")):
                key = "s:" + label
                if key not in seen:
                    seen.add(key)
                    seq.append((key, b))
        elif d == "src" and os.path.exists(os.path.join(pp, flat + ".c")):
            text = open(os.path.join(pp, flat + ".c"), encoding="utf-8", errors="replace").read()
            for sym, b in c_strings(text):
                key = f"c:{d}/{name}.c:{sym}"
                if key not in seen:
                    seen.add(key)
                    seq.append((key, b))
    return seq


def french_units(rom):
    """Pointed-to 0xFF-terminated units: [(offset, bytes)] sorted."""
    # Pointers at ANY byte offset: script bytecode (msgbox...) is not aligned.
    targets = set()
    p = rom.find(b"\x08", 3)
    while p >= 0:
        w = rom[p - 3] | rom[p - 2] << 8 | rom[p - 1] << 16
        if w < len(rom):
            targets.add(w)
        p = rom.find(b"\x08", p + 1)
    out = []
    for off in sorted(targets):
        j = rom.find(b"\xff", off, off + 1200)
        if j < 0:
            continue
        b = rom[off:j + 1]
        if looks_like_text(b):
            out.append((off, b))
    return out


_TEXT_BYTES = None


FRENCH_EXTRA = "ÀÁÂÇÈÉÊËÌÎÏÒÓÔŒÙÚÛàáâçèéêëìîïòóôœùúûºª&+=;%()¿¡'’“”«»…·:!?.,-/"


def set_text_bytes(en):
    """Characters a French string may use: those of the English strings
    plus accented letters. Script bytecode next to a text uses many others
    (katakana range, 0x20-0x9F...), which is how it is told apart."""
    global _TEXT_BYTES
    chars = set()
    for _, b in en:
        i = 0
        while i < len(b) - 1:
            c = b[i]
            if c == 0xFC:
                i += 2 + FC_ARGS.get(b[i + 1], 0)
                continue
            if c == 0xFD:
                i += 2
                continue
            chars.add(c)
            i += 1
    for line in open(os.path.join(ROOT, "charmap.txt"), encoding="utf-8"):
        m = re.match(r"^'(.)'\s*=\s*([0-9A-Fa-f]{2})\s*$", line.strip())
        if m and m.group(1) in FRENCH_EXTRA:
            chars.add(int(m.group(2), 16))
    _TEXT_BYTES = chars


def looks_like_text(b):
    """Every byte is a text character or a valid control code."""
    i, n, letters = 0, len(b) - 1, 0
    while i < n:
        c = b[i]
        if c == 0xFC:
            if i + 1 >= n:
                return False
            i += 2 + FC_ARGS.get(b[i + 1], 0)
            continue
        if c == 0xFD:
            i += 2
            continue
        if c in (0xFA, 0xFB, 0xFE):
            i += 1
            continue
        if c not in _TEXT_BYTES:
            return False
        if 0xA1 <= c <= 0xEE:
            letters += 1
        i += 1
    return n == 0 or letters > 0 or n <= 2


def anchors(en, fr, fixed=()):
    """Longest increasing chain of sure pairs (fixed: exact script pairs) and
    rare pairs: identical bytes (names...) or identical placeholders."""
    by_sk = defaultdict(list)
    by_bytes = defaultdict(list)
    for j, (_, b) in enumerate(fr):
        by_sk[skeleton(b)].append(j)
        by_bytes[b].append(j)
    cand = list(fixed)
    fixed_i = {i for i, _ in fixed}
    for i, (_, b) in enumerate(en):
        if i in fixed_i:
            continue
        same = by_bytes.get(b, [])
        if len(b) >= 5 and 1 <= len(same) <= 2:
            cand.extend((i, j) for j in same)
            continue
        sk = skeleton(b)
        if len(sk) < 1:                  # no placeholder: too common to anchor on
            continue
        js = by_sk.get(sk, [])
        if not js or len(js) > 12:
            continue
        for j in js:
            if match_score(b, fr[j][1]) is not None:
                cand.append((i, j))
    # LIS on j over pairs sorted by i (ties: larger j first so one per i)
    cand.sort(key=lambda p: (p[0], -p[1]))
    tails, tails_idx, prev = [], [], [None] * len(cand)
    for k, (i, j) in enumerate(cand):
        pos = bisect.bisect_left(tails, j)
        if pos == len(tails):
            tails.append(j)
            tails_idx.append(k)
        else:
            tails[pos] = j
            tails_idx[pos] = k
        prev[k] = tails_idx[pos - 1] if pos else None
    chain, k = [], tails_idx[-1] if tails_idx else None
    while k is not None:
        chain.append(cand[k])
        k = prev[k]
    chain.reverse()
    # strictly increasing in i too
    out, last_i = [], -1
    for i, j in chain:
        if i > last_i:
            out.append((i, j))
            last_i = i
    return out


def main():
    rom_path = sys.argv[1]
    build = os.path.abspath(sys.argv[2] if len(sys.argv) > 2 else os.path.join(ROOT, "build-pc"))
    rom = open(rom_path, "rb").read()
    code = rom[0xAC:0xB0].decode("ascii", "replace")
    if code not in ROMS:
        sys.exit(f"{rom_path}: game code {code}, not a French Ruby (AXVF) or Sapphire (AXPF)")
    want_sha, rom_name = ROMS[code]
    out_path = sys.argv[3] if len(sys.argv) > 3 else os.path.join(
        ROOT, "src", "platform_pc", "lang", f"fr_{code.lower()}.tsv")
    sha = hashlib.sha1(rom).hexdigest()
    if sha != want_sha:
        print(f"warning: ROM sha1 {sha}, the map is made for {want_sha} ({rom_name})")

    en = english(build)
    set_text_bytes(en)
    fr = french_units(rom)
    print(f"english strings: {len(en)}, french pointed units: {len(fr)}", flush=True)

    # 1. exact pairs: walk the event scripts next to the French ones
    import script_walk
    pp_s = os.path.join(build, "pp", "data__event_scripts.s")
    version = "SAPPHIRE" if code == "AXPF" else "RUBY"
    defsyms = [f"{version}=1", "REVISION=0", "ENGLISH=1", "DEBUG=0", "DEBUG_FIX=0", "MODERN=1", "PLATFORM_PC=1"]
    obj = script_walk.assemble_all_global(pp_s, ROOT, os.path.join(build, "lang_event_scripts.obj"), defsyms)
    data, labels, relocs = script_walk.read_object(obj, "script_data")
    texts = dict(s_strings(pp_s))
    fr_start = find_fr_scripts(data, labels, relocs, texts, rom)
    ptr_of, pos_of, stats = script_walk.walk(data, labels, relocs, texts, rom, fr_start)
    print(f"script walk from 0x{fr_start:X}: {stats}", flush=True)
    exact = {}
    for label, b in texts.items():
        off = ptr_of.get(label, pos_of.get(label))
        if off is None:
            continue
        j = rom.find(b"\xff", off, off + 2000)
        fb = rom[off:j + 1] if j >= 0 else b""
        if fb and skeleton(fb) == skeleton(b) and 0.3 <= len(fb) / len(b) <= 3.5:
            exact["s:" + label] = (off, fb, "ptr" if label in ptr_of else "pos")

    # French unit list = pointed text units + every exact address
    fr_off = {o for o, _ in fr} | {o for o, _, _ in exact.values()}
    fr = [(o, rom[o:rom.find(b"\xff", o) + 1]) for o in sorted(fr_off)]
    index_of = {o: j for j, (o, _) in enumerate(fr)}

    pairs, how = {}, {}
    for i, (key, _) in enumerate(en):
        if key in exact:
            pairs[i] = index_of[exact[key][0]]
            how[i] = exact[key][2]
    # 2. monotonic subset of exact pairs + rare identical strings = anchors
    anc = anchors(en, fr, fixed=sorted(pairs.items()))
    print(f"exact: {len(pairs)}, anchors: {len(anc)}", flush=True)
    for i, j in anc:
        if i not in pairs:
            pairs[i] = j
            how[i] = "anchor"
    # 3. exact alignment between consecutive anchors
    bounds = [(-1, -1)] + anc + [(len(en), len(fr))]
    for (i0, j0), (i1, j1) in zip(bounds, bounds[1:]):
        ei = [i for i in range(i0 + 1, i1) if i not in pairs]
        fj = list(range(j0 + 1, j1))
        if not ei or not fj or len(ei) * len(fj) > 400_000:
            continue
        seg = align([en[i][1] for i in ei], [fr[j][1] for j in fj])
        for a, b in seg:
            pairs[ei[a]] = fj[b]
            how[ei[a]] = "dp"

    # 4. leftovers between consecutive pairs, same count, same placeholders
    keys = sorted(pairs)
    for a, b in zip([-1] + keys, keys + [len(en)]):
        ja = pairs[a] if a >= 0 else -1
        jb = pairs[b] if b < len(en) else len(fr)
        ei = list(range(a + 1, b))
        fj = list(range(ja + 1, jb))
        if ei and 0 < jb - ja - 1 == len(ei):
            for i, j in zip(ei, fj):
                r = len(fr[j][1]) / max(1, len(en[i][1]))
                if skeleton(fr[j][1]) == skeleton(en[i][1]) and 0.3 <= r <= 3.5:
                    pairs[i] = j
                    how[i] = "gap"

    # 5. C tables of pointers: the French table, found from the strings
    # already placed, gives the exact address of every string it lists
    import glob
    import ptrtables
    key_of, dup = {}, set()
    for i, (key, _) in enumerate(en):
        sym = key.split(":")[-1]
        if sym in key_of:
            dup.add(sym)
        key_of[sym] = i
    for sym in dup:
        key_of.pop(sym, None)
    known = {en[i][0].split(":")[-1]: fr[j][0] for i, j in pairs.items()}
    cache, fixed, tp = {}, 0, None
    # Tables found by name (find_fr_tables.py): every entry's pointer field
    # gives the exact French address (e.g. the Pokedex descriptions, none of
    # which the alignment can place: no anchor among them).
    import find_fr_tables
    known_tables = {}
    for table, field_offsets, stride in KNOWN_TABLES:
        base = find_fr_tables.find_base(rom, table)
        if base is not None:
            for field, off in field_offsets.items():
                known_tables[(table, field)] = (base + off, stride)
            print(f"known table {table}: French base 0x{base:06X}", flush=True)
    for path in sorted(glob.glob(os.path.join(build, "pp", "src__*.c"))):
        text = open(path, "rb").read()
        for table, field, syms in ptrtables.table_lists(text):
            got = ptrtables.resolve(rom, syms, known, cache)
            if (table, field) in known_tables:
                start, stride = known_tables[(table, field)]
                got = {}
                for k in range(len(syms)):
                    w = struct.unpack_from("<I", rom, start + k * stride)[0]
                    if 0x08000000 <= w < 0x08000000 + len(rom):
                        got[k] = w - 0x08000000
            if not got and not field and len(syms) >= 4 and all(s in key_of for s in syms):
                # plain array of strings whose entries are not placed reliably:
                # find the French one by its shape (pointers to texts that match)
                if tp is None:
                    tp = ptrtables.TextPointers(rom, looks_like_text)
                got = tp.find_table([en[key_of[s]][1] for s in syms], match_score)
            for k, off in got.items():
                i = key_of.get(syms[k])
                if i is None:
                    continue
                j = rom.find(b"\xff", off, off + 2000)
                fb = rom[off:j + 1] if j >= 0 else b""
                eb = en[i][1]
                if not fb or not looks_like_text(fb) or skeleton(fb) != skeleton(eb):
                    continue
                if not 0.3 <= len(fb) / len(eb) <= 3.5 and len(eb) > 3:
                    continue
                if i in pairs and fr[pairs[i]][0] == off:
                    if how[i] in ("dp", "anchor", "gap"):
                        how[i] = "table"
                    continue
                fr.append((off, fb))
                pairs[i] = len(fr) - 1
                how[i] = "table"
                fixed += 1
    print(f"pointer tables: {fixed} strings placed or corrected", flush=True)

    # 6. a wrong French text is worse than an English one: pairs found only by
    # alignment are kept in strings.c (checked: in order, ~90% right) or when
    # framed by two sure pairs in the same order on both sides
    sure = ("ptr", "pos", "table", "anchor")
    # Reviewed by hand (PRS_LANG_REVIEW dump, French Ruby rev 1): in
    # src/strings.c the aligned sentences are right; short labels sometimes
    # shift by one (menus listed in another order). Kept: pairs with the same
    # placeholders, >= 10 bytes, similar length; and the short labels below,
    # checked one by one (symbol names only, no game text).
    reviewed_ok = {
        "c:src/strings.c:" + k for k in (
            "gExpandedPlaceholder_Ruby", "gExpandedPlaceholder_Sapphire", "gExpandedPlaceholder_Brendan",
            "gExpandedPlaceholder_May", "gText_EggNickname", "gText_Pokemon",
            "gMainMenuString_NewGame", "gMainMenuString_Continue", "gMainMenuString_Option",
            "gMainMenuString_MysteryEvents", "gMainMenuString_Player", "gMainMenuString_Pokedex",
            "gMainMenuString_Time", "OtherText_Read2", "gOtherText_Hp2", "gOtherText_SpAtk2",
            "gOtherText_SpDef2", "gOtherText_ReturnTo", "gContestStatsText_Spicy",
            "gContestStatsText_Dry", "gContestStatsText_Sweet")}

    def checked(i):
        key, eb = en[i]
        if key in reviewed_ok:
            return True
        if not key.startswith("c:src/strings.c:"):
            return False
        fb = fr[pairs[i]][1]
        return (skeleton(fb) == skeleton(eb) and len(eb) >= 10
                and 0.6 <= len(fb) / len(eb) <= 1.8)
    wrong = set()        # pairs seen wrong in review
    dropped = 0
    review = []          # PRS_LANG_REVIEW=file: the dropped pairs, decoded, to check by eye
    for i in sorted(pairs):
        if en[i][0] in wrong and how[i] in ("dp", "gap"):
            del pairs[i]
            dropped += 1
            continue
        if how[i] not in ("dp", "gap") or checked(i):
            continue
        o = fr[pairs[i]][0]
        a = next((k for k in range(i - 1, max(-1, i - 6), -1) if k in pairs and how[k] in sure), None)
        b = next((k for k in range(i + 1, i + 6) if k in pairs and how[k] in sure), None)
        if a is not None and b is not None and fr[pairs[a]][0] < o < fr[pairs[b]][0] \
                and fr[pairs[b]][0] - fr[pairs[a]][0] < 4096:
            continue
        review.append((en[i][0], en[i][1], fr[pairs[i]][1]))
        del pairs[i]
        dropped += 1
    print(f"unsure aligned pairs dropped (stay English): {dropped}", flush=True)
    if os.environ.get("PRS_LANG_REVIEW"):
        dec = decoder()
        with open(os.environ["PRS_LANG_REVIEW"], "w", encoding="utf-8") as f:
            for key, eb, fb in review:
                f.write(f"{key}\n  EN {dec(eb)}\n  FR {dec(fb)}\n")

    os.makedirs(os.path.dirname(out_path), exist_ok=True)
    with open(out_path, "w", encoding="utf-8", newline="\n") as f:
        f.write(f"# English -> French ({rom_name}, {code}, sha1 {want_sha})\n")
        f.write("# key\tfr_offset\tfr_length\ten_length\thow\n")
        for i in sorted(pairs):
            key, b = en[i]
            off, fb = fr[pairs[i]]
            f.write(f"{key}\t0x{off:06X}\t{len(fb)}\t{len(b)}\t{how[i]}\n")
    n = len(pairs)
    print(f"mapped {n}/{len(en)} ({100 * n / len(en):.1f}%) -> {out_path}")
    for k in ("ptr", "pos", "table", "anchor", "dp", "gap"):
        print(f"  {k}: {sum(1 for v in how.values() if v == k)}")


def decoder():
    """bytes -> readable text (charmap.txt), for reviews only."""
    table = {}
    for line in open(os.path.join(ROOT, "charmap.txt"), encoding="utf-8"):
        m = re.match(r"^'(.)'\s*=\s*([0-9A-Fa-f]{2})\s*$", line.strip())
        if m and int(m.group(2), 16) not in table:
            table[int(m.group(2), 16)] = m.group(1)
    return lambda b: "".join(table.get(c, "{%02X}" % c) for c in b[:-1])


def find_fr_scripts(data, labels, relocs, texts, rom):
    """French offset of the start of the event scripts object: the first
    clean 32-byte bytecode block, found once in the ROM, minus the
    English bytes before it (PC-widened 8-byte pointers count 4)."""
    import script_walk
    for o in sorted(labels):
        if any(n in texts for n in labels[o]) or not script_walk._clean_block(data, relocs, o, 32):
            continue
        block = data[o:o + 32]
        if len(set(block)) < 8 or rom.count(block) != 1:
            continue
        wide = sum(1 for r, (k, _) in relocs.items() if r < o and k.endswith("ADDR64"))
        return rom.find(block) - (o - 4 * wide)
    raise SystemExit("event scripts not found in the French ROM")


if __name__ == "__main__":
    main()
