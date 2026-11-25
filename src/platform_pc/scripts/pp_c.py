#!/usr/bin/env python3
"""C front end of the PC build: the same pipeline as the GBA Makefile,
    cpp | preproc -i <src> charmap.txt | compiler
with Clang's preprocessor. preproc expands INCBIN_* (binary assets, read
from the files `make` generates next to their sources) and _("...")
strings (charmap.txt). The result is plain C, compiled by CMake.

Usage: pp_c.py --clang CLANG --preproc PREPROC --root ROOT
               --out OUT.c --dep OUT.d -- <clang -E flags...> SRC.c
"""
import argparse
import os
import subprocess
import sys


def main():
    if "--" not in sys.argv:
        sys.exit("pp_c.py: missing --")
    split = sys.argv.index("--")
    ap = argparse.ArgumentParser()
    ap.add_argument("--clang", required=True)
    ap.add_argument("--preproc", required=True)
    ap.add_argument("--root", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--dep", required=True)
    ap.add_argument("--data-section", default="",
                    help="put the file's variables in this section (soft reset snapshot)")
    ap.add_argument("--lang-map", default="", help="French text map (scripts/lang/lang_build.py)")
    ap.add_argument("--lang-section", default="prs_lang", help="section of the French text registry")
    a = ap.parse_args(sys.argv[1:split])
    rest = sys.argv[split + 1:]
    src = rest[-1]

    cpp = subprocess.run([a.clang, "-E", "-MD", "-MF", a.dep, "-MT", a.out] + rest,
                         cwd=a.root, capture_output=True)
    if cpp.returncode != 0:
        sys.stderr.buffer.write(cpp.stderr)
        return cpp.returncode
    if cpp.stderr:
        sys.stderr.buffer.write(cpp.stderr)

    rel = os.path.relpath(src, a.root).replace("\\", "/")
    pp = subprocess.run([a.preproc, "-i", rel, "charmap.txt"],
                        cwd=a.root, input=cpp.stdout, capture_output=True)
    if pp.returncode != 0:
        sys.stderr.buffer.write(pp.stderr)
        return pp.returncode

    result = pp.stdout
    if a.lang_map:
        sys.path.insert(0, os.path.join(os.path.dirname(__file__), "lang"))
        import lang_build
        result = lang_build.patch_c(result, rel, a.lang_map, a.lang_section)

    os.makedirs(os.path.dirname(a.out) or ".", exist_ok=True)
    tmp = a.out + ".tmp"
    with open(tmp, "wb") as f:
        if a.data_section:
            # With -fno-zero-initialized-in-bss, every writable global and
            # static of the game lands in this one section.
            f.write(b'#pragma clang section data="%s"\n' % a.data_section.encode())
        f.write(result)
    os.replace(tmp, a.out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
