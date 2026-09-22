# LANDA.SH REST API

Version 1. This contract is fixed in WP0 so the firmware (WP6) and the web UI
(WP7) can be built in parallel. Changing it means changing this file first.

- Base URL: `http://landash.local` (STA) or `http://192.168.4.1` (AP mode).
- All request and response bodies are `application/json; charset=utf-8`.
- No authentication in v1. The dongle is LAN-only.
- All timestamps are **unix seconds (UTC)** as JSON numbers. `0` means "not
  known" — the dongle has not synced NTP yet, so it has no wall-clock time.
- MAC addresses are always lowercase colon-separated: `"aa:bb:cc:dd:ee:ff"`.
- IPv4 addresses are always dotted-quad strings: `"192.168.1.42"`.
- Unknown query parameters and unknown JSON object members are ignored.

## A note on routing

`esp_http_server` matches a URI template ending in a wildcard by prefix, and
matches anything else literally — a wildcard in the *middle* of a template is
just an asterisk, compared character for character. A route such as
`/api/devices/<wildcard>/history` therefore never matches a real request and
silently answers 404 or 405 for ever.

Every per-device sub-resource is therefore dispatched by hand from one wildcard
handler per method. Adding `/api/devices/{mac}/something` means adding a branch
to the matching `devices_*_router()` in `http_server.c`, not a new row in the
route table.

## Errors

Any non-2xx response has this body and nothing else:

```json
{ "error": "device not found" }
```

| Status | When |
|---|---|
| 400 | malformed JSON, bad MAC, value out of range |
| 404 | unknown endpoint, or unknown MAC on a device route |
| 405 | wrong method for a known path |
| 413 | request body over 8 KB |
| 500 | NVS write failed, out of memory |
| 503 | the operation needs Wi-Fi and the dongle has no connection yet |

The `error` string is short, lowercase, and meant to be shown to a human.

---

## GET /

Returns the embedded single-page UI.

- `Content-Type: text/html; charset=utf-8`
- `Content-Encoding: gzip`
- `Cache-Control: no-cache`

Any unknown path that is not under `/api/` also returns this document, so the
UI can use client-side routing.

---

## GET /api/status

Everything the header and the System tab need, in one poll.

```json
{
  "ip": "192.168.1.58",
  "netmask": "255.255.255.0",
  "gateway": "192.168.1.1",
  "mac": "a0:76:4e:11:22:33",
  "mode": "sta",
  "ssid": "MyHomeWiFi",
  "rssi": -54,
  "hostname": "landash",
  "uptime_s": 34512,
  "heap_free": 142336,
  "heap_min_free": 118204,
  "fw": "0.1.0",
  "idf": "v5.5",
  "time": 1789520400,
  "time_synced": true,
  "scanning": false,
  "scan_done": 0,
  "scan_total": 0,
  "last_sweep": 1789520102,
  "subnet": "192.168.1.0/24",
  "sweep_hosts": 253,
  "scan_skipped": null,
  "devices_total": 27,
  "devices_archived": 0,
  "devices_online": 21,
  "devices_new_24h": 1,
  "ap_ssid": "LANDA-A4F3",
  "ap_pass": "kq7mn3rt"
}
```

| Field | Type | Notes |
|---|---|---|
| `ip`, `netmask`, `gateway` | string | `"0.0.0.0"` when not connected |
| `mac` | string | the dongle's own STA MAC |
| `mode` | string | `"off"`, `"sta"`, `"ap"`, `"apsta"` |
| `ssid` | string | associated SSID, `""` in AP-only mode |
| `rssi` | number | dBm, `0` when not associated |
| `uptime_s` | number | seconds since boot |
| `heap_free`, `heap_min_free` | number | bytes |
| `fw` | string | app version from the build |
| `time` | number | unix seconds, `0` before NTP sync |
| `scanning` | bool | a sweep is in progress |
| `scan_done`, `scan_total` | number | host progress, both `0` when idle |
| `last_sweep` | number | unix seconds of the last completed sweep, `0` if none |
| `subnet` | string | the connected network in CIDR form, `""` when not on a LAN |
| `sweep_hosts` | number | addresses one active sweep probes; `0` when the network cannot be swept. Divide by `hosts_per_sec` for the sweep time |
| `devices_total` | number | every device known: active and remembered |
| `devices_archived` | number | remembered devices that are not active, listed by `GET /api/devices?archived=1` |
| `scan_skipped` | string or null | why the last active sweep did not run, in words for the page; `null` when it ran. Networks larger than a /16 are not swept |
| `ap_ssid`, `ap_pass` | string | only present when `mode` is `"ap"` or `"apsta"` |

---

## The device object

Used by `/api/devices`, `/api/devices/{mac}` and `/api/devices/export`.

```json
{
  "mac": "b8:27:eb:0a:1b:2c",
  "hw_mac": "b8:27:eb:0a:1b:2c",
  "shared_mac": false,
  "ip": "192.168.1.31",
  "display_name": "Kitchen Pi",
  "nickname": "Kitchen Pi",
  "hostname": "raspberrypi",
  "vendor": "Raspberry Pi",
  "type": "pc",
  "type_override": null,
  "services": ["ssh", "http"],
  "sources": ["mdns", "rdns"],
  "first_seen": 1786918000,
  "last_seen": 1789520102,
  "online": true,
  "is_new": false,
  "hidden": false,
  "rtt_ms": 4,
  "miss_count": 0
}
```

| Field | Type | Notes |
|---|---|---|
| `mac` | string | lowercase colon-separated, the primary key. The hardware MAC, except for a device behind a shared MAC (below) |
| `hw_mac` | string | the MAC the device actually answers ARP with. Equal to `mac` for every ordinary device; show this one to people |
| `shared_mac` | bool | this MAC answers at more than one address, so the entry is pinned to its `ip` |
| `ip` | string | last known IPv4; for a `shared_mac` entry, the address it is pinned to |
| `display_name` | string | nickname, else hostname, else `"<vendor> xxyy"`, else the MAC. Never empty |
| `nickname` | string | user-set, `""` when unset |
| `hostname` | string | best auto-discovered name, `""` when unknown |
| `vendor` | string | OUI lookup, `""` when unknown or a randomised MAC |
| `type` | string | the effective type: the override when set, else the auto classification |
| `type_override` | string or null | `null` when auto; otherwise the user's choice |
| `services` | array of string | may be empty |
| `sources` | array of string | which discovery sources have named this device |
| `first_seen`, `last_seen` | number | unix seconds, `0` before the first NTP sync |
| `online` | bool | `miss_count == 0` |
| `is_new` | bool | `first_seen` is within the last 24 h (always `false` when `first_seen` is `0`) |
| `hidden` | bool | the user hid it from the default list |
| `rtt_ms` | number | last ICMP round trip, `-1` when the host only answered ARP |
| `miss_count` | number | consecutive sweeps not seen, `0..255` |

