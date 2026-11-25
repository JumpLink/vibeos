# ModemManager 1.24.2-2+surya3

The ModemManager of the VibeOS system image: Debian's `modemmanager` 1.24.2-2 with three patches on top.

| Patch | What it does |
| --- | --- |
| `0001-qmi-retry-answer-while-call-in-setup.patch` | QMI: retry answering while a call is still in setup |
| `0002-generic-net-port-cgact.patch` | generic bearer: connect a net port tagged `ID_MM_NET_CGACT` with `+CGACT` and `+CGCONTRDP` |
| `0003-generic-net-port-data.patch` | generic plugin: a net port tagged `ID_MM_NET_CGACT` is a data port |

`series` lists them in order. Debian's 1.24.2-2 has no `debian/patches` of its own.

## Base

Debian source package `modemmanager` 1.24.2-2 (`modemmanager_1.24.2-2.dsc`, sha256
`51f3682ceb1f0ba154107d9cf2f75218b332823be19302c875d8d6874f1694a2`), from the Debian archive or snapshot.debian.org.

## Rebuild

    apt-get source modemmanager=1.24.2-2    # or dget the .dsc from snapshot.debian.org
    cd modemmanager-1.24.2
    mkdir -p debian/patches
    cp PATCHES/modemmanager/1.24.2-2+surya3/*.patch debian/patches/
    cp PATCHES/modemmanager/1.24.2-2+surya3/series debian/patches/series
    dch -v 1.24.2-2+surya3 -D unstable "VibeOS: QMI answer retry, ID_MM_NET_CGACT net ports"
    dpkg-buildpackage -b -uc -us            # on arm64 (or in an arm64 chroot)

## Shipped in

- `modemmanager`, `libmm-glib0` and `gir1.2-modemmanager-1.0` 1.24.2-2+surya3, installed in the VibeOS system image
  (0.1.4.3 and later). Every device image built on that system image carries them; the POCO X3 NFC image
  replaces them with [1.24.2-2+surya6](../1.24.2-2+surya6/).

## Licence

GPL-2.0-or-later (the daemon and its plugins, which these patches change); libmm-glib is LGPL-2.1-or-later. See
Debian's `debian/copyright`.

## How this set was checked

Debian's 1.24.2-2 source with its own series and then this one gives the build's patched tree file for file
(outside `debian/`; the test fixtures differ only by the test patch Debian's `debian/rules` applies during the
build). The three shipped .debs are byte-identical to that build's output. These patches are also published,
byte-identical, in `mobian/modem/modemmanager/` of
[linux-beyond0lte](https://github.com/pulpoff/linux-beyond0lte).
