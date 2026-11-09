#!/usr/bin/env python3
"""Fetch the Si4732 SSB patch and convert it to a binary blob for the firmware build.

usage: fetch_ssb_patch.py <out.bin>

The patch is not part of this repository. This script downloads patch_init.h from the public PU2CLR SI4735
Arduino library at a pinned commit, extracts the byte array, checks that it is a sequence of 8-byte
command records starting with 0x15 or 0x16 (the patch download commands), and writes the bytes to <out.bin>.
A cached copy of the header next to <out.bin> is reused. On any failure no blob is written and the script exits
0, so the firmware still builds without SSB.
"""
import os
import re
import sys
import urllib.request

COMMIT = "123eccc730dc03c0ccc28526cbea6e305da7d2fc"
URL = f"https://raw.githubusercontent.com/pu2clr/SI4735/{COMMIT}/src/patch_init.h"


def convert(header):
    m = re.search(r"ssb_patch_content\s*\[\s*\]\s*=\s*\{(.*?)\}", header, re.S)
    if not m:
        raise ValueError("no ssb_patch_content array")
    data = bytes(int(x, 16) for x in re.findall(r"0x([0-9A-Fa-f]{2})", m.group(1)))
    if not data or len(data) % 8:
        raise ValueError(f"length {len(data)} is not a multiple of 8")
    for i in range(0, len(data), 8):
        if data[i] not in (0x15, 0x16):
            raise ValueError(f"record {i // 8} starts with 0x{data[i]:02x}")
    return data


def main():
    out = sys.argv[1]
    cache = os.path.join(os.path.dirname(os.path.abspath(out)), "patch_init.h")
    try:
        if not os.path.exists(cache):
            with urllib.request.urlopen(URL, timeout=20) as r:
                text = r.read().decode("utf-8")
            with open(cache, "w", encoding="utf-8") as f:
                f.write(text)
        data = convert(open(cache, encoding="utf-8").read())
    except Exception as e:  # network or format problem: build without SSB
        print(f"SSB patch not available ({e}); LSB/USB will be hidden", file=sys.stderr)
        if os.path.exists(out):
            os.remove(out)
        return
    tmp = out + ".tmp"
    with open(tmp, "wb") as f:
        f.write(data)
    os.replace(tmp, out)
    print(f"SSB patch: {len(data)} bytes, {len(data) // 8} records")


if __name__ == "__main__":
    main()
