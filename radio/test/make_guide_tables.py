#!/usr/bin/env python3
"""Write the guide tables the host and page tests read (radio/test/guide/) with the data repository's converter.

usage: make_guide_tables.py EIBI_BINARY_TABLE_DIR

EIBI_BINARY_TABLE_DIR is a checkout of hoverboardhavoc/eibi-binary-table. Its tools/guide_table.py converts:

- b26.bin and a26.bin (with b26.bin.json, the manifest): EiBi's B26 and A26 CSVs from its tools/fixtures/full/;
- rows.bin: the reader's host test rows below;
- scan.bin: the guide scan's host test rows below;
- sample.bin: the UI prototype's sample guide (GUIDE_CSV in radio/ui/prototype.html).

The tables are committed; make test does not run this script. Run it again when the rows or the format change.
"""
import os
import re
import sys

RADIO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(RADIO, "test", "guide")
HEAD = b"kHz:75;Time(UTC):93;Days:59;ITU:49;Station:201;Lng:49;Target:62;Remarks:135;P:35;Start:60;Stop:60;\r\n"
MODIFIED = 1793448000  # 2026-10-31 12:00 UTC

# The reader's host tests (radio/test/test_guide.cpp): frequencies at the 1 kHz window's edges, every day form,
# a window across midnight, an irregular entry, and 20 entries on one frequency.
HOST_ROWS = [
    b"8998.9;0000-2400;;X;Too low;E;Eu;;;;",
    b"8999;1100-1200;Mo-Fr;X;Weekday;E;Eu;;;;",
    b"9000;1000-1100;;X;Ten;E;Eu;;;;",
    b"9000;2300-0100;Tu;X;Tuesday night;E;NAm;;;;",
    b"9000;0600-0700;irr;X;Irregular;E;Eu;;;;",
    b"9000.5;1200-1300;We;X;Wednesday;F;Af;;;;",
    b"9001;0000-2400;;X;All day;E;Eu;;;;",
    b"9001.1;0000-2400;;X;Too high;E;Eu;;;;",
] + [b"12000;%02d00-%02d30;;X;Hour %02d;E;Eu;;;;" % (h, h, h) for h in range(20)]

# The guide scan's host tests (radio/test/test_scan.cpp): frequencies rounding to the band's edges and across them,
# 0.1 kHz entries rounding together, an exact duplicate, irregular and off-air entries.
SCAN_ROWS = [
    b"5899.4;0000-2400;;X;Below;E;Eu;;;;",
    b"5899.5;1300-1400;;X;Edge low;E;Eu;;;;",
    b"5940;1400-1500;;X;Later;E;Eu;;;;",
    b"5940.1;0000-2400;;X;Rounded;E;Eu;;;;",
    b"5940.4;0000-2400;;X;Rounded too;E;Eu;;;;",
    b"5940.5;0000-2400;;X;Next up;E;Eu;;;;",
    b"6000;1300-1400;;X;Duplicate;E;Eu;;;;",
    b"6000;1300-1400;;X;Duplicate;E;Eu;;;;",
    b"6000;0000-2400;;X;Second;E;Eu;;;;",
    b"6100;0600-0700;irr;X;Irregular;E;Eu;;;;",
    b"6150;1200-1300;Tu;X;Tuesday noon;E;Eu;;;;",
    b"6200.4;0000-2400;;X;Top;E;Eu;;;;",
    b"6200.5;0000-2400;;X;Above;E;Eu;;;;",
    b"7000;0000-2400;;X;Forty;E;Eu;;;;",
]


def table(g, rows, season):
    entries, errors = g.parse_csv(HEAD + b"".join(r + b"\r\n" for r in rows))
    if errors:
        sys.exit(f"rows with errors: {errors}")
    return g.build(entries, season, MODIFIED)


def write(name, data):
    with open(os.path.join(OUT, name), "wb") as f:
        f.write(data)
    print(f"radio/test/guide/{name}: {len(data)} bytes")


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    repo = os.path.abspath(sys.argv[1])
    sys.path.insert(0, os.path.join(repo, "tools"))
    import guide_table as g  # noqa: E402

    os.makedirs(OUT, exist_ok=True)
    for season in ("b26", "a26"):
        out = os.path.join(OUT, f"{season}.bin")
        manifest = ["--manifest", out + ".json"] if season == "b26" else []
        if g.main([os.path.join(repo, "tools", "fixtures", "full", f"sked-{season}.csv"), "-o", out,
                   "--modified", str(MODIFIED), *manifest]) != 0:
            sys.exit(f"{season}: the converter reported errors")
    write("rows.bin", table(g, HOST_ROWS, "A26"))
    write("scan.bin", table(g, SCAN_ROWS, "A26"))
    proto = open(os.path.join(RADIO, "ui", "prototype.html"), encoding="utf-8").read()
    block = re.search(r"const GUIDE_CSV = \[(.*?)\];", proto, re.S).group(1)
    write("sample.bin", table(g, [r.encode("latin-1") + b";;;;" for r in re.findall(r"'([^']*)'", block)], "B26"))


if __name__ == "__main__":
    main()