### Devices that share a MAC

A Wi-Fi extender or bridge in client mode can only present one MAC to the
network, so it rewrites the MAC of every wired device behind it to its own.
The extender and each device behind it then answer ARP with the same MAC at
different addresses. Keyed by MAC alone they would collapse into one entry that
"moves" between addresses every sweep.

The firmware recognises this pattern and splits it: the entry already holding
that MAC keeps it and is pinned to one address, and every other address gets an
entry of its own with `shared_mac: true`, the same `hw_mac`, and a synthetic
`mac` that serves as its key everywhere a key is needed - PATCH, notes,
secrets, links, history, the port scan. A synthetic key starts with `03:`,
which as a multicast address can never be a real device's MAC. It is derived
from `hw_mac` and the address, so the same device gets the same key after a
reboot and its nickname and notes come back with it.

Telling this apart from an ordinary DHCP move needs time rather than a single
sighting, because lwIP keeps a departed host's ARP entry for up to five
minutes: for that long a device that genuinely moved still appears at its old
address as well as its new one. So a MAC is only declared shared when it is
still answering at the address it supposedly left more than six minutes after
the move. Until then the entry follows the newest address, as before, but
sightings at the old address do not move it back.

The cost is that a device behind a shared MAC is identified by its address. If
DHCP gives it a new one, it appears as a new entry and the old one goes
offline; nickname the new one, or give the device a DHCP reservation.

### `type` values

`unknown`, `router`, `mesh_node`, `switch`, `nas`, `tv`, `hub`, `phone`, `pc`,
`iot`, `printer`, `cast`, `console`, `printer_3d`

### `services` values

`http`, `https`, `ssh`, `smb`, `hap`, `ha`, `cast`, `airplay`, `printer`,
`ssdp`, `workstation`

### `sources` values

`mdns`, `rdns`, `nbns`, `ssdp` — in priority order, highest first. The name in
`hostname` came from the highest-priority source present in this array.

---

## GET /api/devices

Returns the active devices as an array of device objects.

The dongle keeps two tiers. The **active** devices - online, or seen recently,
up to `CONFIG_NETDASH_MAX_DEVICES` (144) - are in RAM, and this endpoint, which
the page polls, lists them. Every device ever seen also has a record in the
**register** in flash, up to `CONFIG_NETDASH_REGISTER_DEVICES` (1,024). When
the active table is full, the device offline longest leaves it; its record
keeps its nickname, type, notes, ports and the rest, and it becomes active
again the moment it is seen. Those remembered-only devices are listed with
`archived=1`, on request rather than on every poll, because there can be a
thousand of them. `devices_archived` in `GET /api/status` says how many there
are.

Query parameters:

| Name | Type | Default | Meaning |
|---|---|---|---|
| `hidden` | `0` or `1` | `0` | include devices with `hidden: true` |
| `online` | `0` or `1` | — | when `1`, only online devices |
| `archived` | `0` or `1` | `0` | when `1`, list **only** the remembered devices that are not active, each with `"archived": true`. One flash read per device |

Everything else that takes a `{mac}` - `GET`, `PATCH`, `DELETE`, notes,
secrets, history - works for a remembered device too. It is offline, and it
has no history (that is kept for active devices only).

```http
GET /api/devices?hidden=1
```

```json
[
  { "mac": "b8:27:eb:0a:1b:2c", "ip": "192.168.1.31", "...": "..." },
  { "mac": "bc:ae:c5:aa:bb:cc", "ip": "192.168.1.1",  "...": "..." }
]
```

The array is ordered by IP ascending. An empty table returns `[]`, not an error.

---

## GET /api/devices/{mac}

`{mac}` is the 17-character lowercase colon form. Returns one device object, or
404.

```json
{ "error": "device not found" }
```

---

## PATCH /api/devices/{mac}

Sets the user-owned fields. Every member is optional; members that are absent
are left unchanged. Persists to NVS before replying.

```json
{
  "nickname": "Kitchen Pi",
  "type_override": "pc",
  "hidden": false
}
```

| Member | Type | Meaning |
|---|---|---|
| `nickname` | string, max 31 chars | `""` clears it and falls back to the auto name |
| `type_override` | string or null | a `type` value; `null` or `"unknown"` restores auto classification |
| `hidden` | bool | hide from the default device list |

Returns the full updated device object (200). 400 on an unknown
`type_override` or a nickname over 31 characters, 404 on an unknown MAC.

---

## DELETE /api/devices/{mac}

Forgets a device, in RAM and in NVS. It will reappear as new if it is seen
again.

```json
{ "ok": true }
```

404 when the MAC is unknown.

---

## GET /api/devices/export

The full table plus a header, for backing up nicknames.

```json
{
  "version": 1,
  "exported_at": 1789520400,
  "hostname": "netdash",
  "devices": [
    { "mac": "b8:27:eb:0a:1b:2c", "...": "..." }
  ]
}
```

`Content-Disposition: attachment; filename="landash-devices.json"`.

This is nicknames only. For a file that sets up a replacement dongle
identically - Wi-Fi credentials, hostname, every device, links, icons, notes
and the vault - see **The full backup**, below.

---

## POST /api/devices/import

Restores user fields. Accepts exactly what `export` produces; every field other
than `mac`, `nickname`, `type_override` and `hidden` is ignored, and devices in
the file that are not on the network are created as offline entries. The one
exception is an entry with `shared_mac: true`, where `hw_mac` and `ip` are read
too, because without them a synthetic key cannot be matched to the device it
stands for.

```json
{
  "version": 1,
  "devices": [
    { "mac": "b8:27:eb:0a:1b:2c", "nickname": "Kitchen Pi", "type_override": "pc", "hidden": false }
  ]
}
```

```json
{ "ok": true, "imported": 12, "skipped": 1 }
```

400 when `version` is not 1 or `devices` is not an array.

---

## The full backup

A single `.landash` file that sets up a replacement dongle identically:
settings including the Wi-Fi SSID and password, hostname and setup-AP
password, every device ever seen (nicknames, types, ports), dashboard links
and groups, uploaded icons, notes, and the vault (still encrypted under the
vault passphrase - a backup does not need the vault unlocked, and does not
expose its secrets in the clear). The notification feed and update bookkeeping
are not included, since neither means anything on a different dongle.

