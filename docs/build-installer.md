# Building the Havoc Radio Installer

The Havoc Radio Installer, the firmware for the ESP32-C3 SuperMini that installs Havoc Radio on a K-Radio, lives in
`installer/`. It builds with ESP-IDF, separately from the radio's ESP8266 SDK.

## Requirements

ESP-IDF v5.4.2, at `~/esp/esp-idf` or at the path given as `IDF5=<path>`, and everything
[build-radio.md](build-radio.md) needs: the installer carries a Havoc Radio build.

## Build

```sh
make installer
```

`make installer` builds Havoc Radio and its install image first (`make firmware`, `make install-image`), then the
installer app. It writes two files:

| File | Use |
|---|---|
| `installer/build/havoc-radio-installer.bin` | The whole flash: bootloader, partition table, app and the carried Havoc Radio image. Flash it once over USB at `0x0` ([installer.md](installer.md), "Flash the SuperMini"). |
| `installer/build/installer.bin` | The app alone, for updating an installer over WiFi. |

The build checks the merged image: the carried image is byte-identical to the install image, the page holds the
wiring diagram and the stock pictures, and the app fits its slot. A stale `installer/sdkconfig` overrides
`installer/sdkconfig.defaults`; delete it after changing the defaults.

## Update an installer over WiFi

From the installer's page: Installer, then Update installer, and choose `installer/build/installer.bin`. From a
computer on the same network:

```sh
installer/tools/installer-update --host <address> installer/build/installer.bin
```

`--host` defaults to `192.168.4.1`, the address on the installer's access point `Havoc`. The installer writes the
new app next to the running one and goes back to the old one if the new one does not confirm itself within 30 s.

## Tests

`make test` runs the installer's host tests in `installer/test/`:

- its C modules on the host;
- the update protocol and DNS against a host build of its servers;
- `radio-flash` and `installer-update` against fake installers;
- the page's JavaScript under Node (`NODE=<path>` if `node` is not on the path).

## Layout

| Path | Contents |
|---|---|
| `installer/main/` | The app: access point and DNS, the update server, the UART install, the console bridge, self-update, the LED |
| `installer/page/index.html` | The installer's web page; `installer/tools/make_page.py` inlines the wiring diagram and the stock pictures |
| `installer/tools/` | `radio-flash`, `installer-update`, the page and stage builders |
| `installer/test/` | Host tests, fake installers and the stock client emulator |
| `installer/partitions.csv` | Two app slots of 1.25 MB and the stage for the carried image |
