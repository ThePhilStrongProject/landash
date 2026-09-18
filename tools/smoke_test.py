#!/usr/bin/env python3
"""End-to-end smoke test for a running NetDash dongle.

Exercises every REST endpoint in docs/API.md against real hardware and checks
the response shapes, then does a nickname round-trip to prove the device
database persists user edits.

Nothing here is destructive: reboot and factory-reset are only probed with a
deliberately invalid body to confirm they refuse it.

Usage:
    python tools/smoke_test.py                       # http://netdash.local
    python tools/smoke_test.py http://192.168.1.58
"""

import json
import sys
import urllib.error
import urllib.request

TIMEOUT = 10

passed = 0
failed = 0
failures = []


def check(name, condition, detail=""):
    global passed, failed
    if condition:
        passed += 1
        print(f"  ok   {name}")
    else:
        failed += 1
        failures.append(f"{name}: {detail}")
        print(f"  FAIL {name}  {detail}")


def request(base, method, path, body=None, expect=200):
    """Returns (status, parsed_json_or_raw_bytes, headers)."""
    url = base + path
    data = None
    headers = {"Accept": "application/json"}
    if body is not None:
        data = json.dumps(body).encode()
        headers["Content-Type"] = "application/json"

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
