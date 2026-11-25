# WebKitGTK 2.54.2-2+neo1

The WebKitGTK 6.0 (GTK 4) of the VibeOS system image: Debian's `webkit2gtk` 2.54.2-2 with three patches and a
changed `debian/rules`, cross-built for arm64.

| Patch | What it does |
| --- | --- |
| `touch-scroll-resample.patch` | GTK: touch scrolling resampled to the frame clock (a 240 Hz touchscreen sent 0-3 scroll steps per frame) |
| `gtk-full-refresh-rate.patch` | GTK: pages render at the display's refresh rate, not near 60 fps |
| `cross-gir-scanner-target.patch` | cross-building with clang: the GObject-introspection scanner is given the target |

`series` lists them in order. They go after Debian's own `debian/patches/series`.

`debian.diff` changes `debian/rules`:

- the GTK 3 flavour (the 4.1 API packages) is not built (`ENABLE_GTK3=NO`); `debian/rules` then leaves those
  packages out of the `debian/control` it generates;
- `-g0` instead of `-g1`;
- arm64: `-march=armv8.2-a+dotprod+fp16+crypto -mtune=cortex-a76` (the features common to the Snapdragon 732G and
  the Exynos 9820; no RCpc, which the Exynos lacks);
- cross-building: clang gets the target triple, the bubblewrap sandbox is looked up with the host's
  `pkg-config`, and the paths of `bwrap` and `xdg-dbus-proxy` on the target are given.

## Base

Debian source package `webkit2gtk` 2.54.2-2 (`webkit2gtk_2.54.2-2.dsc`, sha256
`19e2f727e8a9cf58e3005d6bfba8434dac73cb3fa127c882a1ca60fe2780b3c0`).

## Rebuild

    apt-get source webkit2gtk=2.54.2-2      # or dget the .dsc from snapshot.debian.org
    cd webkit2gtk-2.54.2
    patch -p1 < PATCHES/webkit2gtk/2.54.2-2+neo1/debian.diff
    cp PATCHES/webkit2gtk/2.54.2-2+neo1/*.patch debian/patches/
    cat PATCHES/webkit2gtk/2.54.2-2+neo1/series >> debian/patches/series
    dch -v 2.54.2-2+neo1 -D unstable "VibeOS: touch scrolling resampled to the frame clock; pages at the display's refresh rate; GTK 4 flavour only; ARMv8.2"
    DEB_BUILD_OPTIONS="nocheck nodoc parallel=8" dpkg-buildpackage -a arm64 -B -d -Pcross,nocheck,nodoc -uc -us

The build ran in a Debian sid amd64 chroot with the arm64 build dependencies installed by hand (`apt-get build-dep
-a arm64` does not resolve them all).

## Shipped in

`libwebkitgtk-6.0-4` and `libjavascriptcoregtk-6.0-1` 2.54.2-2+neo1 in the VibeOS system image (0.1.4.3 and later).
Later builds of the same tree (+neo2, +neo3) are not in any released image or in the archive.

## Licence

The three files these patches change are BSD-2-Clause (WebKitGTK as a whole is LGPL-2.1-or-later and BSD-2-Clause,
file by file; see Debian's `debian/copyright`). `debian.diff` changes Debian's packaging, which is LGPL-2-or-later.

## How this set was checked

Debian's 2.54.2-2 source with its own series and then this one is, outside `debian/`, file for file the build's
patched tree as it was for the +neo1 build (the later +neo2/+neo3 changes taken back out from quilt's own copies;
only compiled Python caches differ). `debian.diff` is that tree's `debian/rules` against Debian's. The shipped
libraries in the system image are byte-identical to those in the .debs the build's `.changes` lists.
