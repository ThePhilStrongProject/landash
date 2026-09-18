#!/usr/bin/env python3
"""Dependency-free mock of the NetDash REST API (docs/API.md) for developing
main/web/www/index.html without hardware.

Serves index.html at / and implements every endpoint in docs/API.md against
an in-memory table seeded with a plausible home network. PATCH/DELETE/PUT
mutate the in-memory state so the UI can be exercised end to end.

Usage:
    python tools/mock_api.py
Then open http://127.0.0.1:8080/
"""

import json
import re
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import urlsplit, parse_qs

HOST = "127.0.0.1"
PORT = 8080
INDEX_HTML = Path(__file__).resolve().parent.parent / "main" / "web" / "www" / "index.html"

MAC_RE = re.compile(r"^([0-9a-f]{2}:){5}[0-9a-f]{2}$")
TYPES = {"unknown", "router", "mesh_node", "switch", "nas", "tv", "hub", "phone",
         "pc", "iot", "printer", "cast", "console", "printer_3d"}

START = time.time()
NOW0 = time.time()


def ago(seconds):
    return int(NOW0 - seconds)


LOCK = threading.RLock()

# ---------------------------------------------------------------------------
# seed data: 3x ASUS RT-AX59U (router + 2 mesh nodes), TrueNAS, OpenMediaVault,
# a Home Assistant box, 2x LG webOS TVs, a Netgear switch, a handful of
# phones/laptops, and one brand-new unknown Espressif device seen 10 min ago.
# ---------------------------------------------------------------------------

def seed_devices():
    d = {}

    def add(mac, ip, hostname, vendor, dtype, services, sources, first_seen, last_seen,
             nickname="", type_override=None, hidden=False, rtt_ms=1, miss_count=0):
        d[mac] = {
            "mac": mac, "ip": ip, "hostname": hostname, "vendor": vendor,
            "type": dtype, "type_override": type_override, "services": services,
            "sources": sources, "first_seen": first_seen, "last_seen": last_seen,
            "nickname": nickname, "hidden": hidden, "rtt_ms": rtt_ms,
            "miss_count": miss_count,
        }

    add("bc:ae:c5:11:22:01", "192.168.1.1", "RT-AX59U-9A20", "ASUSTek COMPUTER INC.",
        "router", ["http", "https", "ssdp"], ["rdns", "ssdp"],
        ago(120 * 86400), ago(5), nickname="Living Room Router", rtt_ms=1)

    add("bc:ae:c5:11:22:02", "192.168.1.3", "RT-AX59U-9A20-2G", "ASUSTek COMPUTER INC.",
        "mesh_node", ["http", "ssdp"], ["ssdp"],
        ago(118 * 86400), ago(8), nickname="Upstairs Node", rtt_ms=3)

    add("bc:ae:c5:11:22:03", "192.168.1.4", "RT-AX59U-9A20-3G", "ASUSTek COMPUTER INC.",
        "mesh_node", ["http", "ssdp"], ["ssdp"],
        ago(118 * 86400), ago(11), nickname="Garage Node", rtt_ms=4)

    add("00:0d:b9:41:5a:7c", "192.168.1.10", "truenas", "", "nas",
        ["smb", "http", "https", "ssh"], ["rdns", "mdns"],
        ago(300 * 86400), ago(3), nickname="TrueNAS", rtt_ms=1)

    add("dc:a6:32:88:01:cc", "192.168.1.11", "openmediavault", "Raspberry Pi Trading Ltd",
        "nas", ["smb", "http", "ssh"], ["mdns", "rdns"],
        ago(210 * 86400), ago(6), nickname="OMV Backup NAS", rtt_ms=2)

    add("dc:a6:32:88:02:dd", "192.168.1.12", "homeassistant", "Raspberry Pi Trading Ltd",
        "hub", ["ha", "http", "https"], ["mdns"],
        ago(260 * 86400), ago(2), nickname="Home Assistant", rtt_ms=2)

    add("a8:23:fe:cc:10:01", "192.168.1.20", "", "LG Electronics", "tv",
        ["cast", "ssdp"], ["ssdp"],
        ago(400 * 86400), ago(600), nickname="Living Room TV", rtt_ms=6)

    add("a8:23:fe:cc:10:02", "192.168.1.21", "", "LG Electronics", "tv",
        ["cast", "ssdp"], ["ssdp"],
        ago(390 * 86400), ago(30000), nickname="Bedroom TV", rtt_ms=-1, miss_count=4)

    add("28:80:88:2a:33:01", "192.168.1.2", "", "Netgear", "switch", [], [],
        ago(500 * 86400), ago(9), nickname="Netgear 8-port Switch", rtt_ms=-1)

    add("f0:18:98:77:ab:11", "192.168.1.30", "Phils-iPhone", "Apple, Inc.", "phone",
        [], ["nbns"], ago(600 * 86400), ago(45), nickname="Phil's iPhone", rtt_ms=12)

    add("e4:5f:01:9b:44:22", "192.168.1.31", "galaxy-s23", "Samsung Electronics Co.,Ltd",
        "phone", [], ["mdns"], ago(210 * 86400), ago(5400),
        nickname="", rtt_ms=-1, miss_count=6)

    add("f4:5c:89:12:9e:33", "192.168.1.40", "Phils-MacBook-Pro", "Apple, Inc.", "pc",
        ["workstation", "ssh"], ["nbns", "mdns"], ago(700 * 86400), ago(20),
        nickname="Phil's MacBook", rtt_ms=2)

    add("9c:b6:d0:55:6f:44", "192.168.1.41", "DESKTOP-7QK2P9", "Dell Inc.", "pc",
        ["workstation"], ["nbns"], ago(500 * 86400), ago(30), nickname="Office PC", rtt_ms=3)

    add("7c:9e:bd:aa:11:99", "192.168.1.157", "", "Espressif Inc.", "unknown", [], [],
        ago(600), ago(5), nickname="", rtt_ms=8)

    return d


