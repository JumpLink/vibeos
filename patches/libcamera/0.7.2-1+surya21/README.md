# libcamera 0.7.2-1+surya21 (gstreamer1.0-libcamera)

The GStreamer element (`libcamerasrc`) of the VibeOS system image: Debian's `libcamera` 0.7.2-1 with one patch to
`src/gstreamer/`.

| Patch | What it does |
| --- | --- |
| `gst-disconnect-request-completed.patch` | gstreamer: disconnect `requestCompleted` when the camera is closed (a camera opened again delivered every request once per earlier session: a use-after-free) |

`series` lists it. It goes after Debian's own `debian/patches/series`, and applies as well after the
[+surya20](../0.7.2-1+surya20/) series: none of those patches touches `src/gstreamer/`.

## Base

Debian source package `libcamera` 0.7.2-1 (`libcamera_0.7.2-1.dsc`, sha256
`b162125bb8d26641b3b71ed32d344a06ae7fb4e732043db7a081615ffd039983`).

## Rebuild

    apt-get source libcamera=0.7.2-1        # or dget the .dsc from snapshot.debian.org
    cd libcamera-0.7.2
    cp PATCHES/libcamera/0.7.2-1+surya21/*.patch debian/patches/
    cat PATCHES/libcamera/0.7.2-1+surya21/series >> debian/patches/series
    dch -v 0.7.2-1+surya21 -D unstable "VibeOS: libcamerasrc disconnects requestCompleted on close"
    dpkg-buildpackage -b -uc -us            # on arm64; gstreamer1.0-libcamera is the package that carries it

## Shipped in

`gstreamer1.0-libcamera` 0.7.2-1+surya21 in the VibeOS system image (0.1.4.3 and later): the +surya2 package with
`libgstlibcamera.so` replaced by one built with this patch (12 November 2026).

## Licence

LGPL-2.1-or-later; see Debian's `debian/copyright`.

## How this set was checked

The build tree no longer exists. The shipped `libgstlibcamera.so` imports `libcamera::SignalBase::disconnect`, which
only this patch calls (the +surya2 element does not import it), and this patch is the only change to
`src/gstreamer/` in any of these builds.