The whole file is encrypted with a backup passphrase chosen at export time,
separate from the vault passphrase. Restore replaces everything on the dongle
and restarts it.

**Say this plainly: anyone on the LAN can call `POST /api/backup`.** There is
no authentication in v1 (see the top of this document), so the only thing
standing between a stranger on the network and a file containing your Wi-Fi
password is the export passphrase they choose right there in the same
request - which they, being the one calling the endpoint, obviously know. This
is no worse than any other endpoint here, but a backup is the one file that
carries the Wi-Fi password at all: `GET /api/settings` never returns it, and
`GET /api/devices/export` does not touch settings.

### GET /api/backup

```json
{ "last_backup": 1789520400, "restored": null }
```

| Field | Type | Notes |
|---|---|---|
| `last_backup` | number | unix seconds of the last completed download, `0` for never (or before NTP synced). After a restore, this is the `created` time of the file that was restored, not the moment of the restore |
| `restored` | object or null | `null` if this dongle has never been restored onto |

A factory reset clears both.

```json
{ "hostname": "landash", "fw": "v0.19.0", "created": 1789520400 }
```

`restored` describes the backup this dongle was last restored from: the
hostname and firmware version it was made on, and when it was made.

### POST /api/backup

```json
{ "passphrase": "correct horse battery staple" }
```

`passphrase`: 8 to 128 characters, no control characters (so no newline).

Response `200`, `Content-Type: application/octet-stream`, sent chunked, with:

```
Content-Disposition: attachment; filename="landash-<hostname>-<YYYYMMDD>.landash"
```

(just `landash-<hostname>.landash` when the clock has not synced, since there
is no date to put in it; and plain `landash-<YYYYMMDD>.landash` while the
hostname is still the default `landash`). The first byte of the response takes about six
seconds - that is PBKDF2 key derivation, run at idle priority so the rest of the dongle keeps
going, not a stall. A restore spends the same time before it reads the records.

400 on a passphrase outside the length or character rules, in the same
`{"error": "..."}` shape as everywhere else. A failure partway through
streaming (flash read error, heap exhaustion) closes the connection without
sending the final chunk, so the client sees a network error rather than a
200 - it must treat a short read as a failed backup and not save the partial
file.

### POST /api/restore

Body `application/octet-stream`, **not** JSON: the passphrase as UTF-8, one
`\n` (0x0A) byte, then the `.landash` file's bytes unchanged. Max 4 MB total.

```
POST /api/restore
Content-Type: application/octet-stream

correct horse battery staple
<file bytes>
```

The firmware checks every record's authentication tag and stages the whole
file - on the spare OTA slot used as scratch space - before changing anything
live, so a bad file leaves the dongle exactly as it was.

`200`:

```json
{
  "ok": true,
  "restarting": true,
  "hostname": "landash",
  "fw": "v0.19.0",
  "created": 1789520400,
  "devices": 123,
  "links": 12,
  "icons": 4
}
```

The dongle restarts about a second after replying and applies the backup
during boot, before anything else starts. It then comes back up with the
restored hostname and Wi-Fi, so **it may be at a different address or on a
different network** than the one the request was sent to.

The `error` strings are sentences meant to be shown as they are:

| Status | When |
|---|---|
| 400 | the passphrase line is missing, or not 8-128 characters |
| 400 | `This is not a LANDA.SH backup file.` - bad magic, or an unreadable header |
| 400 | `The backup is damaged or incomplete.` - a tag failed after the first record, the file ends before its `E` record, or bytes follow it |
| 403 | `Wrong passphrase.` - the first record fails its tag |
| 409 | the backup was made by newer firmware than this dongle runs, or uses a newer `format` or key settings: update the dongle first |
| 409 | a firmware update is downloading, or was installed so recently that it is still on probation: the spare update slot restore stages into holds the image a rollback would need |
| 408 | the upload stopped part-way |
| 413 | over 4 MB |
| 500 | a flash write failed, or out of memory |

Backups travel forwards only. A dongle restores a backup made by the same or
older firmware - every stored layout since backups began (v0.19.0) must go on
loading, which is the same promise an update already makes - and refuses one
made by newer firmware, whose layouts it may not know. A development build
whose version is a bare commit hash is not checked.

### The `.landash` file format

Documented here so the browser-based installer can be written against it
without reading the firmware.

| Bytes | Content |
|---|---|
| 0-7 | ASCII `LANDASH` followed by `0x0A` |
| 8-9 | header length `H`, unsigned 16-bit little-endian, at most 1024 |
| 10..10+H | the header: UTF-8 JSON, readable without the passphrase |
| 10+H.. | records, until end of file |

Header:

```json
{
  "format": 1,
  "fw": "v0.19.0",
  "hostname": "landash",
  "created": 1789520400,
  "devices": 123,
  "links": 12,
  "icons": 4,
  "kdf": "pbkdf2-sha256",
  "iterations": 40000,
  "salt": "<32 hex chars>",
  "nonce": "<16 hex chars>"
}
```

`devices`, `links` and `icons` are informational, for showing what a file
holds before asking for its passphrase. `devices` counts every device the
dongle knew, as `devices_total` in `GET /api/status` does.

**Key**: `PBKDF2-HMAC-SHA256(passphrase, salt, iterations, 32 bytes)`. The
firmware writes 40,000 iterations and accepts 10,000 to 400,000.

**Records**: each is a 4-byte little-endian length `L` (1..8192) giving the
ciphertext size that follows, then `L` bytes of AES-256-GCM ciphertext, then
its 16-byte tag - so a whole record on disk is `4 + L + 16` bytes, and `L`
itself measures only the ciphertext in the middle. Records run back-to-back
until the file ends.

- **IV** (12 bytes): the header's 8 nonce bytes, then the record's 0-based
  index as a 4-byte big-endian integer. Every record therefore has a distinct
  IV under the one key.
- **AAD**: SHA-256 of every byte of the file before the first record - the
  magic, the length field and the header, exactly as they appear on disk. This
  is what stops the header being edited, or records being dropped, reordered
  or swapped in from a different file, without every remaining tag failing.

Once decrypted, a record's plaintext starts with one type byte:

| Type | Meaning | Layout |
|---|---|---|
| `N` | one NVS entry, carried byte for byte | `u8` namespace length, namespace bytes, `u8` key length, key bytes, `u8 nvs_type_t`, then the value: a little-endian integer of the type's width, string bytes with no NUL, or blob bytes. Only the namespaces listed in `main/db/backup.c` are accepted |
| `F` | start of a file on the storage partition | `u8` name length, name bytes (1-31 of `A-Z a-z 0-9 . _ -`, not starting with `.`), `u32` LE size |
| `D` | file data, following its `F` | up to 4096 bytes, no length prefix - the record framing above already gives its length |
| `E` | end of the backup | `u32` LE count of records before this one |

