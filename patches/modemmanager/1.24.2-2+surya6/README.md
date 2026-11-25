# ModemManager 1.24.2-2+surya6

The ModemManager of the POCO X3 NFC (surya) image: Debian's `modemmanager` 1.24.2-2 with seven patches on top. The
first three are the system image's ([1.24.2-2+surya3](../1.24.2-2+surya3/)); 0004-0007 are for the POCO's
Qualcomm modem (QMI).

| Patch | What it does |
| --- | --- |
| `0001-qmi-retry-answer-while-call-in-setup.patch` | QMI: retry answering while a call is still in setup |
| `0002-generic-net-port-cgact.patch` | generic bearer: connect a net port tagged `ID_MM_NET_CGACT` with `+CGACT` and `+CGCONTRDP` |
| `0003-generic-net-port-data.patch` | generic plugin: a net port tagged `ID_MM_NET_CGACT` is a data port |
| `0004-netlink-transaction-use-after-free.patch` | netlink: don't call a transaction's completion function after freeing it |
| `0005-qmi-bind-wds-clients-to-primary-subscription.patch` | QMI: bind each bearer WDS client to the primary subscription |
| `0006-qmi-activate-primary-gw-provisioning-session.patch` | QMI: activate the primary GW provisioning session when there is none |
| `0007-qmi-retry-start-network-while-emm-not-ready.patch` | QMI: retry Start Network while EMM is not ready right after attach |

`series` lists them in order. Debian's 1.24.2-2 has no `debian/patches` of its own.

## Base

Debian source package `modemmanager` 1.24.2-2 (`modemmanager_1.24.2-2.dsc`, sha256
`51f3682ceb1f0ba154107d9cf2f75218b332823be19302c875d8d6874f1694a2`).

## Rebuild

    apt-get source modemmanager=1.24.2-2    # or dget the .dsc from snapshot.debian.org
    cd modemmanager-1.24.2
    mkdir -p debian/patches
    cp PATCHES/modemmanager/1.24.2-2+surya6/*.patch debian/patches/
    cp PATCHES/modemmanager/1.24.2-2+surya6/series debian/patches/series
    dch -v 1.24.2-2+surya6 -D unstable "VibeOS: QMI fixes for the POCO X3 NFC"
    dpkg-buildpackage -b -uc -us            # on arm64 (or in an arm64 chroot)

## Shipped in

- `modemmanager`, `libmm-glib0` and `gir1.2-modemmanager-1.0` 1.24.2-2+surya6, in the POCO X3 NFC device image
  (`vibeos-device-poco` 0.1.4.3), installed over the system image's +surya3.

## Licence

GPL-2.0-or-later (the daemon and its plugins, which these patches change); libmm-glib is LGPL-2.1-or-later. See
Debian's `debian/copyright`.

## How this set was checked

The build tree's source outside `debian/` is Debian's 1.24.2-2 (dpkg-source unapplied the patches after the build),
and these seven apply to it in order without fuzz. The patch files are the build's own, unchanged since the +surya6
build (0001-0006 dated before it, 0007 at the +surya6 changelog entry). The three shipped .debs are byte-identical to
that build's output, and the shipped daemon carries the strings that 0005, 0006 and 0007 add.
