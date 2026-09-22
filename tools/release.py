#!/usr/bin/env python3
"""Commit build/netdash.bin to the releases repository, ready to push.

Dongles read latest.json from the releases repository (CONFIG_NETDASH_OTA_REPO,
a separate, public repo) and install the image it names when its version is
newer than theirs. This script writes both into a local clone of that repo,
adds the tag's message to its CHANGELOG.md, and commits; pushing is left to
you, because that is the moment it goes live.

The releases repository is public, so the tag message is public text: write
it for someone who owns a dongle, not for whoever reads the source.

It refuses to go on unless:

  * this working tree is clean and HEAD is exactly on a tag,
  * build/netdash.bin carries that same version (git describe baked it in at
    build time) and the project name "netdash",
  * the version is newer than the one latest.json already names, and
  * the releases clone has nothing uncommitted of its own.

    git tag -a v0.14.1 -m "What changed"
    idf.py build
    python tools/release.py            # commits into ../landash-releases
    cd ../landash-releases && git push

It also writes the browser installer's files (install/manifest.json and the
boot parts under install/vX.Y.Z/) for the landing page on GitHub Pages, so a
new board gets the same release that dongles update to. For a release that
was published before the installer existed, --installer-only adds just that.

Standard library only, so it runs from any Python 3.8+.
"""
import argparse
import json
import os
import shutil
import struct
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN = os.path.join(ROOT, "build", "netdash.bin")
FLASH_ARGS = os.path.join(ROOT, "build", "flash_args")
DEFAULT_RELEASES = os.path.join(os.path.dirname(ROOT), "landash-releases")
PROJECT = "netdash"

# esp_app_desc_t sits right after the 24-byte image header and the 8-byte
# header of the first segment.
APP_DESC_OFFSET = 32
APP_DESC_MAGIC = 0xABCD5432

RELEASES_README = """# LANDA.SH firmware

Firmware updates for **LANDA.SH**, a small home-network dashboard that runs on
a [Waveshare ESP32-C6-GEEK](https://www.waveshare.com/wiki/ESP32-C6-GEEK) USB
dongle. Plug it in and it joins your Wi-Fi, finds the devices on your network,
works out what each one is, and serves a dashboard of them from the dongle
itself. It needs no cloud service, app or account.

This repository holds the built firmware that dongles update themselves from.
There is no source code here.

## What's here

| | |
|---|---|
| [`latest.json`](latest.json) | The current release: its version, the image file and its size. |
| [`firmware/`](firmware) | One firmware image per release, `landash-vX.Y.Z.bin` (older ones `netdash-…`). |
| [`CHANGELOG.md`](CHANGELOG.md) | What changed in each release. |
| [`install/`](install) | The browser installer's manifest and boot parts, used by the project page. |
| [`index.html`](index.html) | The project page, served by GitHub Pages. |

## How a dongle updates

Two minutes after it starts, and every 12 hours after that, a dongle reads
`latest.json` over HTTPS. If that version is newer than the one it is running,
it:

1. downloads the image, resuming where it stopped if the connection drops,
2. checks the image is LANDA.SH firmware and is the version `latest.json`
   promised, before writing any of it,
3. verifies the image's built-in SHA-256 checksum, then restarts into it,
4. keeps the new version only once it has run for a minute and got back onto
   Wi-Fi. Otherwise it automatically goes back to the version it had, and will
   not install that release again by itself.

A dongle never installs a version older than its own. It fetches two files and
sends nothing about your network. As with any download, GitHub sees the
request come from your IP address.

You can switch automatic updates off, or have them wait for your go-ahead,
under **Settings › Maintenance** on the dashboard. The same page shows the
installed version and when it last checked.

## Setting up a new board

Use the installer on the project page,
<https://thephilstrongproject.github.io/landash-releases/>, in Chrome or Edge.
It writes the bootloader, partition table and the current release over USB
from `install/manifest.json`, so a new board starts on the same version the
others update to.

The images in `firmware/` are only the application, for dongles that already
run LANDA.SH v0.14.0 or later; on their own they will not set up a new board.

## Checking a file

Every image records its own project name and version. With Python and
[esptool](https://github.com/espressif/esptool) installed:

```
esptool image-info firmware/landash-vX.Y.Z.bin                  # esptool 5
python -m esptool image_info --version 2 firmware/landash-vX.Y.Z.bin   # esptool 4
```

Look for `Project name: netdash` and the version you expect.
"""

CHANGELOG_HEAD = """# Changelog

Newest first. Each version's image is in [`firmware/`](firmware).
"""


