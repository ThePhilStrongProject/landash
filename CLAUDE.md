# LANDA.SH — agent guide

## What this is

The product is **LANDA.SH**. Many internal names predate the rename and keep
the old name on purpose - the `netdash` project and image name, `NETDASH_*`
Kconfig symbols and `netdash_*` C identifiers, the `netdash-*` browser
storage keys. Renaming those would break updates, stored settings or
people's saved preferences for no visible gain. Anything a user or a reader
of the docs sees says LANDA.SH.

Firmware for a **Waveshare ESP32-C6-GEEK** USB dongle that acts as a passive
home-network dashboard:

- Joins the home Wi-Fi (credentials saved from a web Settings page) or hosts a
  softAP when none are stored.
- Quietly discovers every device on the LAN, tracks IP and MAC, auto-identifies
  what each device is, and lets the user set a persistent nickname per device.
- Serves a dark single-page web dashboard from flash.
- Shows its own IP (or the AP setup details) on the 1.14" LCD.

Target: ESP-IDF **v5.5**, ESP32-C6, 16 MB flash, no PSRAM.

## Hardware facts

| Item | Value |
|---|---|
| SoC | ESP32-C6, 16 MB flash, no PSRAM |
| LCD | 1.14" ST7789 IPS 240x135, 4-wire SPI, no MISO, 40 MHz OK |
| LCD pins (GEEK_V2 default) | SCLK 1, MOSI 2, DC 3, RST 4, CS 5, backlight 6 |
| Button | BOOT on GPIO 9 |
| Panel offsets | x 40, y 53 (52 with `NETDASH_LCD_ROTATE_180`); landscape = rotate 270; colours inverted |
| Port | "USB Serial Device", console over USB Serial/JTAG. Was COM4, a second board came up as COM5 - check `[System.IO.Ports.SerialPort]::GetPortNames()` |

