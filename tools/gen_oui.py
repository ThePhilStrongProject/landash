#!/usr/bin/env python3
"""Generate main/net/oui_table.inc from the IEEE OUI registry.

The committed table was produced with:

    python tools/gen_oui.py --curated --per-vendor 5

which gave 324 prefixes. The exact command, the source URL and the date are
recorded in the generated file's header.

--curated keeps only the vendors listed in CURATED below, at most --per-vendor
prefixes each, sampled evenly across that vendor's allocations (the lowest
prefixes are all 2003-era blocks, so a head slice would never match a device
made this decade). Raising --per-vendor is cheap: the vendor names are shared
string literals, so each extra prefix costs about 8 bytes of rodata, and
--per-vendor 40 gives ~2600 entries / ~22 KB with far better real-world hit
rates.

--full emits every MA-L assignment, ~40 000 entries and roughly 1 MB of rodata:

    python tools/gen_oui.py --full --out main/net/oui_table_full.inc

That is too big to compile in; it belongs in the `storage` partition and is a
future work package. oui_table_full.inc is gitignored.

Note that Aruba and ITEAD/Sonoff hold MA-M/MA-S blocks only and so never appear
in the MA-L oui.csv this script reads.

The output is a bare list of initialisers, sorted by prefix, that main/net/oui.c
#includes inside an array definition.
"""

from __future__ import annotations

import argparse
import csv
import io
import re
import sys
import urllib.request
from datetime import date, timezone, datetime

OUI_CSV_URL = "https://standards-oui.ieee.org/oui/oui.csv"

