# Guide test tables

Frequency guide tables (format 1) for the reader's host tests, the screen replay and the page tests: `b26.bin` and
`a26.bin` from EiBi's B26 and A26 CSVs, `b26.bin.json` (the manifest the converter writes for `b26.bin`),
`rows.bin` (rows for every day form, time and frequency rule), `scan.bin` (rows for the guide scan) and
`sample.bin` (the UI prototype's sample guide).

`radio/test/make_guide_tables.py` writes them with the converter of
[eibi-binary-table](https://github.com/hoverboardhavoc/eibi-binary-table), which defines the format:

```
python3 radio/test/make_guide_tables.py <eibi-binary-table checkout>
```
