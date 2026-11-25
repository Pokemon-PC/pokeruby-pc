#!/usr/bin/env python3
"""Finds the structs pokeruby places at fixed EWRAM addresses that grow on PC.

include/ewram.h lays game structures over gSharedMem at fixed offsets
(gSharedMem + 0x17110, ...), sized for the GBA. A struct holding pointers is
larger on a 64-bit host and runs into the next area (e.g. the battle
scripts stack over the battle callbacks stack).

For every struct type used in ewram.h, the size is computed twice with
Clang: for the GBA (ARM, APCS: what agbcc lays out) and for the host. Types
that grow are listed with every offset they are placed at and the room up
to the next offset in ewram.h.

Usage: find_ewram_overlaps.py [--clang CLANG] [--as-pc]
  --as-pc: also list the types that keep their size (sanity check).
Exit status 1 if a grown type has less room than its PC size at some offset
and is not relocated under PLATFORM_PC.
"""
import argparse
import glob
import os
import re
import subprocess
import sys
import tempfile

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))
DEF = re.compile(r'^\s*#\s*define\s+(\w+)\s+(.*)$')
TYPED = re.compile(r'\((?:const\s+)?(struct|union)\s+(\w+)\s*\*\)\s*\(?\s*gSharedMem\s*(?:\+\s*(0x[0-9A-Fa-f]+|\d+))?')
PLAIN = re.compile(r'gSharedMem\s*\+\s*(0x[0-9A-Fa-f]+|\d+)|gSharedMem\s*\[\s*(0x[0-9A-Fa-f]+|\d+)\s*\]')


def ewram_entries(text):
    """[(name, kind, type, offset)] for the GBA view of ewram.h (PLATFORM_PC
    branches skipped)."""
    out, depth_pc = [], []
    for line in text.splitlines():
        s = line.strip()
        if re.match(r'#\s*ifdef\s+PLATFORM_PC\b', s):
            depth_pc.append(True)
            continue
        if re.match(r'#\s*if', s):
            depth_pc.append(False)
            continue
        if re.match(r'#\s*else\b', s) and depth_pc and depth_pc[-1]:
            depth_pc[-1] = "else"
            continue
        if re.match(r'#\s*endif\b', s) and depth_pc:
            depth_pc.pop()
            continue
        if any(d is True for d in depth_pc):
            continue
        m = DEF.match(line)
        if not m:
            continue
        name, body = m.groups()
        t = TYPED.search(body)
        if t:
            out.append((name, t.group(1), t.group(2), int(t.group(3) or "0", 0)))
            continue
        p = PLAIN.search(body)
        if p:
            out.append((name, None, None, int(p.group(1) or p.group(2), 0)))
    return out


def header_for(kind, tname):
    pat = re.compile(r'^\s*%s\s+%s\b[^;]*$' % (kind, tname), re.M)
    for h in sorted(glob.glob(os.path.join(ROOT, "include", "**", "*.h"), recursive=True)):
        try:
            if pat.search(open(h, encoding="utf-8", errors="replace").read()):
                return os.path.relpath(h, os.path.join(ROOT, "include")).replace("\\", "/")
        except OSError:
            pass
    return None


def sizes(clang, types, headers, arm):
    with tempfile.TemporaryDirectory() as tmp:
        src = os.path.join(tmp, "sizes.c")
        with open(src, "w") as f:
            f.write('#include "global.h"\n')
            for h in headers:
                f.write('#include "%s"\n' % h)
            for i, (kind, t) in enumerate(types):
                f.write("const unsigned long long prs_sz_%d = sizeof(%s %s);\n" % (i, kind, t))
        args = [clang, "-S", "-o", "-", "-w", "-iquote", os.path.join(ROOT, "include"),
                "-DRUBY", "-DENGLISH", "-DREVISION=0", "-DDEBUG=0", "-DMODERN=1", src]
        if arm:
            stubs = os.path.join(tmp, "stubs")
            os.makedirs(stubs)
            for h in ("string.h", "stdlib.h"):
                open(os.path.join(stubs, h), "w").write("#include <stddef.h>\n")
            args += ["--target=armv4t-none-eabi", "-mabi=apcs-gnu", "-ffreestanding", "-isystem", stubs]
        else:
            args += ["-DPLATFORM_PC=1", "-fms-extensions", "-iquote", os.path.join(ROOT, "src", "platform_pc")]
        r = subprocess.run(args, capture_output=True, text=True)
        if r.returncode != 0:
            sys.exit(r.stderr[-3000:])
        vals = {}
        cur = None
        for line in r.stdout.splitlines():
            m = re.match(r'^(prs_sz_\d+):', line)
            if m:
                cur = m.group(1)
                continue
            m = re.match(r'^\s*\.(quad|long|xword|word)\s+(\d+)', line)
            if m and cur:
                vals[int(cur.split("_")[-1])] = int(m.group(2))
                cur = None
        return vals


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--clang", default="clang")
    ap.add_argument("--as-pc", action="store_true")
    a = ap.parse_args()

    text = open(os.path.join(ROOT, "include", "ewram.h"), encoding="utf-8").read()
    gba_entries = ewram_entries(text)
    # Names given another definition in an #ifdef PLATFORM_PC branch.
    relocated = set()
    for block in re.findall(r'#\s*ifdef\s+PLATFORM_PC\b(.*?)#\s*(?:else|endif)\b', text, re.S):
        relocated |= set(re.findall(r'#\s*define\s+(\w+)', block))
    found = {(k, t): header_for(k, t) for _, k, t, _ in gba_entries if t}
    for (k, t), h in sorted(found.items()):
        if not h:
            print(f"skip      {k} {t}: defined in a .c file (check by hand)")
    types = sorted(kt for kt, h in found.items() if h)
    headers = sorted({h for h in found.values() if h})
    gba = sizes(a.clang, types, headers, arm=True)
    pc = sizes(a.clang, types, headers, arm=False)
    offsets = sorted({o for _, _, _, o in gba_entries})

    bad = 0
    for i, (kind, t) in enumerate(types):
        g, p = gba.get(i), pc.get(i)
        if g is None or p is None:
            print(f"?? {kind} {t}: size not found")
            continue
        if p == g and not a.as_pc:
            continue
        for name, k, tt, off in gba_entries:
            if (k, tt) != (kind, t):
                continue
            # Room: up to the first area that starts at or after the end of
            # the GBA struct (big structs overlay smaller areas by design:
            # those are used by other screens, never at the same time).
            nxt = next((o for o in offsets if o >= off + g), 0x20000)
            room = nxt - off
            status = "ok" if p <= room else ("relocated" if name in relocated else "OVERLAP")
            if status == "OVERLAP":
                bad += 1
            print(f"{status:9} {name:40} {kind} {t}: GBA {g:#x}, PC {p:#x}, at +{off:#x}, room {room:#x}")
    print(f"{len(types)} struct types in ewram.h, {bad} overlap(s) on PC")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