A file (`F`) without a matching `E` having been reached is incomplete and the
whole restore is refused - see the 400 above. Unknown record types are
refused outright rather than skipped, since skipping one silently would mean
restoring a dongle that is missing whatever that record was.

A plaintext record is at most 8,192 bytes. Records come in this order: the
NVS entries, then the device register (`devices.db`), then one file per
uploaded icon (`<id>.png`), then `E`.

`format` is bumped only when a reader of the current version could not make
sense of a file it produces. Within one `format`, the firmware version check
above still applies.

---

## POST /api/scan

Triggers a sweep now. No request body.

```json
{ "ok": true, "scanning": true }
```

Returns 200 with `"scanning": true` if a sweep was already running (the request
is a no-op, not an error). 503 when the dongle has no IP, or when
`passive_only` is set.

---

## The port scan

A background TCP connect scan runs in tiers, and finishes a tier across every
online device before starting the next, so useful results arrive early:

| Tier | Ports | Rough duration for ~20 devices at 10/s |
|---|---|---|
| 1 | a curated list of ~120 common ports | a few minutes |
| 2 | 1-1024 | under an hour |
| 3 | 1025-65535 | days |

`portscan_rate` is a budget shared by every device, so adding devices makes a
pass longer rather than making the scan noisier. Results and per-device tier
progress are stored in flash and survive a reboot. An open port also feeds
classification: 445 implies SMB, 8123 implies Home Assistant and so on, which
identifies devices that advertise nothing over mDNS or SSDP.

Only TCP is scanned. UDP is not.

### Port fields on the device object

Present on every device in `GET /api/devices`:

| Field | Type | Notes |
|---|---|---|
| `open_port_count` | number | open TCP ports recorded so far |
| `portscan_tier` | number | highest tier finished for this device, `0` = none yet |
| `portscan_last` | number | unix seconds a tier last finished, `0` = never |
| `portscan_active` | bool | a tier is in progress for this device right now |
| `portscan_done`, `portscan_total` | number | progress within the running tier; present only while `portscan_active` |

`GET /api/devices/{mac}` and `PATCH /api/devices/{mac}` additionally return the
full list, ascending. `service` is `null` for a port with no well-known name.

```json
"open_ports": [
  {"port": 22,   "service": "ssh"},
  {"port": 445,  "service": "smb"},
  {"port": 5000, "service": "upnp"},
  {"port": 7654, "service": null}
]
```

---

## GET /api/portscan

Overall scanner progress.

```json
{
  "enabled": true,
  "running": true,
  "tier": 1,
  "max_tier": 3,
  "rate": 10,
  "device_index": 7,
  "device_count": 23,
  "cursor": 48,
  "tier_total": 120,
  "probes": 8134,
  "found": 41,
  "cycle_started": 1789740000
}
```

`device_index` of `device_count` is progress through the device list for the
current tier; `cursor` of `tier_total` is progress within the device being
probed right now.

---

## POST /api/devices/{mac}/portscan

Forgets every port result for that device and puts it at the head of the
queue, starting again at tier 1.

```json
{"ok": true, "queued": "bc:24:11:3b:e5:96"}
```

400 for a malformed MAC, 404 when the device is unknown.

---

## GET /api/events

The ring buffer of the last 100 events, newest first.

Query parameters:

| Name | Type | Default | Meaning |
|---|---|---|---|
| `limit` | number, 1..100 | 100 | how many to return |

```json
[
  {
    "ts": 1789520102,
    "type": "device_new",
    "mac": "b8:27:eb:0a:1b:2c",
    "ip": "192.168.1.31",
    "text": "Kitchen Pi"
  },
  {
    "ts": 1789519800,
    "type": "scan",
    "mac": null,
    "ip": null,
    "text": "sweep done, 21 online"
  }
]
```

| Field | Type | Notes |
|---|---|---|
| `ts` | number | unix seconds, `0` before NTP sync |
| `type` | string | see below |
| `mac` | string or null | `null` when the event is not about one device |
| `ip` | string or null | `null` when not applicable |
| `text` | string, max 47 chars | human-readable detail, may be `""` |

`type` values: `info`, `device_new`, `device_online`, `device_offline`,
`device_ip_changed`, `scan`, `wifi`.

---

## GET /api/settings

The password is **never** returned. `wifi_configured` tells the UI whether one
is stored.

```json
{
  "wifi_ssid": "MyHomeWiFi",
  "wifi_configured": true,
  "hostname": "netdash",
  "ap_pass": "kq7mn3rt",
  "scan_interval_min": 5,
  "hosts_per_sec": 4,
  "passive_only": false,
  "tz": "GMT0BST,M3.5.0/1,M10.5.0",
  "ntp_server": "pool.ntp.org",
  "portscan_enabled": true,
  "portscan_rate": 10,
  "portscan_max_tier": 3,
  "portscan_rescan_days": 7,
  "portscan_rescan_tier": 1,
  "wan_enabled": true,
  "wan_interval_s": 60,
  "wan_ping_host": "1.1.1.1",
  "wan_dns_probe": "example.com",
  "ota_enabled": true,
  "ota_auto": true,
  "ota_interval_h": 12,
  "tour_seen": true,
  "tour_rev": 2,
  "notifications": {
    "new_device": true,
    "ip_changed": true,
    "new_port": true,
    "device_gone": false,
    "device_back": false,
    "wan_down": true,
    "wan_up": true,
    "update": true
  }
}
```

| Field | Type | Notes |
|---|---|---|
| `portscan_enabled` | bool | background TCP port scan on or off |
| `portscan_rate` | number | 1-200 probes per second, shared across every device |
| `portscan_max_tier` | number | how far the first scan of a device goes: 1 about 120 common ports, 2 ports 1-1024, 3 every port |
| `portscan_rescan_days` | number | how often a device is scanned again, 0 to never |
| `portscan_rescan_tier` | number | how far a repeat scan goes, 1-3; never deeper than `portscan_max_tier` in effect |
| `wan_enabled` | bool | run the internet health checks |
| `wan_interval_s` | number | seconds between checks |
| `wan_ping_host` | string | an IP literal, so the test does not depend on DNS |
| `wan_dns_probe` | string | a name to resolve, probed separately from the ping |
| `notifications` | object | one boolean per feed type, keyed by type name |

