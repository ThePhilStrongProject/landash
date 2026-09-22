#!/usr/bin/env python3
"""End-to-end smoke test for a running NetDash dongle.

Exercises every REST endpoint in docs/API.md against real hardware and checks
the response shapes, then does a nickname round-trip to prove the device
database persists user edits.

Nothing here destroys anything you did not ask it to: reboot and factory-reset
are only probed with a deliberately invalid body to confirm they refuse it, the
vault section is skipped entirely when a vault already exists, because
exercising it means destroying it at the end, and a full restore only ever
runs with --restore-roundtrip.

Usage:
    python tools/smoke_test.py                       # http://landash.local
    python tools/smoke_test.py http://192.168.1.58
    python tools/smoke_test.py --restore-roundtrip    # also restores the
                                                       # backup it just made

Decrypting the downloaded backup to check its contents needs the
`cryptography` package (pip install cryptography); without it, that one check
is skipped and noted rather than failed.
"""

import hashlib
import json
import struct
import sys
import time
import urllib.error
import urllib.request
import zlib

try:
    from cryptography.hazmat.primitives import hashes
    from cryptography.hazmat.primitives.ciphers.aead import AESGCM
    from cryptography.hazmat.primitives.kdf.pbkdf2 import PBKDF2HMAC
    HAVE_CRYPTO = True
except ImportError:
    HAVE_CRYPTO = False

TIMEOUT = 10

passed = 0
failed = 0
failures = []


def png(w, h, rgb=(59, 130, 246)):
    """A minimal valid RGB PNG, built by hand so the test needs no image
    library. Flat colour, which is also why it compresses to almost nothing -
    the reason the firmware checks dimensions and not just byte count."""
    raw = b""
    for _ in range(h):
        raw += b"\x00" + bytes(rgb) * w

    def chunk(tag, data):
        c = tag + data
        return struct.pack(">I", len(data)) + c + struct.pack(">I", zlib.crc32(c) & 0xffffffff)

    return (b"\x89PNG\r\n\x1a\n"
            + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(raw, 9))
            + chunk(b"IEND", b""))


def post_raw(base, path, body, ctype="image/png"):
    """Returns (status, parsed_json_or_raw, headers). request() always sends
    JSON, and an icon upload is the raw file."""
    req = urllib.request.Request(base + path, data=bytes(body), method="POST")
    req.add_header("Content-Type", ctype)
    try:
        with urllib.request.urlopen(req, timeout=TIMEOUT) as r:
            return r.status, json.loads(r.read() or b"null"), dict(r.headers)
    except urllib.error.HTTPError as e:
        raw = e.read()
        try:
            return e.code, json.loads(raw or b"null"), dict(e.headers)
        except Exception:  # noqa: BLE001
            return e.code, raw, dict(e.headers)
    except Exception as e:  # noqa: BLE001
        return None, str(e), {}


def get_raw(base, path):
    """Fetches a non-JSON body, such as an icon's PNG bytes."""
    try:
        with urllib.request.urlopen(base + path, timeout=TIMEOUT) as r:
            return r.status, r.read(), dict(r.headers)
    except urllib.error.HTTPError as e:
        return e.code, e.read(), dict(e.headers)
    except Exception as e:  # noqa: BLE001
        return None, str(e).encode(), {}


def is_hex(s, length):
    return isinstance(s, str) and len(s) == length and all(c in "0123456789abcdef" for c in s.lower())


def parse_landash_header(raw):
    """Parses the fixed framing every .landash file starts with - magic,
    header length and the header JSON - none of which needs the passphrase.
    Returns (header_dict, body_bytes) where body is everything after the
    header, i.e. the still-encrypted records."""
    if len(raw) < 10 or raw[:8] != b"LANDASH\n":
        raise ValueError("bad magic")
    hlen = struct.unpack("<H", raw[8:10])[0]
    if hlen > 1024 or len(raw) < 10 + hlen:
        raise ValueError("header length out of range")
    header = json.loads(raw[10:10 + hlen].decode("utf-8"))
    return header, raw[10 + hlen:]


def iter_record_spans(body):
    """Walks the record framing (a 4-byte cleartext length before each
    ciphertext+tag) without decrypting anything, so record boundaries -
    including the truncation point used to drop the last record - can be
    found without the passphrase. Returns a list of (start, end) byte offsets
    into `body`, one per record, each spanning the whole record: its 4-byte
    length prefix plus the ciphertext and tag."""
    spans = []
    off = 0
    while off < len(body):
        if off + 4 > len(body):
            raise ValueError("truncated record length")
        (length,) = struct.unpack("<I", body[off:off + 4])
        if length < 1 or length > 8192:
            raise ValueError(f"record length {length} out of range")
        start = off
        off += 4 + length + 16
        if off > len(body):
            raise ValueError("truncated record body")
        spans.append((start, off))
    return spans


