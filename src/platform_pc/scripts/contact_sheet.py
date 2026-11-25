#!/usr/bin/env python3
"""Puts PRS_SHOT captures (240x160 BMP) side by side in one PNG, scaled 2x,
for looking at a test run at a glance. Pure Python (no Pillow needed).

Usage: contact_sheet.py OUT.png [--cols N] FRAME.bmp...
"""
import struct
import sys
import zlib


def read_bmp(path):
    with open(path, "rb") as f:
        data = f.read()
    off = struct.unpack_from("<I", data, 10)[0]
    w, h = struct.unpack_from("<ii", data, 18)
    bpp = struct.unpack_from("<H", data, 28)[0]
    step = bpp // 8
    row = (w * step + 3) & ~3
    rows = []
    for y in range(abs(h)):
        src = y if h < 0 else abs(h) - 1 - y
        base = off + src * row
        rows.append([(data[base + x * step + 2], data[base + x * step + 1], data[base + x * step])
                     for x in range(w)])
    return w, abs(h), rows


def write_png(path, w, h, pixels):
    raw = b"".join(b"\x00" + bytes(c for px in line for c in px) for line in pixels)
    def chunk(tag, payload):
        return (struct.pack(">I", len(payload)) + tag + payload
                + struct.pack(">I", zlib.crc32(tag + payload) & 0xFFFFFFFF))
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)))
        f.write(chunk(b"IDAT", zlib.compress(raw, 6)))
        f.write(chunk(b"IEND", b""))


def main():
    args = sys.argv[1:]
    out = args.pop(0)
    cols = 3
    if args and args[0] == "--cols":
        cols = int(args[1])
        args = args[2:]
    frames = [read_bmp(p) for p in args]
    fw, fh = 480, 320
    rows_n = (len(frames) + cols - 1) // cols
    W, H = fw * min(cols, len(frames)), fh * rows_n
    canvas = [[(0, 0, 0)] * W for _ in range(H)]
    for i, (w, h, px) in enumerate(frames):
        ox, oy = (i % cols) * fw, (i // cols) * fh
        for y in range(fh):
            src = px[y * h // fh]
            line = canvas[oy + y]
            for x in range(fw):
                line[ox + x] = src[x * w // fw]
    write_png(out, W, H, canvas)


if __name__ == "__main__":
    main()
