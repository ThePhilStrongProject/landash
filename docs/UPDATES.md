# Firmware updates from GitHub

The dongle updates itself from the **releases** of one GitHub repository. Twice
a day (and two minutes after it boots) it asks GitHub for the latest release. If
that release is a newer version than the one it is running and has
`netdash.bin` attached, it downloads it, checks it, and restarts into it.

The repository is fixed when the firmware is built: `ThePhilStrongProject/landash`,
set by `CONFIG_NETDASH_OTA_REPO` under **NetDash** in `idf.py menuconfig`. It is
not a web setting on purpose. The dashboard has no login, so anyone on your
network could otherwise point the dongle at their own firmware.

---

## One-time setup

### 1. Put the code on GitHub

Create the repository **ThePhilStrongProject/landash** as **Private**, and push
this project to it. In GitHub Desktop: *File > Add local repository*, pick this
folder, then *Publish repository*, set the name to `landash`, the owner to
`ThePhilStrongProject`, and tick *Keep this code private*. From a terminal instead:

```bash
git remote add origin https://github.com/ThePhilStrongProject/landash.git
git push -u origin main --tags
```

`--tags` matters: every release hangs off a tag.

### 2. A read-only token for the dongles

A private repository's releases can only be downloaded with a token. The dongle
needs one that can read this repository and nothing else.

1. GitHub > your avatar > **Settings** > **Developer settings** >
   **Personal access tokens** > **Fine-grained tokens** > **Generate new token**.
2. **Token name:** `landash dongles`.
3. **Resource owner:** `ThePhilStrongProject`. If that is an organisation rather
   than your personal account, it has to allow fine-grained tokens
   (*Organisation settings > Personal access tokens*), and may ask an owner (you)
   to approve this one before it works.
4. **Expiration:** up to you. When it expires, updates stop with
   "GitHub rejected the access token" until you paste a new one. A year is a
   sensible middle ground; set a calendar reminder.
5. **Repository access:** *Only select repositories* > `landash`.
6. **Permissions > Repository permissions > Contents: Read-only.** Leave
   everything else at *No access*. (*Metadata: Read-only* is added
   automatically and is harmless.)
7. **Generate token** and copy it. It starts `github_pat_`.

Then, on each dongle: **Settings > Maintenance > Firmware updates > Access
token**, paste it, **Save token**, then **Check now**.

What that token can do if someone extracts it from a dongle: read the source
code and releases of `landash`. Nothing else, and nothing it can change. It is
never shown by the dashboard or returned by its API, and the dongle only ever
sends it to `api.github.com`.

> Prefer no token on the dongles at all? Keep the code private but publish
> releases to a second, **public** repository (for example
> `ThePhilStrongProject/landash-releases`), and set `CONFIG_NETDASH_OTA_REPO`
> to that. The dongles then need no token, and anyone can download a binary
> but nobody can see the source.

### 3. A token for publishing (optional)

Only needed if you want `tools/release.py --publish` to create releases for
you. Otherwise use the GitHub web page (below).

Same steps as above, but name it `landash release` and give it
**Contents: Read and write**. Keep this one on your PC only. It can change the
repository, so it should never go near a dongle.

### 4. Get an update-capable build onto each dongle, over USB, once

Updates rely on a bootloader that can roll back a failed update, and an update
never replaces the bootloader. So the first build with update support has to be
flashed over USB:

```powershell
idf.py build
idf.py -p COM5 flash
```

From then on, everything can arrive over the air.

---

## Publishing a release

Every release is a git tag, a build made on exactly that tag, and that build's
`build/netdash.bin` attached to a GitHub release for the tag.

```powershell
# 1. Commit everything, then tag. The version IS the tag, so it must be a
#    higher vMAJOR.MINOR.PATCH than what the dongles run.
git tag -a v0.13.2 -m "What changed, in a sentence or two"

# 2. Build ON the tag. The version baked into the image comes from
#    git describe, so building first stamps the old version plus -dirty.
idf.py build

# 3. Check the image really is v0.13.2 and stage a copy in dist/.
python tools/release.py

# 4. Push the commit and the tag.
git push origin main v0.13.2
```

Then either:

- **Web page:** GitHub > `landash` > **Releases** > **Draft a new release** >
  choose tag `v0.13.2` > attach `build/netdash.bin` (the file must be called
  exactly `netdash.bin`) > leave *Set as a pre-release* **unticked** >
  **Publish release**.
- **Script:** `$env:GITHUB_TOKEN = "github_pat_…"` (the read-and-write one),
  then `python tools/release.py --publish`.

Dongles pick it up at their next check, within 12 hours, or straight away with
**Settings > Maintenance > Check now**.

A **draft** or **pre-release** is never installed, because the dongle asks for
GitHub's "latest release", which excludes both. That is a handy way to stage a
release before letting it loose.

---

## What the dongle does, in order

1. Asks `api.github.com` for the latest release of `landash` (with the token,
   if one is set).
2. Compares the tag with its own version: `v0.13.2` beats `v0.13.1`, and a
   build a few commits past `v0.13.1` counts as `v0.13.1`.
3. If it is newer and *Install them automatically* is on, it downloads
   `netdash.bin`. If automatic install is off, it puts "v0.13.2 is available"
   in the feed and waits for **Install**.
4. Before writing anything, it checks the image calls itself project `netdash`
   and version `v0.13.2`. A binary attached to the wrong release is refused.
5. It writes the image to the other app slot, restarts into it, and the web page
   reloads itself onto the new version.
6. The new version is on probation. Once it has run for a minute and got back
   onto Wi-Fi, it is kept, and the feed says "Updated to v0.13.2 (was v0.13.1)".
   If it crashes first, or cannot get online within ten minutes, the bootloader
   goes back to v0.13.1, the feed says so, and v0.13.2 is never installed
   automatically again. (Publish a v0.13.3 with the fix.)

**Development builds are left alone.** A dongle running a build with
uncommitted changes (`…-dirty`) is never updated automatically, since whoever
flashed it is presumably working on it. **Install** still works on it by hand.

---

## When it does not work

**Settings > Maintenance** shows the last error. The likely ones:

| Message | Meaning |
|---|---|
| No release found (a private repository needs a token) | No token is set, and the repository is private (or has no releases yet). |
| No release found, or the token cannot see the repository | The token is not scoped to `landash`, or the organisation has not approved it. |
| GitHub rejected the access token | It has expired or been revoked. Generate a new one and paste it in. |
| GitHub refused the request (rate limit or token permissions) | Without a token GitHub allows 60 requests an hour per IP. Otherwise the token lacks *Contents: Read*. |
| Release v0.13.2 has no netdash.bin attached | The asset is missing, or is named something else. |
| The asset says v0.13.2-dirty, the release says v0.13.2 | The build was made before the tag, or with uncommitted changes. Rebuild on the tag and replace the asset. |
| Not connected to Wi-Fi | It retries every five minutes. |
