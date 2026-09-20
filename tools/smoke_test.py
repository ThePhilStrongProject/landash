#!/usr/bin/env python3
"""End-to-end smoke test for a running NetDash dongle.

Exercises every REST endpoint in docs/API.md against real hardware and checks
the response shapes, then does a nickname round-trip to prove the device
database persists user edits.

Nothing here destroys anything you did not ask it to: reboot and factory-reset
are only probed with a deliberately invalid body to confirm they refuse it, and
the vault section is skipped entirely when a vault already exists, because
exercising it means destroying it at the end.

Usage:
    python tools/smoke_test.py                       # http://netdash.local
    python tools/smoke_test.py http://192.168.1.58
"""

import json
import struct
import sys
import urllib.error
import urllib.request
import zlib

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
    base = (sys.argv[1] if len(sys.argv) > 1 else "http://netdash.local").rstrip("/")
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

        st, _, _ = request(base, "POST", "/api/vault/unlock", {"passphrase": "wrong"})
        check("a wrong passphrase is refused", st == 403, f"status {st}")

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
        check("failed uploads leave nothing behind",
              len([i for i in r.get("icons", []) if i["id"] != icon_id]) == 0,
              repr(r.get("icons"))[:120])

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
