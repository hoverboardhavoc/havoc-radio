#!/usr/bin/env python3
"""Checks radio/tools/guide_table.py: the B26 and A26 EiBi CSVs (radio/test/eibi/, kept byte for byte), built rows
for every day form, time, frequency and text rule, the table's header, entries and string table, and --verify
refusing each kind of damage.

usage: test_guide_table.py [OUT_DIR]   (OUT_DIR defaults to radio/build/host/guide)
"""
import hashlib
import json
import os
import struct
import sys
import zlib

RADIO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(RADIO, "tools"))
import guide_table as g  # noqa: E402

OUT = sys.argv[1] if len(sys.argv) > 1 else os.path.join(RADIO, "build", "host", "guide")
FIXTURES = os.path.join(RADIO, "test", "eibi")
HEAD = b"kHz:75;Time(UTC):93;Days:59;ITU:49;Station:201;Lng:49;Target:62;Remarks:135;P:35;Start:60;Stop:60;\r\n"
MODIFIED = 1793448000  # 2026-10-31 12:00 UTC

checks = failures = 0


def check(cond, what):
    global checks, failures
    checks += 1
    if not cond:
        failures += 1
        print("FAIL: " + what)


def csv(*rows):
    return HEAD + b"".join(r + b"\r\n" for r in rows)


def entries(*rows):
    e, errors = g.parse_csv(csv(*rows))
    check(not errors, f"no errors for {rows}: {errors}")
    return e


def read_table(t):
    """(header tuple, [entry tuples with the strings resolved])."""
    h = g.HEADER.unpack_from(t)
    count, str_off = h[3], h[5]
    (n,) = struct.unpack_from("<I", t, str_off)
    offs = struct.unpack_from(f"<{n}I", t, str_off + 4)
    strs = [t[o:t.index(b"\0", o)].decode("utf-8") for o in offs]
    out = []
    for i in range(count):
        khz, tenths, days, start, end, a, b, c = g.ENTRY.unpack_from(t, g.HEADER_LEN + i * g.ENTRY_LEN)
        out.append((khz * 10 + tenths, start, end, days, strs[a], strs[b], strs[c]))
    return h, out, strs


def refused(t, what, reason=None):
    try:
        g.verify(bytes(t))
        check(False, f"--verify refuses {what}")
    except g.TableError as e:
        check(reason is None or reason in str(e), f"--verify refuses {what}: {e}")


def recrc(t):
    t = bytearray(t)
    struct.pack_into("<I", t, 32, zlib.crc32(bytes(t[g.HEADER_LEN:])))
    return t


def fixtures():
    os.makedirs(OUT, exist_ok=True)
    sizes = {}
    for season in ("b26", "a26"):
        path = os.path.join(FIXTURES, f"sked-{season}.csv")
        raw = open(path, "rb").read()
        check(raw.count(b"\r\n") == raw.count(b"\n") and raw.count(b"\n") > 8000, f"{season}: CRLF kept")
        e, errors = g.parse_csv(raw)
        check(not errors, f"{season}: no errors ({errors[:3]})")
        check(1000 <= len(e) <= 30000, f"{season}: {len(e)} entries")
        out = os.path.join(OUT, f"{season}.bin")
        rc = g.main([path, "-o", out, "--modified", str(MODIFIED), "--manifest", out + ".json"])
        check(rc == 0, f"{season}: converted")
        t = open(out, "rb").read()
        info = g.verify(t)
        check(info["season"] == season.upper() and info["entries"] == len(e), f"{season}: header {info}")
        check(len(t) <= g.SLOT_SIZE, f"{season}: {len(t)} B fits a slot")
        m = json.load(open(out + ".json"))
        check(m == {"format": 1, "season": season.upper(), "entries": len(e), "length": len(t),
                    "sha256": hashlib.sha256(t).hexdigest(), "source_modified": "2026-10-31T12:00:00Z",
                    "csv": f"eibi/sked-{season}.csv"}, f"{season}: manifest {m}")
        check(g.main(["--verify", out]) == 0, f"{season}: --verify passes")
        sizes[season] = (len(e), len(t))
    # 6070 kHz in B26: the rows as EiBi lists them, SM Radio Dessau's two dated rows becoming one irregular entry.
    _, ents, _ = read_table(open(os.path.join(OUT, "b26.bin"), "rb").read())
    got = [e for e in ents if e[0] == 60700]
    want = [(60700, 0, 1440, 0x7F, "CFRX Toronto, CFRB 1010", "E", "NAm"),
            (60700, 0, 1440, 0x7F, "Channel 292", "D,E", "Eu"),
            (60700, 420, 840, 0x80, "SM Radio Dessau", "D", "Eu"),
            (60700, 540, 780, 0x7F, "Voice of Korea", "J", "J"),
            (60700, 720, 780, 0x80, "SM Radio Dessau", "D", "Eu"),
            (60700, 1020, 1080, 0x7F, "China Radio Int.", "R", "CAs"),
            (60700, 1080, 1140, 0x7F, "China Radio Int.", "R", "CAs")]
    check(got == want, f"B26 6070 kHz: {got}")
    check(any(e[4] == "Rádio Clube do Pará" for e in ents), "B26: Latin-1 name as UTF-8")
    return sizes