DEVICES = seed_devices()

EVENTS = [
    {"ts": ago(5), "type": "device_new", "mac": "7c:9e:bd:aa:11:99",
     "ip": "192.168.1.157", "text": "new device joined the network"},
    {"ts": ago(120), "type": "scan", "mac": None, "ip": None, "text": "sweep done, 12 online"},
    {"ts": ago(5400), "type": "device_offline", "mac": "e4:5f:01:9b:44:22",
     "ip": "192.168.1.31", "text": "went offline"},
    {"ts": ago(20000), "type": "device_offline", "mac": "a8:23:fe:cc:10:02",
     "ip": "192.168.1.21", "text": "went offline"},
    {"ts": ago(28000), "type": "device_ip_changed", "mac": "e4:5f:01:9b:44:22",
     "ip": "192.168.1.31", "text": "IP changed from 192.168.1.55"},
    {"ts": ago(40000), "type": "wifi", "mac": None, "ip": None,
     "text": "connected to MyHomeWiFi"},
    {"ts": ago(86400 + 600), "type": "device_new", "mac": "9c:b6:d0:55:6f:44",
     "ip": "192.168.1.41", "text": "Office PC"},
    {"ts": ago(200000), "type": "scan", "mac": None, "ip": None, "text": "sweep done, 13 online"},
    {"ts": ago(300000), "type": "info", "mac": None, "ip": None, "text": "NTP time synced"},
    {"ts": ago(305000), "type": "info", "mac": None, "ip": None, "text": "boot"},
]

SETTINGS = {
    "wifi_ssid": "MyHomeWiFi",
    "wifi_pass": "hunter2hunter2",  # never returned by GET
    "wifi_configured": True,
    "hostname": "netdash",
    "ap_pass": "kq7mn3rt",
    "scan_interval_min": 5,
    "hosts_per_sec": 4,
    "passive_only": False,
    "tz": "GMT0BST,M3.5.0/1,M10.5.0",
    "ntp_server": "pool.ntp.org",
}

STATUS_EXTRA = {"scanning": False, "scan_done": 0, "scan_total": 0, "last_sweep": ago(120)}

WIFI_NETWORKS = [
    {"ssid": "MyHomeWiFi", "rssi": -48, "auth": "wpa2_psk", "channel": 6},
    {"ssid": "MyHomeWiFi-5G", "rssi": -55, "auth": "wpa2_wpa3_psk", "channel": 44},
    {"ssid": "NeighbourNet", "rssi": -71, "auth": "wpa2_psk", "channel": 11},
    {"ssid": "", "rssi": -80, "auth": "open", "channel": 1},
    {"ssid": "CoffeeShop Guest", "rssi": -85, "auth": "open", "channel": 6},
]


