# NetDash REST API

Version 1. This contract is fixed in WP0 so the firmware (WP6) and the web UI
(WP7) can be built in parallel. Changing it means changing this file first.

- Base URL: `http://netdash.local` (STA) or `http://192.168.4.1` (AP mode).
- All request and response bodies are `application/json; charset=utf-8`.
- No authentication in v1. The dongle is LAN-only.
- All timestamps are **unix seconds (UTC)** as JSON numbers. `0` means "not
  known" — the dongle has not synced NTP yet, so it has no wall-clock time.
- MAC addresses are always lowercase colon-separated: `"aa:bb:cc:dd:ee:ff"`.
- IPv4 addresses are always dotted-quad strings: `"192.168.1.42"`.
- Unknown query parameters and unknown JSON object members are ignored.

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
  "hostname": "netdash",
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
  "devices_total": 27,
  "devices_online": 21,
  "devices_new_24h": 1,
  "ap_ssid": "NetDash-A4F3",
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
| `ap_ssid`, `ap_pass` | string | only present when `mode` is `"ap"` or `"apsta"` |

---

## The device object

Used by `/api/devices`, `/api/devices/{mac}` and `/api/devices/export`.

```json
{
  "mac": "b8:27:eb:0a:1b:2c",
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
| `mac` | string | lowercase colon-separated, the primary key |
| `ip` | string | last known IPv4 |
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

Returns the whole table as an array of device objects.

Query parameters:

| Name | Type | Default | Meaning |
|---|---|---|---|
| `hidden` | `0` or `1` | `0` | include devices with `hidden: true` |
| `online` | `0` or `1` | — | when `1`, only online devices |

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

`Content-Disposition: attachment; filename="netdash-devices.json"`.

---

## POST /api/devices/import

Restores user fields. Accepts exactly what `export` produces; every field other
than `mac`, `nickname`, `type_override` and `hidden` is ignored, and devices in
the file that are not on the network are created as offline entries.

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

## POST /api/scan

Triggers a sweep now. No request body.

```json
{ "ok": true, "scanning": true }
```

Returns 200 with `"scanning": true` if a sweep was already running (the request
is a no-op, not an error). 503 when the dongle has no IP, or when
`passive_only` is set.

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
  "ntp_server": "pool.ntp.org"
}
```

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
| `passive_only` | bool | | disables active sweeps |
| `tz` | string | 0..47 chars | POSIX TZ string |
| `ntp_server` | string | 0..63 chars | |

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
reconnect to the new IP (or to `http://netdash.local`).

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
