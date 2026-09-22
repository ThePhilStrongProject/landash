# Security

LANDA.SH is a hobby project, built for a home network where everyone who can
reach the dongle is trusted. This page is honest about what that means.

## Reporting a problem

Please report security problems privately, using GitHub's **private
vulnerability reporting** on this repository (*Security › Report a
vulnerability*), rather than in a public issue. Include what you found, how
to reproduce it, and which firmware version (Settings › Maintenance, or the
System tab). You will get a reply, and a fix will go out as a normal
over-the-air update.

## What LANDA.SH does not protect against

**Anyone on your network.** The dashboard and its API have no login. Anyone
who can reach the dongle can see your devices, change settings, add or remove
dashboard links, turn off updates, forget the Wi-Fi settings, or factory-reset
it. Do not put it on a network shared with people you do not trust, such as a
guest network or a shared house. If you need to reach it from outside, use a
VPN into your network; never forward a port to it.

**Someone watching your network traffic.** The dashboard is plain HTTP.
Browsers cannot trust a certificate for a device on a home network without
warnings, so it does not use one. Everything the dashboard shows, and
everything typed into it, crosses the network unencrypted. That includes the
Wi-Fi password when it is set and the credential vault's passphrase and
contents.

## The credential vault

The vault stores logins for your services so they are not sitting in plain
text on the dongle.

- Each entry is encrypted with **AES-256-GCM**. The key is derived from your
  passphrase with **PBKDF2-HMAC-SHA256** (40,000 rounds and a random salt)
  every time you unlock it, and is never written to flash.
- Each entry is bound to the device or link it belongs to, so entries cannot
  be swapped between them.
- It locks itself after 15 minutes unused, and on every restart.
- The passphrase cannot be recovered. Forgetting it means resetting the vault.

**It protects against** someone who takes the dongle and reads its flash: they
get encrypted entries. Each passphrase guess costs 40,000 rounds of hashing,
so a long, random passphrase is what makes this hold.

**It does not protect against** someone watching your network (see above), or
someone on your network when the vault is unlocked.

## Firmware updates

- Updates come from one GitHub repository fixed when the firmware is built.
  It cannot be changed from the dashboard, so nobody on your network can point
  the dongle at their own firmware.
- They are downloaded over HTTPS, with the server's certificate checked
  against a bundle of trusted authorities.
- Before anything is written, the image must identify itself as LANDA.SH
  firmware and as the version announced. Its SHA-256 checksum is checked
  before it is started.
- A new version runs on probation, and the dongle rolls back by itself if it
  crashes or cannot get back online.
- **Images are not cryptographically signed.** Whoever controls the releases
  repository can publish firmware that dongles will install. Keep that
  account secure with two-factor authentication.

If you build your own firmware, point `CONFIG_NETDASH_OTA_REPO` at a releases
repository you control; see [docs/UPDATES.md](docs/UPDATES.md).

## What it does on your network

So that nothing it does surprises you, or your security software:

- **Discovery:** an ICMP echo (ping) to every address in the subnet, one
  address at a time at a steady rate (16 a second by default, adjustable),
  every few minutes, followed by a read of the ARP table.
- **Names:** mDNS queries, an SSDP search, NetBIOS name queries, and reverse
  DNS lookups against your router.
- **Port scan:** TCP connection attempts to each device, at 10 a second by
  default. You choose how far it goes during setup: off, about 120 common
  services (the default), every port from 1 to 1024, or all 65,535. The
  deeper settings are what an intrusion detection system is most likely to
  flag. Change it any time under **Settings › Scanning & health**.
- **Checks:** one TCP connection per dashboard link once a minute, plus a
  ping to 1.1.1.1 and a DNS lookup of `example.com` once a minute by
  default. Both intervals are configurable.
- Outside your network: time from `pool.ntp.org`, and update checks against
  `raw.githubusercontent.com`. Nothing about your network or devices is
  ever sent anywhere.