def is_new(first_seen):
    return bool(first_seen) and (time.time() - first_seen) < 86400


def effective_type(rec):
    return rec["type_override"] or rec["type"]


def display_name(rec):
    if rec["nickname"]:
        return rec["nickname"]
    if rec["hostname"]:
        return rec["hostname"]
    if rec["vendor"]:
        return "%s %s" % (rec["vendor"].split(",")[0].split(" ")[0], rec["mac"][-5:].replace(":", ""))
    return rec["mac"]


def device_json(rec):
    online = rec["miss_count"] == 0
    return {
        "mac": rec["mac"],
        "ip": rec["ip"],
        "display_name": display_name(rec),
        "nickname": rec["nickname"],
        "hostname": rec["hostname"],
        "vendor": rec["vendor"],
        "type": effective_type(rec),
        "type_override": rec["type_override"],
        "services": rec["services"],
        "sources": rec["sources"],
        "first_seen": int(rec["first_seen"]),
        "last_seen": int(rec["last_seen"]),
        "online": online,
        "is_new": is_new(rec["first_seen"]),
        "hidden": rec["hidden"],
        "rtt_ms": rec["rtt_ms"],
        "miss_count": rec["miss_count"],
    }


def run_fake_sweep():
    STATUS_EXTRA["scanning"] = True
    STATUS_EXTRA["scan_total"] = 254
    STATUS_EXTRA["scan_done"] = 0

    def step(n):
        with LOCK:
            if not STATUS_EXTRA["scanning"]:
                return
            STATUS_EXTRA["scan_done"] = n
            if n >= STATUS_EXTRA["scan_total"]:
                STATUS_EXTRA["scanning"] = False
                STATUS_EXTRA["last_sweep"] = int(time.time())
                online = sum(1 for r in DEVICES.values() if r["miss_count"] == 0)
                EVENTS.insert(0, {"ts": int(time.time()), "type": "scan", "mac": None,
                                   "ip": None, "text": "sweep done, %d online" % online})
                return
        threading.Timer(0.15, step, args=(n + 40,)).start()

    threading.Timer(0.15, step, args=(40,)).start()