def days():
    cases = {"": 0x7F, "Mo": 0x01, "Su": 0x40, "SaSu": 0x60, "Tu,Th": 0x0A, "Mo-Fr": 0x1F, "We-Mo": 0x7D,
             "Th-Tu": 0x7B, "Su-Mo": 0x41, "Mo,We-Fr": 0x1D, "1357": 0x55, "12356": 0x37, "7": 0x40, "1-5": 0x1F,
             " Mo-Fr ": 0x1F, "Mo-Mo": 0x01, "MoWe": 0x05,
             "irr": 0x80, "alt": 0x80, "29Dec": 0x80, "2.Su": 0x80, "1.Sa": 0x80, "Last7": 0x80, "USB": 0x80,
             "Tests": 0x80, "spur": 0x80, "8": 0x80, "0": 0x80, "Mo-": 0x80, "-Fr": 0x80, "Mo-Fr-Su": 0x80,
             "Mo,,Tu": 0x80, "mo": 0x80, "SaSu-Mo": 0x80}
    for text, bits in cases.items():
        check(g.parse_days(text) == bits, f"days {text!r}: 0x{g.parse_days(text):02x}, want 0x{bits:02x}")


def times():
    for text, want in {"0000-2400": (0, 1440), "1700-1800": (1020, 1080), "2300-0100": (1380, 60),
                       "2200-2400": (1320, 1440), "2200-0000": (1320, 1440), "2359-0001": (1439, 1),
                       "1241-1241": (761, 761)}.items():
        check(g.parse_time(text) == want, f"time {text}: {g.parse_time(text)}")
    for bad in ("2400-0100", "1260-1300", "0900-2401", "900-1000", "abcd-efgh", "", "0900"):
        try:
            g.parse_time(bad)
            check(False, f"time {bad!r} is an error")
        except ValueError:
            check(True, "")


