# Frequency guide table, format 1

Havoc Radio shows what is on air on the tuned LW, MW or SW frequency from a table converted from the
[EiBi](http://www.eibispace.de/) shortwave schedule, compiled by Eike Bierwirth. EiBi's README says: "any person is
absolutely free to download, use, copy, or distribute these files or to use them within third-party software."

The table is made by `radio/tools/guide_table.py` from EiBi's `sked-<season>.csv` and read by the radio's `guide`
module (`radio/components/guide/`). The data repository
[havoc-radio-guide](https://github.com/hoverboardhavoc/havoc-radio-guide) keeps the current table as
`v1/stations.bin`, with `v1/manifest.json` beside it. A later format is written to `v2/` beside `v1/`.

```
python3 radio/tools/guide_table.py sked-b26.csv -o stations.bin --modified "Sat, 31 Oct 2026 12:00:00 GMT" \
    --manifest manifest.json
python3 radio/tools/guide_table.py --verify stations.bin
```

## Layout

All numbers are little-endian.

### Header, 48 bytes

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | Magic `HVGD` |
| 4 | 2 | Format, 1 |
| 6 | 2 | Header length, 48 |
| 8 | 4 | Entry count |
| 12 | 4 | Offset of the entries (48) |
| 16 | 4 | Offset of the string table (48 + 14 × entry count) |
| 20 | 4 | Total length of the table |
| 24 | 4 | Season (`B26`), NUL-padded |
| 28 | 4 | EiBi's Last-Modified time of the CSV, as Unix time |
| 32 | 4 | CRC-32 (IEEE, as zlib) of every byte after the header |
| 36 | 12 | Zero |

### Entries, 14 bytes each

Sorted by frequency, then start time.

| Offset | Size | Field |
|---|---|---|
| 0 | 2 | Frequency, whole kHz |
| 2 | 1 | Tenths of a kHz (0 to 9) |
| 3 | 1 | Days: bit 0 Monday to bit 6 Sunday, the UTC day a broadcast starts; bit 7 irregular, with no day bits |
| 4 | 2 | Start, minutes after 00:00 UTC (0 to 1439) |
| 6 | 2 | End, minutes (1 to 1440). An end before the start runs past midnight. |
| 8 | 2 | Station name: string index |
| 10 | 2 | Language code: string index |
| 12 | 2 | Target area code: string index |

### String table

A u32 count, then one u32 offset per string (from the start of the table), then the strings, each NUL-terminated
UTF-8 and stored once. Names, language codes and target codes share the table. A string is at most 64 bytes, valid
UTF-8, with no control characters (below 0x20, and 0x7F).

## Conversion

From EiBi's columns kHz, Time(UTC), Days, ITU, Station, Lng, Target, Remarks, P, Start and Stop:

- Rows from 150 to 30000 kHz are kept, with the frequency to 0.1 kHz.
- `HHMM-HHMM` gives the start and end; an end of `2400` or `0000` is 1440.
- Days: empty is every day. Day names `Mo Tu We Th Fr Sa Su` and digits 1 (Monday) to 7 (Sunday) can be joined
  (`SaSu`, `1357`), listed with commas (`Tu,Th`) or given as ranges, which may wrap (`We-Mo`). Any other form
  (`irr`, `alt`, a date such as `29Dec`, `2.Su`) is irregular.
- Station, language and target are converted from Latin-1 to UTF-8, trimmed, stripped of control characters and
  cut to 64 bytes.
- Rows that give the same entry are kept once. ITU, Remarks, P, Start and Stop are not used.
- A row whose frequency or time cannot be read is an error; with any error no table is written.

## Checks

The radio checks a table before it uses it, at boot and when a new one arrives, and `--verify` runs the same
checks: the magic, format and header length; the total length against the data and the 480 KB slot; the CRC; the
entries and string table in place; the entries sorted, with minutes and tenths in range and every string index in
range; every string offset inside the table, each string terminated within 64 bytes, valid UTF-8 and free of
control characters. A table that fails any check is not used.

## Manifest

`v1/manifest.json` describes `v1/stations.bin`:

```json
{"format": 1, "season": "B26", "entries": 8780, "length": 137325, "sha256": "…",
 "source_modified": "2026-10-31T12:00:00Z", "csv": "eibi/sked-b26.csv"}
```

The radio and its web page compare `sha256` with the loaded table's to tell whether an update is needed, and check
a downloaded table's length and SHA-256 against it before they use it.
