# LANDA.SH

**A home-network dashboard that lives on a USB stick.**

LANDA.SH turns a [Waveshare ESP32-C6-GEEK](https://www.waveshare.com/wiki/ESP32-C6-GEEK)
(a small USB dongle with a screen) into a quiet window onto your home network.
Plug it into any USB port for power and it joins your Wi-Fi, finds every
device on your network, works out what each one is, and serves a dashboard you
open in any browser. Everything runs on the dongle: no cloud, no account, no
app.

**[Project page and browser installer](https://thephilstrongproject.github.io/landash-releases/)**
· **[Firmware releases](https://github.com/ThePhilStrongProject/landash-releases)**
· [Changelog](https://github.com/ThePhilStrongProject/landash-releases/blob/main/CHANGELOG.md)

## What it does

- **A start page for your home.** Pin the things you use every day, like your
  router, NAS or Home Assistant, as tiles in named groups, with your own icons.
  A dot on each tile shows whether that service is actually answering, not
  just whether the box is switched on.
- **Every device, found for you.** Everything on your network appears on its
  own, with its maker, its name where it announces one, and a guess at what it
  is. Give devices names you'll recognise. Each one keeps a 24-hour
  availability history, notes, and a list of the ports it has open.
- **Only what matters.** A notification bell for new devices, address changes,
  newly opened ports and internet outages, each switchable on or off.
- **Internet health.** Separate checks for "is there a route out" and "is DNS
  working", on the dashboard and the dongle's screen.
- **A credential vault** for the logins to your services, locked with a
  passphrase that is never stored.
- **Quietly polite.** One gentle sweep of the network every few minutes, at a
  steady pace a router will not mistake for an attack, plus listening for the
  names devices announce about themselves. It also checks, slowly, which
  common services each device offers. You choose how far that goes when you
  first set it up, from off to every port.
- **Keeps itself up to date** over the air, rolling back on its own if a new
  version ever fails to start.
- **Yours to arrange:** five themes, four levels of detail, and a welcome tour
  on first use.

## What you need

- A Waveshare **ESP32-C6-GEEK**. Several revisions exist with different screen
  wiring; see [Hardware](#hardware).
- Chrome or Edge on a computer, to install it from the
  [project page](https://thephilstrongproject.github.io/landash-releases/#install).
  Or **ESP-IDF v5.5**, if you want to build it yourself.
- A 2.4 GHz Wi-Fi network. The ESP32-C6 does not do 5 GHz.

## Getting started

### 1. Install

The quickest way is the browser installer on the
[project page](https://thephilstrongproject.github.io/landash-releases/#install):
plug the dongle in, press **Install** in Chrome or Edge, and it writes the
latest release from
[landash-releases](https://github.com/ThePhilStrongProject/landash-releases).

To build it yourself instead, install [ESP-IDF v5.5](https://docs.espressif.com/projects/esp-idf/en/v5.5/esp32c6/get-started/),
open its terminal (or source `export.sh` / `export.ps1`), then:

```bash
git clone https://github.com/ThePhilStrongProject/landash.git
cd landash
idf.py set-target esp32c6
idf.py build
idf.py -p <port> flash       # e.g. COM4 on Windows, /dev/ttyACM0 on Linux
```

The first build downloads its components (LVGL, mDNS and a button driver), so
it needs the internet and takes a few minutes.

### 2. Connect it to your Wi-Fi

1. With no Wi-Fi saved, the dongle starts its own setup network. Its screen
   shows the network name (`LANDA-XXXX`), the password and the address
   `http://192.168.4.1`.
2. Join that network with a phone or laptop and open the address. A card
   takes you straight to choosing your Wi-Fi.
3. The dongle joins your network and its screen shows its new address.
   Reconnect to your home Wi-Fi and open that address, or
   `http://landash.local`.

A welcome tour shows you round the first time.

The **BOOT** button cycles the screen's pages. Holding it for five seconds
forgets the Wi-Fi details and returns the dongle to setup mode.

### 3. Updates

From then on the dongle checks
[landash-releases](https://github.com/ThePhilStrongProject/landash-releases)
for new firmware twice a day and installs it by itself. You can turn that off, or have it wait for you, under
**Settings › Maintenance**.

> **If you build your own firmware**, point it at your own releases: set
> *LANDA.SH › GitHub repository updates come from* in `idf.py menuconfig`
> (`CONFIG_NETDASH_OTA_REPO`). By default a dongle installs the maintainer's
> releases, which would replace your changes. [docs/UPDATES.md](docs/UPDATES.md)
> explains how releases are published.

## Security: read this before trusting it with anything

LANDA.SH is designed for a home network you trust.

- **The dashboard has no login.** Anyone who can reach it on your network can
  use it, including changing its settings.
- **It is plain HTTP.** A browser cannot trust a certificate for a device on a
  home network without warnings, so the dashboard does not use one.
- **The credential vault** encrypts your logins on the dongle with a key
  derived from your passphrase, and the key is never stored. But the
  passphrase, and a login when you view it, cross your network unencrypted. It
  protects against someone who steals the dongle, not against someone
  watching your network.

[SECURITY.md](SECURITY.md) has the full picture and how to report a problem.

## Hardware

| | |
|---|---|
| Board | Waveshare ESP32-C6-GEEK: ESP32-C6, 16 MB flash, no PSRAM |
| Screen | 1.14" ST7789 IPS, 240 × 135 |
| Default screen pins | SCLK 1, MOSI 2, DC 3, RST 4, CS 5, backlight 6 |
| Button | BOOT (GPIO 9) |

If the screen stays blank or shows garbage, your board is a different revision:
choose *Custom pins* under **LANDA.SH** in `idf.py menuconfig` and set the pins
there. The screen's orientation and colour options are in the same menu.

The dongle keeps up to 144 devices active in its 512 KB of RAM, and remembers
up to 1,024 in flash. When the active list is full, the device that has been
offline longest is put away, with its name, notes and everything else, and
comes straight back if it reappears. Devices put away are under *Show them*
on the Devices page.

## For developers

- [docs/API.md](docs/API.md): the REST API the dashboard is built on. Every
  feature goes through it, so it is also the way to script the dongle.
- [CLAUDE.md](CLAUDE.md): the developer's guide. It has the module map, the
  coding conventions, the version-numbering scheme, and the hard-won traps
  worth reading before changing the scanner, the HTTP server or updates. It
  is written for AI coding agents and people alike.
- [CONTRIBUTING.md](CONTRIBUTING.md): how to build, test and propose a change.
- `tools/mock_api.py` serves the real dashboard against a fake network, for
  working on the web interface without hardware:
  `python tools/mock_api.py`, then open `http://127.0.0.1:8080`.
- `tools/smoke_test.py http://landash.local` exercises every endpoint of a
  live dongle and checks the responses against the API documentation.

## Acknowledgements

Built on [ESP-IDF](https://github.com/espressif/esp-idf) and
[LVGL](https://lvgl.io). Manufacturer names come from the IEEE's public
registry of MAC address blocks. [THIRD_PARTY.md](THIRD_PARTY.md) lists every
component and its licence.

## Licence

Copyright (C) 2026 Philip Strong.

LANDA.SH is free software, released under the
[GNU General Public License v3.0](LICENSE): you may use, change and share it,
including selling it, as long as anything you distribute that is based on it
comes with its source under the same licence. The components it builds on
keep their own licences; see [THIRD_PARTY.md](THIRD_PARTY.md).
