# LANDA.SH

**The standalone homelab dashboard that maintains itself.**

A USB dongle that finds every device and service on your network and builds
your dashboard for you. No server, no Docker, no config files.

**[landa.sh](https://landa.sh)** (project page and browser installer)
· [Firmware releases](https://github.com/ThePhilStrongProject/landash-releases)
· [Changelog](https://github.com/ThePhilStrongProject/landash-releases/blob/main/CHANGELOG.md)

## How it works

1. **Plug it in.** Any USB port. It joins your Wi-Fi and shows its address on
   screen.
2. **It finds everything.** Devices, their names, and the services each one
   runs.
3. **Pin what you use.** One click turns a found service into a tile.

## What it does

- **Finds every device.** A paced ping and ARP sweep every few minutes, with
  names from mDNS, SSDP, NetBIOS and the router's reverse DNS, and the maker
  from the MAC address. Each device gets a type, a 24-hour presence history,
  notes, and a name you can change.
- **Finds what each device runs.** A slow background port scan, as deep as you
  choose, repeated on a schedule so services that stop are dropped.
- **Never goes stale.** Tiles follow the device through IP changes and show
  whether each service is up.
- **Tells you what changed.** New devices, services opening or closing, and
  internet or DNS outages.
- **Passwords beside services.** An encrypted vault for your router and NAS
  logins, unlocked with your passphrase. Read [Security](#security) first.
- **Updates itself.** New releases install automatically and roll back if
  they fail.

It also has five themes, four levels of detail and a welcome tour.

## What you need

- A Waveshare **ESP32-C6-GEEK**. Several revisions exist with different screen
  wiring; see [Hardware](#hardware).
- Chrome or Edge on a computer, to install from [landa.sh](https://landa.sh/#install).
  Or **ESP-IDF v5.5**, to build it yourself.
- A 2.4 GHz Wi-Fi network. The ESP32-C6 does not do 5 GHz.

## Getting started

### 1. Install

Plug the dongle into your computer, open [landa.sh](https://landa.sh/#install)
in Chrome or Edge and press **Install**. It writes the latest release from
[landash-releases](https://github.com/ThePhilStrongProject/landash-releases).
Tick **Erase device** for a new board; leave it unticked to keep your data.

To build it yourself, install
[ESP-IDF v5.5](https://docs.espressif.com/projects/esp-idf/en/v5.5/esp32c6/get-started/),
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

1. Join the `LANDA-XXXX` network shown on the dongle's screen, using the
   password shown under it.
2. Open `http://192.168.4.1` and pick your Wi-Fi.
3. Go back to your own Wi-Fi and open `http://landash.local`, or the address
   on the screen.

A welcome tour shows you round the first time. The **BOOT** button flips the
screen's pages; holding it for five seconds forgets the Wi-Fi details and
returns the dongle to setup mode.

### 3. Updates

The dongle checks
[landash-releases](https://github.com/ThePhilStrongProject/landash-releases)
twice a day and installs new firmware by itself. You can turn that off, or
have it wait for you, under **Settings › Maintenance**.

> **If you build your own firmware**, point it at your own releases: set
> *LANDA.SH › GitHub repository updates come from* in `idf.py menuconfig`
> (`CONFIG_NETDASH_OTA_REPO`). Otherwise the dongle installs the maintainer's
> releases over your changes. [docs/UPDATES.md](docs/UPDATES.md) explains how
> releases are published.

## Security

LANDA.SH is meant for a home network you trust.

- **No login.** Anyone who can reach the dashboard can use it, including its
  settings.
- **Plain HTTP.** A browser can't trust a certificate for a device on a home
  network without warnings, so the dashboard doesn't use one.
- **The vault** encrypts logins on the dongle with a key derived from your
  passphrase, and never stores the key. But the passphrase, and a login when
  you view it, cross your network unencrypted. It protects against someone
  stealing the dongle, not someone watching your network.
- **Port scanning** is slow and paced, but an intrusion detection system will
  still notice it. You can turn it off.

[SECURITY.md](SECURITY.md) has the details and how to report a problem.

## Hardware

| | |
|---|---|
| Board | Waveshare ESP32-C6-GEEK: ESP32-C6, 2.4 GHz Wi-Fi 6, 16 MB flash, no PSRAM |
| Screen | 1.14" ST7789 IPS, 240 × 135 |
| Default screen pins | SCLK 1, MOSI 2, DC 3, RST 4, CS 5, backlight 6 |
| Button | BOOT (GPIO 9) |

[Waveshare's page](https://www.waveshare.com/wiki/ESP32-C6-GEEK) ·
[One place to buy it](https://www.aliexpress.com/item/1005011960314326.html).
Not an affiliate link. LANDA.SH isn't connected to Waveshare or the seller and
can't vouch for either. It must be the ESP32-C6-GEEK; other boards won't work.

If the screen stays blank or shows garbage, your board is a different revision:
choose *Custom pins* under **LANDA.SH** in `idf.py menuconfig` and set the pins
there. The screen's orientation and colour options are in the same menu.

The dongle keeps up to 144 devices active in RAM and remembers up to 1,024 in
flash. When the active list is full, the device offline longest is stored
away with its name, notes and ports, and comes back if it reappears. Stored
devices are under *Show them* on the Devices page.

## For developers

- [docs/API.md](docs/API.md): the REST API the dashboard uses. Everything goes
  through it, so it's also how to script the dongle.
- [CLAUDE.md](CLAUDE.md): the developer's guide, with the module map, coding
  conventions, version numbering, and known traps in the scanner, HTTP server
  and updates. Written for AI coding agents and people alike.
- [CONTRIBUTING.md](CONTRIBUTING.md): how to build, test and propose a change.
- `python tools/mock_api.py` serves the dashboard against a fake network at
  `http://127.0.0.1:8080`, for working on the web interface without hardware.
- `python tools/smoke_test.py http://landash.local` exercises every endpoint of
  a live dongle against the API documentation.

## Acknowledgements

Built on [ESP-IDF](https://github.com/espressif/esp-idf) and
[LVGL](https://lvgl.io). Manufacturer names come from the IEEE's public
registry of MAC address blocks. [THIRD_PARTY.md](THIRD_PARTY.md) lists every
component and its licence.

## Licence

Copyright (C) 2026 Philip Strong.

LANDA.SH is free software under the [GNU General Public License v3.0](LICENSE).
You may use, change and share it, including selling it, as long as anything
you distribute that is based on it comes with its source under the same
licence. The components it builds on keep their own licences; see
[THIRD_PARTY.md](THIRD_PARTY.md).
