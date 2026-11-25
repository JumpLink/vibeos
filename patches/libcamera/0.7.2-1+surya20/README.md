# libcamera 0.7.2-1+surya20

The libcamera of the VibeOS system image: Debian's `libcamera` 0.7.2-1 with the soft-ISP patches written for the
POCO X3 NFC (`surya-*`) and the Galaxy S10e's (`s10e-exynos9820.patch`).

| Patch | What it does |
| --- | --- |
| `surya-softisp-agc-awb.patch` | soft ISP: gentler auto exposure and white balance; digital gain at the end of the exposure range |
| `surya-softisp-egl-release.patch` | soft ISP: the shared EGL display is terminated only when the last stream using it stops |
| `surya-softisp-af.patch` | soft ISP: contrast autofocus (sharpness statistics, a lens control loop) |
| `surya-softisp-awb-neutral.patch` | soft ISP: colour correction and white balance from near-neutral pixels |
| `surya-softisp-agc-fast.patch` | soft ISP: large exposure steps when far off |
| `surya-softisp-bin-denoise.patch` | soft ISP: 4x4 raw averaging (binning) for less noise when the output is small |
| `surya-softisp-af-perf.patch` | soft ISP: the focus measure on every frame while autofocus scans, without mapping the whole raw frame |
| `surya-softisp-agc-ratio.patch` | soft ISP: far from the target, exposure jumps by the ratio of target to measured mean (settles in a few frames, also in the dark) |
| `surya-sensor-helpers.patch` | libipa: sensor helpers (gain models) for the POCO X3 NFC's camera sensors |
| `surya-softisp-af-timing.patch` | soft ISP: autofocus timing in frames at any frame rate, without hunting |
| `surya-softisp-bin-texel-centre.patch` | soft ISP: binned sampling at texel centres (no checkerboard at 2:1) |
| `surya-simple-frame-duration.patch` | simple pipeline: frame duration limits from the sensor's blanking (frame rates through PipeWire) |
| `s10e-exynos9820.patch` | Galaxy S10e (Exynos 9820): CSIS in the simple pipeline, sensor helpers for s5k2l4/s5k3p9/s5k3j1, the focus measure on unpacked 16-bit raw |

`series` lists them in order. They go after Debian's own `debian/patches/series` (`skip_failing_tests.patch`,
`Fix_detection_tensorflow-lite.patch`).

## Base

Debian source package `libcamera` 0.7.2-1 (`libcamera_0.7.2-1.dsc`, sha256
`b162125bb8d26641b3b71ed32d344a06ae7fb4e732043db7a081615ffd039983`).

## Rebuild

    apt-get source libcamera=0.7.2-1        # or dget the .dsc from snapshot.debian.org
    cd libcamera-0.7.2
    cp PATCHES/libcamera/0.7.2-1+surya20/*.patch debian/patches/
    cat PATCHES/libcamera/0.7.2-1+surya20/series >> debian/patches/series
    dch -v 0.7.2-1+surya20 -D unstable "VibeOS: soft ISP for the POCO X3 NFC and the Galaxy S10e"
    dpkg-buildpackage -b -uc -us            # on arm64 (or in an arm64 chroot)

## Shipped in

`libcamera0.7` and `libcamera-ipa` 0.7.2-1+surya20 in the VibeOS system image (0.1.4.3 and later), with
`libcamera-v4l2` 0.7.2-1+surya2 and `gstreamer1.0-libcamera` [0.7.2-1+surya21](../0.7.2-1+surya21/).

These packages were not made by one full build. The files this series changes - `libcamera.so.0.7.2`,
`ipa_soft_simple.so` and `soft_ipa_proxy` - come from a build of Debian's source with this series. Every other file
in them (`libcamera-base.so`, the other IPA modules and proxies, `v4l2-compat.so`) comes unchanged from an earlier
full build, 0.7.2-1+surya2, whose only patch was an earlier `surya-softisp-agc-awb.patch`. That patch changes
`src/ipa/simple/` alone, so those files are built from Debian's 0.7.2-1 source as it is. A full build of this
series produces all of them.

## Licence

LGPL-2.1-or-later, as the files they change, except the changes to the BSD-2-Clause shader
`src/libcamera/shaders/bayer_1x_packed.frag`; see Debian's `debian/copyright`.

## How this set was checked

Debian's 0.7.2-1 source with its own series and then this one is, outside `debian/`, file for file the tree the
shipped binaries were built in. The GNU build IDs of the shipped `libcamera.so.0.7.2`, `ipa_soft_simple.so` and
`soft_ipa_proxy` equal those of that tree's build output. The other files are byte-identical to the POCO image's
[0.7.2-1+surya17](../0.7.2-1+surya17/) packages, which carry the same +surya2 files.
