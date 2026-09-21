# NetDash

A home-network dashboard that runs entirely on a **Waveshare ESP32-C6-GEEK**
USB dongle.

Plug it into any USB port for power. It joins your Wi-Fi, quietly discovers
every device on the LAN, works out what each one is, and serves a dark
single-page dashboard at `http://netdash.local`. The 1.14" LCD shows its own IP
so you always know where to find it.

- **Passive by design.** One ICMP echo per host every few minutes at a few
  hosts per second, plus mDNS / SSDP / NetBIOS / reverse-DNS listening. Nothing
  a router logs as a scan.
- **Names things.** Vendor from the IEEE OUI table, hostname from mDNS,
  the router's DHCP leases, or NetBIOS; and a nickname you set yourself, which
  survives reboots.
- **No cloud, no app.** Everything is on the dongle.

Status: feature-complete and running on hardware. The firmware boots, brings up
the LCD, starts the setup access point, serves the dashboard and starts the
scanner and discovery tasks. The step that still needs a human is the one only
you can do: joining the setup access point and entering your Wi-Fi password.
The LAN-side behaviour (discovery, naming, classification) gets its first real
exercise after that.

See `docs/API.md` for the REST contract and `CLAUDE.md` for the module map and
conventions.

## Hardware

Waveshare ESP32-C6-GEEK: ESP32-C6, 16 MB flash, 1.14" ST7789 240x135 IPS LCD,
BOOT button, USB-A plug. Nothing else is needed.

## Build and flash

Requires ESP-IDF v5.5.

```powershell
# The IDF virtualenv is Python 3.11. Put it ahead of any newer python on PATH
# first, or export.ps1 cannot find its environment.
$env:PATH = "C:\Users\PhilStrong\.espressif\tools\idf-python\3.11.2;" + $env:PATH
. C:\Users\PhilStrong\esp\v5.5\esp-idf\export.ps1
idf.py set-target esp32c6        # once
idf.py build
idf.py -p COM4 flash monitor     # Ctrl+] exits the monitor
```

```bash
# Linux / macOS
. ~/esp/v5.5/esp-idf/export.sh
idf.py set-target esp32c6
idf.py build
idf.py -p /dev/ttyACM0 flash monitor
```

Board pins, scan rate and the default hostname live under **NetDash** in
`idf.py menuconfig`.

## Updates

Once one build with update support is on the dongle (over USB, as above), later
versions arrive by themselves: it checks the GitHub repository's latest release
twice a day and installs anything newer, with an automatic rollback if the new
version fails to start. **docs/UPDATES.md** covers setting up the repository,
the read-only token a private one needs, and publishing a release.

## First run

1. With no saved credentials the dongle starts a softAP. The LCD shows the
   SSID (`NetDash-XXXX`), the password, and `http://192.168.4.1`.
2. Join that network, open the page, go to **Settings**, pick your Wi-Fi from
   the scan list and save.
3. The dongle reconnects to your LAN and the LCD shows its new IP. The
   dashboard is then at `http://netdash.local`.

Clicking the BOOT button cycles the LCD pages. Holding it for 5 seconds wipes
the Wi-Fi credentials and returns the dongle to setup mode.

## Checking it works

Once the dongle is on your LAN:

```bash
python tools/smoke_test.py http://netdash.local
```

That exercises every REST endpoint, checks the response shapes against
`docs/API.md` and round-trips a nickname. It changes nothing permanent: the
reboot and factory-reset endpoints are only probed with an invalid body, to
confirm they refuse it.

To work on the dashboard without touching hardware, run the mock API. It serves
the real `index.html` against a fake version of a home network:

```bash
python tools/mock_api.py     # http://127.0.0.1:8080
```