# Short display name -> regexes matched (case-insensitively) against the IEEE
# "Organization Name" column. Order matters: the first match wins, so put the
# specific patterns before the generic ones.
#
# Keep the short names <= 23 characters: they are copied into
# netdash_device_t::vendor[24].
CURATED: list[tuple[str, list[str]]] = [
    ("ASUSTek",          [r"^asustek", r"^asus\b"]),
    ("LG Electronics",   [r"^lg electronics", r"^lg innotek"]),
    ("Netgear",          [r"^netgear"]),
    ("Apple",            [r"^apple, inc", r"^apple inc"]),
    ("Samsung",          [r"^samsung"]),
    ("Google",           [r"^google"]),
    ("Nest Labs",        [r"^nest labs"]),
    ("Amazon",           [r"^amazon"]),
    ("Ring",             [r"^ring\b", r"^ring llc", r"^ring inc"]),
    ("Espressif",        [r"^espressif"]),
    ("Raspberry Pi",     [r"^raspberry pi"]),
    ("Intel",            [r"^intel corporate", r"^intel corporation"]),
    ("Realtek",          [r"^realtek"]),
    ("TP-Link",          [r"^tp-link", r"^tplink"]),
    ("Sonos",            [r"^sonos"]),
    ("Signify (Hue)",    [r"^signify", r"^philips lighting"]),
    ("Philips",          [r"^philips"]),
    ("Ubiquiti",         [r"^ubiquiti"]),
    ("Synology",         [r"^synology"]),
    ("QNAP",             [r"^qnap"]),
    ("Xiaomi",           [r"^xiaomi", r"^beijing xiaomi"]),
    ("OnePlus",          [r"^oneplus"]),
    ("Roku",             [r"^roku"]),
    ("Sony",             [r"^sony"]),
    ("Nintendo",         [r"^nintendo"]),
    ("Microsoft",        [r"^microsoft"]),
    ("Dell",             [r"^dell\b", r"^dell inc"]),
    ("HP",               [r"^hewlett packard", r"^hp inc", r"^hewlett-packard"]),
    ("Lenovo",           [r"^lenovo"]),
    ("Shelly",           [r"^allterco", r"^shelly"]),
    ("Tuya",             [r"^tuya"]),
    ("Broadcom",         [r"^broadcom"]),
    ("Qualcomm",         [r"^qualcomm"]),
    ("MediaTek",         [r"^mediatek"]),
    ("AzureWave",        [r"^azurewave"]),
    ("Liteon",           [r"^liteon", r"^lite-on"]),
    ("Murata",           [r"^murata"]),
    ("Texas Instruments", [r"^texas instruments"]),
    ("Nordic Semi",      [r"^nordic semiconductor"]),
    ("Cisco",            [r"^cisco system", r"^cisco-linksys", r"^cisco spvtg"]),
    ("Linksys",          [r"^linksys"]),
    ("Hikvision",        [r"^hangzhou hikvision", r"^hikvision"]),
    ("Reolink",          [r"reolink"]),
    ("Dyson",            [r"^dyson"]),
    ("iRobot",           [r"^irobot"]),
    ("Bosch",            [r"^bosch", r"^robert bosch"]),
    ("Siemens",          [r"^siemens"]),
    ("Sagemcom",         [r"^sagemcom"]),
    ("Technicolor",      [r"^technicolor"]),
    ("AVM",              [r"^avm (gmbh|audiovisuelles)"]),
    ("Hon Hai/Foxconn",  [r"^hon hai"]),
    ("Wistron",          [r"^wistron"]),
    ("Sercomm",          [r"^sercomm"]),
    ("D-Link",           [r"^d-link"]),
    ("Zyxel",            [r"^zyxel"]),
    ("Brother",          [r"^brother industries"]),
    ("Canon",            [r"^canon"]),
    ("Epson",            [r"^seiko epson", r"^epson"]),
    ("Panasonic",        [r"^panasonic"]),
    ("Sharp",            [r"^sharp corporation"]),
    ("Toshiba",          [r"^toshiba"]),
    ("Vizio",            [r"^vizio"]),
    ("Hisense",          [r"^hisense"]),
    ("TCL",              [r"^tcl\b", r"^tcl technology"]),
    ("Withings",         [r"^withings"]),
    ("Garmin",           [r"^garmin"]),
    ("Fitbit",           [r"^fitbit"]),
    ("Wyze",             [r"^wyze"]),
    ("Arlo",             [r"^arlo"]),
    ("Eero",             [r"^eero"]),
    ("Tado",             [r"^tado"]),
    ("Netatmo",          [r"^netatmo"]),
    ("Honeywell",        [r"^honeywell"]),
    ("Zebra",            [r"^zebra technologies"]),
    # NOTE: Aruba and ITEAD/Sonoff hold MA-M/MA-S blocks only, so they do not
    # appear in the MA-L oui.csv this script reads.
    ("VMware",           [r"^vmware"]),
]

COMPILED = [(short, [re.compile(p, re.I) for p in pats]) for short, pats in CURATED]


def fetch(url: str) -> str:
    sys.stderr.write(f"downloading {url}\n")
    req = urllib.request.Request(url, headers={"User-Agent": "netdash-gen-oui/1.0"})
    with urllib.request.urlopen(req, timeout=120) as resp:
        return resp.read().decode("utf-8", errors="replace")


def parse(text: str) -> list[tuple[int, str]]:
    """Returns [(prefix, organization)] from the IEEE MA-L csv."""
    rows: list[tuple[int, str]] = []
    reader = csv.DictReader(io.StringIO(text))
    for row in reader:
        assignment = (row.get("Assignment") or "").strip()
        org = (row.get("Organization Name") or "").strip()
        if len(assignment) != 6 or not org:
            continue
        try:
            prefix = int(assignment, 16)
        except ValueError:
            continue
        # Skip multicast / locally administered prefixes; they are never real
        # vendor assignments and oui_lookup() rejects them anyway.
        if (prefix >> 16) & 0x03:
            continue
        rows.append((prefix, org))
    return rows


def short_name(org: str) -> str | None:
    for short, pats in COMPILED:
        for pat in pats:
            if pat.search(org):
                return short
    return None


