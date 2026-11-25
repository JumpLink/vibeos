# mutter: 51.0-3+gp1 and the mobile shell's mutter-mobile 51.1.mobile.22

Five patches to mutter 51, for a phone. They do nothing on a desktop.

| Patch | What it does |
| --- | --- |
| `0001-clutter-blur-keep-the-blur-s-buffers-for-the-next-frame.patch` | clutter: the blur keeps its buffers for the next frame instead of making new ones for every frame it changes in |
| `0002-monitor-manager-let-the-compositor-keep-the-outputs-on.patch` | monitor manager: the compositor can keep the outputs on (`power-save-blocked`), for the always-on display |
| `0003-monitor-allow-a-phone-s-logical-size.patch` | monitor: a phone's logical size is allowed (a 1080x2280 panel at scale 3) |
| `0004-orientation-manager-export-the-tracking-inhibit.patch` | orientation manager: `meta_orientation_manager_inhibit_tracking()` and `_uninhibit_tracking()` exported (META_EXPORT) |
| `0005-window-actor-wayland-sync-again-when-a-surface-changes-size.patch` | window actor (Wayland): a fullscreen window syncs again when its surface changes size |

`series` lists them in order. The build scripts take `0*.patch` from this folder in name order.

## Builds that carry them

**mutter 51.0-3+gp1** (`libmutter-51-0`, `gir1.2-mutter-51`, `mutter-common`, `mutter-common-bin`), in the
VibeOS apt archive (component `mutter`, with its source package) and in the system image (0.1.4.3 and later):
Debian's `mutter` 51.0-3 (`mutter_51.0-3.dsc`, sha256
`c8277b85d7cd80f11c9c47fe804573a248fb5e60e7dd888f37a31d5b9ad22759`) with these five after Debian's own series, in
`debian/patches/gnome-phone/`, and the two symbols 0004 exports added to `debian/libmutter-51-0.symbols`.
[`image/build-mutter.sh`](../../image/build-mutter.sh) rebuilds it:

    sudo image/build-mutter.sh OUTDIR       # Debian's mutter 51.0-3 + patches/mutter = 51.0-3+gp1

**mutter-mobile 51.1.mobile.22** in `/opt/gnome-mobile` (`lib/libmutter-51.so.0`, `lib/mutter-51/`), the
compositor of the phones' GNOME Shell mobile: the mobile shell project's
[mutter-mobile](https://gitlab.gnome.org/World/MobileShell/mutter-mobile) at tag 51.1.mobile.22 (commit
7e7b25f68c7719ebf3b651808824e0c8bd33020d). The system image's copy was built on a phone on 5-7 November 2026 with
0001, 0002, 0003 and 0005; 0004 was written later, for Debian's mutter. [`image/build-mobile.sh`](../../image/build-mobile.sh)
builds `/opt/gnome-mobile` from the tag with all five:

    sudo image/build-mobile.sh OUT.tar      # mutter-mobile + gnome-shell-mobile 51.1.mobile.22 + patches

## Licence

GPL-2.0-or-later, as mutter.

## How this set was checked

- 51.0-3+gp1: the archive's source package carries these five in `debian/patches/gnome-phone/`, byte-identical except
  0004, whose description says "vibeos" where the built copy said "gnome-phone" (the project's rename; the code is
  the same). Debian's 51.0-3 with that series is file for file the build's tree, and the system image's
  `libmutter-51.so.0.0.0` matches the .deb the build's `.changes` lists.
- mutter-mobile in `/opt/gnome-mobile`: the build tree no longer exists. The shipped libraries carry the strings
  0001 and 0002 add, and their build times follow the commits of 0001-0003 and 0005 in this repository.
