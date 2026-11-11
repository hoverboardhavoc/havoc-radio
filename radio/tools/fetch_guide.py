#!/usr/bin/env python3
"""Fetch the starting frequency guide for the install image.

usage: fetch_guide.py <out.bin>

Downloads v1/stations.bin from the data repository hoverboardhavoc/havoc-radio-guide and checks it with
guide_table.py's checks (the radio's own). On any failure no table is written, a warning is printed and the script
exits 0: the build continues and the radio starts with no guide.
"""
import os
import sys
import urllib.request

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import guide_table  # noqa: E402

URL = "https://raw.githubusercontent.com/hoverboardhavoc/havoc-radio-guide/main/v1/stations.bin"


def main():
    out = sys.argv[1]
    try:
        with urllib.request.urlopen(URL, timeout=20) as r:
            data = r.read(guide_table.SLOT_SIZE + 1)
        info = guide_table.verify(data)
    except Exception as e:  # network or table problem: build without a starting guide
        print(f"warning: frequency guide not available ({e}); the install image carries no guide", file=sys.stderr)
        if os.path.exists(out):
            os.remove(out)
        return
    tmp = out + ".tmp"
    with open(tmp, "wb") as f:
        f.write(data)
    os.replace(tmp, out)
    print(f"Frequency guide: EiBi {info['season']}, {info['entries']} entries, {len(data)} bytes")


if __name__ == "__main__":
    main()
