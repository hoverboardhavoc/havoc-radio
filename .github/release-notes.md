Havoc Radio @VERSION@ and the Havoc Radio Installer @VERSION@.

| File | Use |
|---|---|
| `havoc-radio-installer.bin` | The Havoc Radio Installer for an ESP32-C3 SuperMini, carrying Havoc Radio @VERSION@. Flash it once over USB at `0x0`: `esptool.py --chip esp32c3 -p PORT write_flash 0x0 havoc-radio-installer.bin`. Then follow [docs/installer.md](https://github.com/hoverboardhavoc/havoc-radio/blob/main/docs/installer.md). |
| `havoc-radio.bin` | Havoc Radio for a K-Radio that already runs it: upload it on the radio's web page under Firmware, then press the knob. |
| `havoc-radio-install.bin` | Havoc Radio as the installer carries it: upload it on the installer's page under Carried firmware to install this version with an older installer. |
| `installer.bin` | The installer alone, to update a running installer from its page under Installer. |
| `SHA256SUMS` | SHA-256 checksums of the four files. |

Using the radio: [docs/user-guide.md](https://github.com/hoverboardhavoc/havoc-radio/blob/main/docs/user-guide.md).

The firmware includes Silicon Labs' Si4732 SSB patch, fetched at build time from the
[PU2CLR SI4735 library](https://github.com/pu2clr/SI4735), for LSB and USB.