Notification toggles go out keyed by name rather than as a bitmask so the UI
can render a row per type without carrying its own copy of the bit order.

Arrivals and departures (`device_gone`, `device_back`) are **off by default**.
Phones and laptops sleep all day, and a feed that reports every one of those is
a feed nobody reads.

## PUT /api/settings

Every member is optional; absent members are left unchanged.

```json
{
  "wifi_ssid": "MyHomeWiFi",
  "wifi_pass": "hunter2hunter2",
  "hostname": "netdash",
  "ap_pass": "kq7mn3rt",
  "scan_interval_min": 5,
  "hosts_per_sec": 4,
  "passive_only": false,
  "tz": "GMT0BST,M3.5.0/1,M10.5.0",
  "ntp_server": "pool.ntp.org"
}
```

| Member | Type | Range | Notes |
|---|---|---|---|
| `wifi_ssid` | string | 0..32 chars | `""` clears the credentials |
| `wifi_pass` | string | 0..64 chars | **`""` means "unchanged"**, so the UI can submit the form without re-typing it. To clear it, clear `wifi_ssid` |
| `hostname` | string | 1..31 chars | `[a-z0-9-]`, also the mDNS name |
| `ap_pass` | string | 8..63 chars | WPA2 limits |
| `scan_interval_min` | number | 1..1440 | |
| `hosts_per_sec` | number | 1..64 | |
| `passive_only` | bool | | "quiet mode": no ping sweep. Only devices the dongle happens to hear from are found, and online/offline stops updating. Name lookups and the port scan are unaffected |
| `tz` | string | 0..47 chars | POSIX TZ string |
| `ntp_server` | string | 0..63 chars | |
| `portscan_rescan_days` | number | 0..365 | 0 disables repeat scans |
| `portscan_rescan_tier` | number | 1..3 | |
| `wan_enabled` | bool | | |
| `wan_interval_s` | number | 15..3600 | below 15 s the checks are their own noise |
| `wan_ping_host` | string | 1..39 chars | |
| `wan_dns_probe` | string | 1..47 chars | |
| `notifications` | object | | partial: only the named types change |

An unknown key or a non-boolean value inside `notifications` is a 400, rather
than being ignored — a typo in a type name should not silently do nothing.

Returns the same shape as `GET /api/settings` (200), with an extra hint when
the change needs a reconnect:

```json
{
  "wifi_ssid": "MyHomeWiFi",
  "wifi_configured": true,
  "...": "...",
  "reconnecting": true
}
```

`reconnecting` is `true` when the SSID or password changed; the dongle applies
them a second after replying, so the client loses the connection and should
reconnect to the new IP (or to `http://landash.local`).

400 on any value outside its range.

---

## POST /api/wifi/scan

Blocking active scan, roughly 2-4 seconds. No request body.

```json
[
  { "ssid": "MyHomeWiFi",   "rssi": -48, "auth": "wpa2_psk",       "channel": 6 },
  { "ssid": "MyHomeWiFi-5G","rssi": -61, "auth": "wpa2_wpa3_psk",  "channel": 44 },
  { "ssid": "",             "rssi": -77, "auth": "open",           "channel": 11 }
]
```

Strongest first, duplicates collapsed, hidden networks appear with
`"ssid": ""`. `auth` values: `open`, `wep`, `wpa_psk`, `wpa2_psk`,
`wpa_wpa2_psk`, `wpa2_enterprise`, `wpa3_psk`, `wpa2_wpa3_psk`, `unknown`.

---

## POST /api/system/reboot

```json
{ "ok": true }
```

The dongle replies first, then reboots about a second later.

## POST /api/system/factory-reset

Erases the `cfg` and `dev` NVS namespaces (Wi-Fi credentials, settings, every
nickname) and reboots into AP mode.

Requires a confirmation body, so a stray POST cannot wipe the device:

```json
{ "confirm": "factory-reset" }
```

```json
{ "ok": true }
```

400 when `confirm` is missing or wrong.

---

## Dashboard quick links

The Dashboard tab is a list of shortcuts to services on the network. A link is
stored against a **MAC address and a port, never an IP**, and the address is
resolved from the device table on every read. That is what makes a link keep
working when DHCP gives the device a different address: nothing has to be
edited, the next `GET` simply returns the new `url`.

Up to 48 links are kept, in user-defined order, persisted in flash, and
optionally filed under a group heading.

### GET /api/links

```json
{
  "groups": [{ "id": 1, "name": "Infrastructure" }],
  "max_links": 48,
  "max_groups": 8,
  "links": [
  {
    "id": 1,
    "mac": "bc:24:11:3b:e5:96",
    "port": 8123,
    "scheme": "http",
    "group": 2,
    "icon": "",
    "service": "home-assistant",
    "label": "Home Assistant",
    "ip": "192.168.1.203",
    "url": "http://192.168.1.203:8123",
    "display_name": "homeassistant",
    "type": "hub",
    "online": true
  }
  ]
}
```

Groups and links come back together because the dashboard needs both to paint
a frame; returning them separately means the first paint after a reload has no
headings.

| Field | Type | Notes |
|---|---|---|
| `id` | number | stable while the link exists |
| `group` | number | group id, `0` when ungrouped |
| `icon` | string | icon override: a sprite name, `u:<id>` for an uploaded image, or empty to derive it from `service` |
| `service` | string | what the firmware makes of the port, e.g. `portainer` |
| `note` | string | the note shown on the tile, `""` when unset |
| `has_secret` | bool | whether credentials are stored; the secret itself never appears here |
| `mac`, `port`, `scheme` | | what is actually stored |
| `label` | string | the user's name for it, defaulting to the device name |
| `ip`, `url`, `display_name`, `type`, `online` | | resolved live on every request |

When the MAC is not a device the dongle currently knows, `ip` is `"0.0.0.0"`,
`url` is `null` and `online` is `false`. The link is kept, not dropped, so a
device that is merely switched off does not lose its shortcut.

### POST /api/links

```json
{ "mac": "bc:24:11:3b:e5:96", "port": 8123, "scheme": "http", "label": "Home Assistant" }
```

`scheme` and `label` are optional. `scheme` defaults by port (443, 8443, 8006,
9090 and 5001 give `https`, anything else `http`). `label` defaults to
`"<device> - <Service>"`, for example `"truenas - Portainer"`, using the
well-known name of the port written for a person to read; a port with no
known service falls back to its number, as in `"truenas - 4000"`. Returns the
created link object.

**`label` is at most 31 characters.** A longer one is rejected with 400 rather
than truncated, so the stored label is always exactly what was asked for. The
same limit and the same behaviour apply to `PATCH`.

