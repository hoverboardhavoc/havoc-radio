# Havoc Radio firmware

This branch holds the app image of the latest Havoc Radio release, which a radio downloads in Settings > Firmware >
Update. The release workflow writes it; every release, with the install image and the installer, is on the
[releases page](https://github.com/hoverboardhavoc/havoc-radio/releases).

Each flash layout has its own folder. `layout1` is the layout with two 1 MB app slots at `0x100000` and `0x200000`:

- `layout1/havoc-radio.bin`: the app image, the same file as the release's `havoc-radio.bin`.
- `layout1/manifest.json`: its version, its length in bytes, its SHA-256 and the time of the release.
