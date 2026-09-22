#!/usr/bin/env python3
"""Restore a .landash backup onto a dongle over USB.

The same conversation the web installer has with a freshly flashed dongle
(see main/ui/usb_restore.h), for doing it from a terminal instead:

    python tools/usb_restore.py COM4 landash-20260922.landash
    python tools/usb_restore.py /dev/ttyACM0 backup.landash --passphrase "..."

Needs pyserial, which ESP-IDF's Python environment already has. The dongle
must run v0.20.0 or later; an older one never answers the restore command.
"""
import argparse
import getpass
import json
import re
import sys
import time

import serial

ANSI = re.compile(rb"\x1b\[[0-9;]*m")
WINDOW = 3072   # bytes the host may send beyond the last ACK; see usb_restore.h


class Link:
    """Reads the dongle's console line by line and picks out LANDASH: lines."""

    def __init__(self, port):
        # Opening leaves DTR and RTS as the OS sets them, which is not a reset
        # on USB Serial/JTAG. Toggling them one at a time could pass through
        # the reset state, so they are left alone.
        self.s = serial.Serial(port, 115200, timeout=0.2)
        self.buf = b""

    def lines(self, until):
        """Yields (kind, rest) for every LANDASH: line until time `until`."""
        while time.time() < until:
            self.buf += self.s.read(4096)
            while b"\n" in self.buf:
                raw, self.buf = self.buf.split(b"\n", 1)
                line = ANSI.sub(b"", raw).strip().decode("utf-8", "replace")
                if line.startswith("LANDASH:"):
                    kind, _, rest = line[len("LANDASH:"):].partition(" ")
                    yield kind, rest

    def write(self, data):
        self.s.write(data)
        self.s.flush()


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("port", help="serial port, e.g. COM4 or /dev/ttyACM0")
    ap.add_argument("backup", help="the .landash file")
    ap.add_argument("--passphrase", help="the backup's passphrase (asked for if omitted)")
    ap.add_argument("--wait", type=float, default=60,
                    help="seconds to wait for the dongle to answer (default 60)")
    args = ap.parse_args()

    data = open(args.backup, "rb").read()
    if data[:8] != b"LANDASH\n":
        sys.exit(f"{args.backup} is not a LANDA.SH backup")
    hlen = int.from_bytes(data[8:10], "little")
    header = json.loads(data[10:10 + hlen])
    print(f"backup of {header.get('hostname')} ({header.get('fw')}): "
          f"{header.get('devices')} devices, {header.get('links')} links, "
          f"{header.get('icons')} icons")

    passphrase = args.passphrase or getpass.getpass("backup passphrase: ")
    if "\n" in passphrase or "\r" in passphrase:
        sys.exit("a passphrase cannot contain a newline")
    body = passphrase.encode("utf-8") + b"\n" + data

    link = Link(args.port)
    command = f"\nLANDASH-RESTORE {len(body)}\n".encode()

    # It may still be booting: repeat the command until it answers.
    print("waiting for the dongle...", flush=True)
    deadline = time.time() + args.wait
    ready = False
    while not ready and time.time() < deadline:
        link.write(command)
        for kind, rest in link.lines(time.time() + 1.5):
            if kind == "READY":
                ready = True
                break
            if kind == "ERR":
                sys.exit(f"refused: {rest}")
    if not ready:
        sys.exit("no answer - is it running LANDA.SH v0.20.0 or later?")

    # The dongle drops what overflows its 4 KB buffer, so never get more than
    # WINDOW bytes ahead of its acknowledgements. Waits are long because it
    # stops reading for several seconds while it derives the key.
    print(f"sending {len(body)} bytes...", flush=True)
    t0 = time.time()
    sent = acked = 0
    result = None
    while result is None:
        if sent < len(body) and sent - acked < WINDOW:
            n = min(WINDOW - (sent - acked), len(body) - sent)
            link.write(body[sent:sent + n])
            sent += n
        for kind, rest in link.lines(time.time() + 30):
            if kind == "ACK":
                acked = int(rest)
                break
            if kind in ("OK", "ERR"):
                result = (kind, rest)
                break
        else:
            sys.exit(f"no answer after {acked} of {len(body)} bytes")
    kind, rest = result
    if kind == "ERR":
        sys.exit(f"refused: {rest}")
    print(f"accepted in {time.time() - t0:.1f}s: {rest}")

    print("restarting and applying it...", flush=True)
    for kind, rest in link.lines(time.time() + 90):
        if kind == "IP":
            print(f"back on the network at http://{rest}/")
            return
    print("restored, but no network address was reported within 90 s - "
          "check the dongle's screen")


if __name__ == "__main__":
    main()