400 for a bad MAC, port or scheme, or an over-long label. 404 when the MAC is
not a known device. 409 when the list already holds 48 links.

### PATCH /api/links/{id}

Any of `label`, `port`, `scheme`, `icon`, `group`. Absent members are left
alone. `icon` accepts an empty string to clear the override and hand the choice
back to the service lookup; `group` accepts `0` to file the link under no
heading. Returns the updated link.

400 for an over-long label or icon, a bad port or scheme, or a group id that
does not exist. 404 for an unknown link id.

### Notes on links

A link can carry a short note, shown on its tile. A link is a service rather
than a box, so this is the place for "admin account, 8443 is the HTTPS one" -
the device note in `GET /api/devices/{mac}` is for the machine as a whole.

Set it through `PATCH /api/links/{id}` with a `note` member, at most 159
characters; an empty string erases it. It comes back on every link object as
`note`, and `GET /api/links` reports the limit as `max_note`.

400 when the note is too long, 404 when the link does not exist.

Notes live in their own NVS namespace keyed by the link id, not inline in the
links blob: they are sparse, that blob is rewritten on every reordering, and
widening the link record would mean migrating the stored layout again. Deleting
a link - including implicitly, through the bulk delete in `PUT /api/links` -
takes its note with it.

Like a device note this is **plain text**, readable by anyone who can reach the
web UI. Credentials belong in the vault.

### Credentials on a link

The vault holds two kinds of secret, because a device and a service are not the
same thing. One box may run half a dozen services, each with its own login, so
the service is usually the useful unit; the device-level secret is for the box
itself, such as a console or BMC password.

Same vault, same passphrase, same token, same fifteen-minute relock. Changing
the passphrase re-encrypts **both** kinds in one pass.

```
GET /api/links/{id}/secret
X-Vault-Token: <token>
```

```json
{ "secret": "admin / hunter2" }
```

```
PUT /api/links/{id}/secret
X-Vault-Token: <token>

{ "secret": "admin / hunter2" }
```

At most 191 characters; an empty string erases it. Returns
`{ "ok": true, "has_secret": true }`.

| Status | Meaning |
|---|---|
| 401 | the vault is locked, or the token is wrong or missing |
| 404 | no such link, or nothing stored on it |
| 409 | the ciphertext failed its authentication tag |

Every link object carries `has_secret`, and **never** the secret itself.
The dashboard deliberately does not act on `has_secret` when drawing a tile:
a key icon on the ones that have credentials would tell anyone glancing at
the screen which services have a login saved, which is not theirs to know.
`GET /api/vault` reports `secrets` and `link_secrets` separately.

Deleting a link destroys its credentials, including through the bulk delete in
`PUT /api/links`.

The two kinds use differently shaped additional authenticated data - six raw
MAC bytes for a device, the text `link:<id>` for a link - so a ciphertext
lifted from one slot and dropped into another fails its tag rather than
decrypting under the wrong name, in either direction.

### DELETE /api/links/{id}

```json
{ "ok": true }
```

404 for an unknown id.

### PUT /api/links

Sets the order, and drops any link whose id is not listed, so this doubles as
a bulk delete.

```json
[{ "id": 3 }, { "id": 1 }, { "id": 2 }]
```

A bare array of numbers is accepted too. Returns the reordered list. 400 if any
id is unknown or repeated, in which case the stored list is left untouched.

## Link groups

A group is a heading on the Dashboard. Links are filed under one by id, and a
link with `group: 0` is ungrouped. Groups are ordered by their position in the
table; links are ordered within a group by their position in the link list.

Up to 8 groups. Ids are never reused, so a link still pointing at a deleted
group cannot be adopted by a new one that lands on the same number — it simply
falls back to ungrouped.

### POST /api/links/groups

```json
{ "name": "Infrastructure" }
```

The name is trimmed and must be 1–23 characters once trimmed. Returns
`{ "id": 1, "groups": [...] }`.

400 for an empty or over-long name, 409 when 8 groups already exist.

### PATCH /api/links/groups/{id}

```json
{ "name": "Infra" }
```

Returns the group table. 400 for a bad name, 404 for an unknown id.

### DELETE /api/links/groups/{id}

Deletes the heading. **The links filed under it are kept and become
ungrouped** — deleting a heading never deletes shortcuts. Returns
`{ "groups": [...], "links": [...] }` so the caller can repaint in one step.

404 for an unknown id.

### PUT /api/links/groups

```json
[3, 1, 2]
```

Sets the heading order. Unlike `PUT /api/links` this is **not** a bulk delete:
the ids must be a permutation of the existing ones. 400 otherwise, with the
stored order untouched.

## Per-device notes

A plain-text note kept against a device, for the things you would otherwise
have to remember — where its admin page is, which vault entry holds its
password, what it is actually for.

Notes are served to anyone who can reach the web UI, exactly like a nickname.
Anything that should not be is a **secret**, below.

### PUT /api/devices/{mac}/note

```json
{ "note": "Proxmox host. Root pw in Bitwarden under 'pve'. IPMI on .211." }
```

At most 255 characters; an empty string erases the note. Returns
`{ "ok": true, "has_note": true }`.

400 when the note is too long, 404 for an unknown device.

The note itself comes back on `GET /api/devices/{mac}` as `note`. The device
list carries only the `has_note` flag, so the polled endpoint stays small.

## The secret vault

Credentials, encrypted with AES-256-GCM under a key derived from a passphrase
with PBKDF2-HMAC-SHA256. The key is derived on every unlock and **never
stored**; only a salt, the iteration count and a verifier hash live in flash.

**What this protects against, stated plainly.** It keeps credentials out of the
web UI for anyone who does not have the passphrase, out of the device export,
and out of a physical flash dump, which yields only hardened ciphertext.

**What it does not.** The dashboard is served over plain HTTP, so the
passphrase and any secret you reveal cross the LAN in the clear. Doing the
crypto in the browser instead would fix that and was the first design tried;
`crypto.subtle` is unavailable because it is gated behind a secure context and
`http://landash.local` is not one.

The vault relocks itself after 15 minutes idle and on every reboot.

### GET /api/vault

```json
{
  "configured": true,
  "unlocked": false,
  "idle_timeout_s": 900,
  "expires_in_s": 0,
  "secrets": 3,
  "link_secrets": 5,
  "max_len": 191,
  "min_passphrase": 8
}
```

### PUT /api/vault

Creates the vault, or changes its passphrase.

```json
{ "old_passphrase": "...", "passphrase": "..." }
```