def clean_full_name(org: str, limit: int = 23) -> str:
    name = re.sub(r"[^\x20-\x7e]", "", org)
    name = re.sub(r"\s+", " ", name).strip()
    name = re.sub(
        r",?\s*\b(inc|inc\.|llc|ltd|ltd\.|limited|corp|corp\.|corporation|co|co\.,?|gmbh|s\.a\.|b\.v\.|a/s|ab|plc|pte|technologies|technology)\b\.?$",
        "",
        name,
        flags=re.I,
    ).strip(" ,.")
    return name[:limit]


def select(rows: list[tuple[int, str]], curated: bool, per_vendor: int) -> list[tuple[int, str]]:
    out: list[tuple[int, str]] = []
    if not curated:
        for prefix, org in rows:
            name = clean_full_name(org)
            if name:
                out.append((prefix, name))
        return sorted(out)

    buckets: dict[str, list[int]] = {}
    for prefix, org in rows:
        short = short_name(org)
        if short is None:
            continue
        buckets.setdefault(short, []).append(prefix)

    for short, _pats in CURATED:
        prefixes = sorted(buckets.get(short, []))
        if not prefixes:
            sys.stderr.write(f"warning: no IEEE rows matched vendor '{short}'\n")
            continue
        # Sample evenly across the vendor's allocations rather than taking
        # the first N: the low prefixes are all 2003-era blocks, so a naive
        # head slice would never match a device made this decade.
        n = min(per_vendor, len(prefixes))
        step = len(prefixes) / n
        for i in range(n):
            out.append((prefixes[int(i * step)], short))
    return sorted(out)


def emit(entries: list[tuple[int, str]], curated: bool, per_vendor: int, source: str) -> str:
    stamp = datetime.now(timezone.utc).strftime("%Y-%m-%d")
    mode = f"--curated --per-vendor {per_vendor}" if curated else "--full"
    lines = [
        "/*",
        " * GENERATED FILE - DO NOT EDIT.",
        " *",
        f" * tools/gen_oui.py {mode}",
        f" * source: {source}",
        f" * date:   {stamp}",
        f" * count:  {len(entries)} prefixes, sorted ascending for binary search",
        " *",
        " * Included by main/net/oui.c inside an array of oui_entry_t.",
        " */",
    ]
    for prefix, name in entries:
        escaped = name.replace("\\", "\\\\").replace('"', '\\"')
        lines.append(f'    {{ 0x{prefix:06x}u, "{escaped}" }},')
    return "\n".join(lines) + "\n"


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    group = ap.add_mutually_exclusive_group()
    group.add_argument("--curated", action="store_true",
                       help="keep only the vendors in CURATED (default)")
    group.add_argument("--full", action="store_true",
                       help="emit every MA-L assignment (large)")
    ap.add_argument("--per-vendor", type=int, default=8,
                    help="max prefixes per curated vendor (default 8)")
    ap.add_argument("--url", default=OUI_CSV_URL)
    ap.add_argument("--input", help="read a local copy of oui.csv instead of downloading")
    ap.add_argument("--out", default="main/net/oui_table.inc")
    args = ap.parse_args()

    curated = not args.full

    if args.input:
        with open(args.input, "r", encoding="utf-8", errors="replace") as fh:
            text = fh.read()
        source = args.input
    else:
        text = fetch(args.url)
        source = args.url

    rows = parse(text)
    if not rows:
        sys.stderr.write("error: no usable rows in the OUI csv\n")
        return 1
    sys.stderr.write(f"parsed {len(rows)} MA-L assignments\n")

    entries = select(rows, curated, args.per_vendor)
    if not entries:
        sys.stderr.write("error: nothing selected\n")
        return 1

    with open(args.out, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(emit(entries, curated, args.per_vendor, source))

    sys.stderr.write(f"wrote {args.out}: {len(entries)} entries\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