def decrypt_landash(raw, passphrase):
    """Decrypts every record of a downloaded .landash file, the real proof
    the format matches docs/API.md rather than just its framing. Returns
    (header, records), where each record is (type_byte, plaintext_after_type).
    Raises (InvalidTag, from `cryptography`) on a wrong passphrase or a
    damaged file, same as the firmware refuses one."""
    header, body = parse_landash_header(raw)
    header_len = len(raw) - len(body)
    aad = hashlib.sha256(raw[:header_len]).digest()

    kdf = PBKDF2HMAC(algorithm=hashes.SHA256(), length=32,
                      salt=bytes.fromhex(header["salt"]), iterations=header["iterations"])
    aesgcm = AESGCM(kdf.derive(passphrase.encode()))
    nonce_prefix = bytes.fromhex(header["nonce"])

    records = []
    for idx, (start, end) in enumerate(iter_record_spans(body)):
        # Spans include the 4-byte length prefix (so a truncation point can
        # drop a whole record, prefix and all) - skip it for decryption.
        nonce = nonce_prefix + struct.pack(">I", idx)
        pt = aesgcm.decrypt(nonce, body[start + 4:end], aad)
        records.append((pt[0:1], pt[1:]))
    return header, records


def parse_record_ns_key(type_byte, payload):
    """For an 'N' record's plaintext (after the type byte): (namespace, key)."""
    if type_byte != b"N":
        return None, None
    nlen = payload[0]
    ns = payload[1:1 + nlen].decode("utf-8", "replace")
    klen = payload[1 + nlen]
    key = payload[2 + nlen:2 + nlen + klen].decode("utf-8", "replace")
    return ns, key


def parse_record_filename(type_byte, payload):
    """For an 'F' record's plaintext (after the type byte): the file name."""
    if type_byte != b"F":
        return None
    nlen = payload[0]
    return payload[1:1 + nlen].decode("utf-8", "replace")


def check(name, condition, detail=""):
    global passed, failed
    if condition:
        passed += 1
        print(f"  ok   {name}")
    else:
        failed += 1
        failures.append(f"{name}: {detail}")
        print(f"  FAIL {name}  {detail}")


def request(base, method, path, body=None, expect=200, extra_headers=None):
    """Returns (status, parsed_json_or_raw_bytes, headers)."""
    url = base + path
    data = None
    headers = {"Accept": "application/json"}
    if body is not None:
        data = json.dumps(body).encode()
        headers["Content-Type"] = "application/json"
    if extra_headers:
        headers.update(extra_headers)

    req = urllib.request.Request(url, data=data, method=method, headers=headers)
    try:
        with urllib.request.urlopen(req, timeout=TIMEOUT) as r:
            raw = r.read()
            status = r.status
            hdrs = dict(r.headers)
    except urllib.error.HTTPError as e:
        raw = e.read()
        status = e.code
        hdrs = dict(e.headers)
    except Exception as e:  # noqa: BLE001 - surfaced as a test failure
        return None, str(e), {}

    try:
        parsed = json.loads(raw.decode("utf-8", "replace"))
    except Exception:  # noqa: BLE001 - some endpoints return HTML
        parsed = raw

    if expect is not None and status != expect:
        return status, parsed, hdrs
    return status, parsed, hdrs


STATUS_FIELDS = [
    "ip", "netmask", "gateway", "mac", "mode", "ssid", "rssi", "hostname",
    "uptime_s", "heap_free", "heap_min_free", "fw", "idf", "time",
    "scanning", "last_sweep", "devices_total", "devices_online",
]

DEVICE_FIELDS = [
    "mac", "ip", "display_name", "hostname", "vendor", "type",
    "services", "online", "is_new", "first_seen", "last_seen",
]


