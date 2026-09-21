# Firmware updates

The dongle updates itself from a **releases repository**: a separate, public
GitHub repo that holds nothing but built firmware and one small file,
`latest.json`, saying which one is current:

```json
{
  "version": "v0.14.1",
  "file": "firmware/netdash-v0.14.1.bin",
  "size": 1982176
}
```

Two minutes after it boots, and every 12 hours after that, the dongle reads
`latest.json` from `raw.githubusercontent.com`. If the version is newer than
its own, it downloads that file, checks it, and restarts into it.

Publishing is therefore a commit and a push. `tools/release.py` makes the
commit; the push is yours, because that is the moment it goes live.

| | |
|---|---|
| Source code | `ThePhilStrongProject/landash`, private |
| Releases | `ThePhilStrongProject/landash-releases`, public, branch `main` |
| Set by | `CONFIG_NETDASH_OTA_REPO` and `CONFIG_NETDASH_OTA_BRANCH`, under **NetDash** in `idf.py menuconfig` |

The releases repository is fixed when the firmware is built, not a web
setting, on purpose. The dashboard has no login, so anyone on your network
could otherwise point the dongle at their own firmware.

Because it is public, dongles need no token, and nothing expires. Anyone can
download a built image, but nobody can see the source.

---

## One-time setup

1. **Clone the releases repository next to this one**, so the two folders sit
   side by side:

   ```
   Documents/GitHub/landash
   Documents/GitHub/landash-releases
   ```

   `tools/release.py` looks for it there; `--releases <path>` points it elsewhere.

2. **Flash each dongle over USB once** with a build from v0.14.0 or later.
   Updates rely on a bootloader that can roll back a failed update, and an
   update never replaces the bootloader:

   ```powershell
   idf.py build
   idf.py -p COM5 flash
   ```

   From then on everything arrives over the air.

---

## Publishing a release

```powershell
# 1. Commit everything, then tag. The tag IS the version, so it must be a
#    higher vMAJOR.MINOR.PATCH than the one out there now.
git tag -a v0.14.2 -m "What changed, in a sentence or two"

# 2. Build ON the tag. The version inside the image comes from git describe,
#    so building before tagging stamps the old version plus -dirty.
idf.py build

# 3. Check the image, copy it into ../landash-releases, rewrite latest.json,
#    and commit there. Nothing is live yet.
python tools/release.py

# 4. Go live.
cd ../landash-releases
git push

# 5. And keep the source repository's tags in step.
cd ../landash
git push origin main v0.14.2
```

`tools/release.py` refuses, and says why, if:

- this tree has uncommitted changes, or HEAD is not exactly on a tag,
- `build/netdash.bin` is not project `netdash` with that same version (the
  usual cause is building before tagging),
- the version is not newer than the one `latest.json` already names, or
- the releases clone has uncommitted changes of its own.

Dongles pick a pushed release up within 12 hours, or straight away with
**Settings › Maintenance › Check now**. GitHub caches raw files for up to five
minutes, so a check in the first few minutes after a push may still see the
previous `latest.json`.

To **hold a release back**, commit it and do not push. To **withdraw** one
that is already out, publish a newer version. Dongles never install a version
lower than their own, so pointing `latest.json` back at an old file does
nothing for dongles that already have the bad one.

---

## What the dongle does, in order

1. Reads `latest.json` from the releases repository.
2. Compares its `version` with its own. `v0.14.2` beats `v0.14.1`, and a build
   a few commits past `v0.14.1` counts as `v0.14.1`.
3. If the release is newer and *Install them automatically* is on, it
   downloads `file`. If automatic install is off, it puts "v0.14.2 is
   available" in the feed and waits for **Install**.
4. Before writing anything, it checks the image calls itself project `netdash`
   and version `v0.14.2`, so a manifest pointing at the wrong file is refused.
   The image's own SHA-256 is checked once it is written.
5. It restarts into the new image, and an open dashboard page reloads itself
   onto the new version.
6. The new version is on probation. Once it has run for a minute and got back
   onto Wi-Fi, it is kept, and the feed says "Updated to v0.14.2 (was
   v0.14.1)". If it crashes first, or cannot get online within ten minutes, the
   bootloader goes back to v0.14.1, the feed says so, and v0.14.2 is never
   installed automatically again. Publish a v0.14.3 with the fix.

**Development builds are left alone.** A dongle running a build with
uncommitted changes (`…-dirty`) is never updated automatically, since whoever
flashed it is presumably working on it. **Install** still works on it by hand.

---

## A private releases repository

A private releases repository works too, if you would rather binaries were not
public. The dongles then each need a read-only token:

1. GitHub › **Settings** › **Developer settings** › **Fine-grained tokens** ›
   **Generate new token**. Set the owner to `ThePhilStrongProject`, choose
   *Only select repositories* › `landash-releases`, and give it
   **Contents: Read-only** and nothing else.
2. On each dongle, go to **Settings › Maintenance › Access token**, paste the
   token and press **Save token**.

The token is never shown again or returned by the API, and is sent only to
`raw.githubusercontent.com`. When it expires, updates stop with "GitHub
refused the access token" until a new one is pasted in. That is why the public
repository is the default.

---

## When it does not work

**Settings › Maintenance** shows the last error. The likely ones:

| Message | Meaning |
|---|---|
| No latest.json found (a private repository needs a token) | Nothing has been pushed yet, or the branch is not `main`, or the repository is private. |
| latest.json has no usable "version" / "file" | The manifest was edited by hand. Let `tools/release.py` write it. |
| The image says v0.14.2-dirty, latest.json says v0.14.2 | The build was made before the tag, or with uncommitted changes. |
| The file is not a netdash image | `file` points at something else. |
| GitHub is rate-limiting requests | It retries at the next interval. |
| Not connected to Wi-Fi | It retries every five minutes. |
