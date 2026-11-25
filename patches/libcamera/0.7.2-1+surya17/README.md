# libcamera 0.7.2-1+surya17

The libcamera of the POCO X3 NFC (surya) image: Debian's `libcamera` 0.7.2-1 with the twelve soft-ISP patches
written for the POCO. They are the first twelve of the system image's
[0.7.2-1+surya20](../0.7.2-1+surya20/), which adds the Galaxy S10e's patch; see there for what each does.

`series` lists them in order. They go after Debian's own `debian/patches/series` (`skip_failing_tests.patch`,
`Fix_detection_tensorflow-lite.patch`).

## Base

Debian source package `libcamera` 0.7.2-1 (`libcamera_0.7.2-1.dsc`, sha256
`b162125bb8d26641b3b71ed32d344a06ae7fb4e732043db7a081615ffd039983`).

## Rebuild

    apt-get source libcamera=0.7.2-1        # or dget the .dsc from snapshot.debian.org
    cd libcamera-0.7.2
    cp PATCHES/libcamera/0.7.2-1+surya17/*.patch debian/patches/
    cat PATCHES/libcamera/0.7.2-1+surya17/series >> debian/patches/series
    dch -v 0.7.2-1+surya17 -D unstable "VibeOS: soft ISP for the POCO X3 NFC"
    dpkg-buildpackage -b -uc -us            # on arm64 (or in an arm64 chroot)

## Shipped in

`libcamera0.7` and `libcamera-ipa` 0.7.2-1+surya17 (with `libcamera-v4l2` 0.7.2-1+surya2) in the POCO X3 NFC device
image (`vibeos-device-poco` 0.1.4.3), installed over the system image's packages.

As with +surya20, these packages were not made by one full build: `libcamera.so.0.7.2`, `ipa_soft_simple.so` and
`soft_ipa_proxy` were built from this series on the phone (15 October 2026); every other file comes unchanged from
the 0.7.2-1+surya2 build, whose only patch (an earlier `surya-softisp-agc-awb.patch`) changes `src/ipa/simple/`
alone, so those files are built from Debian's 0.7.2-1 source as it is.

## Licence

LGPL-2.1-or-later, as the files they change, except the changes to the BSD-2-Clause shader
`src/libcamera/shaders/bayer_1x_packed.frag`; see Debian's `debian/copyright`.

## How this set was checked

The build tree on the phone no longer exists, so this set rests on the record and the binaries: the twelve patches
are the versions committed with the build (the last one 9 minutes after it), and the shipped `libcamera.so.0.7.2` and
`ipa_soft_simple.so` carry the strings the last three patches add (frame duration, autofocus timing, texel-centre
shader), which an older build lacks. The other files are byte-identical to the system image's +surya20 packages.
