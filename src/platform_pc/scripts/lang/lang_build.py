"""Build side of the French text (used by pp_c.py and pp_s.py).

src/platform_pc/lang/fr_bpef.tsv lists, for English strings, where their
French version is in the French ROM and how long it is (numbers only, no
game text). At build time each such string gets room for its French version
(0xFF padding after its terminator) and a registry entry
    { address of the string, French offset, French length }
in a dedicated section. At run time, with French chosen and the player's
French ROM present, src/platform_pc/lang_pc.c copies the French strings over
the English ones. Without it the game is the English one.
"""
import os
import re

C_DEF = re.compile(rb"^((?:static\s+)?(?:const\s+)?u8\s+(\w+)\s*\[\s*\]\s*=\s*\{)([^}]*)(\}\s*;)", re.M)
# "Name:" (local) or "Name: ; .global Name" (preproc's form of "Name::")
LABEL = re.compile(rb"^([A-Za-z_]\w*)::?\s*(?:;\s*\.global\s+\w+\s*)?$")
BYTES = re.compile(rb"^\s*\.byte\s+(.*)$")

_cache = {}


def load_map(path):
    """{key: (fr_offset, fr_length, en_length)}"""
    if path in _cache:
        return _cache[path]
    m = {}
    with open(path, encoding="utf-8") as f:
        for line in f:
            if line.startswith("#") or not line.strip():
                continue
            key, off, fl, el = line.rstrip("\n").split("\t")[:4]
            m[key] = (int(off, 16), int(fl), int(el))
    _cache[path] = m
    return m


def _count_bytes(init):
    return len([v for v in init.split(b",") if v.strip()])


def patch_c(text, rel, map_path, section):
    """text: preprocessed C (bytes). Returns the patched text."""
    m = load_map(map_path)
    prefix = f"c:{rel}:"
    entries = []

    def sub(mo):
        sym = mo.group(2).decode()
        e = m.get(prefix + sym)
        if e is None:
            return mo.group(0)
        off, fl, el = e
        n = _count_bytes(mo.group(3))
        if n != el:
            return mo.group(0)        # not the string the map was made from
        pad = fl - n
        if pad > 0:
            if re.search(rb"sizeof\s*\(\s*" + re.escape(mo.group(2)) + rb"\s*\)", text):
                return mo.group(0)    # its size is used: keep it, stays English
            init = mo.group(3).rstrip() + b", " + b", ".join([b"0xFF"] * pad) + b" "
        else:
            init = mo.group(3)
        entries.append((sym, off, fl))
        return mo.group(1) + init + mo.group(4)

    text = C_DEF.sub(sub, text)
    if entries:
        out = [b"\n/* French text registry (scripts/lang/lang_build.py) */",
               b"struct PemLangText_ { const unsigned char *text; unsigned int frOffset, frLength; };",
               b'static const struct PemLangText_ sPemLangTexts_[] __attribute__((section("%s"), used, aligned(8))) = {'
               % section.encode()]
        for sym, off, fl in entries:
            out.append(b"    { %s, 0x%06X, %d }," % (sym.encode(), off, fl))
        out.append(b"};\n")
        text += b"\n".join(out)
    return text


def patch_s(text, map_path, section_directive):
    """text: preprocessed data .s (bytes). Pads labeled texts of the map and
    appends their registry."""
    m = load_map(map_path)
    lines = text.split(b"\n")
    out, entries = [], []
    label, count = None, 0
    for line in lines:
        out.append(line)
        lm = LABEL.match(line)
        if lm:
            label, count = lm.group(1).decode(), 0
            continue
        if label is None:
            continue
        bm = BYTES.match(line)
        if not bm:
            label = None
            continue
        vals = [v.strip() for v in bm.group(1).split(b",") if v.strip()]
        count += len(vals)
        try:
            last = int(vals[-1], 0) & 0xFF if vals else None
        except ValueError:
            label = None
            continue
        if last == 0xFF or any(_is_ff(v) for v in vals):
            e = m.get("s:" + label)
            if e is not None and any(_is_ff(v) for v in vals[:-1]) is False and e[2] == count:
                off, fl, el = e
                if fl > el:
                    out.append(b"\t.fill %d, 1, 0xFF" % (fl - el))
                entries.append((label, off, fl))
            label = None
    if entries:
        out.append(section_directive.encode())
        out.append(b"\t.balign 8")
        for label, off, fl in entries:
            out.append(b"\t.8byte %s" % label.encode())
            out.append(b"\t.4byte 0x%06X, %d" % (off, fl))
    return b"\n".join(out)


def _is_ff(v):
    try:
        return int(v, 0) & 0xFF == 0xFF
    except ValueError:
        return False
