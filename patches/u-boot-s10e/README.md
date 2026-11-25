# U-Boot for the Galaxy S10e (beyond0lte), VibeOS 0.1.4.3 and 0.1.5

The U-Boot the S10e starts Linux from (it sits in the recovery partition, and in the boot partition for a
whole-phone install). Seven patches to [chiffathefox/u-boot](https://github.com/chiffathefox/u-boot), branch
`exynos9820`, at commit `dec4927b4ad5` ("exynos9820: add a readme"), which is the Exynos 9820 / 7870 U-Boot port
for Samsung phones.

| Patch | What it does |
| --- | --- |
| `0001-exynos-mobile-Samsung-Galaxy-S10e-beyond0lte-Exynos-.patch` | exynos-mobile: a board entry for the S10e (model G970) |
| `0002-dt-bindings-clock-exynos9820-the-peric1-clock-ids.patch` | dt-bindings: the `CLK_*_PERIC1_*` clock ids the devicetree uses |
| `0003-dts-exynos9820-describe-more-of-the-SoC.patch` | dts: exynos9820.dtsi: watchdog, USI/I2C/SPI/UART, DisplayPort, PCIe, GPU, audio, camera, MFC and DECON blocks |
| `0004-dts-exynos9820-common-Bixby-key-opens-Recents-frameb.patch` | dts: the Bixby key reports KEY_APPSELECT; labels on the framebuffer and modem regions and an `iommu-addresses` entry on the framebuffer; a passive GPU trip in the TMU |
| `0005-dts-exynos9820-beyond0lte-add-the-Galaxy-S10e-device.patch` | dts: `exynos9820-beyond0lte.dts`, the S10e's devicetree (U-Boot's own, and the kernel's) |
| `0006-video-a-32x64-console-font.patch` | video: `CONFIG_VIDEO_FONT_32X64`, Terminus 16x32 with every pixel doubled |
| `0007-exynos9820-quiet-boot-of-Linux-from-the-linux-partit.patch` | the environment (quiet boot, Linux from the `linux` partition with booti, the Volume Down menu) and `exynos9820_defconfig` (beyond0lte in `CONFIG_OF_LIST`, no logo, no framebuffer clear) |

`series` lists them in order.

## What ships

`u-boot.bin`, sha256 `d74392cdce194e2e...` (1038600 bytes; "U-Boot 2026.01-gdec4927b4ad5-dirty"), the kernel of an
Android boot image: `uboot.img` in the device zip (4 MiB) is that image, and the device packages of 0.1.4.3 and
0.1.5 write those 4 MiB to the recovery partition (and to boot for a whole-phone install).

## Build

    git clone https://github.com/chiffathefox/u-boot -b exynos9820 && cd u-boot
    git checkout dec4927b4ad5 && git am PATCHES/u-boot-s10e/*.patch
    make CC=aarch64-linux-gnu-gcc CROSS_COMPILE=aarch64-linux-gnu- exynos9820_defconfig
    make CC=aarch64-linux-gnu-gcc CROSS_COMPILE=aarch64-linux-gnu- -j"$(nproc)"     # u-boot.bin

(the dependencies are those of the base's own README). The boot image is an Android boot image whose kernel is
`u-boot.bin`; the shipped one reuses the header, the recovery devicetree overlay and the ramdisk slot of
LineageOS's recovery image for the phone (not part of this change, and not in this folder). With `mkbootimg`:
`mkbootimg --kernel u-boot.bin -o boot.img`, as the base's README does.

To get the shipped bytes the build pins three things, as the release build does: `.scmversion` holding
`-gdec4927b4ad5-dirty` (the checkout is a commit here, "dirty" there), a `date` in `PATH` that answers
`date -d @1794191327` for the `+FORMAT` call (with `TZ=Europe/Berlin`: Nov 09 2026 - 03:28:47 +0100), and the time
stamp of the FIT that holds the devicetrees (`fit-dtb.blob`), which `mkimage` takes from `SOURCE_DATE_EPOCH`
(`1794191329`) when it is set. Without them the file differs in the version string and in those four bytes only.

## How this set was checked

- The shipped build's tree was the base plus nine edited files and two new ones (`exynos9820-beyond0lte.dts`,
  `include/video_font_ter32x64.h`), all eleven are in the patches. After `git am` the tree equals that tree file for
  file (11 of 11 equal).
- `exynos9820_defconfig` gives exactly the `.config` of that build.
- Rebuilt from a fresh clone of the base with the patches, nice 19: `u-boot-nodtb.bin` is **byte for byte** the build's;
  `u-boot.bin` is byte for byte the shipped one (sha256
  `d74392cdce194e2ee3ac5426134da47694e4f8d1c9542516a6de853829b5f56c`) once the FIT time stamp is pinned as above, and
  it is exactly the kernel of the shipped boot image.
- The public `u-boot-s10e.patch` of linux-beyond0lte (`mobian/uboot/`) does not carry these exact files (its
  dt-bindings header and three devicetree files differ); this set is the tree the shipped binary was built from.

## Licence

U-Boot is GPL-2.0-or-later as a whole; each patch is under the licence of the files it changes, as their SPDX lines
say: `exynos-mobile.c` GPL-2.0, the devicetree files GPL-2.0 OR BSD-3-Clause (`exynos9820-tmu.dtsi` GPL-2.0), the
clock dt-bindings header GPL-2.0-only OR BSD-2-Clause, and the rest GPL-2.0-or-later. The 32x64 font is U-Boot's `video_font_ter16x32.h` (a Terminus font
copied from Linux, GPL-2.0) with every pixel doubled. The devicetree sources keep their
`GPL-2.0 OR BSD-3-Clause` headers. Credits: the U-Boot project; chiffathefox, whose Exynos 9820 port this is
built on; the authors of the Linux Exynos 9820 devicetree work the files descend from. The patches are by pulpoff.
