#!/usr/bin/env python3
"""Check that the PC port leaves the GBA sources unchanged (matching ROM).

Adapted from the HeartGold port's check_ds_sources.py. Every tracked C/asm
source or header changed since the pret import commit must, once the
PLATFORM_PC-only code is removed (keeping the #else branches) and comments /
whitespace are ignored, equal the original. The Makefile is allowed exactly
one change: excluding src/platform_pc/ from the ROM sources.

`make compare` remains the real proof (needs devkitARM + agbcc); this check
needs only git and Python.

Usage: python src/platform_pc/scripts/check_gba_sources.py [BASE_COMMIT]
  BASE_COMMIT: the untouched pret sources (default: PRET_BASE below, the
  pret/pokeruby commit the port started from).
Exit status 1 if a file differs on the GBA.
"""
import re
import subprocess
import sys

SOURCE_EXT = (".c", ".h", ".s", ".inc", ".ld", ".txt")


def git(*args):
    return subprocess.run(["git", *args], capture_output=True, text=True, encoding="utf-8",
                          errors="replace", check=True).stdout


def strip_pc(text):
    """Removes PLATFORM_PC-only branches, keeps the GBA ones."""
    out, stack, keep = [], [], True
    for line in text.split("\n"):
        m = re.match(r"\s*#\s*(ifdef|ifndef|if|elif|else|endif)\b(.*)", line)
        if m:
            d = m.group(1)
            rest = re.sub(r"/\*.*?\*/|//.*", "", m.group(2)).strip()
            if d in ("ifdef", "ifndef", "if"):
                pc = None
                if d == "ifdef" and rest == "PLATFORM_PC":
                    pc = False
                elif d == "ifndef" and rest == "PLATFORM_PC":
                    pc = True
                elif d == "if" and re.fullmatch(r"!\s*defined\s*\(?\s*PLATFORM_PC\s*\)?", rest):
                    pc = True
                elif d == "if" and re.fullmatch(r"defined\s*\(?\s*PLATFORM_PC\s*\)?", rest):
                    pc = False
                if pc is None:
                    stack.append(("other", keep))
                    if keep:
                        out.append(line)
                else:
                    stack.append(("pc", keep, pc))
                    keep = keep and pc
                continue
            top = stack[-1] if stack else ("other", True)
            if d in ("else", "elif") and top[0] == "pc":
                if d == "elif":
                    keep = top[1] and not top[2]
                    stack[-1] = ("other", keep)
                    if keep:
                        out.append("#if " + rest)
                    continue
                keep = top[1] and not top[2]
                stack[-1] = ("pc", top[1], not top[2])
                continue
            if d == "endif":
                top = stack.pop()
                if top[0] == "pc":
                    keep = top[1]
                    continue
                if top[1]:
                    out.append(line)
                continue
        if keep:
            out.append(line)
    return "\n".join(out)


def norm(text):
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    text = re.sub(r"//[^\n]*", " ", text)
    text = re.sub(r"\s+", " ", text).strip()
    return re.sub(r"\s*\*\s*", "*", text)   # 'u8 * p' == 'u8 *p'


def gba(text):
    """The GBA view of a source: PLATFORM_PC branches removed, and the
    PC_PTR32 annotation (defined empty on the GBA) dropped."""
    text = strip_pc(text)
    text = re.sub(r"^\s*#\s*define\s+PC_PTR32\s*$", "", text, flags=re.M)
    text = re.sub(r"\bPC_PTR32\b", "", text)
    text = re.sub(r"^\s*#\s*define\s+PC_APCS\s*$", "", text, flags=re.M)
    text = re.sub(r"\bPC_APCS\b", "", text)
    return norm(text)


MAKEFILE_ALLOWED = (
    "C_SOURCES := $(wildcard src/*.c src/*/*.c src/*/*/*.c)",
    "C_SOURCES := $(filter-out src/platform_pc/%,$(wildcard src/*.c src/*/*.c src/*/*/*.c))",
)

# pret/pokeruby master when the port started ("Sync a lot of player
# controller with pokeemerald (#892)").
PRET_BASE = "5784633ce4ef7ade1a7f2d2d0c288e3d5e6cdd7f"


def main():
    base = sys.argv[1] if len(sys.argv) > 1 else PRET_BASE
    # Committed and uncommitted changes, plus new files not yet added.
    changed = [f for f in git("diff", "--name-only", base).split("\n") if f]
    changed += [f for f in git("ls-files", "--others", "--exclude-standard").split("\n") if f]
    bad = []
    for rel in changed:
        if rel.startswith("src/platform_pc/") or rel.startswith("cmake/") or rel in (
                "CMakeLists.txt", ".gitignore", ".gitattributes", "PORTING_ROADMAP.md", "README_PC.md") or rel.startswith(("docs/", ".github/")):
            continue
        try:
            cur = open(rel, encoding="utf-8", errors="replace").read()
        except FileNotFoundError:
            bad.append(("deleted", rel))
            continue
        if subprocess.run(["git", "cat-file", "-e", f"{base}:{rel}"], capture_output=True).returncode != 0:
            # New file: harmless unless it is a source the GBA build could pick up.
            if rel.endswith(SOURCE_EXT):
                bad.append(("added", rel))
            continue
        orig = git("show", f"{base}:{rel}")
        if rel == "Makefile":
            # Make syntax: only '#' comments ('/*' is a glob here).
            def ws(l):
                return re.sub(r"\s+", " ", l).strip()
            a = [ws(l) for l in cur.split("\n") if ws(l) and not l.lstrip().startswith("#")]
            b = [ws(l) for l in orig.split("\n") if ws(l) and not l.lstrip().startswith("#")]
            a = [MAKEFILE_ALLOWED[0] if l == MAKEFILE_ALLOWED[1] else l for l in a]
            if a != b:
                bad.append(("differs", rel))
            continue
        if not rel.endswith(SOURCE_EXT):
            if cur.replace("\r\n", "\n") != orig.replace("\r\n", "\n"):   # not just line endings
                bad.append(("non-source change", rel))
            continue
        if gba(cur) != norm(orig):
            bad.append(("differs", rel))
    for kind, rel in bad:
        print(kind, rel)
    print(f"{len(changed)} changed files checked, {len(bad)} differ from the original on the GBA")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
