# iio-sensor-proxy 3.9-1+gts4lv3

The iio-sensor-proxy of the Galaxy Tab S5e (gts4lv) image in VibeOS 0.1.4.3: Debian's `iio-sensor-proxy` 3.9-1,
built on Debian trixie against libssc 0.4.4, with two patches.

| Patch | What it does |
| --- | --- |
| `iio-sensor-proxy-early-claim.patch` | a sensor claimed while it is still being opened is started once it is open (the SSC drivers run a nested main loop during discovery, so GNOME claimed the accelerometer first and auto-rotation never got a reading) |
| `iio-sensor-proxy-light-smoothing.patch` | the light level smoothed (about 3 s average, a +15 %/-25 % dead band): the SSC light sensor streams at about 13 Hz |

`series` lists them in order. Debian's 3.9-1 has no `debian/patches`; the build applied these to the source
directly. Their headers here are the usual `a/` `b/` paths, for `patch -p1` or quilt.

`debian.diff` adds `dh_shlibdeps --dpkg-shlibdeps-params=--ignore-missing-info` (libssc was built from source and
installed without package information).

[3.9-1+vibeos1](../3.9-1+vibeos1/) replaces this build in VibeOS 0.1.5.

## Base

Debian source package `iio-sensor-proxy` 3.9-1 (`iio-sensor-proxy_3.9-1.dsc`, sha256
`92fa4df9f49c8c1596dad44b44212ba30f4958acf86327dec215548ffaa716bd`).

## Rebuild

    apt-get source iio-sensor-proxy=3.9-1   # or dget the .dsc from snapshot.debian.org
    cd iio-sensor-proxy-3.9
    patch -p1 < PATCHES/iio-sensor-proxy/3.9-1+gts4lv3/debian.diff
    mkdir -p debian/patches
    cp PATCHES/iio-sensor-proxy/3.9-1+gts4lv3/*.patch debian/patches/
    cp PATCHES/iio-sensor-proxy/3.9-1+gts4lv3/series debian/patches/series
    dch -v 3.9-1+gts4lv3 -D trixie "VibeOS: early claims, light smoothing (Galaxy Tab S5e)"
    dpkg-buildpackage -b -uc -us            # on arm64, Debian trixie, with libssc-dev 0.4.4 installed

## Shipped in

`iio-sensor-proxy` 3.9-1+gts4lv3 in the Galaxy Tab S5e device image (`vibeos-device-s5e` 0.1.4.3).

## Licence

GPL-3, as `src/iio-sensor-proxy.c` and `COPYING` (see Debian's `debian/copyright` for the per-file terms).
`debian.diff` changes Debian's packaging, which is GPL-2.0-or-later.

## How this set was checked

Debian's 3.9-1 source with these two patches and `debian.diff` gives the build's tree file for file (outside build
products). The shipped .deb matches the build's `.changes`, and the GNU build ID of its `iio-sensor-proxy` equals
that of the tree's build output. The patches are also published, with the build's original headers, in
`mobian/` of [linux-gts4lv](https://github.com/pulpoff/linux-gts4lv).
