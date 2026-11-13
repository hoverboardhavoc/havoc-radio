# Guide test tables

Frequency guide tables (format 1) for the reader's host tests, the screen replay and the page tests: `b26.bin` and
`a26.bin` from EiBi's B26 and A26 CSVs, `b26.bin.json` (the manifest the converter writes for `b26.bin`),
`rows.bin` (rows for every day form, time and frequency rule) and `sample.bin` (the UI prototype's sample guide).

They are written by the converter tests of
[eibi-binary-table](https://github.com/hoverboardhavoc/eibi-binary-table) at commit `39749a8`:

```
python3 tools/test_guide_table.py OUT_DIR
```