def fail(msg):
    print("release: " + msg, file=sys.stderr)
    sys.exit(1)


def git(cwd, *args, check=False):
    # git speaks UTF-8; Python on Windows would otherwise decode it as cp1252 and
    # turn "›" in a tag message into "â€º" in the public changelog.
    r = subprocess.run(["git"] + list(args), cwd=cwd, capture_output=True, text=True,
                       encoding="utf-8", errors="replace")
    if check and r.returncode != 0:
        fail("git %s failed in %s:\n%s" % (" ".join(args), cwd, r.stderr.strip()))
    return r


def version_tuple(v):
    v = v[1:] if v[:1] in ("v", "V") else v
    parts = v.split("-")[0].split(".")
    if len(parts) != 3 or not all(p.isdigit() for p in parts):
        return None
    return tuple(int(p) for p in parts)


def read_app_desc(path):
    with open(path, "rb") as f:
        head = f.read(APP_DESC_OFFSET + 256)
    if len(head) < APP_DESC_OFFSET + 80 or head[0] != 0xE9:
        fail("%s is not an ESP-IDF app image" % path)
    magic, = struct.unpack_from("<I", head, APP_DESC_OFFSET)
    if magic != APP_DESC_MAGIC:
        fail("%s has no app descriptor" % path)
    version = head[APP_DESC_OFFSET + 16:APP_DESC_OFFSET + 48].split(b"\0")[0].decode()
    project = head[APP_DESC_OFFSET + 48:APP_DESC_OFFSET + 80].split(b"\0")[0].decode()
    return version, project


def add_changelog_entry(path, tag, notes):
    """Puts the newest release at the top, under the heading."""
    import datetime
    entry = "## %s (%s)\n\n%s\n" % (tag, datetime.date.today().isoformat(), notes.strip())
    text = open(path, encoding="utf-8").read() if os.path.exists(path) else CHANGELOG_HEAD
    first = text.find("\n## ")
    if first < 0:
        text = text.rstrip("\n") + "\n\n" + entry
    else:
        text = text[:first + 1] + entry + "\n" + text[first + 1:]
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)


