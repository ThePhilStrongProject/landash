#!/usr/bin/env python3
"""Commit build/netdash.bin to the releases repository, ready to push.

Dongles read latest.json from the releases repository (CONFIG_NETDASH_OTA_REPO,
a separate, public repo) and install the image it names when its version is
newer than theirs. This script writes both into a local clone of that repo and
commits them; pushing is left to you, because that is the moment it goes live.

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
DEFAULT_RELEASES = os.path.join(os.path.dirname(ROOT), "landash-releases")
PROJECT = "netdash"

# esp_app_desc_t sits right after the 24-byte image header and the 8-byte
# header of the first segment.
APP_DESC_OFFSET = 32
APP_DESC_MAGIC = 0xABCD5432

RELEASES_README = """# LANDA.SH firmware releases

Built firmware for the LANDA.SH network dashboard (a Waveshare ESP32-C6-GEEK
dongle). Dongles read `latest.json` from this branch and install the image it
names when it is newer than what they run.

Nothing here is edited by hand: `tools/release.py` in the source repository
writes each release and commits it. Pushing that commit is what publishes it.

To hold a release back, do not push. To withdraw one that is out, point
`latest.json` back at an older file *and* publish a newer version number than
the bad one - dongles never install a version lower than their own.
"""


def fail(msg):
    print("release: " + msg, file=sys.stderr)
    sys.exit(1)


def git(cwd, *args, check=False):
    r = subprocess.run(["git"] + list(args), cwd=cwd, capture_output=True, text=True)
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


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--releases", default=DEFAULT_RELEASES,
                    help="local clone of the releases repository (default: %(default)s)")
    args = ap.parse_args()

    # --- this repository and the build -------------------------------------
    if git(ROOT, "status", "--porcelain", "--untracked-files=no").stdout.strip():
        fail("this working tree has uncommitted changes; commit, tag, then build")
    tag = git(ROOT, "describe", "--exact-match", "--tags", "HEAD").stdout.strip()
    if not tag:
        fail("HEAD is not on a tag. Tag it (git tag -a vX.Y.Z -m ...) and rebuild")
    if version_tuple(tag) is None:
        fail("tag %s is not vMAJOR.MINOR.PATCH, which dongles need to compare versions" % tag)
    if not os.path.exists(BIN):
        fail("no %s; run idf.py build" % BIN)

    version, project = read_app_desc(BIN)
    if project != PROJECT:
        fail("the image is project %r, expected %r" % (project, PROJECT))
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
    if os.path.exists(manifest_path):
        with open(manifest_path, encoding="utf-8") as f:
            current = json.load(f).get("version", "")
        cur = version_tuple(current)
        if cur is not None and version_tuple(tag) <= cur:
            fail("latest.json already names %s; %s is not newer, and dongles would ignore it"
                 % (current, tag))

    rel_file = "firmware/netdash-%s.bin" % tag
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
    git(rel, "add", "latest.json", rel_file, "README.md", ".gitattributes", check=True)
    git(rel, "commit", "-q", "-m", "%s\n\n%s" % (tag, notes), check=True)
    print("committed %s and latest.json in %s" % (rel_file, rel))
    print("\nNothing is live yet. To publish:  cd %s && git push" % rel)


if __name__ == "__main__":
    main()