class Handler(BaseHTTPRequestHandler):
    server_version = "NetDashMock/0.1"

    def log_message(self, fmt, *args):
        print("%s %s" % (self.address_string(), fmt % args))

    # ---- helpers ----
    def _send_json(self, status, obj, headers=None):
        body = json.dumps(obj).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        for k, v in (headers or {}).items():
            self.send_header(k, v)
        self.end_headers()
        self.wfile.write(body)

    def _error(self, status, msg):
        self._send_json(status, {"error": msg})

    def _read_json(self):
        length = int(self.headers.get("Content-Length") or 0)
        if length > 8192:
            return None, "too large"
        if length == 0:
            return {}, None
        raw = self.rfile.read(length)
        try:
            return json.loads(raw.decode("utf-8")), None
        except Exception:
            return None, "malformed json"

    def _send_html(self):
        try:
            body = INDEX_HTML.read_bytes()
        except OSError:
            self._error(500, "index.html missing")
            return
        self.send_response(200)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        self.send_header("Cache-Control", "no-cache")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    # ---- routing ----
    def do_GET(self):
        parts = urlsplit(self.path)
        path, qs = parts.path, parse_qs(parts.query)
        with LOCK:
            if path == "/api/status":
                return self._status()
            if path == "/api/devices":
                return self._list_devices(qs)
            if path == "/api/devices/export":
                return self._export()
            m = re.match(r"^/api/devices/([0-9a-f:]+)$", path)
            if m:
                return self._get_device(m.group(1))
            if path == "/api/events":
                return self._get_events(qs)
            if path == "/api/settings":
                return self._get_settings()
        if path.startswith("/api/"):
            return self._error(404, "unknown endpoint")
        return self._send_html()

    def do_POST(self):
        parts = urlsplit(self.path)
        path = parts.path
        with LOCK:
            if path == "/api/scan":
                return self._scan()
            if path == "/api/devices/import":
                return self._import()
            if path == "/api/wifi/scan":
                return self._wifi_scan()
            if path == "/api/system/reboot":
                return self._send_json(200, {"ok": True})
            if path == "/api/system/factory-reset":
                return self._factory_reset()
        return self._error(404, "unknown endpoint")

    def do_PATCH(self):
        parts = urlsplit(self.path)
        path = parts.path
        m = re.match(r"^/api/devices/([0-9a-f:]+)$", path)
        with LOCK:
            if m:
                return self._patch_device(m.group(1))
        return self._error(404, "unknown endpoint")

    def do_DELETE(self):
        parts = urlsplit(self.path)
        path = parts.path
        m = re.match(r"^/api/devices/([0-9a-f:]+)$", path)
        with LOCK:
            if m:
                return self._delete_device(m.group(1))
        return self._error(404, "unknown endpoint")

    def do_PUT(self):
        parts = urlsplit(self.path)
        path = parts.path
        with LOCK:
            if path == "/api/settings":
                return self._put_settings()
        return self._error(404, "unknown endpoint")

    # ---- handlers ----
    def _status(self):
        online = sum(1 for r in DEVICES.values() if r["miss_count"] == 0)
        new24 = sum(1 for r in DEVICES.values() if is_new(r["first_seen"]))
        out = {
            "ip": "192.168.1.58", "netmask": "255.255.255.0", "gateway": "192.168.1.1",
            "mac": "a0:76:4e:11:22:33", "mode": "sta", "ssid": SETTINGS["wifi_ssid"],
            "rssi": -54, "hostname": SETTINGS["hostname"],
            "uptime_s": int(time.time() - START),
            "heap_free": 142336, "heap_min_free": 118204,
            "fw": "0.1.0", "idf": "v5.5",
            "time": int(time.time()), "time_synced": True,
            "scanning": STATUS_EXTRA["scanning"],
            "scan_done": STATUS_EXTRA["scan_done"], "scan_total": STATUS_EXTRA["scan_total"],
            "last_sweep": STATUS_EXTRA["last_sweep"],
            "devices_total": len(DEVICES), "devices_online": online, "devices_new_24h": new24,
        }
        self._send_json(200, out)

    def _list_devices(self, qs):
        want_hidden = qs.get("hidden", ["0"])[0] == "1"
        online_only = qs.get("online", [None])[0]
        items = [device_json(r) for r in DEVICES.values() if want_hidden or not r["hidden"]]
        if online_only == "1":
            items = [d for d in items if d["online"]]
        items.sort(key=lambda d: tuple(int(x) for x in d["ip"].split(".")))
        self._send_json(200, items)

    def _get_device(self, mac):
        rec = DEVICES.get(mac)
        if not rec:
            return self._error(404, "device not found")
        self._send_json(200, device_json(rec))

    def _patch_device(self, mac):
        rec = DEVICES.get(mac)
        if not rec:
            return self._error(404, "device not found")
        body, err = self._read_json()
        if err:
            return self._error(400, err)
        if "nickname" in body:
            nick = body["nickname"]
            if not isinstance(nick, str) or len(nick) > 31:
                return self._error(400, "nickname too long")
            rec["nickname"] = nick
        if "type_override" in body:
            t = body["type_override"]
            if t is None or t == "unknown":
                rec["type_override"] = None
            elif t in TYPES:
                rec["type_override"] = t
            else:
                return self._error(400, "unknown type_override")
        if "hidden" in body:
            if not isinstance(body["hidden"], bool):
                return self._error(400, "hidden must be a bool")
            rec["hidden"] = body["hidden"]
        self._send_json(200, device_json(rec))

    def _delete_device(self, mac):
        if mac not in DEVICES:
            return self._error(404, "device not found")
        del DEVICES[mac]
        self._send_json(200, {"ok": True})

    def _export(self):
        out = {
            "version": 1, "exported_at": int(time.time()), "hostname": SETTINGS["hostname"],
            "devices": [device_json(r) for r in DEVICES.values()],
        }
        self._send_json(200, out, {"Content-Disposition": 'attachment; filename="netdash-devices.json"'})

    def _import(self):
        body, err = self._read_json()
        if err:
            return self._error(400, err)
        if body.get("version") != 1 or not isinstance(body.get("devices"), list):
            return self._error(400, "invalid import file")
        imported, skipped = 0, 0
        for item in body["devices"]:
            mac = item.get("mac")
            if not mac or not MAC_RE.match(mac):
                skipped += 1
                continue
            if mac in DEVICES:
                rec = DEVICES[mac]
            else:
                rec = {
                    "mac": mac, "ip": "0.0.0.0", "hostname": "", "vendor": "",
                    "type": "unknown", "services": [], "sources": [],
                    "first_seen": 0, "last_seen": 0, "rtt_ms": -1, "miss_count": 255,
                    "type_override": None, "nickname": "", "hidden": False,
                }
                DEVICES[mac] = rec
            if "nickname" in item and isinstance(item["nickname"], str):
                rec["nickname"] = item["nickname"][:31]
            if "type_override" in item:
                t = item["type_override"]
                rec["type_override"] = t if t in TYPES else None
            if "hidden" in item and isinstance(item["hidden"], bool):
                rec["hidden"] = item["hidden"]
            imported += 1
        self._send_json(200, {"ok": True, "imported": imported, "skipped": skipped})

    def _get_events(self, qs):
        try:
            limit = int(qs.get("limit", ["100"])[0])
        except ValueError:
            limit = 100
        limit = max(1, min(100, limit))
        self._send_json(200, EVENTS[:limit])

    def _scan(self):
        if STATUS_EXTRA["scanning"]:
            return self._send_json(200, {"ok": True, "scanning": True})
        if SETTINGS["passive_only"]:
            return self._error(503, "passive_only is enabled")
        run_fake_sweep()
        self._send_json(200, {"ok": True, "scanning": True})

    def _wifi_scan(self):
        time.sleep(0.4)
        self._send_json(200, WIFI_NETWORKS)

    def _get_settings(self):
        out = dict(SETTINGS)
        del out["wifi_pass"]
        self._send_json(200, out)

    def _put_settings(self):
        body, err = self._read_json()
        if err:
            return self._error(400, err)

        def bad(msg):
            return self._error(400, msg)

        reconnecting = False
        if "wifi_ssid" in body:
            v = body["wifi_ssid"]
            if not isinstance(v, str) or len(v) > 32:
                return bad("wifi_ssid out of range")
            if v != SETTINGS["wifi_ssid"]:
                reconnecting = True
            SETTINGS["wifi_ssid"] = v
            SETTINGS["wifi_configured"] = bool(v)
        if "wifi_pass" in body:
            v = body["wifi_pass"]
            if not isinstance(v, str) or len(v) > 64:
                return bad("wifi_pass out of range")
            if v:
                SETTINGS["wifi_pass"] = v
                reconnecting = True
        if "hostname" in body:
            v = body["hostname"]
            if not isinstance(v, str) or not (1 <= len(v) <= 31) or not re.match(r"^[a-z0-9-]+$", v):
                return bad("hostname out of range")
            SETTINGS["hostname"] = v
        if "ap_pass" in body:
            v = body["ap_pass"]
            if not isinstance(v, str) or not (8 <= len(v) <= 63):
                return bad("ap_pass out of range")
            SETTINGS["ap_pass"] = v
        if "scan_interval_min" in body:
            v = body["scan_interval_min"]
            if not isinstance(v, (int, float)) or not (1 <= v <= 1440):
                return bad("scan_interval_min out of range")
            SETTINGS["scan_interval_min"] = int(v)
        if "hosts_per_sec" in body:
            v = body["hosts_per_sec"]
            if not isinstance(v, (int, float)) or not (1 <= v <= 64):
                return bad("hosts_per_sec out of range")
            SETTINGS["hosts_per_sec"] = int(v)
        if "passive_only" in body:
            if not isinstance(body["passive_only"], bool):
                return bad("passive_only must be a bool")
            SETTINGS["passive_only"] = body["passive_only"]
        if "tz" in body:
            v = body["tz"]
            if not isinstance(v, str) or len(v) > 47:
                return bad("tz out of range")
            SETTINGS["tz"] = v
        if "ntp_server" in body:
            v = body["ntp_server"]
            if not isinstance(v, str) or len(v) > 63:
                return bad("ntp_server out of range")
            SETTINGS["ntp_server"] = v

        out = dict(SETTINGS)
        del out["wifi_pass"]
        if reconnecting:
            out["reconnecting"] = True
        self._send_json(200, out)

    def _factory_reset(self):
        body, err = self._read_json()
        if err or body.get("confirm") != "factory-reset":
            return self._error(400, "confirmation required")
        self._send_json(200, {"ok": True})


def main():
    if not INDEX_HTML.exists():
        print("warning: %s not found" % INDEX_HTML)
    httpd = ThreadingHTTPServer((HOST, PORT), Handler)
    print("NetDash mock API on http://%s:%d/  (Ctrl+C to stop)" % (HOST, PORT))
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
