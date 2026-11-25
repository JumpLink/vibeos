# GNOME Shell mobile 51.1.mobile.22

Five patches to the mobile shell project's [gnome-shell-mobile](https://gitlab.gnome.org/World/MobileShell/gnome-shell-mobile)
at tag 51.1.mobile.22 (commit c602286053f9e06b3493decb4a9dabe8ba20ad9c), the GNOME Shell the phones run from
`/opt/gnome-mobile`.

| Patch | What it does |
| --- | --- |
| `0001-keyboard-stop-waiting-for-the-actor-of-a-window-that.patch` | keyboard: stop waiting for the actor of a window that is gone |
| `0002-blur-effect-blur-at-a-quarter-of-the-resolution-at-least.patch` | blur effect: blur at a quarter of the resolution at least, so a growing blur does not reallocate full-size buffers |
| `0003-keyboard-keep-a-window-moved-for-the-osk-above-it.patch` | keyboard: a window moved for the on-screen keyboard is kept above it while the keyboard is up |
| `0004-layout-let-an-extension-hold-the-stage-hidden-at-startup.patch` | layout: an extension can hold the stage hidden at startup (the splash stays until the lock screen) |
| `0005-screenShield-connect-the-power-save-handler-only-where-mutter-exports-the-mode.patch` | screenShield: the power-save handler is connected only where mutter exports the mode |

`series` lists them in order. [`image/build-mobile.sh`](../../image/build-mobile.sh) applies them (`0*.patch`, in
name order) with [`patches/mutter`](../mutter/) to mutter-mobile:

    sudo image/build-mobile.sh OUT.tar      # mutter-mobile + gnome-shell-mobile 51.1.mobile.22 + patches

## Shipped in

`/opt/gnome-mobile` in the VibeOS system image (0.1.4.3 and later): `lib/gnome-shell/libshell-51.so` (the shell's
JavaScript is a GResource inside it) is the tag with all five. The shell's other files there (`libst-51.so`,
`bin/gnome-shell`, ...) are built from the tag: none of the five changes their sources. The mutter-mobile shipped in the
same `/opt/gnome-mobile` was built with [`patches/mutter`](../mutter/) 0001-0003 and 0005; 0004 came later.

`lib/gnome-shell/libshell-51.so.before-osk-top` is a leftover of an earlier build that nothing loads; its source is the
tag with 0001 (with or without 0002).

## Licence

GPL-2.0-or-later, as GNOME Shell.

## How this set was checked

The tag with these five is file for file the tree the shipped `libshell-51.so` was built in, except one comment in
0003 that now says "vibeos" where the built copy had the project's former name. All 162 JavaScript
resources in the shipped library equal that tree's files; the shipped library is byte-identical to the build's
output.
