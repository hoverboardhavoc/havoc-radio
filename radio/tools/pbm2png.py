#!/usr/bin/env python3
"""Render 128x32 PBMs (the goldens, or captures from GET /api/screen) to PNG at 4x (or --scale N) for review.

usage: pbm2png.py [--scale N] <in.pbm>... <out dir>

Lit pixels are drawn pale blue on black with a one-pixel grid gap, like the OLED.
"""
import os
import struct
import sys
import zlib

ON = (0xE6, 0xF4, 0xFF)
OFF = (0x07, 0x09, 0x0B)
GAP = (0, 0, 0)


def read_pbm(path):
    """Width, height and rows of bools of a PBM: plain (P1, the goldens) or binary (P4, GET /api/screen)."""
    data = open(path, "rb").read()
    if data.startswith(b"P4"):
        magic, size, bits = data.split(b"\n", 2)
        w, h = map(int, size.split())
        stride = (w + 7) // 8
        return w, h, [[bool(bits[y * stride + x // 8] >> (7 - x % 8) & 1) for x in range(w)] for y in range(h)]
    tokens = []
    for line in data.decode("ascii").splitlines():
        line = line.split("#", 1)[0]
        tokens += line.split()
    if tokens[0] != "P1":
        raise ValueError(f"{path}: not a PBM")
    w, h = int(tokens[1]), int(tokens[2])
    bits = "".join(tokens[3:])
    return w, h, [[bits[y * w + x] == "1" for x in range(w)] for y in range(h)]


def png(path, w, h, rows):
    raw = b"".join(b"\x00" + bytes(c for px in r for c in px) for r in rows)

    def chunk(kind, data):
        c = struct.pack(">I", len(data)) + kind + data
        return c + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)

    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)))
        f.write(chunk(b"IDAT", zlib.compress(raw, 9)))
        f.write(chunk(b"IEND", b""))


def render(src, dst, scale):
    w, h, px = read_pbm(src)
    rows = []
    for y in range(h * scale):
        row = []
        for x in range(w * scale):
            gap = scale >= 4 and (x % scale == scale - 1 or y % scale == scale - 1)
            row.append(GAP if gap else ON if px[y // scale][x // scale] else OFF)
        rows.append(row)
    png(dst, w * scale, h * scale, rows)


def main():
    args = sys.argv[1:]
    scale = 4
    if args and args[0] == "--scale":
        scale, args = int(args[1]), args[2:]
    *inputs, outdir = args
    os.makedirs(outdir, exist_ok=True)
    for src in inputs:
        dst = os.path.join(outdir, os.path.splitext(os.path.basename(src))[0] + ".png")
        render(src, dst, scale)
        print(dst)


if __name__ == "__main__":
    main()