def rows():
    e = entries(b"6070;1700-1800;;CHN;China Radio Int.;R;CAs;x;0;;")
    check(e == [(60700, 1020, 1080, 0x7F, "China Radio Int.", "R", "CAs")], f"one row: {e}")
    # Frequencies kept to 0.1 kHz (half up), 150 to 30000 kHz.
    e = entries(b"6070.04;0000-0100;;X;A;;;;;;", b"6070.05;0000-0100;;X;B;;;;;;", b"149.9;0000-0100;;X;C;;;;;;",
                b"150;0000-0100;;X;D;;;;;;", b"30000;0000-0100;;X;E;;;;;;", b"30000.1;0000-0100;;X;F;;;;;;",
                b"16.3;0000-0100;;X;G;;;;;;", b"88500;0000-0100;;X;H;;;;;;")
    check([(x[0], x[4]) for x in e] == [(1500, "D"), (60700, "A"), (60701, "B"), (300000, "E")], f"frequencies: {e}")
    # Latin-1 to UTF-8, trimmed; control characters dropped; cut to 64 bytes on a character boundary.
    e = entries(b"6000;0000-0100;;X; R\xe1dio Clube do Par\xe1 ; pt ; SAm ;;;;",
                b"6001;0000-0100;;X;A\x01B\x7fC\x85D;;;;;;",
                b"6002;0000-0100;;X;" + b"x" * 70 + b";;;;;;",
                b"6003;0000-0100;;X;" + b"x" * 63 + b"\xe9;;;;;;")
    check(e[0][4:] == ("Rádio Clube do Pará", "pt", "SAm"), f"Latin-1, trimmed: {e[0]}")
    check(e[1][4] == "ABCD", f"control characters dropped: {e[1][4]!r}")
    check(e[2][4] == "x" * 64, "cut to 64 bytes")
    check(e[3][4] == "x" * 63, f"cut before a two-byte character: {len(e[3][4].encode())} bytes")
    # Exact duplicates once (rows differing only in unused columns are the same entry); sorted by frequency, then
    # start time.
    e = entries(b"9000;1000-1100;;X;A;E;Eu;;;;", b"6000;1200-1300;;X;B;E;Eu;;;;", b"6000;0800-0900;;X;C;E;Eu;;;;",
                b"9000;1000-1100;;X;A;E;Eu;;;;", b"9000;1000-1100;;Y;A;E;Eu;rem;1;0101;0202")
    check([(x[0], x[1], x[4]) for x in e] == [(60000, 480, "C"), (60000, 720, "B"), (90000, 600, "A")],
          f"deduplicated and sorted: {e}")
    # Errors carry the line number; no table is written.
    _, errors = g.parse_csv(csv(b"6000;0000-0100;;X;A;;;;;;", b"abc;0000-0100;;X;A;;;;;;", b"6000;25:00;;X;B;;;;;;",
                                b"6000;0000-0100;;X", b"", b"6000;2400-0100;;X;C;;;;;;"))
    check([n for n, _ in errors] == [3, 4, 5, 7], f"errors by line: {errors}")
    path = os.path.join(OUT, "bad.csv")
    with open(path, "wb") as f:
        f.write(csv(b"abc;0000-0100;;X;A;;;;;;"))
    out = os.path.join(OUT, "bad.bin")
    if os.path.exists(out):
        os.remove(out)
    check(g.main([path, "-o", out, "--season", "B26", "--modified", "0"]) == 1 and not os.path.exists(out),
          "a CSV with errors writes no table")


def layout():
    e = entries(b"6070;1700-1800;Mo-Fr;CHN;China Radio Int.;R;CAs;;;;", b"6070;0000-2400;;D;Channel 292;D,E;Eu;;;;",
                b"7300.5;2300-0100;irr;D;Channel 292;;Eu;;;;")
    t = g.build(e, "B26", MODIFIED)
    h, ents, strs = read_table(t)
    check(h[:8] == (b"HVGD", 1, 48, 3, 48, 48 + 3 * 14, len(t), b"B26\0"), f"header {h[:8]}")
    check(h[8] == MODIFIED and h[9] == zlib.crc32(t[48:]) and h[10] == bytes(12), "modified, CRC, zero tail")
    check(strs == ["Channel 292", "D,E", "Eu", "China Radio Int.", "R", "CAs", ""], f"strings once: {strs}")
    check(ents == [(60700, 0, 1440, 0x7F, "Channel 292", "D,E", "Eu"),
                   (60700, 1020, 1080, 0x1F, "China Radio Int.", "R", "CAs"),
                   (73005, 1380, 60, 0x80, "Channel 292", "", "Eu")], f"entries {ents}")
    raw = t[48:48 + 14]
    check(raw == bytes([0xB6, 0x17, 0, 0x7F, 0, 0, 0xA0, 5, 0, 0, 1, 0, 2, 0]), f"entry bytes {raw.hex()}")
    (n,) = struct.unpack_from("<I", t, h[5])
    first = struct.unpack_from("<I", t, h[5] + 4)[0]
    check(n == 7 and first == h[5] + 4 + 4 * 7 and t.endswith(b"CAs\0\0"), "string table: count, offsets, NULs")
    for bad in ("C26", "B2", "b26"):
        try:
            g.build(e, bad, 0)
            check(False, f"season {bad} refused")
        except g.TableError:
            check(True, "")
    return t


