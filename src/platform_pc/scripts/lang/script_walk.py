"""Exact French addresses of every label of a data object (event scripts).

The English object (assembled for the PC build, COFF/ELF) and the French ROM
hold the same bytecode: only the texts (other lengths) and the pointers
(other addresses) differ. Walking both in step:
  - a relocated 4-byte field (pointer to label L) -> the French ROM has the
    French address of L at the same place: recorded, exact;
  - the start of an English text unit -> skip to 0xFF on both sides;
  - alignment padding -> each side pads to its own alignment;
  - any other byte must be equal (a check that the walk is in step).
"""
import os
import re
import struct
import subprocess
import tempfile


def assemble_all_global(pp_s, root, out_obj, defsyms=()):
    """Assembles a copy of a preprocessed .s where every label is global, so
    that local text labels (Name:) appear in the symbol table."""
    text = open(pp_s, encoding="utf-8", errors="replace").read()
    # the English layout: without the French-room padding and registry
    # that lang_build.py adds to the build
    text = re.sub(r"^\t\.fill \d+, 1, 0xFF\n", "", text, flags=re.M)
    cut = re.search(r'^\.section (\.rdata\$prslang1,"dr"|prs_lang,"a")$', text, flags=re.M)
    if cut:
        text = text[:cut.start()]
    text = re.sub(r"^([A-Za-z_]\w*):(?!:)", r".global \1\n\1:", text, flags=re.M)
    src = out_obj + ".s"
    open(src, "w", encoding="utf-8", newline="\n").write(text)
    args = []
    for d in defsyms:          # the game's switches (ASFLAGS: --defsym SAPPHIRE=1...)
        args += ["--defsym", d]
    subprocess.run(["as"] + args + ["-I", root, "-I", os.path.join(root, "include"), "-o", out_obj, src], check=True)
    return out_obj


def read_object(obj, section):
    """(bytes, {offset: symbol}, {offset: symbol of reloc}) of one section."""
    with tempfile.TemporaryDirectory() as d:
        raw = os.path.join(d, "s.bin")
        subprocess.run(["objcopy", "-O", "binary", "--only-section=" + section, obj, raw], check=True)
        data = open(raw, "rb").read()
    syms = {}
    for line in subprocess.run(["objdump", "-t", obj], capture_output=True, text=True, check=True).stdout.splitlines():
        # COFF: [  n](sec  4)(fl 0x00)(ty    0)(scl   2) (nx 0) 0x0000000000001234 Name
        m = re.search(r"\(sec\s+(\d+)\).*?0x([0-9a-f]+)\s+(\S+)$", line)
        if m:
            syms.setdefault(int(m.group(2), 16), []).append((int(m.group(1)), m.group(3)))
            continue
        # ELF: 0000000000001234 l  .script_data 0000 Name
        m = re.match(r"^([0-9a-f]+)\s.{7}\s(\S+)\s+[0-9a-f]+\s+(\S+)$", line)
        if m and m.group(2) == section:
            syms.setdefault(int(m.group(1), 16), []).append((-1, m.group(3)))
    sec_index = None
    for line in subprocess.run(["objdump", "-h", obj], capture_output=True, text=True, check=True).stdout.splitlines():
        m = re.match(r"^\s*(\d+)\s+(\S+)\s", line)
        if m and m.group(2) == section:
            sec_index = int(m.group(1)) + 1          # COFF section numbers start at 1
    if "elf" in subprocess.run(["objdump", "-f", obj], capture_output=True, text=True).stdout:
        sec_index = -1
    labels = {}
    for off, lst in syms.items():
        for sec, name in lst:
            if sec == sec_index and not name.startswith(".") and name != section:
                labels.setdefault(off, []).append(name)
    relocs = {}
    in_sec = False
    for line in subprocess.run(["objdump", "-r", obj], capture_output=True, text=True, check=True).stdout.splitlines():
        if line.startswith("RELOCATION RECORDS FOR"):
            in_sec = ("[" + section + "]") in line
            continue
        m = re.match(r"^([0-9a-f]+)\s+(\S+)\s+(\S+)$", line)
        if in_sec and m:
            relocs[int(m.group(1), 16)] = (m.group(2), m.group(3))
    return data, labels, relocs


