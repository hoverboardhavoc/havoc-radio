# Building Havoc Radio

Havoc Radio, the firmware for the K-Radio's ESP8266, lives in `radio/`. It builds with Espressif's
ESP8266_RTOS_SDK, which the build installs under `radio/third_party/`.

## Requirements

macOS or Linux, Python 3, CMake, Ninja, git and a C++17 host compiler (for the tests).

## Build

```sh
make setup      # SDK v3.4, xtensa-lx106 toolchain and a Python venv, all under radio/third_party/
make test       # host tests
make firmware   # radio/build/havoc-radio.bin
make install-image  # radio/build/havoc-radio-install.bin, the image the installer carries
```

`make setup` downloads ESP8266_RTOS_SDK v3.4 and the gcc 8.4.0 (esp-2020r3) toolchain into `radio/third_party/`
(git-ignored). On Apple silicon the toolchain runs under Rosetta. `radio/tools/idf.sh` runs `idf.py` on `radio/`
with that environment and `radio/build/` as the build directory, so any `idf.py` command works through it, for
example `./radio/tools/idf.sh menuconfig` or `./radio/tools/idf.sh size`.

The partition table, `radio/partitions.csv`, has two 1 MB app slots, `ota_0` at `0x100000` and `ota_1` at
`0x200000`. The ESP8266 runs its app from one 1 MB window of flash, so each slot fills a window from offset 0. The
build fails when `havoc-radio.bin` is longer than a slot. A stale `radio/sdkconfig` overrides
`radio/sdkconfig.defaults`; delete it after changing the defaults.

The build fetches the Si4732 SSB patch from the PU2CLR SI4735 library on GitHub into `radio/build/ssb/`. The patch
is not part of this repository. Without network access the firmware still builds, without LSB and USB.

## Tests

`make test` builds and runs the host tests in `radio/test/`:

- the font tables against the prototype;
- 118 framebuffer goldens;
- the navigation and text entry rules;
- the band, step and preset math;
- the encoder decoder and the button timing;
- the fit of every string in every language on every screen;
- the receiver driver's opcode guard.

`STOCK_IMAGE=<path>` adds tests against a 4 MB flash backup of a K-Radio on the stock firmware.

It also builds the firmware and the install image and checks the image (`radio/test/test_install_image.py`), then
runs the installer's tests ([build-installer.md](build-installer.md)).

The goldens in `radio/test/golden/` are rendered by the UI prototype, `radio/ui/prototype.html`. After a change to
the prototype or to `radio/test/scenarios.txt`:

```sh
make goldens    # runs the prototype in QuickJS and rewrites radio/test/golden/*.pbm
make pngs       # renders every golden to radio/build/png/ at 4x for review
```

A failing golden comparison writes the firmware's frame to `radio/build/host/actual/`.
`radio/tools/pbm2png.py` renders it.

## Flash

The Havoc Radio Installer puts the build on a K-Radio over WiFi or over UART: [installer.md](installer.md). Its
"Without the installer" section flashes with a USB-UART adapter and esptool. Once Havoc Radio runs, updates go
through its web page: Firmware, choose `radio/build/havoc-radio.bin`, Upload, then press the knob on the radio
within 30 s.

## Layout

| Path | Contents |
|---|---|
| `radio/CMakeLists.txt`, `radio/sdkconfig.defaults` | The ESP8266_RTOS_SDK project |
| `radio/partitions.csv` | The partition table: two 1 MB app slots |
| `radio/main/` | Startup and the UI loop |
| `radio/components/` | `board`, `i2c`, `si4732`, `ssd1306`, `gfx`, `ui`, `input`, `radio`, `rds`, `clock`, `store`, `net`, `web`, `power` |
| `radio/components/web/page/index.html` | The web page served at `/` |
| `radio/ui/prototype.html` | The UI reference |
| `radio/tools/` | SDK setup, font and string generators, golden renderer, PBM to PNG, SSB patch fetch, `idf.sh`, install image builder, app size check |
| `radio/test/` | Host tests, scenarios and goldens |