def write_installer(rel, tag, app_rel_file):
    """The ESP Web Tools manifest and the boot parts beside the app image.

    Separate parts at their own offsets, not one merged image: a merged image
    fills the gaps with 0xFF, which would wipe the settings partition and the
    device register even when the user chose not to erase. Written this way, a
    reinstall without erasing keeps everything, and the otadata part points the
    board at ota_0 where the new app has just gone. Returns the paths to add.
    """
    if not os.path.exists(FLASH_ARGS):
        fail("no %s; run idf.py build" % FLASH_ARGS)
    parts = []
    with open(FLASH_ARGS, encoding="utf-8") as f:
        for line in f:
            bits = line.split()
            if len(bits) == 2 and bits[0].startswith("0x"):
                parts.append((int(bits[0], 16), bits[1]))
    if not any(os.path.basename(p) == "netdash.bin" for _, p in parts):
        fail("%s does not name netdash.bin" % FLASH_ARGS)

    out_dir = os.path.join(rel, "install", tag)
    os.makedirs(out_dir, exist_ok=True)
    written, manifest_parts = [], []
    for offset, path in sorted(parts):
        if os.path.basename(path) == "netdash.bin":
            manifest_parts.append({"path": "../" + app_rel_file, "offset": offset})
            continue
        name = os.path.basename(path)
        shutil.copyfile(os.path.join(ROOT, "build", path), os.path.join(out_dir, name))
        written.append("install/%s/%s" % (tag, name))
        manifest_parts.append({"path": "%s/%s" % (tag, name), "offset": offset})

    manifest = {
        "name": "LANDA.SH",
        "version": tag,
        "new_install_prompt_erase": True,
        "new_install_improv_wait_time": 0,
        "builds": [{"chipFamily": "ESP32-C6", "parts": manifest_parts}],
    }
    with open(os.path.join(rel, "install", "manifest.json"), "w", encoding="utf-8",
              newline="\n") as f:
        json.dump(manifest, f, indent=2)
        f.write("\n")
    print("installer: install/manifest.json -> %s, %d parts" % (tag, len(manifest_parts)))
    return written + ["install/manifest.json"]


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--releases", default=DEFAULT_RELEASES,
                    help="local clone of the releases repository (default: %(default)s)")
    ap.add_argument("--installer-only", action="store_true",
                    help="only (re)write the browser installer for the release latest.json "
                         "already names, which must be this tag")
    args = ap.parse_args()

    # --- this repository and the build -------------------------------------
    if not os.path.exists(BIN):
        fail("no %s; run idf.py build" % BIN)
    version, project = read_app_desc(BIN)
    if project != PROJECT:
        fail("the image is project %r, expected %r" % (project, PROJECT))

    if args.installer_only:
        # The byte comparison with the published image below is the check: the
        # tree may have moved on since, but this build must be that release.
        tag = version
    else:
        if git(ROOT, "status", "--porcelain", "--untracked-files=no").stdout.strip():
            fail("this working tree has uncommitted changes; commit, tag, then build")
        tag = git(ROOT, "describe", "--exact-match", "--tags", "HEAD").stdout.strip()
        if not tag:
            fail("HEAD is not on a tag. Tag it (git tag -a vX.Y.Z -m ...) and rebuild")
    if version_tuple(tag) is None:
        fail("tag %s is not vMAJOR.MINOR.PATCH, which dongles need to compare versions" % tag)
    if version != tag:
        fail("the image says %s but HEAD is %s. Rebuild on the tag (idf.py build)" % (version, tag))
    size = os.path.getsize(BIN)
    print("ok: build/netdash.bin is %s, %d bytes" % (version, size))

    # --- the releases repository -------------------------------------------
    rel = os.path.abspath(args.releases)
    if not os.path.isdir(os.path.join(rel, ".git")):
        fail("%s is not a git clone of the releases repository (--releases)" % rel)
    if git(rel, "status", "--porcelain").stdout.strip():
        fail("%s has uncommitted changes of its own; sort those out first" % rel)

    manifest_path = os.path.join(rel, "latest.json")
    rel_file = "firmware/landash-%s.bin" % tag

    if args.installer_only:
        with open(manifest_path, encoding="utf-8") as f:
            published = json.load(f)
        if published.get("version") != tag or published.get("file") != rel_file:
            fail("latest.json names %s, not %s; --installer-only is for the published release"
                 % (published.get("version"), tag))
        with open(os.path.join(rel, rel_file), "rb") as a, open(BIN, "rb") as b:
            if a.read() != b.read():
                fail("build/netdash.bin differs from the published %s; the boot parts must "
                     "come from the build that was released" % rel_file)
        paths = write_installer(rel, tag, rel_file)
        git(rel, "add", *paths, check=True)
        git(rel, "commit", "-q", "-m", "Browser installer for %s" % tag, check=True)
        print("committed the installer for %s in %s\n\nTo publish:  cd %s && git push"
              % (tag, rel, rel))
        return

    if os.path.exists(manifest_path):
        with open(manifest_path, encoding="utf-8") as f:
            current = json.load(f).get("version", "")
        cur = version_tuple(current)
        if cur is not None and version_tuple(tag) <= cur:
            fail("latest.json already names %s; %s is not newer, and dongles would ignore it"
                 % (current, tag))

    os.makedirs(os.path.join(rel, "firmware"), exist_ok=True)
    shutil.copyfile(BIN, os.path.join(rel, rel_file))
    with open(manifest_path, "w", encoding="utf-8", newline="\n") as f:
        json.dump({"version": tag, "file": rel_file, "size": size}, f, indent=2)
        f.write("\n")
    readme = os.path.join(rel, "README.md")
    if not os.path.exists(readme):
        with open(readme, "w", encoding="utf-8", newline="\n") as f:
            f.write(RELEASES_README)
    # A line-ending "fix" on an image corrupts it silently; say it is binary
    # rather than trusting text=auto to guess.
    attrs = os.path.join(rel, ".gitattributes")
    existing = open(attrs, encoding="utf-8").read() if os.path.exists(attrs) else ""
    if "*.bin" not in existing:
        with open(attrs, "a", encoding="utf-8", newline="\n") as f:
            f.write(("" if existing.endswith("\n") or not existing else "\n") + "*.bin binary\n")

    notes = git(ROOT, "tag", "-l", "--format=%(contents)", tag).stdout.strip() or tag
    add_changelog_entry(os.path.join(rel, "CHANGELOG.md"), tag, notes)
    installer = write_installer(rel, tag, rel_file)
    git(rel, "add", "latest.json", rel_file, "README.md", ".gitattributes", "CHANGELOG.md",
        *installer, check=True)
    git(rel, "commit", "-q", "-m", "%s\n\n%s" % (tag, notes), check=True)
    print("committed %s and latest.json in %s" % (rel_file, rel))
    print("\nNothing is live yet. To publish:  cd %s && git push" % rel)


if __name__ == "__main__":
    main()