def damage(t):
    g.verify(t)
    h = g.HEADER.unpack_from(t)
    str_off = h[5]
    n = struct.unpack_from("<I", t, str_off)[0]
    offs = list(struct.unpack_from(f"<{n}I", t, str_off + 4))

    def at(off, fmt, v, crc=True):
        d = bytearray(t)
        struct.pack_into(fmt, d, off, v)
        return recrc(d) if crc else d

    refused(t[:47], "a table shorter than the header", "shorter")
    refused(b"HVGX" + t[4:], "another magic", "magic")
    refused(at(4, "<H", 2), "format 2", "format")
    refused(at(6, "<H", 52), "another header length", "header length")
    refused(t[:-1], "a cut-off table", "total length")
    refused(t + b"\0", "bytes past the total length", "total length")
    refused(at(20, "<I", len(t) + 1), "a total length past the end", "total length")
    try:
        g.verify(t, max_len=len(t) - 1)
        check(False, "a table longer than the partition refused")
    except g.TableError as e:
        check("at most" in str(e), str(e))
    flip = bytearray(t)
    flip[60] ^= 1
    refused(flip, "a flipped bit", "CRC")
    refused(at(12, "<I", 52), "entries out of place", "out of place")
    refused(at(8, "<I", 0xFFFFFFFF), "a huge entry count", "out of place")
    refused(at(16, "<I", len(t)), "the string table at the end", "out of place")
    refused(at(str_off, "<I", 0x40000000), "a huge string count", "string offsets")
    refused(at(str_off + 4, "<I", len(t)), "a string offset at the end", "out of range")
    refused(at(str_off + 4, "<I", 0xFFFFFFF0), "a string offset far past the end", "out of range")
    refused(at(str_off + 4, "<I", 0), "a string offset into the header", "out of range")
    refused(at(len(t) - 1, "<B", 0x41), "an unterminated last string", "not terminated")
    d = bytearray(t[:offs[0]]) + b"x" * 65 + b"\0"
    struct.pack_into("<I", d, 20, len(d))
    refused(recrc(d), "a 65-byte string", "not terminated")
    refused(at(offs[0], "<B", 0xFF), "invalid UTF-8", "UTF-8")
    refused(at(offs[0], "<2s", b"\xc0\xaf"), "an overlong UTF-8 form", "UTF-8")
    refused(at(offs[0], "<3s", b"\xed\xa0\x80"), "a UTF-16 surrogate in UTF-8", "UTF-8")
    refused(at(offs[0], "<B", 0x09), "a control character", "UTF-8")
    refused(at(offs[0], "<B", 0x7F), "DEL", "UTF-8")
    e1 = 48 + 14
    refused(at(e1, "<H", 6000), "entries not sorted by frequency", "not sorted")
    refused(at(48 + 4, "<H", 1021), "entries not sorted by start time", "not sorted")
    refused(at(48 + 2, "<B", 10), "tenths of 10", "out of range")
    refused(at(48 + 4, "<H", 1440), "a start of 1440", "out of range")
    refused(at(48 + 6, "<H", 0), "an end of 0", "out of range")
    refused(at(48 + 6, "<H", 1441), "an end of 1441", "out of range")
    for k, what in ((8, "name"), (10, "language"), (12, "target")):
        refused(at(48 + k, "<H", n), f"a {what} index past the strings", "index")
    path = os.path.join(OUT, "damaged.bin")
    with open(path, "wb") as f:
        f.write(flip)
    check(g.main(["--verify", path]) == 1, "--verify exits 1 on damage")


def main():
    sizes = fixtures()
    days()
    times()
    rows()
    damage(layout())
    b = sizes.get("b26", (0, 0))
    print(f"guide table: {checks} checks, {failures} failures (B26 {b[0]} entries, {b[1]} B)")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