def _clean_block(data, relocs, e, n=12):
    """True if data[e:e+n] holds no relocated field."""
    for k in range(e - 7, e + n):
        r = relocs.get(k)
        if r is not None:
            size = 8 if r[0].endswith("ADDR64") or r[0].endswith("_64") else 4
            if k + size > e and k < e + n:
                return False
    return True


def walk(data, labels, relocs, texts, rom, fr_start, max_resync=200000, section_name="script_data"):
    """Returns (ptr_of, pos_of, stats).
    ptr_of: label -> French offset read from a French pointer (exact).
    pos_of: label -> French offset where the walk found it (when in step).
    texts: label -> English text bytes."""
    import bisect
    from textlib import skeleton
    ptr_of, pos_of = {}, {}
    e, f = 0, fr_start
    label_offsets = sorted(labels)
    stats = {"checked": 0, "resync": 0, "lost": 0}
    while e < len(data):
        names = labels.get(e, ())
        tname = next((n for n in names if n in texts), None)
        if tname is not None:
            en = texts[tname]
            j = rom.find(b"\xff", f, f + 2000)
            fr = rom[f:j + 1] if j >= 0 else b""
            if fr and skeleton(fr) == skeleton(en) and 0.3 <= len(fr) / len(en) <= 3.5:
                for name in names:
                    pos_of.setdefault(name, f)
                e += len(en)
                f = j + 1
                continue
            # the French text here is another one: resync below
        else:
            for name in names:
                pos_of.setdefault(name, f)
            r = relocs.get(e)
            if r is not None:
                kind, sym = r
                w = struct.unpack_from("<I", rom, f)[0]
                if 0x08000000 <= w < 0x0A000000:
                    if sym == section_name:
                        # COFF: a reference inside the section is "section +
                        # offset", the offset being stored in the field
                        wide = kind.endswith("ADDR64") or kind.endswith("_64")
                        tgt = struct.unpack_from("<Q" if wide else "<I", data, e)[0]
                        for n in labels.get(tgt, ()):
                            ptr_of.setdefault(n, w - 0x08000000)
                    elif not sym.startswith("."):
                        ptr_of.setdefault(sym, w - 0x08000000)
                e += 8 if kind.endswith("ADDR64") or kind.endswith("_64") else 4   # PC-widened tables
                f += 4
                continue
            if data[e] == rom[f]:
                e += 1
                f += 1
                stats["checked"] += 1
                continue
            k = bisect.bisect_right(label_offsets, e)
            nxt = label_offsets[k] if k < len(label_offsets) else len(data)
            if data[e:nxt] == bytes(nxt - e) and nxt - e < 4:     # English padding
                align = 4 if nxt % 4 == 0 else (2 if nxt % 2 == 0 else 1)
                e = nxt
                while f % align:
                    f += 1
                continue
            if rom[f] == 0 and f % 4:                            # French padding
                while f % 4 and rom[f] == 0:
                    f += 1
                continue
        # out of step: find the next clean English bytecode block a bit
        # further in the French ROM (texts placed elsewhere, other lengths)
        if stats["resync"] >= max_resync:
            break
        stats["resync"] += 1
        found = False
        k = bisect.bisect_right(label_offsets, e)
        for tries in range(40):
            if k >= len(label_offsets):
                break
            le = label_offsets[k]
            k += 1
            if any(n in texts for n in labels[le]) or not _clean_block(data, relocs, le):
                continue
            if not _clean_block(data, relocs, le, 16):
                continue
            block = data[le:le + 16]
            if len(set(block)) < 5:
                continue
            pos = rom.find(block, f - 64, f + 0x200000)
            if pos >= 0:
                e, f = le, pos
                found = True
                break
        if not found:
            stats["lost"] += 1
            # skip this label entirely and try from the next one
            k = bisect.bisect_right(label_offsets, e)
            if k >= len(label_offsets):
                break
            e = label_offsets[k]
    return ptr_of, pos_of, stats
