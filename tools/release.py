#!/usr/bin/env python3
"""Publish build/netdash.bin as a GitHub release that dongles will install.

The dongle only accepts an image that names itself as the release it is
attached to, so this refuses to go on unless:

  * the working tree is clean, and HEAD is exactly on an annotated tag, and
  * build/netdash.bin carries that same version (git describe baked it in at
    build time), and the project name "netdash".

Build first, on the tag:

    git tag -a v0.13.1 -m "What changed"
    idf.py build
    python tools/release.py                # checks, and stages dist/netdash-v0.13.1.bin
    python tools/release.py --publish      # ...and creates the GitHub release

--publish needs the tag already pushed (git push origin v0.13.1) and a token
in the GITHUB_TOKEN environment variable with "Contents: Read and write" on the
repository. That is a different, stronger token from the read-only one on the
dongle; keep it on this machine only.

Standard library only, so it runs from any Python 3.8+.
"""
import argparse
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import urllib.error
import urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN = os.path.join(ROOT, "build", "netdash.bin")
ASSET_NAME = "netdash.bin"
PROJECT = "netdash"

# esp_app_desc_t sits right after the 24-byte image header and the 8-byte
# header of the first segment.
APP_DESC_OFFSET = 32
APP_DESC_MAGIC = 0xABCD5432


def fail(msg):
    print("release: " + msg, file=sys.stderr)
    sys.exit(1)


def git(*args):
    return subprocess.run(["git"] + list(args), cwd=ROOT, capture_output=True, text=True)


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


def repo_from_sdkconfig():
    path = os.path.join(ROOT, "sdkconfig")
    try:
        with open(path, encoding="utf-8") as f:
            m = re.search(r'^CONFIG_NETDASH_OTA_REPO="([^"]*)"', f.read(), re.M)
            return m.group(1) if m else ""
    except OSError:
        return ""


def api(method, url, token, body=None, data=None, content_type="application/json"):
    headers = {
        "Accept": "application/vnd.github+json",
        "Authorization": "Bearer " + token,
        "X-GitHub-Api-Version": "2022-11-28",
        "User-Agent": "landash-release-script",
    }
    if body is not None:
        data = json.dumps(body).encode()
    if data is not None:
        headers["Content-Type"] = content_type
    req = urllib.request.Request(url, data=data, method=method, headers=headers)
    try:
        with urllib.request.urlopen(req, timeout=120) as r:
            return json.loads(r.read() or b"null")
    except urllib.error.HTTPError as e:
        detail = e.read().decode("utf-8", "replace")
        fail("%s %s -> HTTP %d\n%s" % (method, url.split("?")[0], e.code, detail))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--publish", action="store_true", help="create the GitHub release")
    ap.add_argument("--repo", default=None, help="owner/name (default: from sdkconfig)")
    ap.add_argument("--notes", default=None, help="release notes (default: the tag message)")
    args = ap.parse_args()

    if git("status", "--porcelain", "--untracked-files=no").stdout.strip():
        fail("the working tree has uncommitted changes; commit, tag, then build")
    tag = git("describe", "--exact-match", "--tags", "HEAD").stdout.strip()
    if not tag:
        fail("HEAD is not on a tag. Tag it (git tag -a vX.Y.Z -m ...) and rebuild")
    if not os.path.exists(BIN):
        fail("no %s; run idf.py build" % BIN)

    version, project = read_app_desc(BIN)
    if project != PROJECT:
        fail("the image is project %r, expected %r" % (project, PROJECT))
    if version != tag:
        fail("the image says %s but HEAD is %s. Rebuild on the tag (idf.py build)" % (version, tag))
    size = os.path.getsize(BIN)
    print("ok: %s is %s, %d bytes" % (os.path.relpath(BIN, ROOT), version, size))

    dist = os.path.join(ROOT, "dist")
    os.makedirs(dist, exist_ok=True)
    staged = os.path.join(dist, "netdash-%s.bin" % tag)
    shutil.copyfile(BIN, staged)
    print("staged: %s" % os.path.relpath(staged, ROOT))

    repo = args.repo or repo_from_sdkconfig()
    if not args.publish:
        print("\nTo publish by hand: GitHub > %s > Releases > Draft a new release," % (repo or "<repo>"))
        print("choose tag %s, attach the staged file renamed to %s, and Publish." % (tag, ASSET_NAME))
        print("Or rerun with --publish and GITHUB_TOKEN set.")
        return

    token = os.environ.get("GITHUB_TOKEN", "").strip()
    if not token:
        fail("--publish needs GITHUB_TOKEN (Contents: Read and write on %s)" % repo)
    if not repo:
        fail("no repository: pass --repo owner/name")
    if not git("ls-remote", "--tags", "origin", "refs/tags/" + tag).stdout.strip():
        fail("tag %s is not on origin yet: git push origin main %s" % (tag, tag))

    notes = args.notes
    if notes is None:
        notes = git("tag", "-l", "--format=%(contents)", tag).stdout.strip() or tag

    base = "https://api.github.com/repos/" + repo
    rel = api("POST", base + "/releases", token, body={
        "tag_name": tag, "name": tag, "body": notes, "draft": False, "prerelease": False,
    })
    upload = rel["upload_url"].split("{")[0] + "?name=" + ASSET_NAME
    with open(BIN, "rb") as f:
        api("POST", upload, token, data=f.read(), content_type="application/octet-stream")
    print("published: %s" % rel["html_url"])
    print("Dongles pick it up at their next check, or at once from Settings > Maintenance > Check now.")


if __name__ == "__main__":
    main()