`old_passphrase` is required once a vault exists, and is ignored when one does
not. Changing it re-encrypts every stored secret; every secret is decrypted
into a scratch buffer first, so one unreadable blob aborts the rotation before
anything has been rewritten under a key the rest cannot open.

Returns the vault state **plus a `token`** — the caller has just proved they
own the vault, so they get a session rather than having to unlock again.

400 when the new passphrase is under 8 characters, 403 when `old_passphrase`
is wrong.

### POST /api/vault/unlock

```json
{ "passphrase": "..." }
```

Returns the vault state plus `token`. **This call is deliberately slow** —
about 2.6 s on this hardware — because the key derivation is the entire cost of
guessing the passphrase from a flash dump.

404 when no vault exists, 403 on a wrong passphrase.

### POST /api/vault/lock

Wipes the key from RAM and invalidates the token. Returns the vault state.

### DELETE /api/vault

```json
{ "confirm": "destroy secrets" }
```

Destroys the vault and every secret in it. This is the way out when the
passphrase has been lost; the secrets are not recoverable, by design. The
confirmation phrase must match exactly.

### GET /api/devices/{mac}/secret

Requires an `X-Vault-Token` header from an unlock. Returns
`{ "secret": "..." }`.

401 when the vault is locked or the token is wrong, 404 when there is no
secret, 409 when the ciphertext fails its authentication tag.

### PUT /api/devices/{mac}/secret

```json
{ "secret": "root / hunter2" }
```

Requires `X-Vault-Token`. At most 191 characters; an empty string erases it.
Returns `{ "ok": true, "has_secret": true }`.

The device object carries `has_secret` but **never** the secret itself.

## GET /api/devices/{mac}/history

Whether the device answered, one bit per five-minute slot, over the last 24
hours.

```json
{ "slots": 288, "slot_sec": 300, "valid": 96, "bits": "ffff...3f" }
```

`bits` is 36 bytes as 72 hex characters, **oldest slot first**: slot *i* is bit
`i % 8` of byte `i / 8`. `valid` is how many of the 288 slots have actually
elapsed, so a short history renders as short rather than as a day of downtime.

Slots are cut on wall-clock time, so nothing is recorded until NTP has synced —
a slot number from a wrong clock would file the samples in the wrong place.
This lives in RAM only, for active devices: 36 bytes each, and a day of history
is not worth the flash writes. It refills within a day of a reboot.

## WAN health

Two probes on an interval: an ICMP echo to a fixed address, and a name lookup.
Splitting them is the point — "the internet is down" and "DNS is down" look
identical from a browser and want completely different responses.

A single failed check is not an outage; the state only moves after three
consecutive failures, so one lost echo does not flap the LCD.

Note that lwIP caches DNS answers for their TTL, so a lookup that succeeds may
have been answered from cache. A real resolver failure shows up within a TTL,
not instantly.

### GET /api/wan

```json
{
  "state": "up",
  "icmp_ok": true,
  "dns_ok": true,
  "rtt_ms": 16,
  "last_check": 1789860444,
  "changed_at": 1789860393,
  "checks": 41,
  "failures": 0
}
```

`state` is `up` (both probes answered), `degraded` (one did), `down` (neither),
or `unknown` (not checked yet, switched off, or the dongle is in AP mode with
no WAN to speak of).

The same object is embedded in `GET /api/status` as `wan`.

### POST /api/wan/check

Runs a check on the next tick instead of waiting out the interval.
`{ "ok": true }`.

## Firmware updates

The dongle updates itself from a releases repository fixed at build time
(`CONFIG_NETDASH_OTA_REPO`, default `ThePhilStrongProject/landash-releases`,
branch `CONFIG_NETDASH_OTA_BRANCH`, default `main`). It reads `latest.json`
from `raw.githubusercontent.com`:

```json
{"version": "v0.14.1", "file": "firmware/landash-v0.14.1.bin", "size": 1982176}
```

and installs the named file when `version` is newer than the running firmware.
`file` must be a plain relative path inside the repository; the manifest
chooses which image, never which host. docs/UPDATES.md is the guide to
publishing one.

Why the repository is not a setting: this API has no authentication, so
anything it can change, anyone on the LAN can change. A writable repository
field would let any of them install their own firmware.

Before an image is written, the dongle checks that it names itself as project
`netdash` and as the version `latest.json` gave. A new image boots on probation and is
kept only once it has been up for a minute and, if Wi-Fi is configured, has
got back online within ten; otherwise the bootloader returns to the previous
version, and that version is then never installed automatically again.

Versions compare numerically on `vMAJOR.MINOR.PATCH`; a build a few commits
past a tag counts as that tag. A build with uncommitted changes (`-dirty`) or
without a version tag behind it is a development build and is never replaced
automatically - `auto_blocked` says so - though `POST /api/ota/install` still
installs over it.

Settings, in `GET/PUT /api/settings`:

| Member | Type | Meaning |
|---|---|---|
| `ota_enabled` | bool | check for new releases every `ota_interval_h` hours (and two minutes after boot) |
| `ota_auto` | bool | install a newer release without asking |
| `ota_interval_h` | number, 1-168 | hours between checks, default 12 |

`tour_seen` (bool) records that the welcome tour has been finished or skipped
on this dongle. It is `false` on a new or factory-reset dongle and on one that
has just updated from a version without it, so every user sees the tour once;
the page sets it with `PUT /api/settings`. It lives on the dongle rather than
in the browser so that finishing the tour on one device finishes it for all.

`tour_rev` (number, 0-255) is the revision of the tour the dongle has been
through. When a release adds cards to the tour, it raises the page's revision;
a dongle whose `tour_seen` is true but whose `tour_rev` is lower is shown only
the new cards, once, and the page then sets `tour_rev`. `0` on a dongle that
has not been through a tour since this field was added.

The releases repository is public, so no credentials are involved.

### GET /api/ota

```json
{
  "state": "available",
  "current": "v0.14.0",
  "latest": "v0.14.1",
  "available": true,
  "auto_blocked": false,
  "error": null,
  "rolled_back": null,
  "last_check": 1790000000,
  "bytes_done": 0,
  "bytes_total": 1975488,
  "repo": "ThePhilStrongProject/landash-releases"
}
```

