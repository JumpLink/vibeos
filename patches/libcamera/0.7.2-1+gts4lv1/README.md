# libcamera 0.7.2-1+gts4lv1

The libcamera of the Galaxy Tab S5e (gts4lv) image: Debian's `libcamera` 0.7.2-1 with eight of the POCO's soft-ISP
patches and sensor helpers for the tablet's cameras, built in one full `dpkg-buildpackage` run on the tablet.

| Patch | What it does |
| --- | --- |
| `surya-softisp-agc-awb.patch` ... `surya-softisp-agc-ratio.patch` | the first eight soft-ISP patches of [0.7.2-1+surya20](../0.7.2-1+surya20/); see there |
| `gts4lv-sensor-helpers.patch` | libipa: sensor helpers for the Galaxy Tab S5e cameras (s5k3m5, s5k4ha) |

`series` lists the nine in the order they were applied. They go after Debian's own `debian/patches/series`
(`skip_failing_tests.patch`, `Fix_detection_tensorflow-lite.patch`).

`debian.diff` is the one packaging change: `libtensorflow-lite-dev` is dropped from Build-Depends for the build on
the tablet (Debian trixie). Without it the Raspberry Pi neural-network AWB, which nothing on these devices uses, is
not built.

The tablet's own `debian/patches/series` also named `surya-softisp-af-timing.patch`,
`surya-softisp-bin-texel-centre.patch` and `surya-simple-frame-duration.patch`, added after the build: they were not
in it, so they are not here.

## Base

Debian source package `libcamera` 0.7.2-1 (`libcamera_0.7.2-1.dsc`, sha256
`b162125bb8d26641b3b71ed32d344a06ae7fb4e732043db7a081615ffd039983`).

## Rebuild

    apt-get source libcamera=0.7.2-1        # or dget the .dsc from snapshot.debian.org
    cd libcamera-0.7.2
    patch -p1 < PATCHES/libcamera/0.7.2-1+gts4lv1/debian.diff
    cp PATCHES/libcamera/0.7.2-1+gts4lv1/*.patch debian/patches/
    cat PATCHES/libcamera/0.7.2-1+gts4lv1/series >> debian/patches/series
    dch -v 0.7.2-1+gts4lv1 -D trixie "VibeOS: soft ISP and sensor helpers for the Galaxy Tab S5e"
    dpkg-buildpackage -b -uc -us            # on arm64, Debian trixie

## Shipped in

`libcamera0.7`, `libcamera-ipa`, `libcamera-v4l2` and `gstreamer1.0-libcamera` 0.7.2-1+gts4lv1 in the Galaxy Tab S5e
device image (`vibeos-device-s5e` 0.1.4.3 and 0.1.5), installed over the system image's packages.

## Licence

LGPL-2.1-or-later, as the files they change, except the changes to the BSD-2-Clause shader
`src/libcamera/shaders/bayer_1x_packed.frag`; see Debian's `debian/copyright`.
`debian.diff` changes Debian's packaging, which is GPL-2.0-or-later.

## How this set was checked

Debian's 0.7.2-1 source with its own series, this series and `debian.diff` gives the build's tree file for file
(outside build products). The GNU build IDs of the shipped `libcamera.so.0.7.2` and `ipa_soft_simple.so` equal those
of that tree's build output, the four shipped .debs match the checksums in the build's `.changes`, and the shipped
binaries lack the strings the three later patches add.
