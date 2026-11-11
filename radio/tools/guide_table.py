#!/usr/bin/env python3
"""Convert an EiBi schedule CSV into a frequency guide table (format 1), or check a table.

usage: guide_table.py CSV -o TABLE [--season B26] [--modified TIME] [--manifest JSON [--csv-name PATH]]
       guide_table.py --verify TABLE [--max BYTES]

The CSV is EiBi's sked-<season>.csv as served: Latin-1, CRLF, columns kHz, Time(UTC), Days, ITU, Station, Lng,
Target, Remarks, P, Start, Stop. The season defaults to the one in the file name; --modified (EiBi's
Last-Modified, as an HTTP date, ISO 8601 or Unix time) defaults to the file's modification time. A row that cannot
be read is reported with its line number; with any such error no table is written and the exit status is 1.

--manifest also writes the table's manifest. --verify runs every check the radio runs on a table before it uses
it and exits 1 with the reason when one fails. The format is described in docs/guide-format.md.
"""
import argparse
import datetime
import email.utils
import hashlib
import json
import os
import re
import struct
import sys
import zlib
from decimal import ROUND_HALF_UP, Decimal, InvalidOperation

MAGIC = b"HVGD"
FORMAT = 1
HEADER_LEN = 48
ENTRY_LEN = 14
SLOT_SIZE = 0x78000          # one slot of the radio's guide partition
STRING_MAX = 64              # bytes of UTF-8, without the NUL
KHZ_MIN, KHZ_MAX = 150, 30000
IRREGULAR = 0x80
EVERY_DAY = 0x7F
DAY_NAMES = ["Mo", "Tu", "We", "Th", "Fr", "Sa", "Su"]
HEADER = struct.Struct("<4sHHIIII4sII12s")
ENTRY = struct.Struct("<HBBHHHHH")
assert HEADER.size == HEADER_LEN and ENTRY.size == ENTRY_LEN


class TableError(Exception):
    pass


# ---- conversion ----

def _day(tok, i):
    """The day (0 Monday .. 6 Sunday) at tok[i] and the index after it, or None."""
    if i < len(tok) and tok[i] in "1234567":
        return int(tok[i]) - 1, i + 1
    name = tok[i:i + 2]
    if name in DAY_NAMES:
        return DAY_NAMES.index(name), i + 2
    return None


def parse_days(text):
    """Day bits (bit 0 Monday .. bit 6 Sunday) for EiBi's Days column; IRREGULAR for any other form."""
    text = text.strip()
    if not text:
        return EVERY_DAY
    bits = 0
    for tok in text.split(","):
        days, i = [], 0
        while i < len(tok):
            d = _day(tok, i)
            if d is None:
                break
            days.append(d[0])
            i = d[1]
            if i < len(tok) and tok[i] == "-" and len(days) == 1:
                e = _day(tok, i + 1)
                if e is None or e[1] != len(tok):
                    return IRREGULAR
                k = days[0]
                while k != e[0]:
                    k = (k + 1) % 7
                    days.append(k)
                i = e[1]
        if not days or i != len(tok):
            return IRREGULAR
        for d in days:
            bits |= 1 << d
    return bits


def parse_time(text):
    """(start, end) minutes for HHMM-HHMM: start 0..1439, end 1..1440 (2400 and 0000 as an end are 1440)."""
    m = re.fullmatch(r"(\d\d)(\d\d)-(\d\d)(\d\d)", text.strip())
    if not m:
        raise ValueError(f"time {text!r} is not HHMM-HHMM")
    h1, m1, h2, m2 = (int(x) for x in m.groups())
    if m1 > 59 or m2 > 59 or h1 * 60 + m1 > 1439 or h2 * 60 + m2 > 1440:
        raise ValueError(f"time {text!r} out of range")
    start, end = h1 * 60 + m1, h2 * 60 + m2
    return start, end or 1440


def clean(raw):
    """A Latin-1 field as UTF-8 text: trimmed, without control characters, cut to STRING_MAX bytes."""
    s = "".join(c for c in raw.decode("latin-1") if not (ord(c) < 0x20 or 0x7F <= ord(c) < 0xA0)).strip()
    while len(s.encode("utf-8")) > STRING_MAX:
        s = s[:-1]
    return s.rstrip()


