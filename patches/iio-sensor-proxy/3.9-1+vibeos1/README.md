# iio-sensor-proxy 3.9-1+vibeos1

The iio-sensor-proxy of the Galaxy Tab S5e (gts4lv) image from VibeOS 0.1.5: Debian's `iio-sensor-proxy` 3.9-1 with
two patches, built against Debian's libssc 0.4.4-2.1. It replaces [3.9-1+gts4lv3](../3.9-1+gts4lv3/).

| Patch | What it does |
| --- | --- |
| `vibeos-sensors-found-while-calls-come-in.patch` | sensors found and opened while D-Bus calls come in (the SSC drivers run the main loop while they wait for the sensor core): claims wait for a sensor being opened, a sensor claimed meanwhile starts once open, and a sensor's Has property goes to every client whenever it changes; a test for it in `tests/integration-test.py` |
| `vibeos-ssc-light-smoothing.patch` | the SSC light level averaged over about 3 s and reported only past a +15 %/-25 % dead band (SSC light sensor only) |

`series` lists them in order. Debian's 3.9-1 has no `debian/patches` of its own; `debian/rules` is Debian's.

## Base

Debian source package `iio-sensor-proxy` 3.9-1 (`iio-sensor-proxy_3.9-1.dsc`, sha256
`92fa4df9f49c8c1596dad44b44212ba30f4958acf86327dec215548ffaa716bd`).

## Rebuild

    apt-get source iio-sensor-proxy=3.9-1   # or dget the .dsc from snapshot.debian.org
    cd iio-sensor-proxy-3.9
    mkdir -p debian/patches
    cp PATCHES/iio-sensor-proxy/3.9-1+vibeos1/*.patch debian/patches/
    cp PATCHES/iio-sensor-proxy/3.9-1+vibeos1/series debian/patches/series
    dch -v 3.9-1+vibeos1 -D unstable "VibeOS: sensors found while D-Bus calls come in; SSC light smoothing"
    dpkg-buildpackage -b -uc -us            # on arm64 (Debian sid), with libssc-dev 0.4.4-2.1

## Shipped in

`iio-sensor-proxy` 3.9-1+vibeos1 in the Galaxy Tab S5e device image of VibeOS 0.1.5 (`vibeos-device-s5e` 0.1.5).

## Licence

GPL-3, as `src/iio-sensor-proxy.c` and `COPYING`; the test the first patch adds is in `tests/integration-test.py`,
which is GPL-2.0-or-later. See Debian's `debian/copyright` for the per-file terms.

## How this set was checked

These are the build tree's own `debian/patches`; the build log shows dpkg-source applying exactly these two, and the
tree's source outside `debian/` is Debian's 3.9-1 (dpkg-source unapplied them after the build). The .deb the image
build takes is the one the build's `.changes` lists.
