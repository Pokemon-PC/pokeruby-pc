#!/usr/bin/env python3
"""Turns the addresses of [prs:crash] / [prs:hang] backtraces in a log into
function + file:line (the exe has a fixed base, so addresses are stable).

Usage: python src/platform_pc/scripts/symbolize.py run.log [build-pc/pokeruby.exe]
"""
import os
import re
import shutil
import subprocess
import sys

log = sys.argv[1]
exe = sys.argv[2] if len(sys.argv) > 2 else os.path.join("build-pc", "pokeruby.exe")
tool = shutil.which("llvm-addr2line") or shutil.which("addr2line")
if not tool:
    sys.exit("llvm-addr2line / addr2line not found")
root = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", "..")).replace("\\", "/") + "/"
pat = re.compile(r"#(\d\d) 0x([0-9A-Fa-f]{16})")
for line in open(log, encoding="utf-8", errors="replace"):
    line = line.rstrip("\n")
    m = pat.search(line)
    if not m or int(m.group(2), 16) >= 0x100000000:
        print(line)
        continue
    addr = int(m.group(2), 16)
    # Return addresses point after the call: look up addr - 1 for callers.
    q = addr - 1 if m.group(1) != "00" else addr
    out = subprocess.run([tool, "-f", "-C", "-e", exe, hex(q)], capture_output=True, text=True).stdout.split("\n")
    fn = out[0] if out else "?"
    loc = (out[1] if len(out) > 1 else "?").replace("\\", "/").replace(root, "")
    print(f"{line}  {fn}  {loc}")