def main():
    args = sys.argv[1:]
    restore_roundtrip = "--restore-roundtrip" in args
    args = [a for a in args if a != "--restore-roundtrip"]
    base = (args[0] if args else "http://landash.local").rstrip("/")
    print(f"NetDash smoke test against {base}\n")

    # --- the web UI itself -------------------------------------------------
    print("static")
    st, body, hdrs = request(base, "GET", "/")
    check("GET / returns 200", st == 200, f"status {st}")
    check("GET / is gzipped",
          hdrs.get("Content-Encoding") == "gzip",
          f"Content-Encoding {hdrs.get('Content-Encoding')!r}")

    # --- status ------------------------------------------------------------
    print("\nstatus")
    st, status, _ = request(base, "GET", "/api/status")
    check("GET /api/status returns 200", st == 200, f"status {st}")
    if isinstance(status, dict):
        missing = [f for f in STATUS_FIELDS if f not in status]
        check("status has every documented field", not missing, f"missing {missing}")
        check("mode is valid",
              status.get("mode") in ("off", "sta", "ap", "apsta"),
              f"mode {status.get('mode')!r}")
        print(f"       mode={status.get('mode')} ip={status.get('ip')} "
              f"heap={status.get('heap_free')} devices={status.get('devices_total')}")
    else:
        check("status is JSON", False, repr(status)[:120])
        return

    # --- devices -----------------------------------------------------------
    print("\ndevices")
    st, devices, _ = request(base, "GET", "/api/devices?hidden=1")
    check("GET /api/devices returns 200", st == 200, f"status {st}")
    check("devices is a list", isinstance(devices, list), type(devices).__name__)

    if isinstance(devices, list) and devices:
        d = devices[0]
        missing = [f for f in DEVICE_FIELDS if f not in d]
        check("device has every documented field", not missing, f"missing {missing}")
        check("services is a list", isinstance(d.get("services"), list),
              type(d.get("services")).__name__)
        check("mac is formatted", isinstance(d.get("mac"), str)
              and len(d["mac"]) == 17 and d["mac"].count(":") == 5,
              repr(d.get("mac")))

        mac = d["mac"]
        st, one, _ = request(base, "GET", f"/api/devices/{mac}")
        check("GET /api/devices/{mac} returns 200", st == 200, f"status {st}")
        check("single device matches", isinstance(one, dict) and one.get("mac") == mac)

        # nickname round-trip
        original = d.get("nickname") or ""
        probe = "smoke-test-nickname"
        st, _, _ = request(base, "PATCH", f"/api/devices/{mac}", {"nickname": probe})
        check("PATCH nickname returns 200", st == 200, f"status {st}")

        st, after, _ = request(base, "GET", f"/api/devices/{mac}")
        check("nickname round-trips",
              isinstance(after, dict) and after.get("nickname") == probe,
              f"got {after.get('nickname')!r}" if isinstance(after, dict) else "")
        check("display_name follows nickname",
              isinstance(after, dict) and after.get("display_name") == probe,
              f"got {after.get('display_name')!r}" if isinstance(after, dict) else "")

        # restore
        request(base, "PATCH", f"/api/devices/{mac}", {"nickname": original})

        st, _, _ = request(base, "PATCH", f"/api/devices/{mac}",
                           {"type_override": "definitely-not-a-type"})
        check("PATCH rejects a bad type", st == 400, f"status {st}")
    else:
        print("       (no devices discovered yet, skipping device checks)")

    st, _, _ = request(base, "GET", "/api/devices/zz:zz:zz:zz:zz:zz")
    check("GET unknown mac is rejected", st in (400, 404), f"status {st}")

    # --- export ------------------------------------------------------------
    print("\nexport")
    st, exported, hdrs = request(base, "GET", "/api/devices/export")
    check("GET /api/devices/export returns 200", st == 200, f"status {st}")
    check("export is an attachment",
          "attachment" in hdrs.get("Content-Disposition", ""),
          repr(hdrs.get("Content-Disposition")))
    check("export is JSON", isinstance(exported, (list, dict)), type(exported).__name__)

    # --- scan, events ------------------------------------------------------
    print("\nscan and events")
    st, scan, _ = request(base, "POST", "/api/scan")
    check("POST /api/scan returns 200", st == 200, f"status {st}")

    st, events, _ = request(base, "GET", "/api/events?limit=10")
    check("GET /api/events returns 200", st == 200, f"status {st}")
    check("events is a list", isinstance(events, list), type(events).__name__)

    # --- settings ----------------------------------------------------------
    print("\nsettings")
    st, settings, _ = request(base, "GET", "/api/settings")
    check("GET /api/settings returns 200", st == 200, f"status {st}")
    if isinstance(settings, dict):
        check("wifi_pass is never returned", "wifi_pass" not in settings
              or settings.get("wifi_pass") == "",
              f"got {settings.get('wifi_pass')!r}")
        for f in ("wifi_ssid", "hostname", "scan_interval_min", "hosts_per_sec",
                  "passive_only", "tz", "ntp_server"):
            check(f"settings has {f}", f in settings)

        # a harmless round-trip: write the current values back unchanged
        echo = dict(settings)
        echo.pop("wifi_pass", None)
        st, _, _ = request(base, "PUT", "/api/settings", echo)
        check("PUT /api/settings accepts its own output", st == 200, f"status {st}")

        st, _, _ = request(base, "PUT", "/api/settings", {"scan_interval_min": 99999})
        check("PUT rejects an out-of-range interval", st == 400, f"status {st}")

    # --- wifi scan ---------------------------------------------------------
    print("\nwifi scan")
    st, aps, _ = request(base, "POST", "/api/wifi/scan")
    check("POST /api/wifi/scan returns 200", st == 200, f"status {st}")
    check("wifi scan is a list", isinstance(aps, list), type(aps).__name__)
    if isinstance(aps, list) and aps:
        check("ap has ssid and rssi",
              "ssid" in aps[0] and "rssi" in aps[0], repr(aps[0])[:120])
        print(f"       {len(aps)} networks, strongest "
              f"{aps[0].get('ssid')!r} at {aps[0].get('rssi')} dBm")


    # --- dashboard groups ---------------------------------------------------
    print("\nlink groups")
    st, links, _ = request(base, "GET", "/api/links")
    check("links returns groups and links",
          st == 200 and isinstance(links, dict) and "groups" in links and "links" in links,
          repr(links)[:120])

    # Clean up anything a previous interrupted run left behind.
    for g in (links or {}).get("groups", []):
        if g.get("name", "").startswith("smoketest"):
            request(base, "DELETE", f"/api/links/groups/{g['id']}")

    st, created, _ = request(base, "POST", "/api/links/groups", {"name": "  smoketest-a  "})
    gid = (created or {}).get("id")
    check("create a group", st == 200 and gid, f"status {st} {created!r}"[:120])
    check("group name is trimmed",
          any(g["name"] == "smoketest-a" for g in (created or {}).get("groups", [])),
          repr(created)[:120])

    st, _, _ = request(base, "POST", "/api/links/groups", {"name": ""})
    check("empty group name is rejected", st == 400, f"status {st}")

    st, _, _ = request(base, "PATCH", "/api/links/groups/250", {"name": "nope"})
    check("renaming an unknown group is 404", st == 404, f"status {st}")

    # --- notes on links -----------------------------------------------------
    check("the link payload advertises a note limit",
          isinstance((links or {}).get("max_note"), int), (links or {}).get("max_note"))

    existing = (links or {}).get("links", [])
    if not existing:
        print("       no links to annotate - skipping the link-note checks")
    else:
        lid = existing[0]["id"]
        before_note = existing[0].get("note", "")
        check("links carry a note field", "note" in existing[0], sorted(existing[0])[:10])

        st, l, _ = request(base, "PATCH", f"/api/links/{lid}",
                           {"note": "smoke test link note"})
        check("save a link note",
              st == 200 and (l or {}).get("note") == "smoke test link note",
              f"status {st} {(l or {}).get('note')!r}")

        st, r, _ = request(base, "GET", "/api/links")
        got = [x for x in r.get("links", []) if x["id"] == lid]
        check("the note comes back on the list",
              got and got[0].get("note") == "smoke test link note",
              repr(got)[:120])

        st, _, _ = request(base, "PATCH", f"/api/links/{lid}", {"note": "x" * 400})
        check("an over-long link note is rejected", st == 400, f"status {st}")

        st, _, _ = request(base, "PATCH", "/api/links/64000", {"note": "nobody"})
        check("a note on an unknown link is 404", st == 404, f"status {st}")

        st, l, _ = request(base, "PATCH", f"/api/links/{lid}", {"note": ""})
        check("clear a link note", st == 200 and not (l or {}).get("note"),
              f"status {st}")

        if before_note:
            request(base, "PATCH", f"/api/links/{lid}", {"note": before_note})

    if gid:
        st, renamed, _ = request(base, "PATCH", f"/api/links/groups/{gid}",
                                 {"name": "smoketest-b"})
        check("rename a group",
              st == 200 and any(g["name"] == "smoketest-b" for g in (renamed or [])),
              f"status {st}")
        st, _, _ = request(base, "DELETE", f"/api/links/groups/{gid}")
        check("delete a group", st == 200, f"status {st}")

    # --- notes --------------------------------------------------------------
    print("\nnotes")
    st, devices, _ = request(base, "GET", "/api/devices")
    mac = devices[0]["mac"] if isinstance(devices, list) and devices else None
    check("a device is available to annotate", mac is not None, "no devices listed")

    if mac:
        check("device list carries note and secret markers",
              "has_note" in devices[0] and "has_secret" in devices[0],
              sorted(devices[0])[:10])

        st, r, _ = request(base, "PUT", f"/api/devices/{mac}/note",
                           {"note": "smoke test note"})
        check("save a note", st == 200 and (r or {}).get("has_note"), f"status {st}")

        st, d, _ = request(base, "GET", f"/api/devices/{mac}")
        check("note round-trips", (d or {}).get("note") == "smoke test note",
              repr((d or {}).get("note"))[:80])

        st, _, _ = request(base, "PUT", f"/api/devices/{mac}/note", {"note": "x" * 300})
        check("an over-long note is rejected", st == 400, f"status {st}")

        st, r, _ = request(base, "PUT", f"/api/devices/{mac}/note", {"note": ""})
        check("clear a note", st == 200 and not (r or {}).get("has_note"), f"status {st}")

    # --- availability history ----------------------------------------------
    print("\nhistory")
    if mac:
        st, h, _ = request(base, "GET", f"/api/devices/{mac}/history")
        check("history endpoint answers", st == 200 and isinstance(h, dict), f"status {st}")
        check("history is a 288-slot day",
              (h or {}).get("slots") == 288 and (h or {}).get("slot_sec") == 300,
              repr(h)[:120])
        check("history bitmap is 36 bytes of hex",
              len((h or {}).get("bits", "")) == 72, len((h or {}).get("bits", "")))

    # --- WAN health ---------------------------------------------------------
    print("\nwan health")
    st, w, _ = request(base, "GET", "/api/wan")
    check("wan endpoint answers", st == 200 and "state" in (w or {}), f"status {st}")
    check("wan state is one of the four",
          (w or {}).get("state") in ("up", "degraded", "down", "unknown"),
          (w or {}).get("state"))
    check("wan reports both probes separately",
          "icmp_ok" in (w or {}) and "dns_ok" in (w or {}), sorted(w or {}))
    st, _, _ = request(base, "POST", "/api/wan/check")
    check("wan check can be forced", st == 200, f"status {st}")

    st, status, _ = request(base, "GET", "/api/status")
    check("status embeds the wan verdict", isinstance((status or {}).get("wan"), dict),
          repr((status or {}).get("wan"))[:80])

    # --- firmware updates ---------------------------------------------------
    # Read-only apart from the check interval, which is put back.
    print("\nfirmware updates")
    st, o, _ = request(base, "GET", "/api/ota")
    o = o if isinstance(o, dict) else {}
    check("ota endpoint answers", st == 200 and "state" in o, f"status {st}")
    check("ota state is a known one",
          o.get("state") in ("idle", "checking", "up_to_date", "available",
                             "downloading", "rebooting", "error"), repr(o.get("state")))
    check("ota reports the running version as status does",
          o.get("current") == (status or {}).get("fw"),
          f"{o.get('current')!r} vs {(status or {}).get('fw')!r}")
    check("ota names the repository it trusts", isinstance(o.get("repo"), str) and "/" in o.get("repo", ""),
          repr(o.get("repo")))
    st, cfg, _ = request(base, "GET", "/api/settings")
    cfg = cfg if isinstance(cfg, dict) else {}
    check("settings carry the update switches",
          all(k in cfg for k in ("ota_enabled", "ota_auto", "ota_interval_h", "tour_seen")),
          ", ".join(k for k in ("ota_enabled", "ota_auto", "ota_interval_h", "tour_seen")
                    if k not in cfg))
    old_interval = cfg.get("ota_interval_h", 12)
    st, _, _ = request(base, "PUT", "/api/settings", {"ota_interval_h": 169}, expect=None)
    check("an out-of-range check interval is refused", st == 400, f"status {st}")
    st, _, _ = request(base, "PUT", "/api/settings", {"ota_interval_h": 24})
    st2, cfg2, _ = request(base, "GET", "/api/settings")
    check("the check interval round-trips", (cfg2 or {}).get("ota_interval_h") == 24,
          repr((cfg2 or {}).get("ota_interval_h")))
    request(base, "PUT", "/api/settings", {"ota_interval_h": old_interval})
    st, _, _ = request(base, "POST", "/api/ota/check", expect=None)
    check("a check can be requested", st == 202, f"status {st}")

    # --- notification feed --------------------------------------------------
    print("\nnotifications")
    st, n, _ = request(base, "GET", "/api/notifications")
    check("feed endpoint answers",
          st == 200 and isinstance((n or {}).get("items"), list), f"status {st}")
    check("feed reports an unread count", isinstance((n or {}).get("unread"), int),
          repr(n)[:80])
    check("status embeds the unread count", isinstance((status or {}).get("unread"), int),
          (status or {}).get("unread"))
    st, _, _ = request(base, "POST", "/api/notifications/read")
    check("mark all read", st == 200, f"status {st}")

    st, cfg, _ = request(base, "GET", "/api/settings")
    check("settings carry per-type notification toggles",
          isinstance((cfg or {}).get("notifications"), dict),
          repr((cfg or {}).get("notifications"))[:80])
    st, _, _ = request(base, "PUT", "/api/settings", {"notifications": {"nonsense": True}})
    check("an unknown notification type is rejected", st == 400, f"status {st}")

    # --- vault --------------------------------------------------------------
    print("\nsecret vault")
    st, v, _ = request(base, "GET", "/api/vault")
    check("vault status answers", st == 200 and "configured" in (v or {}), f"status {st}")

    if (v or {}).get("configured"):
        print("       a vault is already set up - skipping the destructive checks")
    elif mac:
        st, _, _ = request(base, "PUT", "/api/vault", {"passphrase": "short"})
        check("a short passphrase is rejected", st == 400, f"status {st}")

        st, v, _ = request(base, "PUT", "/api/vault", {"passphrase": "smoke test passphrase"})
        token = (v or {}).get("token")
        check("create the vault", st == 200 and token, f"status {st}")

        st, _, _ = request(base, "PUT", f"/api/devices/{mac}/secret", {"secret": "hunter2"})
        check("storing a secret needs a token", st == 401, f"status {st}")

        auth = {"X-Vault-Token": token or ""}
        st, r, _ = request(base, "PUT", f"/api/devices/{mac}/secret",
                           {"secret": "hunter2"}, extra_headers=auth)
        check("store a secret", st == 200 and (r or {}).get("has_secret"), f"status {st}")

        st, r, _ = request(base, "GET", f"/api/devices/{mac}/secret", extra_headers=auth)
        check("read the secret back", st == 200 and (r or {}).get("secret") == "hunter2",
              repr(r)[:80])

        st, d, _ = request(base, "GET", f"/api/devices/{mac}")
        check("the secret never rides on the device object", "secret" not in (d or {}),
              sorted(d or {})[:10])

        st, _, _ = request(base, "GET", f"/api/devices/{mac}/secret",
                           extra_headers={"X-Vault-Token": "0" * 32})
        check("a wrong token is rejected", st == 401, f"status {st}")

        st, v2, _ = request(base, "PUT", "/api/vault",
                            {"old_passphrase": "smoke test passphrase",
                             "passphrase": "smoke test passphrase two"})
        token2 = (v2 or {}).get("token")
        check("rotate the passphrase", st == 200 and token2, f"status {st}")

        st, r, _ = request(base, "GET", f"/api/devices/{mac}/secret",
                           extra_headers={"X-Vault-Token": token2 or ""})
        check("the secret survives re-encryption",
              st == 200 and (r or {}).get("secret") == "hunter2", repr(r)[:80])

        # --- credentials on a link, which share the vault -------------------
        current_pass = "smoke test passphrase two"
        st, ls, _ = request(base, "GET", "/api/links")
        link_ids = [x["id"] for x in (ls or {}).get("links", [])]
        if not link_ids:
            print("       no links to attach credentials to - skipping")
        else:
            lid = link_ids[0]
            auth2 = {"X-Vault-Token": token2 or ""}
            check("links advertise a credentials flag",
                  "has_secret" in ls["links"][0], sorted(ls["links"][0])[:10])

            st, r, _ = request(base, "PUT", f"/api/links/{lid}/secret",
                               {"secret": "smoke / linkpass"})
            check("link credentials need a token", st == 401, f"status {st}")

            st, r, _ = request(base, "PUT", f"/api/links/{lid}/secret",
                               {"secret": "smoke / linkpass"}, extra_headers=auth2)
            check("store credentials on a link",
                  st == 200 and (r or {}).get("has_secret"), f"status {st}")

            st, r, _ = request(base, "GET", f"/api/links/{lid}/secret",
                               extra_headers=auth2)
            check("read link credentials back",
                  st == 200 and (r or {}).get("secret") == "smoke / linkpass",
                  repr(r)[:80])

            st, r, _ = request(base, "GET", "/api/links")
            got = [x for x in r.get("links", []) if x["id"] == lid]
            check("the flag shows on the link", got and got[0].get("has_secret"),
                  repr(got)[:100])
            check("credentials never ride on the link object",
                  got and "secret" not in got[0], sorted(got[0])[:12] if got else "")

            # A rotation that forgot a namespace would lose these silently,
            # so check both kinds come through the same one.
            st, v5, _ = request(base, "PUT", "/api/vault",
                                {"old_passphrase": "smoke test passphrase two",
                                 "passphrase": "smoke test passphrase three"})
            token3 = (v5 or {}).get("token")
            check("rotate again with both kinds stored", st == 200 and token3,
                  f"status {st}")
            auth3 = {"X-Vault-Token": token3 or ""}

            st, r, _ = request(base, "GET", f"/api/links/{lid}/secret",
                               extra_headers=auth3)
            check("link credentials survive rotation",
                  st == 200 and (r or {}).get("secret") == "smoke / linkpass",
                  repr(r)[:80])
            st, r, _ = request(base, "GET", f"/api/devices/{mac}/secret",
                               extra_headers=auth3)
            check("device secrets survive the same rotation",
                  st == 200 and (r or {}).get("secret") == "hunter2", repr(r)[:80])

            st, v6, _ = request(base, "GET", "/api/vault")
            check("the vault counts both kinds",
                  (v6 or {}).get("secrets") == 1 and (v6 or {}).get("link_secrets") == 1,
                  repr(v6)[:120])

            st, _, _ = request(base, "PUT", f"/api/links/{lid}/secret",
                               {"secret": ""}, extra_headers=auth3)
            check("clear link credentials", st == 200, f"status {st}")

            token2 = token3
            current_pass = "smoke test passphrase three"

        st, _, _ = request(base, "POST", "/api/vault/unlock", {"passphrase": "wrong"})
        check("a wrong passphrase is refused", st == 403, f"status {st}")
        st, _, _ = request(base, "POST", "/api/vault/unlock", {"passphrase": current_pass})
        check("unlock with the current passphrase", st == 200, f"status {st}")

        st, _, _ = request(base, "POST", "/api/vault/lock")
        check("lock the vault", st == 200, f"status {st}")
        st, _, _ = request(base, "GET", f"/api/devices/{mac}/secret",
                           extra_headers={"X-Vault-Token": token2 or ""})
        check("the token dies with the lock", st == 401, f"status {st}")

        st, _, _ = request(base, "DELETE", "/api/vault", {"confirm": "wrong words"})
        check("destroying the vault needs the exact phrase", st == 400, f"status {st}")

        st, v3, _ = request(base, "DELETE", "/api/vault", {"confirm": "destroy secrets"})
        check("destroy the vault", st == 200 and not (v3 or {}).get("configured"),
              f"status {st}")


    # --- uploaded icons -----------------------------------------------------
    print("\nuploaded icons")
    st, ic, _ = request(base, "GET", "/api/icons")
    check("icon endpoint answers", st == 200 and isinstance(ic, dict), f"status {st}")

    if not (ic or {}).get("available"):
        print("       storage is not mounted - skipping the upload checks")
    else:
        check("icon limits are reported",
              (ic or {}).get("px") and (ic or {}).get("max_icons") and (ic or {}).get("max_bytes"),
              repr(ic)[:120])
        print(f"       {ic.get('used', 0)} of {ic.get('total', 0)} bytes used, "
              f"{len(ic.get('icons', []))} stored")

        pre_ids = {i["id"] for i in ic.get("icons", [])}

        blob = png(64, 64)
        st, r, _ = post_raw(base, "/api/icons", blob)
        icon_id = (r or {}).get("id") if isinstance(r, dict) else None
        check("upload a 64x64 png", st == 200 and icon_id, f"status {st} {r!r}"[:120])
        check("upload returns a link-ready ref",
              isinstance(r, dict) and r.get("ref") == f"u:{icon_id}", repr(r)[:80])

        if icon_id:
            st, raw, hdrs = get_raw(base, f"/api/icons/{icon_id}")
            check("serve the icon back", st == 200 and raw == blob,
                  f"{len(raw)} bytes vs {len(blob)}")
            check("served as image/png",
                  hdrs.get("Content-Type", "").startswith("image/png"),
                  hdrs.get("Content-Type"))
            check("served with an immutable cache",
                  "immutable" in hdrs.get("Cache-Control", ""), hdrs.get("Cache-Control"))

        st, r, _ = post_raw(base, "/api/icons", b"definitely not a png")
        check("a non-png is rejected", st == 415, f"status {st}")

        # Flat colour at 2000x2000 compresses to well under the byte cap, so
        # this only fails if the dimensions are actually being checked.
        big = png(2000, 2000)
        st, r, _ = post_raw(base, "/api/icons", big)
        check("an oversized png is rejected on its dimensions", st == 415,
              f"status {st}, {len(big)} bytes")

        st, r, _ = post_raw(base, "/api/icons", b"")
        check("an empty upload is rejected", st in (400, 413), f"status {st}")

        st, _, _ = get_raw(base, "/api/icons/65000")
        check("an unknown icon is 404", st == 404, f"status {st}")

        st, r, _ = request(base, "GET", "/api/icons")
        stray = [i["id"] for i in r.get("icons", [])
                 if i["id"] != icon_id and i["id"] not in pre_ids]
        check("failed uploads leave nothing behind", not stray, stray)

        if icon_id:
            st, r2, _ = post_raw(base, "/api/icons", png(48, 48))
            second = (r2 or {}).get("id") if isinstance(r2, dict) else None
            request(base, "DELETE", f"/api/icons/{second}")
            st, r3, _ = post_raw(base, "/api/icons", png(32, 32))
            third = (r3 or {}).get("id") if isinstance(r3, dict) else None
            check("ids are never reused", third and second and third != second,
                  f"second {second}, third {third}")
            request(base, "DELETE", f"/api/icons/{third}")

            st, _, _ = request(base, "DELETE", f"/api/icons/{icon_id}")
            check("delete an icon", st == 200, f"status {st}")
            st, _, _ = get_raw(base, f"/api/icons/{icon_id}")
            check("it is gone afterwards", st == 404, f"status {st}")

    # --- full backup / restore ----------------------------------------------
    print("\nbackup and restore")
    st, b, _ = request(base, "GET", "/api/backup")
    check("GET /api/backup answers", st == 200 and isinstance(b, dict), f"status {st}")
    check("backup status has last_backup and restored",
          isinstance((b or {}).get("last_backup"), int) and "restored" in (b or {}),
          repr(b)[:120])

    st, _, _ = request(base, "POST", "/api/backup", {"passphrase": "xyz"})
    check("a too-short backup passphrase is rejected", st == 400, f"status {st}")

    st, _, _ = request(base, "POST", "/api/backup", {"passphrase": "has a\nnewline in it"})
    check("a backup passphrase with a newline is rejected", st == 400, f"status {st}")

    backup_pass = "smoke test backup passphrase"
    st, raw, hdrs = request(base, "POST", "/api/backup", {"passphrase": backup_pass})
    check("POST /api/backup returns 200", st == 200, f"status {st}")

    cd = hdrs.get("Content-Disposition", "")
    check("backup is an attachment ending in .landash",
          "attachment" in cd and cd.rstrip('"').endswith(".landash"), repr(cd))

    header, body, spans = None, b"", []
    if st == 200 and isinstance(raw, (bytes, bytearray)):
        check("backup body starts with the LANDASH magic", raw[:8] == b"LANDASH\n",
              repr(raw[:12]))
        try:
            header, body = parse_landash_header(raw)
            spans = iter_record_spans(body)
            check("header parses and records follow it", True)
        except Exception as e:  # noqa: BLE001
            check("header parses and records follow it", False, str(e))
    else:
        check("backup body is bytes", False, repr(raw)[:120])

    if header is not None:
        for f in ("format", "fw", "hostname", "created", "devices", "links",
                  "icons", "kdf", "iterations", "salt", "nonce"):
            check(f"backup header has {f}", f in header, sorted(header))
        check("format is 1", header.get("format") == 1, repr(header.get("format")))
        check("kdf is pbkdf2-sha256", header.get("kdf") == "pbkdf2-sha256", repr(header.get("kdf")))
        check("iterations is an int", isinstance(header.get("iterations"), int),
              repr(header.get("iterations")))
        check("salt is 32 hex chars", is_hex(header.get("salt"), 32), repr(header.get("salt")))
        check("nonce is 16 hex chars", is_hex(header.get("nonce"), 16), repr(header.get("nonce")))
        check("at least one record follows the header", len(spans) > 0, len(spans))

        if HAVE_CRYPTO:
            try:
                _, records = decrypt_landash(raw, backup_pass)
                types = {r[0] for r in records}
                check("every decrypted record has a known type",
                      types <= {b"N", b"F", b"D", b"E"}, types)
                last_ok = bool(records) and records[-1][0] == b"E" and \
                    struct.unpack("<I", records[-1][1][:4])[0] == len(records) - 1
                check("the last record is E with the right count", last_ok,
                      repr(records[-1]) if records else "no records")
                has_cfg = any(parse_record_ns_key(*r)[0] == "cfg" for r in records if r[0] == b"N")
                check("an N record for namespace \"cfg\" exists", has_cfg)
                has_devdb = any(parse_record_filename(*r) == "devices.db"
                                for r in records if r[0] == b"F")
                check("an F record named \"devices.db\" exists", has_devdb)
            except Exception as e:  # noqa: BLE001
                check("the backup decrypts under its own passphrase", False, str(e))
        else:
            print("       cryptography package not importable - skipping the decrypt checks")

    st, b2, _ = request(base, "GET", "/api/backup")
    check("GET /api/backup still answers after a download",
          st == 200 and isinstance((b2 or {}).get("last_backup"), int), repr(b2)[:120])
    if (status or {}).get("time_synced"):
        check("last_backup is nonzero once the clock has synced",
              (b2 or {}).get("last_backup", 0) != 0, repr(b2)[:120])

    # restore: three ways to refuse, none of which should touch the dongle
    st, _, _ = post_raw(base, "/api/restore", backup_pass.encode() + b"\n" + b"x" * 100,
                         ctype="application/octet-stream")
    check("restore rejects a body that is not a backup at all", st == 400, f"status {st}")

    if header is not None:
        st, _, _ = post_raw(base, "/api/restore", b"the wrong passphrase\n" + raw,
                             ctype="application/octet-stream")
        check("restore rejects the wrong passphrase", st == 403, f"status {st}")

        if spans:
            truncated = raw[:len(raw) - len(body) + spans[-1][0]]
            st, _, _ = post_raw(base, "/api/restore",
                                 backup_pass.encode() + b"\n" + truncated,
                                 ctype="application/octet-stream")
            check("restore rejects a backup missing its last record", st == 400, f"status {st}")
        else:
            check("restore rejects a backup missing its last record", False, "no records to drop")

    st, _, _ = request(base, "GET", "/api/status")
    check("the dongle is still up after the restore refusals", st == 200, f"status {st}")

    if restore_roundtrip and header is not None:
        print("\nrestore round-trip (--restore-roundtrip)")
        st, r, _ = post_raw(base, "/api/restore", backup_pass.encode() + b"\n" + raw,
                             ctype="application/octet-stream")
        check("restore accepts the real backup", st == 200 and isinstance(r, dict) and r.get("ok"),
              f"status {st} {r!r}"[:160])
        if st == 200:
            print("       waiting for the dongle to come back...")
            # It restarts a second after replying; don't mistake the old
            # instance, still up for that second, for the restored one.
            time.sleep(8)
            deadline = time.time() + 90
            back = False
            while time.time() < deadline and not back:
                time.sleep(3)
                st2, _, _ = request(base, "GET", "/api/status")
                back = st2 == 200
            check("the dongle comes back within 90s", back)
            if back:
                st3, b3, _ = request(base, "GET", "/api/backup")
                check("GET /api/backup.restored is set after the round trip",
                      bool((b3 or {}).get("restored")), repr(b3)[:120])
                st4, s4, _ = request(base, "GET", "/api/status")
                check("device count matches the header",
                      isinstance(s4, dict) and s4.get("devices_total") == header.get("devices"),
                      f"status {st4} got {(s4 or {}).get('devices_total')} want {header.get('devices')}")
                st5, l5, _ = request(base, "GET", "/api/links")
                got_links = len((l5 or {}).get("links", [])) if isinstance(l5, dict) else None
                check("link count matches the header", got_links == header.get("links"),
                      f"status {st5} got {got_links} want {header.get('links')}")
    elif restore_roundtrip:
        print("       no usable backup to restore - skipping the round trip")

    # --- destructive endpoints, non-destructively ---------------------------
    print("\nguards")
    st, _, _ = request(base, "POST", "/api/system/factory-reset", {"confirm": "nope"})
    check("factory-reset refuses a bad confirmation", st == 400, f"status {st}")

    st, _, _ = request(base, "GET", "/api/does-not-exist")
    check("unknown api path is 404", st == 404, f"status {st}")

    # --- summary -----------------------------------------------------------
    print(f"\n{passed} passed, {failed} failed")
    if failures:
        print("\nfailures:")
        for f in failures:
            print(f"  - {f}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
