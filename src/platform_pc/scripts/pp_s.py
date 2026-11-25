#!/usr/bin/env python3
"""Assembly front end of the PC build: the GBA Makefile's data pipeline,
    preproc <src> charmap.txt | cpp -iquote include | preproc -ie <src> charmap.txt
then the result is assembled for the host by GNU as (see PrsGame.cmake):
LLVM's assembler cannot evaluate the `.if label == .` tests of asm/macros.inc.

Script bytecode keeps its 4-byte pointers (.4byte label): the executable is
linked below 4 GiB, so 32-bit absolute relocations hold every address.

--raw: no preproc/cpp (songs made by mid2agb, assembled directly in
audio_rules.mk); only the host syntax fixes below are applied.

Host syntax fixes (ARM gas -> x86 gas), on the generated text only:
  - '@' starts a comment on ARM, not on x86: removed outside strings (the
    macro counter '\\@' is kept);
  - .word is 4 bytes on ARM, 2 on x86 -> .4byte;
  - .align N is 2^N bytes on ARM, N bytes on x86 -> .p2align N;
  - --coff: PE/COFF gas has no ELF section types (%progbits) nor
    .size/.type directives.

Usage: pp_s.py [--coff] [--raw] [--clang CLANG --preproc PREPROC] --root ROOT
               --out OUT.s --dep OUT.d -- <cpp flags...> SRC.s
"""
import argparse
import os
import re
import subprocess
import sys

ELF_SECTION_TYPE = re.compile(rb'(\.section\s+[^,\n]+,\s*"[^"\n]*")\s*,\s*[%@]\w+')
ELF_SIZE_TYPE = re.compile(rb'^\s*\.(size|type)\b[^\n;]*', re.M)
ARM_WORD = re.compile(rb'(^|[\s;])\.word\b', re.M)
ARM_ALIGN = re.compile(rb'(^|[\s;])\.align\b', re.M)

# data/specials.inc: files that include it with ALLOCATE_SPECIAL_TABLE = 0
# (mystery_gift.s) leave SPECIAL_x to the linker, in 16-bit fields - a
# relocation PE/COFF does not have. Also define the value locally (same
# number), so it is resolved at assembly time.
DEF_SPECIAL_WAITSTATE = re.compile(rb'^(\s*)\.set SPECIAL_WAITSTATE_\\ptr, \\waitstate[ \t]*$', re.M)
DEF_SPECIAL_LOCAL = (rb"\g<0>\n\1.ifndef SPECIAL_\\ptr\n\1.set SPECIAL_\\ptr, __special__\n\1.endif")

QUOTE, BACKSLASH, AT = 0x22, 0x5C, 0x40


def strip_arm_comments(text):
    """Removes '@ ...' comments (ARM syntax), outside strings and not '\\@'."""
    out = []
    for line in text.split(b"\n"):
        if b"@" in line:
            in_str = False
            for i, c in enumerate(line):
                prev = line[i - 1] if i else 0
                if c == QUOTE and prev != BACKSLASH:
                    in_str = not in_str
                elif c == AT and not in_str and prev != BACKSLASH:
                    line = line[:i]
                    break
        out.append(line)
    return b"\n".join(out)


def host_syntax(text, coff):
    text = strip_arm_comments(text)
    text = ARM_WORD.sub(rb"\1.4byte", text)
    text = ARM_ALIGN.sub(rb"\1.p2align", text)
    text = DEF_SPECIAL_WAITSTATE.sub(DEF_SPECIAL_LOCAL, text)
    if coff:
        text = ELF_SECTION_TYPE.sub(rb"\1", text)
        text = ELF_SIZE_TYPE.sub(b"", text)
    return text


ARM_P2ALIGN = re.compile(rb'(^|[\s;])\.p2align\s+[0-2]\b', re.M)
ARM_4BYTE = re.compile(rb'(^|[\s;])\.4byte\b', re.M)


def widen(text):
    """--wide: files whose every .4byte is a pointer read by C as a 64-bit
    pointer (sprite animation/image tables, callbacks, text tables). Fields
    that must stay 4 bytes are written .long (PLATFORM_PC blocks, macros)."""
    text = ARM_4BYTE.sub(rb"\1.8byte", text)
    return ARM_P2ALIGN.sub(rb"\1.p2align 3", text)


def main():
    split = sys.argv.index("--")
    ap = argparse.ArgumentParser()
    for k in ("--root", "--out", "--dep"):
        ap.add_argument(k, required=True)
    ap.add_argument("--clang")
    ap.add_argument("--preproc")
    ap.add_argument("--coff", action="store_true", help="Windows objects")
    ap.add_argument("--raw", action="store_true", help="no preproc/cpp")
    ap.add_argument("--wide", action="store_true",
                    help="typed pointer tables: .4byte -> .8byte, 8-byte alignment")
    ap.add_argument("--lang-map", default="", help="French text map (scripts/lang/lang_build.py)")
    a = ap.parse_args(sys.argv[1:split])
    rest = sys.argv[split + 1:]
    src = os.path.relpath(rest[-1], a.root).replace("\\", "/")
    flags = rest[:-1]

    def run(cmd, data=None):
        r = subprocess.run(cmd, cwd=a.root, input=data, capture_output=True)
        if r.returncode != 0:
            sys.stderr.buffer.write(r.stderr)
            sys.exit(r.returncode)
        return r.stdout

    if a.raw:
        with open(os.path.join(a.root, src), "rb") as f:
            text = f.read()
        deps = (a.out + ": " + os.path.join(a.root, src).replace("\\", "/") + "\n").encode()
    else:
        step1 = run([a.preproc, src, "charmap.txt"])
        step2 = run([a.clang, "-E", "-x", "assembler-with-cpp", "-P"] + flags + ["-"], step1)
        text = run([a.preproc, "-ie", src, "charmap.txt"], step2)
        # cpp read stdin: list the headers with a separate -M pass on the source.
        deps = run([a.clang, "-M", "-x", "assembler-with-cpp", "-MT", a.out] + flags + [src])
    text = host_syntax(text, a.coff)
    if a.wide:
        text = widen(text)
    if a.lang_map and not a.raw:
        # After widen(): the registry's 4-byte fields stay 4 bytes.
        sys.path.insert(0, os.path.join(os.path.dirname(__file__), "lang"))
        import lang_build
        section = '.section .rdata$prslang1,"dr"' if a.coff else '.section prs_lang,"a"'
        text = lang_build.patch_s(text, a.lang_map, section)

    os.makedirs(os.path.dirname(a.out) or ".", exist_ok=True)
    with open(a.dep, "wb") as f:
        f.write(deps)
    tmp = a.out + ".tmp"
    with open(tmp, "wb") as f:
        f.write(text)
    os.replace(tmp, a.out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