| Field | Type | Notes |
|---|---|---|
| `state` | string | `idle` (not checked since boot), `checking`, `up_to_date`, `available`, `downloading`, `rebooting`, `error` |
| `current` | string | the running version, as `GET /api/status` reports `fw` |
| `latest` | string or null | the version `latest.json` names, once a check has succeeded |
| `available` | bool | `latest` is newer than `current` |
| `auto_blocked` | bool | available, but will not install itself: a development build, or `latest` was rolled back here before |
| `error` | string or null | why the last check or install failed |
| `rolled_back` | string or null | a version that failed to start and was rolled back |
| `last_check` | number | unix seconds of the last completed check, `0` for never or before NTP |
| `bytes_done`, `bytes_total` | number | download progress while `downloading` |
| `repo` | string | where releases come from, `""` when none is built in |

### POST /api/ota/check

Checks GitHub now. Returns `202` with the object above straight away; poll
`GET /api/ota` for the result.

### POST /api/ota/install

Checks, then downloads and installs the latest release if it is newer, even
when `auto_blocked`. `202` with the object above; the dongle restarts about two
seconds after `state` becomes `rebooting`. `409` while a download is running.

---

## The notification feed

Deliberately not the events log. `/api/events` is a raw trace of everything
that happened; this is the short list of things worth telling someone about,
with a read/unread state, so it can answer *what changed since I last looked*
rather than *what happened*.

The feed holds 48 entries and is persisted to flash, so a reboot does not erase
the answer. Writes are coalesced on a 10 s timer, because a sweep can land
several at once.

**Nothing is recorded until the first sweep has finished.** On a freshly
flashed dongle every device on the network is one it has never seen, and
delivering that as twenty-odd "new device" alerts would teach the reader to
ignore the feed on day one.

Each type can be switched off in settings, and a disabled type is dropped at
push time rather than filtered on read — so turning one off does not leave old
entries of it stranded in the ring.

### GET /api/notifications

```json
{
  "items": [
    {
      "id": 12,
      "ts": 1789860444,
      "type": "ip_changed",
      "read": false,
      "text": "Was 192.168.1.31",
      "mac": "bc:24:11:3b:e5:96",
      "device": "truenas",
      "ip": "192.168.1.40"
    }
  ],
  "unread": 3,
  "count": 12
}
```

Newest first. `device` is resolved on every read rather than stored, so a
device renamed since the notification landed reads by its new name. `mac` and
`ip` are `null` when the notification is not about a device.

`type` is one of `new_device`, `ip_changed`, `new_port`, `device_gone`,
`device_back`, `wan_down`, `wan_up`, `update`, `port_closed`.

`new_port` and `port_closed` come only from a repeat scan. A device's first
scan finds its ports without announcing them. A repeat scan works up from the
common ports to `portscan_rescan_tier`, announces a port it finds open that was
not on the device's list, and drops - announcing with `port_closed`, which is
off by default - a listed port it checked and found closed. Nothing is dropped
if the device was not online at the end of the pass, since a device that left
part-way would look as if every port had closed.

`update` covers three things: a newer release was found and is waiting for you
(only when automatic install is off, or blocked - see below), the dongle has
just come back up on a new version, and a new version failed to start and was
rolled back. The first carries no device, so `mac` and `ip` are `null`.

`ip_changed` is posted once the old address has been silent for six minutes -
typically six to eleven minutes after the move, not when it happens. That delay
is how a real move is told apart from a MAC shared by several devices (see
"Devices that share a MAC"): if the old address keeps answering, it was never a
move, and the feed gets a `new_device` for the device that was
hiding behind the shared MAC instead of a stream of moves back and forth. The
device entry itself follows the new address straight away; only the
notification waits.

`unread` is also embedded in `GET /api/status`, so a badge can be kept right
without a second poll.

### POST /api/notifications/read

Marks everything read. `{ "ok": true, "unread": 0 }`.

### DELETE /api/notifications/{id}

Drops one. `DELETE /api/notifications` drops the lot. Returns
`{ "ok": true, "unread": n, "count": n }`. 404 for an unknown id.

## Uploaded icons

A link can wear an actual logo instead of one of the drawn glyphs. Icons are
small PNGs in the `storage` SPIFFS partition, one file each, referenced from a
link's `icon` field as `u:<id>`.

**The browser does the hard part.** It decodes whatever the user picked - PNG,
JPEG, WebP, a screenshot, a 12-megapixel photo - draws it into a 64x64 canvas
preserving aspect ratio, and uploads the result as PNG. So the firmware only
ever sees a small PNG of known dimensions and never has to decode, rescale or
sniff anything. It still validates, because a browser is not the only thing
that can POST.

A real 64x64 logo lands around 2 KB, so 48 icons cost under 100 KB of a 1 MB
partition. The binding limit is the count, not the space.

**Ids are never reused**, which is what lets `GET /api/icons/{id}` be served
with a one-year immutable cache: the bytes behind an id can never change.
Replacing an icon means uploading a new one.

### GET /api/icons

```json
{
  "icons": [{ "id": 4, "bytes": 1930 }],
  "available": true,
  "max_icons": 48,
  "max_bytes": 16384,
  "px": 64,
  "used": 2259,
  "total": 956561
}
```

`available` is `false` when the storage partition could not be mounted. That
is not fatal - uploaded icons are a convenience, and the dashboard boots
without them - so clients should hide the upload control rather than treat it
as an error.

### POST /api/icons

The PNG as the **raw request body**, with `Content-Type: image/png`. Not
multipart: `esp_http_server` has no multipart parser, and a raw body needs no
decoding and no temporary copy in heap, so the bytes go from the socket to
flash a kilobyte at a time.

```
POST /api/icons
Content-Type: image/png

<png bytes>
```

```json
{ "id": 4, "ref": "u:4" }
```

`ref` is exactly what a link's `icon` field wants, so the caller never has to
know how an uploaded icon is spelled.

| Status | Meaning |
|---|---|
| 413 | empty, or over `max_bytes` |
| 415 | not a PNG, or larger than `px` square |
| 409 | `max_icons` already stored |
| 503 | storage is not mounted |

Nothing is left behind on any failure path.

**Why the dimensions are checked and not just the byte count**: flat colour
compresses extraordinarily well. A 2000x2000 single-colour PNG is under 15 KB,
so it would slip past a 16 KB cap and then be decoded at full size by every
browser that loaded the dashboard. The check reads width and height straight
out of the IHDR chunk.

### GET /api/icons/{id}

The PNG, with `Content-Type: image/png` and
`Cache-Control: public, max-age=31536000, immutable`. 404 for an unknown id.

### DELETE /api/icons/{id}

```json
{ "ok": true, "icons": [...] }
```

Links still pointing at the deleted icon are **not** rewritten. An icon
reference that no longer resolves is not an error: the UI falls back to the
derived icon, which is the same thing it would show if the reference had never
been set.
