#!/usr/bin/env python3
"""Builds the distributable zip of the PC version.

Contents: pokeruby.exe and pokesapphire.exe, whichever are built (debug
symbols stripped: smaller; crash addresses stay valid against the unstripped
build exe, the base is fixed), the README, and a version note. Everything the
games need is built into the exes; the French text comes at run time from the
player's own ROM.

Never packaged: saves, config.ini, ROMs, logs.

Usage: python src/platform_pc/scripts/package.py [dist]
  (exes taken from build-pc/ and build-sapphire/)
"""
import datetime
import os
import shutil
import subprocess
import sys
import zipfile

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))
FORBIDDEN = (".bin", ".rtc", ".sav", ".gba", ".ini", ".log", ".wav", ".bmp")


def main():
    dist = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "dist"))
    ext = ".exe" if os.name == "nt" else ""
    exes = [os.path.join(ROOT, b, n + ext) for b, n in (("build-pc", "pokeruby"), ("build-sapphire", "pokesapphire"))]
    exes = [e for e in exes if os.path.exists(e)]
    if not exes:
        sys.exit("no pokeruby / pokesapphire executable found: build first")

    stage = os.path.join(dist, "pokeruby-pc")
    shutil.rmtree(stage, ignore_errors=True)
    os.makedirs(stage)
    strip = shutil.which("llvm-strip") or shutil.which("strip")
    for exe in exes:
        out_exe = os.path.join(stage, os.path.basename(exe))
        if strip:
            subprocess.run([strip, "--strip-debug", "-o", out_exe, exe], check=True)
        else:
            shutil.copy2(exe, out_exe)
    shutil.copy2(os.path.join(ROOT, "README_PC.md"), os.path.join(stage, "README_PC.md"))
    commit = subprocess.run(["git", "rev-parse", "--short", "HEAD"], cwd=ROOT,
                            capture_output=True, text=True).stdout.strip() or "unknown"
    with open(os.path.join(stage, "VERSION.txt"), "w", encoding="utf-8") as f:
        f.write(f"pokeruby PC port (Ruby / Sapphire)\ncommit {commit}\nbuilt {datetime.date.today()}\n"
                "Crash addresses: symbolize with the matching unstripped build exe.\n")

    for dirpath, _, files in os.walk(stage):
        for name in files:
            if name.lower().endswith(FORBIDDEN):
                sys.exit(f"refusing to package {name}: saves, ROMs and configs stay private")

    zip_path = os.path.join(dist, f"pokeruby-pc-{commit}.zip")
    with zipfile.ZipFile(zip_path, "w", zipfile.ZIP_DEFLATED) as z:
        for dirpath, _, files in os.walk(stage):
            for name in files:
                full = os.path.join(dirpath, name)
                z.write(full, os.path.relpath(full, dist))
    size = os.path.getsize(zip_path) / (1 << 20)
    print(f"{zip_path} ({size:.1f} MiB)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