**Several hardware revisions of this board exist with different LCD wiring.**
The pin defaults in `main/Kconfig.projbuild` must not be changed without
confirming them against the physical board (or Waveshare's own demo). If a
board does not match, select `NETDASH_BOARD_CUSTOM` in menuconfig and set the
pins there — do not edit the `GEEK_V2` defaults.

## Build / flash / monitor (Windows PowerShell)

```powershell
# This machine has Python 3.14 first on PATH, but the IDF venv is 3.11, so
# export.ps1 fails unless the IDF python is put in front of it first:
$env:PATH = "C:\Users\PhilStrong\.espressif\tools\idf-python\3.11.2;" + $env:PATH
. C:\Users\PhilStrong\esp\v5.5\esp-idf\export.ps1

idf.py set-target esp32c6        # once, or after deleting sdkconfig
idf.py build
idf.py -p COM4 flash monitor     # Ctrl+] exits the monitor
idf.py menuconfig                # project options live under "LANDA.SH"
```

`idf.py fullclean` if the component manager or the partition table gets
confused. `sdkconfig` is generated and gitignored; edit `sdkconfig.defaults`
and re-run `idf.py set-target esp32c6` (or `idf.py reconfigure`) instead.

Sources are globbed by `main/CMakeLists.txt`, so **never add files to a SRCS
list** — just create the `.c` under `main/` and run `idf.py reconfigure` if the
build does not pick it up.

## Version numbering

Every release is `vMAJOR.MINOR.PATCH`, marked by an annotated git tag on the
commit it was built from. Dongles compare the three numbers to decide whether
a release is newer (`net/ota.c`), so the format is load-bearing: no suffixes,
no leading zeros, always the `v`.

**Which number to bump** - pick the highest that applies:

| Bump | When | Examples |
|---|---|---|
| MAJOR | A change a dongle cannot take over the air, or that breaks what users or other tools rely on. Needs a USB reflash, or loses or reinterprets stored data. | a new partition table, a bootloader change, dropping a REST endpoint or field, a settings layout that is not append-only |
| MINOR | Something a user would call new, or a change to what is stored or served that older firmware would not understand. Always over the air. | a new feature or page, a new settings field, a new NVS blob version, a new REST endpoint or field |
| PATCH | Fixes and polish with no new stored data and no API change. | a bug fix, clearer wording, a layout tweak, a performance fix |

- **Before 1.0** the same rules apply one place down in spirit, but not in
  numbers: MAJOR stays 0, and a MAJOR-class change bumps MINOR and says so
  loudly in the tag message. 1.0 is the first release the maintainer is
  willing to support for other people.
- Bumping MINOR resets PATCH to 0; bumping MAJOR resets both.
- The 0.14.x series broke the MINOR rule (new features and a new settings
  field shipped as patches). It stands as history; from 0.15.0 on, follow the
  table.

**A tag is the release.** `tools/release.py` refuses to publish unless HEAD is
exactly on a tag and the built image carries that same version, and the tag
message becomes the release's public changelog entry in the releases
repository - so write it for someone who owns a dongle, not for someone
reading the source. Once a tag has been pushed, never move or reuse it: a
fixed release is a new PATCH. An unpushed tag may be moved (`git tag -d`, then
tag again) when a fix is found before publishing.

**How the version gets into the firmware.** `PROJECT_VER` is deliberately not
set, so ESP-IDF derives it at build time with
`git describe --always --tags --dirty`. That string is baked into the app
descriptor, returned as `fw` by `GET /api/status`, and shown on the System tab.
(`idf` is a different field: the SDK version.)

| Repo state | `fw` reads | Updated over the air? |
|---|---|---|
| exactly on a tag, clean tree | `v0.15.0` | yes, when a newer release appears |
| three commits past a tag | `v0.15.0-3-g0fd17ce` | yes; counts as `v0.15.0` |
| uncommitted changes | the above plus `-dirty` | no - a development build is left alone |
| no tag reachable | a bare commit hash | no |

**Commit and tag before building the image you intend to keep.** Building
first stamps the binary with the *previous* commit plus `-dirty`, which is how
a device once reported `a53e2ce-dirty` while HEAD was two commits further on.
For throwaway test flashes `-dirty` is expected and fine.

```bash
git tag -a v0.15.0 -m "What changed, for someone who owns a dongle"
idf.py build
python tools/release.py        # checks, then commits into ../landash-releases
```

docs/UPDATES.md has the whole publishing procedure.

## Module map

```
main/
  main.c                    app_main: tls_mem -> nvs -> event loop -> netif ->
                            settings -> display -> button -> wifi_mgr ->
                            device_db -> http_server -> scanner, starts a 30 s
                            heap heartbeat on an esp_timer, and returns, which
                            frees the main task's 8 KB stack. Defines the
                            NETDASH_EVENT base.
  app_events.h              NETDASH_EVENT ids and their data structs.
  Kconfig.projbuild         board pin variant, backlight polarity, panel
                            offsets, scan defaults, hostname.
  idf_component.yml         lvgl, esp_lvgl_port, mdns, button.

  settings/settings.c/.h    netdash_settings_t <-> NVS namespace "cfg", one
                            versioned blob, mutex-guarded, copy in / copy out.
                            Generates the softAP password once from the MAC.

  net/wifi_mgr.c/.h         STA with backoff, APSTA fallback, hostname, SNTP,
                            mDNS advertisement. Owns all esp_wifi calls.
  net/scanner.c/.h          paced ICMP echo + ARP lookup sweep of the connected
                            subnet, plus a passive ARP-table snapshot.
  net/disc_mdns.c/.h        mDNS service browse -> hostnames + service bits.
  net/disc_ssdp.c/.h        SSDP M-SEARCH -> SERVER / LOCATION / friendlyName.
  net/disc_nbns.c/.h        NetBIOS node status (UDP 137) for Windows PCs.
  net/disc_rdns.c/.h        hand-rolled DNS PTR to the gateway (dnsmasq gives
                            DHCP lease hostnames).
  net/oui.c/.h              binary search over the generated OUI table.
  net/oui_table.inc         GENERATED by tools/gen_oui.py. Do not hand-edit.

  db/device_db.c/.h         two tiers of devices, plus the events ring buffer.
                            Active devices (CONFIG_NETDASH_MAX_DEVICES, 144) are
                            in RAM; every device ever seen has a 192-byte record
                            in the register (db/dev_store.c, 1,024). A full RAM
                            table moves the device offline longest out rather
                            than refusing new ones, and a remembered device is
                            loaded back when seen. Port lists live only in the
                            register. NVS "dev" and "ports" are the pre-v0.16
                            storage: copied into the register on its first
                            boot and left alone, so a rollback still has them.
  db/dev_store.c/.h         the register: one file, /ic/devices.db, on the
                            storage partition that db/icons.c mounts, with a
                            4-byte fingerprint per slot in RAM so a lookup is
                            one flash read. Atomic read-modify-write under its
                            own lock; never call it holding device_db_lock().
  db/classify.c/.h          vendor + services + hostname -> netdash_type_t, and
                            the type / service / event name strings the REST API
                            and the UI depend on.
  db/links.c/.h             dashboard quick links and their group headings,
                            NVS namespace "links". A link stores a MAC and a
                            port, never an IP, so it follows the device through
                            a DHCP change; http_server resolves the address on
                            every read. One versioned blob (v2; the v1 migration
                            was removed in v0.14.5).

  db/icons.c/.h             uploaded dashboard icons in the "storage" SPIFFS
                            partition, one small PNG per file. The browser
                            resizes to 64x64 and re-encodes as PNG before
                            uploading, so the firmware never decodes an image -
                            it checks the PNG signature and the IHDR dimensions
                            and streams the bytes to flash. Ids are never
                            reused, which is what makes the served bytes
                            immutably cacheable.

  db/notes.c/.h             plain-text notes on dashboard links (NVS "lnote"),
                            plus the AES-256-GCM vault holding credentials for
                            links (NVS "lsec"), with its metadata in "vault".
                            Devices had a note and a secret of their own until
                            v0.21.0 (NVS "note" and "sec"); nothing reads those
                            now, but a factory reset still erases them and a
                            restore drops them from older backups.
                            The key is derived from a passphrase with PBKDF2 on
                            every unlock and never stored. Everything encrypted
                            is addressed through a sec_ref_t - namespace, key,
                            and the AAD that binds it to its owner - so another
                            kind of secret means extending build_refs() rather
                            than writing another rotation loop. Read the
                            header before trusting it with anything: it does not
                            defend against LAN traffic capture, because the
                            dashboard is plain HTTP.

  db/notify.c/.h            the notification feed: the short, read/unread list
                            of things worth saying, as opposed to the raw events
                            log. Persisted to NVS "notif", writes coalesced by a
                            timer, and armed only after the first sweep so a
                            fresh flash does not announce the whole network.

  db/backup.c/.h            the .landash backup: every NVS namespace that holds
                            the user's setup plus every file on the storage
                            partition, byte for byte, encrypted with a
                            passphrase. A restore is checked in full and staged
                            in the idle update slot (borrowed from net/ota.c),
                            then applied by backup_apply_pending() at the top
                            of app_main, before anything reads NVS. Its own
                            NVS "backup" holds when the last one was made.

  net/linkcheck.c/.h        one TCP connect per dashboard link, once a minute,
                            so a tile reports whether the service answers
                            rather than whether the box pings. Links whose
                            device is already offline are skipped rather than
                            waited out.

  net/tls_mem.c/.h          the mbedTLS allocator (CONFIG_MBEDTLS_CUSTOM_MEM_ALLOC):
                            one 17 KB static block kept for the TLS read
                            buffer, everything else from the heap. Without it
                            a fragmented heap stops updates downloading - see
                            Traps in OTA.

  net/wan.c/.h              WAN health: an ICMP echo and a name lookup on an
                            interval, reported separately so "no internet" and
                            "no DNS" can be told apart. Uses esp_ping rather than
                            a second raw socket of our own.

  net/ota.c/.h              updates from latest.json in the releases repo baked
                            in by CONFIG_NETDASH_OTA_REPO (never a web setting:
                            the API has no auth), read through
                            raw.githubusercontent.com. Checks the image names
                            itself as project netdash and as that version, and
                            confirms or rolls back a new image after boot.
                            docs/UPDATES.md is the publishing guide.

  net/portscan.c/.h         tiered background TCP connect scan. Each device
                            advances through its own tiers in order (common ->
                            1-1024 -> 1025-65535) up to the first-look depth,
                            and a never-scanned device jumps the queue. Every
                            portscan_rescan_days a device is scanned again up
                            to portscan_rescan_tier: new ports are announced,
                            and listed ports it re-checked and found closed are
                            dropped (not if the device left mid-pass). Results
                            live in the device register (db/dev_store.c).

  web/http_server.c/.h      esp_http_server: gzipped index.html + the REST API.
  web/www/index.html        the whole dashboard, gzipped at build time by CMake
                            and embedded as _binary_index_html_gz_start.
  web/www/fonts/            the Cyberpunk theme's two OFL typefaces (licence
                            beside them), embedded as they are and served at
                            /fonts/ as immutable. Only that theme loads them.

  ui/display.c/.h           esp_lcd ST7789 + esp_lvgl_port, three pages, LEDC
                            backlight dimming.
  ui/button.c/.h            BOOT button: short press = next page, 5 s hold =
                            wipe Wi-Fi credentials and reboot.
  ui/usb_restore.c/.h       the web installer's restore: a line protocol over
                            the console's USB Serial/JTAG that feeds the same
                            backup_restore() as POST /api/restore, with ACKs as
                            flow control. Installs the USB Serial/JTAG driver
                            under the console, so all logging goes through it.

docs/API.md                 the REST contract. Firmware and UI both follow it;
                            change this file before changing either side.
tools/gen_oui.py            IEEE oui.csv -> oui_table.inc (--curated | --full).
tools/release.py            checks build/netdash.bin is exactly the tag HEAD is
                            on and newer than what is published, then commits
                            it, latest.json and the browser installer's
                            install/manifest.json + boot parts into
                            ../landash-releases. The push there is what
                            publishes it. That repo is also the GitHub Pages
                            landing page (index.html, hand-written).
partitions.csv              nvs 64K, otadata 8K, phy 4K, ota_0 3M, ota_1 3M,
                            storage 1M. "storage" is a SPIFFS volume mounted
                            at /ic by db/icons.c; it is formatted on first use,
                            which costs a few seconds on one boot only.
```

## Conventions

- **C11**, 4-space indent, no tabs, ~100 column lines.
- One `static const char *TAG = "<module>";` per `.c`, and log through
  `ESP_LOGE/W/I/D/V`. Never `printf`.
- Public functions return `esp_err_t` unless they are simple getters. Check
  every `esp_err_t`; `ESP_ERROR_CHECK` only in `app_main` where a failure
  really is fatal.
- Headers declare the full contract; `.c` files may be stubs but the header is
  never a stub.
- **No dynamic allocation in hot paths** (the sweep loop, the event handlers,
  the HTTP handlers' per-row work). Fixed-size buffers and the preallocated
  device table. cJSON allocation in an HTTP handler is fine, but free it on
  every path.
- **Every lwIP `etharp_*` call goes through `esp_netif_tcpip_exec()`.** Calling
  them from a task other than the tcpip thread is a race and reviewers check it
  explicitly.
- **device_db locking**: `device_db_get_at()` is only valid while you hold
  `device_db_lock()`; take the lock once around the whole iteration, never per
  row. Every other accessor takes the lock itself, so do not call them while
  holding it. Do no I/O (HTTP send, SPI, NVS) while holding the lock — build
  the response into a buffer first, then release it.
- JSON via **cJSON** (`json` component). Never build JSON with `sprintf`.
- **The web UI updates in place.** A renderer that runs on the 5 s poll builds
  an HTML string and hands it to `morph(container, html)`, never
  `innerHTML`: rebuilding every element restarted animations, flickered
  hover controls and icons, and dropped text selections. Consequences: give
  list items a `data-key` so a re-sort moves them rather than rebuilding
  them; mark an element another function fills with `data-keep`; attach
  event handlers to the container by delegation, never to rendered elements
  (they would be attached again on every render, and would see stale data);
  and keep client-only state such as an expanded row in `state`, or the next
  render undoes it. Poll results are painted through `paint(key, fn)` so a
  tick lands as one repaint. A relative time or the uptime goes out as
  `relSpan(ts)` or `uptimeSpan()`, which count every second on their own;
  do not poll faster to make them move - a Devices-tab poll keeps the
  dongle's web server busy for about 270 ms.
- Events go on the default event loop with base `NETDASH_EVENT`; device event
  data is a `netdash_device_t` **copied by value**, never a pointer into the
  table.
- Times are unix seconds (`int64_t`), `0` when NTP has not synced yet. Use
  `esp_timer_get_time()` for durations, never wall-clock arithmetic.
- **Every module owns its own files and does not edit anyone else's.** If you
  need something from another module, add it to that module's header and say so
  in your report — do not reach into its internals or duplicate its state.
- `main/net/oui_table.inc` is generated: change `tools/gen_oui.py` and re-run
  it, never hand-edit the table.
- Do not edit `.vscode/`, and do not commit `sdkconfig`, `build/` or
  `managed_components/`. `dependencies.lock` **is** committed.

## Work package status

| WP | Scope | State |
|---|---|---|
| 0 | scaffold, headers, settings, OUI, docs | done |
| 1 | wifi_mgr + SNTP + mDNS advert | done |
| 2 | display + button | done |
| 3 | device_db, classify, events log | done |
| 4 | scanner | done |
| 5 | disc_mdns / ssdp / nbns / rdns | done |
| 6 | http_server REST API | done |
| 7 | web/www/index.html | done |
| 8 | hardware integration, smoke test, README | done |

Every module is implemented. There are no stubs left.

## Two traps in the HTTP layer

**esp_http_server only honours a wildcard at the END of a URI template.** A
template like `/api/devices/*/history` is matched literally, so it never fires
and the endpoint answers 404 or 405 as though it had been forgotten. This is
not hypothetical: `POST /api/devices/{mac}/portscan` shipped that way and the
UI's "Rescan ports" button never worked. Per-device sub-resources are now
dispatched by hand from one wildcard handler per method
(`devices_get_router()` and friends).

**The httpd route table is fixed at start-up and overflowing it is silent.**
`httpd_config_t::max_uri_handlers` comes from `NETDASH_HTTPD_MAX_URI_HANDLERS`
in `http_server.h`; routes past the limit fail to register and look exactly
like a routing bug at runtime. There is now a `_Static_assert` against the size
of the route table, so raise the constant when the build tells you to.

## Traps in OTA

**Releases are files on a branch, not GitHub release objects.** A release
object cannot be pushed, only created through the website or the API, which
made publishing a separate manual step. `latest.json` plus the image, committed
to the public releases repo and read from `raw.githubusercontent.com`, makes a
push the whole of publishing, needs no token, and has no redirect to a signed
URL on another host.

If you ever go back to release assets: `esp_http_client_get_url()` drops the
query string. It rebuilds the URL from scheme, host, port and path, and a
GitHub asset download redirects to a signed URL whose signature *is* the query
string. Following the redirect with `esp_http_client_set_redirection()` plus
`get_url()` produced an unsigned URL and a baffling `Server error (618)`.
Capture the `Location` header from `HTTP_EVENT_ON_HEADER` instead.

**An update never replaces the bootloader.** Rollback
(`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`) lives in the bootloader, so the first
image with it must go on over USB. Anything else that changes the bootloader
has the same property: it reaches only boards that are reflashed by cable.

**Heap during TLS, and why the download resumes.** A GitHub handshake took
free heap from ~90 KB to ~35 KB with static mbedTLS buffers.
`CONFIG_MBEDTLS_DYNAMIC_BUFFER` and its two `FREE_*` companions bring the low
point to ~55 KB; do not turn them off. Even so, streaming the 2 MB image dips
free heap to ~23 KB, and the first real update died a megabyte in with
`esp-tls-mbedtls: read error :-0x7F00` (`MBEDTLS_ERR_SSL_ALLOC_FAILED`).
esp_https_ota cannot carry on from an offset, so `ota.c` writes with
`esp_ota_*` itself and reopens a stream that stops short with a `Range`
request at the byte it had reached, up to six times. The verified run needed
one resume, at 1,272,832 of 1,980,640 bytes.

**An update needs 16.7 KB of contiguous heap per TLS record.** With the
dynamic buffers above, mbedTLS allocates a fresh read buffer for every 16 KB
record and frees it after, so a 2 MB image makes about 125 of those
allocations, each needing one unbroken 16,749-byte block. By v0.20 the heap had
~70 KB free but its largest block sat at 20-30 KB and fell below 16 KB at
times; every download died within 400 KB with `Dynamic Impl: alloc(16749
bytes) failed` and ran out of resumes. v0.21.0 was published, could not be
installed by anyone, and was withdrawn. The failure is in the *running*
firmware, so the fix (v0.21.1) had to go onto dongles by USB.

`net/tls_mem.c` now serves that buffer from a static block of its own, and
v0.21.1 paid for the block by freeing more than it costs: `app_main` returns
(8 KB of main-task stack), the LCD draws 10 lines at a time rather than 20
(9.6 KB), and LVGL's pool is 20 KB rather than 24 (it peaked at 14.8 KB).
With that, the same 2 MB downloaded in 14 s with no resumes and free heap
never under 42 KB. The heartbeat prints the largest free block and the
reserve's counters (`tls reserve uses/busy`); `busy` or `failed` above zero
means a second big TLS buffer wanted the block at the same time. Never trim
the heap back below this without repeating the download test below from a
cold boot with the full device table.

To try an update without publishing one, build into a separate directory with
a fake low version, so the real release looks new, and watch the console:

```powershell
# build_otatest/sdkconfig: a copy of sdkconfig.
idf.py -B build_otatest "-DSDKCONFIG=build_otatest/sdkconfig" "-DPROJECT_VER=v0.0.1" build
git checkout dependencies.lock    # see the next trap
```

## Traps in backup and restore

**A backup carries stored layouts, not fields, so every loader is a promise.**
Settings, links, notes, vault entries and register records go into a
`.landash` file exactly as they sit in flash, and a restore writes them back
for the firmware to load as it would after an update. A dongle refuses a
backup from newer firmware, but must accept one from any older firmware back
to v0.19.0. So a blob's migration code can never be dropped the way the links
v1 migration was in v0.14.5: a file made on v0.19.0 may be restored years
later.

**A new NVS namespace must go on two lists.** `k_nvs_namespaces` in
`http_server.c` (what a factory reset erases) and `k_namespaces` in
`db/backup.c` (what a backup carries). Forget the second and the data quietly
fails to move to a replacement dongle; a restore also refuses any namespace
not on it.

**A restore uses the idle update slot as scratch.** That slot holds the
previous firmware, so `ota_borrow_slot()` refuses while an update downloads
or the running image is still on probation, and an update refuses to start
while the slot is lent. A refused or failed restore has already erased the
slot's first sector, so the old image is gone either way; nothing needs it
once the running image is confirmed.

**Restoring over USB needs its own flow control.** The USB Serial/JTAG
driver empties the hardware FIFO into its ring buffer on every packet and
drops what does not fit; USB's own back-pressure never reaches the host. The
first USB restore lost bytes during the key derivation and timed out. Keep
the ACK window in `ui/usb_restore.h`, `tools/usb_restore.py` and the releases
repo's `install/restore.js` in step.

To try a restore end to end, run `tools/smoke_test.py --restore-roundtrip`
against a dongle whose data you are happy to have replaced by a copy of
itself.

## The dependencies.lock trap

**Any build in a fresh build directory rewrites `dependencies.lock` with
absolute paths on this machine** when `managed_components/` already exists:
the component manager takes the downloaded components for local overrides.
That happened after `idf.py fullclean` and again with `build_otatest`, and the
first time it got committed. Check `git diff dependencies.lock` before every
commit. If it shows `type: local` and a `C:\` path, run
`git checkout dependencies.lock`, delete `managed_components/`, and build
again; a fresh download leaves the lock alone.

## The esp_ping session lifetime trap

**Never create and delete an esp_ping session per check, and never put its
callback context on the caller's stack.** The ping task can still be inside
`recvfrom()` when `esp_ping_delete_session()` returns, and its `on_ping_end`
then gives a semaphore that has already been deleted, through a stack frame
that no longer exists:

```
assert failed: xQueueGenericSend queue.c:936 (pxQueue)
```

That rebooted the dongle within a few minutes of the WAN probes starting to
fail - exactly when the feature is supposed to be working. `net/wan.c` now
keeps one session and one context alive for the lifetime of the task, and
drains any stale semaphore give before each probe. The session is only rebuilt
when the configured target actually changes.

If you touch this, reproduce the failure before believing the fix: point
`wan_ping_host` at an unroutable address such as `192.0.2.1`, set
`wan_interval_s` to 15, attach the serial console, and watch `uptime_s` across
a full down-and-back cycle.

## Two traps in the scanner

**The lwIP ARP cache is the scanner's bottleneck, and it must be sized for the
probe rate.** `ARP_TABLE_SIZE` is raised to 192 by a project-wide compile
definition in the top-level `CMakeLists.txt`, and `scanner.c` harvests the
whole table every few probes rather than only at the end of a sweep. Both are
needed. lwIP recycles the oldest *stable* entry before any pending one, so
every probe to an address that does not answer can evict a host the scanner had
already resolved. At the stock size of 10 a /24 sweep found 6 of 23 devices.

The table size and `hosts_per_sec` are coupled: an address that never answers
leaves a pending entry for about five seconds, so a sweep at R probes per
second keeps roughly 5R of them alive at once. Raising the rate without
raising the table brings the bug straight back. Measured on a /24 with 23
live hosts:

| Table | Rate | Devices found |
|---|---|---|
| 10 | 4/s | 6 |
| 64 | 4/s | 23 |
| 64 | 16/s | 13-16 |
| 192 | 16/s | 23-24 |

**Verify discovery from a cold boot, never from a warm table.** A sweep that
only has to keep already-online devices online will pass while a sweep that has
to find them from scratch fails. That is exactly how the 16/s regression got
through: reboot first, then count.

**A MAC is not always one device.** A Wi-Fi extender in client mode (a TP-Link
RE700X, for one) rewrites the MAC of every wired device behind it to its own,
so the extender and the PC behind it both answer ARP with one MAC at two
addresses. Keyed by MAC alone that was one entry flipping between the two
every sweep, with an `ip_changed` notification each time. `device_db` now
tracks MACs seen at a second address and, once both addresses have been alive
together for longer than lwIP's five-minute ARP cache can explain, pins the
entry and gives every other address its own entry under a synthetic key
(`03:` + a hash of MAC and address). The scanner's per-sweep dedupe is keyed
on (MAC, address) for the same reason. Do not shorten `SHARED_CONFIRM_S` below
`ARP_MAXAGE`: a stale entry for a device that really moved would then look like
a second device. The API doc's "Devices that share a MAC" has the rest.

**The active device table must never thrash, and nothing per-sweep may be sized
by it.** With more devices online than the RAM table holds, moving out the
least-recent device to make room for the next sighting swapped the same few
devices in and out dozens of times a second, two flash writes each - found by
a test build with a 20-device table on a network with 26 online. Only a
device that missed at least the last sweep may leave RAM; when every active
device is still around, a newcomer is recorded in the register only
(rate-limited to one write per ten minutes) and listed with the stored-away
devices. The same test showed the scanner's per-sweep "seen" set, sized by
the active table, overflowing and counting 183 alive out of 26: it is now its
own 512-entry fingerprint set. To reproduce either, build with
`CONFIG_NETDASH_MAX_DEVICES=20` in a separate build directory (see Traps in
OTA) and watch the console through two sweeps.

**The OUI table must be generated with every prefix per vendor.** Sampling a
handful of blocks per manufacturer looks fine and matches almost nothing real:
ASUS alone holds dozens of blocks. Regenerate with
`--curated --per-vendor 100000`, which yields about 13,500 entries for roughly
110 KB of flash. Note that phones randomise their MAC per network, so a blank
vendor on a phone is correct, not a lookup failure.

## What has and has not been verified on hardware

Verified on a live /24 home network (23 devices):

- Boots clean; LCD, BOOT button, setup access point and station mode all work.
- Settings persist across a reflash; Wi-Fi credentials survive reboots.
- A sweep finds every device the router lists, plus the router and both mesh
  nodes. 253 addresses probed, 23 alive.
- Naming works from mDNS, the router's reverse DNS and SSDP; vendors resolve
  for every device that is not using a randomised MAC.
- `tools/smoke_test.py` passes against the real device, covering groups, link
  notes, link credentials in the vault and a passphrase rotation carrying
  them, history, WAN, the feed, uploaded icons and backups. It ran 131 checks
  with a vault already present (v0.21.0 work), and skips the vault's own
  checks then, because exercising the vault means destroying it at the end
  and the suite will not do that to someone's real credentials. Do not work around that guard by hand - a vault
  created between two runs belongs to the user, not to the test.
- The browser half of the icon upload - decode, resize, re-encode, POST, and
  the tile repainting with the result - was driven end to end in headless Edge
  against the live dongle. A 300x120 JPEG became a 1,930-byte 64x64 PNG.
- Free heap: about 112 KB with a low-water mark of 88 KB in normal polling,
  early on. By v0.20 it was ~50-70 KB with 33 devices and the low-water mark
  under 15 KB, which is what broke updates (see Traps in OTA). Startup costs,
  measured in v0.21: Wi-Fi 48 KB, LCD and LVGL task 19 KB (28.5 KB before the
  draw buffers were halved), web server 14 KB, USB restore 12 KB, the update
  task 8.6 KB, every other task 4-7 KB of stack.
- The LCD's orientation, colour order and panel window (v0.18.1): upright,
  correct colours, and no noise along any edge with the y gap at 53. On a
  board mounted the other way up, `NETDASH_LCD_ROTATE_180` wants y 52.

### Heap, and the one endpoint that still costs

`GET /api/devices` used to build one cJSON tree for the whole table and then
print it, so peak usage was the object graph and the finished string at once —
about 58 KB for 28 devices, scaling with the table. It is now streamed a device
at a time with `httpd_resp_send_chunk()` and costs almost nothing.

`GET /api/devices/export` still builds the whole thing in one go and dips the
low-water mark to about 47 KB. That is tolerable because it is a manual,
one-off action rather than a five-second poll, but it is the next thing to
stream if the device table ever gets close to its 128-device capacity.

Not verified:

- The dashboard across browsers. It has been driven in headless Edge against
  live data (v0.21.2): elements survive polls, times tick every second, a
  poll lands as one DOM update, no page errors. Firefox and Safari have not
  been tried.
- Long-term stability beyond a few minutes of uptime.
