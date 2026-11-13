#!/usr/bin/env python3
"""Fetch the starting frequency guide for the install image.

usage: fetch_guide.py <out.bin>

Downloads v1/manifest.json and v1/stations.bin from the data repository hoverboardhavoc/eibi-binary-table and keeps
the table when its length and SHA-256 are the manifest's, it starts with the magic HVGD and format 1, and it fits a
slot of the guide partition. The radio runs the format's full checks (FORMAT.md in the data repository) before it
uses a table. On any failure no table is written, a warning is printed and the script exits 0: the build continues
and the radio starts with no guide.
"""
import hashlib
import json
import os
import struct
import sys
import urllib.request

BASE = "https://raw.githubusercontent.com/hoverboardhavoc/eibi-binary-table/main/v1/"
FORMAT = 1
SLOT_SIZE = 0x78000  # one slot of the radio's guide partition


def fetch(name, limit):
    with urllib.request.urlopen(BASE + name, timeout=20) as r:
        return r.read(limit + 1)


def check(manifest, data):
    if manifest.get("format") != FORMAT:
        raise ValueError(f"manifest format {manifest.get('format')}, not {FORMAT}")
    if len(data) != manifest["length"] or len(data) > SLOT_SIZE:
        raise ValueError(f"{len(data)} bytes, manifest {manifest['length']}, slot {SLOT_SIZE}")
    if hashlib.sha256(data).hexdigest() != manifest["sha256"]:
        raise ValueError("SHA-256 is not the manifest's")
    magic, fmt = struct.unpack_from("<4sH", data)
    if magic != b"HVGD" or fmt != FORMAT:
        raise ValueError(f"magic {magic!r}, format {fmt}")


def main():
    out = sys.argv[1]
    try:
        manifest = json.loads(fetch("manifest.json", 4096))
        data = fetch("stations.bin", SLOT_SIZE)
        check(manifest, data)
    except Exception as e:  # network or table problem: build without a starting guide
        print(f"warning: frequency guide not available ({e}); the install image carries no guide", file=sys.stderr)
        if os.path.exists(out):
            os.remove(out)
        return
    tmp = out + ".tmp"
    with open(tmp, "wb") as f:
        f.write(data)
    os.replace(tmp, out)
    print(f"Frequency guide: EiBi {manifest['season']}, {manifest['entries']} entries, {len(data)} bytes")


if __name__ == "__main__":
    main()
