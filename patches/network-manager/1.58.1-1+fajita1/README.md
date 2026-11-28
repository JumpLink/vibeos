# NetworkManager 1.58.1-1+fajita1

Debian's `network-manager` 1.58.1-1 with one patch after Debian's own series: when NetworkManager disconnects a
mobile connection, it disconnects only the bearer it connected, not every bearer of the modem.

| Patch | What it does |
| --- | --- |
| `0001-wwan-disconnect-only-own-bearer.patch` | wwan: disconnect only the bearer NetworkManager connected; while a connection attempt runs, all as before |

`series` lists it. Debian's 1.58.1-1 has a `debian/patches/series` of its own; this one goes after it.

## Why

NetworkManager disconnects the modem when the phone goes to sleep, and it did so with no bearer path, which makes
ModemManager disconnect all bearers. With VoLTE the modem has a second bearer, for IMS, which belongs to the VoLTE
daemon. It dropped on every suspend, the daemon connected it again, and that woke the phone a few milliseconds
later, so it never stayed in deep sleep. NetworkManager issue
[#2050](https://gitlab.freedesktop.org/NetworkManager/NetworkManager/-/issues/2050) reports the same on
postmarketOS; there is no fix upstream yet.

On the OnePlus 6T this is one of two changes deep sleep needs; the other is in ModemManager (the bearer's WDS event
reports).

## Base

Debian source package `network-manager` 1.58.1-1 (`network-manager_1.58.1-1.dsc`, sha256
`4019446cd3e731da0fe8e45aecde9da2113666072d4d6f80f080dd2cb5135ade`), from the Debian archive or snapshot.debian.org.

## Rebuild

    apt-get source network-manager=1.58.1-1    # or dget the .dsc from snapshot.debian.org
    cd network-manager-1.58.1
    cp PATCHES/network-manager/1.58.1-1+fajita1/*.patch debian/patches/
    cat PATCHES/network-manager/1.58.1-1+fajita1/series >> debian/patches/series
    dch -v 1.58.1-1+fajita1 -D unstable "VibeOS: wwan disconnects only its own bearer"
    dpkg-buildpackage -b -uc -us               # on arm64 (or in an arm64 chroot)

## Shipped in

- `network-manager`, `libnm0`, `gir1.2-nm-1.0`, `network-manager-l10n` and `network-manager-tui` 1.58.1-1+fajita1,
  in the OnePlus 6T's device zip `vibeos-device-oneplus-6t-0.1.5-kernel7.4.zip` (package
  `vibeos-device-nura-oneplus-fajita` 0.1.5+nura20261114.0517-5). They replace the system image's Debian 1.58.1-1
  builds at the first boot and stay held. Built from this folder as above: Debian's `.dsc` as in Base, the patch
  applied at fuzz 0 after Debian's own, in an arm64 chroot with the build profiles `nocheck nodoc`. Of
  NetworkManager's own tests, 74 of 85 passed; the other 11 cannot run under qemu-user: 9 need a generic netlink
  socket, which qemu-user does not offer, the nmcli tests' D-Bus stub service did not start in time, and the
  translation file check ran past its 30 s.
- The contributor's builds of the same set are attached to the OnePlus 6T kernel pre-release
  [fajita-7.4.0-rc2-1](https://github.com/JumpLink/linux-fajita/releases/tag/fajita-7.4.0-rc2-1).

## Licence

GPL-2.0-or-later (the daemon, which this patch changes); libnm is LGPL-2.1-or-later. See Debian's
`debian/copyright`.

## How this set was checked

The packages were built from Debian's 1.58.1-1 source with this patch after Debian's series, in an arm64 Debian
sid container (`dpkg-buildpackage -b`, `DEB_BUILD_OPTIONS=nocheck`). The patch here is that build's diff with a
commit message added. [`image/test/nm-own-bearer-test.sh`](../../../image/test/nm-own-bearer-test.sh) applies
Debian's series and this one to the source from snapshot.debian.org and checks the patched `disconnect()`. On a
OnePlus 6T with VoLTE, together with the ModemManager change, the phone stayed in deep sleep for 21 minutes and an
incoming call woke it.
