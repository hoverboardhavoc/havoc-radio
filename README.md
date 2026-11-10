# Havoc Radio

Havoc Radio is open firmware for the Xiao K Workshop (小K制作所) "Radio Studio" pocket receiver, an ESP8266 and
Si4732 radio sold on AliExpress. It replaces the stock firmware, whose menus are only in Chinese, with an interface
driven by the radio's wheel, in English and 40 other languages.

![The radio, front and back](docs/images/radio.jpg)

![Havoc Radio's screens: the splash, tuning in AM, FM and LSB, the main menu, the band list, presets, WiFi and the
language list](docs/images/screens.png)

## Features

- It receives FM from 64 to 108 MHz, and LW, MW and SW from 150 kHz to 30 MHz in AM, LSB and USB.
- The wheel tunes. Holding it while turning sets the volume, and a long press saves the frequency as a preset (up
  to 20).
- The band list holds every broadcast and amateur band from LW to 10 m. Each band remembers its last frequency.
- Seek runs up and down on FM and AM.
- On FM it shows the station's RDS name on the home screen, and its programme type and RadioText on a Station
  screen. The web page adds the traffic flags, the station code and the station's other frequencies.
- The menus set the step, bandwidth, AGC, display contrast and the light.
- The side button mutes, steps to the next preset or goes back, as chosen in Settings.
- The interface comes in 41 languages, each listed under its own name.
- A web page on the local network shows the frequency and signal live, tunes the radio, edits presets, sets up
  WiFi and uploads firmware. A new firmware only installs after a press of the knob on the radio.
- The radio sets its clock in UTC from NTP while it is on a network and shows the time on the home screen. NTP is
  its one outside network connection, and Settings turns it off. Without a home network it runs its own access
  point `Havoc-Radio`.
- Havoc Radio installs over WiFi on a radio with the stock firmware, through the radio's own update menu, with no
  soldering: [docs/installer.md](docs/installer.md).

## Documentation

- [docs/user-guide.md](docs/user-guide.md): using Havoc Radio: the controls, the screens and every menu.
- [docs/installer.md](docs/installer.md): installing Havoc Radio with the Havoc Radio Installer on an ESP32-C3
  SuperMini, over WiFi on a radio with the stock firmware or over UART on any radio; flashing with a USB-UART
  adapter; updates.
- [docs/hardware.md](docs/hardware.md): the radio, where to buy it, its parts, pins and flash layout.
- [docs/translations.md](docs/translations.md): how the translations are kept and checked.
- [docs/build-radio.md](docs/build-radio.md) and [docs/build-installer.md](docs/build-installer.md): building and
  testing the firmware and the installer.
- [radio/ui/prototype.html](radio/ui/prototype.html): the UI reference. Open it in a browser to try every screen;
  its fonts, string tables and behaviour are what the firmware renders.

## The radio

Where to buy it: [Aisanray Tech Official Store on AliExpress](https://s.click.aliexpress.com/e/_c3crzxeF)
(affiliate link). [docs/hardware.md](docs/hardware.md) has the radio's other names, its parts and pins, and the
flash layout.

## Building

| Folder | Contents | Build |
|---|---|---|
| `radio/` | Havoc Radio, the K-Radio's firmware (ESP8266) | [docs/build-radio.md](docs/build-radio.md) |
| `installer/` | The Havoc Radio Installer for an ESP32-C3 SuperMini | [docs/build-installer.md](docs/build-installer.md) |
| `docs/` | The guides and pictures | |
