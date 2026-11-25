#!/usr/bin/env python3
"""Generate every asset the game includes (graphics, maps, sound samples...)
with the original GBA make rules, but without devkitARM/agbcc.

1. `make generated` (map/json headers) - tools must already be built
   (cmake -S src/platform_pc/tools -B build-tools && cmake --build build-tools).
2. scaninc lists, for every C and asm source, the files it INCBIN's/.incbin's.
3. Missing ones are built by `make <targets>` (pattern rules of Makefile and
   its *.mk files), plus the map data (mapjson) and the songs (mid2agb).

Usage: python src/platform_pc/scripts/gen_assets.py [-j N] [--make mingw32-make]
"""
import argparse
import glob
import os
import shutil
import subprocess
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))
EXE = ".exe" if os.name == "nt" else ""
GEN_EXT = (".1bpp", ".4bpp", ".8bpp", ".gbapal", ".lz", ".rl", ".bin", ".fwjpnfont",
           ".hwjpnfont", ".latfont")


def run(cmd, **kw):
    return subprocess.run(cmd, cwd=ROOT, check=True, **kw)


def find_make(user):
    for cand in ([user] if user else []) + ["make", "mingw32-make", "gmake"]:
        if cand and shutil.which(cand):
            return cand
    sys.exit("make introuvable (make, mingw32-make ou gmake)")


def scan_deps(scaninc, src, includes):
    args = [scaninc] + sum((["-I", i] for i in includes), []) + [src]
    out = subprocess.run(args, cwd=ROOT, capture_output=True, text=True)
    if out.returncode != 0:
        sys.stderr.write(f"scaninc {src}: {out.stderr}")
        return []
    return out.stdout.split()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("-j", type=int, default=os.cpu_count() or 4)
    ap.add_argument("--make")
    ap.add_argument("--list", action="store_true", help="only print missing targets")
    a = ap.parse_args()

    make = find_make(a.make)
    scaninc = os.path.join("tools", "scaninc", "scaninc" + EXE)
    if not os.path.exists(os.path.join(ROOT, scaninc)):
        sys.exit("outils non compilés : cmake -S src/platform_pc/tools -B build-tools && cmake --build build-tools")

    if not a.list:
        print("== make generated")
        run([make, "generated", f"-j{a.j}"])

    srcs = []
    for pat in ("src/*.c", "src/*/*.c", "src/*/*/*.c", "src/*.s", "src/*/*.s",
                "data/*.s", "sound/*.s"):
        srcs += glob.glob(pat, root_dir=ROOT)
    srcs = [s.replace("\\", "/") for s in srcs if "platform_pc" not in s]

    deps = set()
    for s in srcs:
        inc = ["include"] if s.endswith(".c") else ["", "include"]
        for d in scan_deps(scaninc, s, inc):
            deps.add(d.replace("\\", "/"))

    missing = sorted(d for d in deps
                     if d.endswith(GEN_EXT) and not os.path.exists(os.path.join(ROOT, d)))
    # Map data (mapjson): dependencies of data/maps.s and data/map_events.s.
    map_targets = ["data/maps/connections.inc", "data/maps/groups.inc", "data/maps/events.inc",
                   "data/maps/headers.inc", "data/layouts/layouts.inc", "data/layouts/layouts_table.inc"]
    for mj in glob.glob("data/maps/*/map.json", root_dir=ROOT):
        d = os.path.dirname(mj).replace("\\", "/")
        map_targets += [f"{d}/header.inc", f"{d}/events.inc", f"{d}/connections.inc"]
    missing += [t for t in map_targets if not os.path.exists(os.path.join(ROOT, t))]
    # Songs: sound/songs/midi/*.mid -> *.s (mid2agb), objects of their own.
    for mid in glob.glob("sound/songs/midi/*.mid", root_dir=ROOT):
        s = mid.replace("\\", "/")[:-4] + ".s"
        if not os.path.exists(os.path.join(ROOT, s)):
            missing.append(s)
    print(f"== {len(srcs)} sources, {len(deps)} dépendances, {len(missing)} fichiers à générer")
    if a.list:
        print("\n".join(missing))
        return
    # Build in chunks to stay under the Windows command-line limit.
    for i in range(0, len(missing), 400):
        run([make, f"-j{a.j}", "NODEP=1"] + missing[i:i + 400])
    still = [m for m in missing if not os.path.exists(os.path.join(ROOT, m))]
    if still:
        print(f"!! {len(still)} fichiers toujours absents, ex. : {still[:5]}")
        sys.exit(1)
    print("== données générées")


if __name__ == "__main__":
    main()