def parse_csv(data):
    """Entries (freq in 0.1 kHz, start, end, days, name, language, target) of an EiBi CSV, deduplicated and sorted,
    and the errors as (line, message)."""
    entries, errors = set(), []
    lines = data.split(b"\n")
    for n, line in enumerate(lines[1:], start=2):
        line = line.rstrip(b"\r")
        if not line.strip():
            continue
        f = line.split(b";")
        if len(f) < 7:
            errors.append((n, f"{len(f)} fields"))
            continue
        try:
            khz = Decimal(f[0].decode("latin-1").strip())
            freq = int((khz * 10).quantize(Decimal(1), rounding=ROUND_HALF_UP))
        except (InvalidOperation, ValueError):
            errors.append((n, f"frequency {f[0].decode('latin-1')!r}"))
            continue
        if not KHZ_MIN * 10 <= freq <= KHZ_MAX * 10:
            continue
        try:
            start, end = parse_time(f[1].decode("latin-1"))
        except ValueError as e:
            errors.append((n, str(e)))
            continue
        days = parse_days(f[2].decode("latin-1"))
        entries.add((freq, start, end, days, clean(f[4]), clean(f[5]), clean(f[6])))
    return sorted(entries), errors


def build(entries, season, modified):
    """The table for sorted entries."""
    strings, index = [], {}
    for e in entries:
        for s in e[4:]:
            if s not in index:
                index[s] = len(strings)
                strings.append(s)
    if len(strings) > 0xFFFF:
        raise TableError(f"{len(strings)} strings, at most 65535")
    body = bytearray()
    for freq, start, end, days, name, lang, target in entries:
        body += ENTRY.pack(freq // 10, freq % 10, days, start, end, index[name], index[lang], index[target])
    str_off = HEADER_LEN + len(body)
    blobs = [s.encode("utf-8") + b"\0" for s in strings]
    pos = str_off + 4 + 4 * len(strings)
    offsets = []
    for b in blobs:
        offsets.append(pos)
        pos += len(b)
    body += struct.pack(f"<I{len(offsets)}I", len(offsets), *offsets) + b"".join(blobs)
    total = HEADER_LEN + len(body)
    tag = season.encode("ascii")
    if not re.fullmatch(rb"[AB]\d\d", tag):
        raise TableError(f"season {season!r} is not A or B and two digits")
    header = HEADER.pack(MAGIC, FORMAT, HEADER_LEN, len(entries), HEADER_LEN, str_off, total, tag, modified,
                         zlib.crc32(body), bytes(12))
    return header + bytes(body)


# ---- checks ----

def valid_utf8_text(b):
    """True for valid UTF-8 without control characters (below 0x20, and 0x7F)."""
    try:
        s = b.decode("utf-8", errors="strict")
    except UnicodeDecodeError:
        return False
    return not any(ord(c) < 0x20 or ord(c) == 0x7F for c in s)


def verify(data, max_len=SLOT_SIZE):
    """Checks a table as the radio does. Returns its header fields; raises TableError with the reason."""
    if len(data) < HEADER_LEN:
        raise TableError("shorter than the header")
    magic, fmt, hlen, count, ent_off, str_off, total, season, modified, crc, _ = HEADER.unpack_from(data)
    if magic != MAGIC:
        raise TableError("not a guide table (magic)")
    if fmt != FORMAT:
        raise TableError(f"format {fmt}, not {FORMAT}")
    if hlen != HEADER_LEN:
        raise TableError(f"header length {hlen}")
    if total != len(data) or total > max_len:
        raise TableError(f"total length {total}, data {len(data)}, at most {max_len}")
    if zlib.crc32(data[HEADER_LEN:]) != crc:
        raise TableError("CRC does not match")
    if ent_off != HEADER_LEN or count > (total - HEADER_LEN) // ENTRY_LEN or str_off != HEADER_LEN + count * ENTRY_LEN:
        raise TableError("entries or string table out of place")
    if str_off + 4 > total:
        raise TableError("string table past the end")
    (nstr,) = struct.unpack_from("<I", data, str_off)
    first = str_off + 4 + 4 * nstr
    if nstr > (total - str_off - 4) // 4 or first > total:
        raise TableError("string offsets past the end")
    for i in range(nstr):
        (off,) = struct.unpack_from("<I", data, str_off + 4 + 4 * i)
        if not first <= off < total:
            raise TableError(f"string {i} out of range")
        end = data.find(b"\0", off, min(total, off + STRING_MAX + 1))
        if end < 0:
            raise TableError(f"string {i} not terminated within {STRING_MAX} bytes")
        if not valid_utf8_text(data[off:end]):
            raise TableError(f"string {i} not valid UTF-8 text")
    prev = None
    for i in range(count):
        khz, tenths, days, start, end, name, lang, target = ENTRY.unpack_from(data, HEADER_LEN + i * ENTRY_LEN)
        if tenths > 9 or start > 1439 or not 1 <= end <= 1440:
            raise TableError(f"entry {i} out of range")
        if max(name, lang, target) >= nstr:
            raise TableError(f"entry {i}: string index out of range")
        key = (khz * 10 + tenths, start)
        if prev is not None and key < prev:
            raise TableError(f"entry {i} not sorted")
        prev = key
    return {"season": season.rstrip(b"\0").decode("ascii", "replace"), "entries": count, "length": total,
            "modified": modified, "strings": nstr}


# ---- command line ----

def parse_modified(text):
    if re.fullmatch(r"\d+", text):
        return int(text)
    try:
        return int(email.utils.parsedate_to_datetime(text).timestamp())
    except (TypeError, ValueError):
        pass
    return int(datetime.datetime.fromisoformat(text.replace("Z", "+00:00")).timestamp())


def iso(t):
    return datetime.datetime.fromtimestamp(t, datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def manifest(table, csv_name):
    info = verify(table)
    return {"format": FORMAT, "season": info["season"], "entries": info["entries"], "length": len(table),
            "sha256": hashlib.sha256(table).hexdigest(), "source_modified": iso(info["modified"]), "csv": csv_name}


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("csv", nargs="?")
    ap.add_argument("-o", "--out")
    ap.add_argument("--season")
    ap.add_argument("--modified")
    ap.add_argument("--manifest")
    ap.add_argument("--csv-name")
    ap.add_argument("--verify", metavar="TABLE")
    ap.add_argument("--max", type=lambda s: int(s, 0), default=SLOT_SIZE)
    a = ap.parse_args(argv)
    if a.verify:
        try:
            with open(a.verify, "rb") as f:
                info = verify(f.read(), a.max)
        except (OSError, TableError) as e:
            print(f"{a.verify}: {e}", file=sys.stderr)
            return 1
        print(f"{a.verify}: EiBi {info['season']} of {iso(info['modified'])}, {info['entries']} entries, "
              f"{info['strings']} strings, {info['length']} bytes")
        return 0
    if not a.csv or not a.out:
        ap.error("a CSV and -o TABLE, or --verify TABLE")
    season = a.season
    if not season:
        m = re.fullmatch(r"sked-([ab]\d\d)\.csv", os.path.basename(a.csv), re.I)
        if not m:
            ap.error("--season is needed when the file is not named sked-<season>.csv")
        season = m.group(1).upper()
    modified = parse_modified(a.modified) if a.modified else int(os.path.getmtime(a.csv))
    with open(a.csv, "rb") as f:
        entries, errors = parse_csv(f.read())
    for line, msg in errors:
        print(f"{a.csv}:{line}: {msg}", file=sys.stderr)
    if errors:
        print(f"{len(errors)} errors, no table written", file=sys.stderr)
        return 1
    try:
        table = build(entries, season, modified)
        info = verify(table)
    except TableError as e:
        print(f"{a.csv}: {e}", file=sys.stderr)
        return 1
    with open(a.out, "wb") as f:
        f.write(table)
    if a.manifest:
        with open(a.manifest, "w", encoding="utf-8") as f:
            json.dump(manifest(table, a.csv_name or "eibi/" + os.path.basename(a.csv)), f, indent=2)
            f.write("\n")
    print(f"{a.out}: EiBi {season}, {info['entries']} entries, {info['strings']} strings, {len(table)} bytes")
    return 0


if __name__ == "__main__":
    sys.exit(main())
