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

The build also fetches the current frequency guide table, `v1/stations.bin`, from the data repository
[eibi-binary-table](https://github.com/hoverboardhavoc/eibi-binary-table) into `radio/build/guide/`, checks its
length and SHA-256 against `v1/manifest.json` and its magic and format, and flashes it at the guide partition
(`0x10000`) with the app, so a UART install and the install image carry a starting guide. The radio runs the
format's full checks before it uses a table. When the fetch or the check fails the build prints a warning and
continues; the radio then starts with no guide. The format and its converter belong to the data repository
([FORMAT.md](https://github.com/hoverboardhavoc/eibi-binary-table/blob/main/FORMAT.md)).

## Tests

`make test` builds and runs the host tests in `radio/test/`:

- the font tables against the prototype;
- 278 framebuffer goldens;
- the navigation and text entry rules;
- the band, step and preset math;
- the encoder decoder and the button timing;
- the input script parser and the injection of its steps among the hardware's events;
- the fit of every string in every language on every screen;
- the receiver driver's opcode guard;
- the guide reader on the data repository's tables (`radio/test/guide/`) and hostile ones, the on-air rule, the
  two slots, and the download mode with a fake transport, for the guide and, with a fake app slot, for the firmware;
- the web page's functions under Node, the guide update against a fake manifest and table among them;
- the release job's firmware branch step against local repositories (`radio/test/test_firmware_branch.py`).

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

## Bench

`GET /api/screen` returns the last frame sent to the display as a binary PBM (`P4`, 128 × 32, lit pixels 1, as in
the goldens). `radio/tools/pbm2png.py` renders it:

```sh
curl -s http://radio.local/api/screen -o screen.pbm
python3 radio/tools/pbm2png.py screen.pbm .   # writes screen.png
```

`POST /api/input` presses the radio's controls from a script. It takes lines of the input syntax of
`radio/test/scenarios.txt`: `turn N` (N detents, negative anticlockwise), `press`, `long`, `side`, `sidehold` and
`wait MS` (at most 10,000), at most 64 lines and 1 KB, with `#` starting a comment. The events are put into the
queue that the encoder and the buttons fill, after the hardware's events, one step per pass of the UI loop; `long`
and `sidehold` are the decoded long press and hold. The answer is sent once the UI loop has taken every step and
drawn a frame after the last: 200 with `{"steps": n}`, or 503 with the steps taken so far when that takes over 20 s
(the rest are still taken). A line that is not understood is answered with 400 and `{"line": n}` (line 0 for a body
over 1 KB); a request made while an earlier script is still being taken is answered with 409. Injected events count
as user activity. On the firmware confirm screen they are dropped, so an upload is installed only by the real knob.
The web page's Screen view sends the knob and the side button under the screen this way.

`radio/tools/radio_screens.py` runs a script of such lines against a radio and compares the captures of two runs.
Each line is sent as its own request. A script may also contain:

| Line | Effect |
|---|---|
| `shot NAME` | The screen is captured to `NAME.pbm` and `NAME.png` in the output directory. |
| `mask X Y W H` | The rectangle is left out of the comparison of the next `shot`; several may be given. |
| `tune KHZ`, `mode M`, `band B` | The radio is set through `/api/tune`, `/api/mode` or `/api/band`. |

`radio/test/bench/screens.txt` walks home on 6070 kHz, the main menu, the Station screen, Settings and each of its
pages and the lamp screen, with masks over the clock, the signal bars, the on-air names, the scanned networks, the
battery and the guide entries. It is run twice and the runs are compared:

```sh
python3 radio/tools/radio_screens.py run radio/test/bench/screens.txt --host radio.local --out radio/build/screens/a
python3 radio/tools/radio_screens.py run radio/test/bench/screens.txt --host radio.local --out radio/build/screens/b
python3 radio/tools/radio_screens.py compare radio/build/screens/b radio/build/screens/a
```

The masks of each capture are written to `masks.json` in the output directory. `compare` checks every capture
against the one of the same name in the reference directory outside the masks of either run, writes a side-by-side
PNG of each one that differs to `diff/` (the capture, the reference and the differing pixels in red), and exits
with status 1 when any capture differs. Only the Python standard library is needed.

## Flash

The Havoc Radio Installer puts the build on a K-Radio over WiFi or over UART: [installer.md](installer.md). Its
"Without the installer" section flashes with a USB-UART adapter and esptool. Once Havoc Radio runs, updates go
through its web page: Firmware, choose `radio/build/havoc-radio.bin`, Upload, then press the knob on the radio
within 30 s. A radio also installs the latest release itself, from Settings > Firmware > Update (below).

## Releases

A tag `vYYYY.WW.P` (year, ISO week, patch) builds and publishes a release with the four binaries
(`.github/workflows/build.yml`). The tag must match the versions in `radio/main/main.cpp` and `installer/version.txt`.
The release job then commits the app image and its manifest to the orphan branch `firmware`, from which radios update
themselves:

| Path | Contents |
|---|---|
| `layout1/havoc-radio.bin` | The app image of the latest release, the same file as the release's `havoc-radio.bin` |
| `layout1/manifest.json` | `{"version": "2026.47.0", "length": n, "sha256": hex, "released": iso8601}` |
| `README.md` | What the branch is, and the releases page for the full history |

`layout1` is the flash layout of `radio/partitions.csv`, with two 1 MB app slots at `0x100000` and `0x200000`. A
layout that needs a reinstall gets its own folder, `layout2/`, and radios on layout 1 keep reading `layout1/`. The
branch keeps its history; each release adds about 1 MB. raw.githubusercontent.com serves each file with one request,
so the radio needs one TLS connection for the manifest and one for the image.

`radio/tools/firmware_branch.py` makes the commit from git objects alone, as `github-actions[bot]` with the message
`Havoc Radio <version>`, and pushes it with the job's own token (`contents: write`). The first release creates the
branch with no parent; later ones add a commit on top and keep any folder they do not write. A repeated release
commits nothing.

The radio reads the branch named by the build setting `HAVOC_FIRMWARE_BRANCH`, `firmware` by default
(`./radio/tools/idf.sh menuconfig`, menu Havoc Radio, or `CONFIG_HAVOC_FIRMWARE_BRANCH="<branch>"` in
`radio/sdkconfig`). A build pointed at a branch with a test manifest and image checks and installs from there.

## Layout

| Path | Contents |
|---|---|
| `radio/CMakeLists.txt`, `radio/sdkconfig.defaults` | The ESP8266_RTOS_SDK project |
| `radio/partitions.csv` | The partition table: two 1 MB app slots |
| `radio/main/` | Startup and the UI loop |
| `radio/components/` | `board`, `i2c`, `si4732`, `ssd1306`, `gfx`, `ui`, `input`, `radio`, `rds`, `clock`, `lamp`, `store`, `net`, `web`, `power`, `guide`, `download` (the download mode for the guide and the firmware) |
| `radio/components/web/page/index.html` | The web page served at `/` |
| `radio/ui/prototype.html` | The UI reference |
| `radio/tools/` | SDK setup, font and string generators, golden renderer, PBM to PNG, SSB patch and guide fetch, guide table converter, `idf.sh`, install image builder, app size check, firmware branch commit, the bench screen tool |
| `radio/test/` | Host tests, scenarios and goldens; `bench/`, the bench screen script |
