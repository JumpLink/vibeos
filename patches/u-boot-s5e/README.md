# U-Boot for the Galaxy Tab S5e (gts4lvwifi), VibeOS 0.1.4.3 and 0.1.5

The U-Boot in the tablet's recovery partition, the one that starts Linux (and, from its menu, LineageOS or its
recovery). Five patches to the [sm7150-mainline U-Boot](https://github.com/sm7150-mainline/u-boot) (the
"Tauchgang" tree, branch `tauchgang`) at commit `f8c04469e828` ("HACK: Hardcode text offset for sunfish to boot").

| Patch | What it does |
| --- | --- |
| `0001-button-qcom-pmic-keep-the-function-of-any-other-key-.patch` | button-qcom-pmic: a key with any other code keeps its own code, so the power key selects (KEY_ENTER) in the menu |
| `0002-video-simplefb-take-the-framebuffer-from-a-memory-re.patch` | simplefb: take the framebuffer from a `memory-region` (reserved-memory) node when the node has no `reg` |
| `0003-video-logo-placement-for-the-Galaxy-Tab-S5e.patch` | video: `CONFIG_VIDEO_LOGO_GTS4LV` clears the screen and draws the logo where the tablet's Plymouth theme has it |
| `0004-video-the-Debian-swirl-as-the-logo.patch` | video: the logo is Debian's swirl (242x300, 8 bit; binary patch) |
| `0005-qualcomm-the-Galaxy-Tab-S5e-gts4lvwifi-configuration.patch` | qualcomm: `gts4lv.config` (Kconfig fragment) and `gts4lv.env` (the default environment and boot menu) |

`series` lists them in order.

## What ships

`u-boot-nodtb.bin`, sha256 `3376fbf29db4b674...`, at the start of the recovery partition of the tablet's boot image
(`recovery.img` of the device zip, gzip-compressed as the Android kernel of that image, with a devicetree appended).
The same binary is in 0.1.4.3 and 0.1.5.

## Build

    git clone https://github.com/sm7150-mainline/u-boot && cd u-boot
    git checkout f8c04469e828 && git am PATCHES/u-boot-s5e/*.patch
    make O=build CROSS_COMPILE=aarch64-linux-gnu- qcom_defconfig qcom-phone.config gts4lv.config
    make O=build CROSS_COMPILE=aarch64-linux-gnu- -j"$(nproc)"      # build/u-boot-nodtb.bin

The result is `build/u-boot-nodtb.bin`. The shipped file also carries the version string and the build time
(`U-Boot Tauchgang 2026.01-rc3-gf8c04469e828-dirty (Nov 11 2026 - 03:33:47 +0100)`); to get the same bytes, pin them
as the release build does: put `-gf8c04469e828-dirty` in `.scmversion` in the source tree and run `make` with a
`date` in `PATH` that answers `date -d @1794364427` for the `+FORMAT` call, and `TZ=Europe/Berlin`. The environment
in the patch (`board/qualcomm/gts4lv.env`) is the one `image/s5e/uboot.env` of the VibeOS repository holds.
Needs: gcc-aarch64-linux-gnu, bison, flex, swig, libssl-dev, libgnutls28-dev, python3-dev, python3-setuptools,
python3-pyelftools, uuid-dev, bc, device-tree-compiler.

## How this set was checked

- The shipped binary was built from a working tree of sm7150-mainline/u-boot at `f8c04469e828` with five edited
  files (`button-qcom-pmic.c`, `video/Kconfig`, `simplefb.c`, `u_boot_logo.bmp`, `video-uclass.c`) and
  `board/qualcomm/gts4lv.config`, with the environment file the release build script (`image/s5e/build-uboot.sh`) copies over
  `gts4lv.env` (`image/s5e/uboot.env`, sha256 `9ef01e42e606e55f...`). The tree's own `gts4lv.env` and its
  `gts4lv-linux-only.env` (a Linux-only variant) are not what the shipped build used and are left out. Everything the
  build used is in the patches (the logo is a binary patch); the tree after `git am` equals that tree file for file,
  except `video-uclass.c`, whose comment here says "the Debian Plymouth theme" where the original named the VibeOS
  theme (a comment only, no code differs).
- The configuration `qcom_defconfig` + `qcom-phone.config` + `gts4lv.config` is identical to the `.config` of that
  build.
- Rebuilt from a fresh clone of the base with the patches applied, the recipe above (nice 19): `u-boot-nodtb.bin` is
  **byte for byte** the shipped one (1311008 bytes, sha256 `3376fbf29db4b674626070c0de7334bce64817dc4676fc3c7660ed2a7a99e240`),
  which is also what the shipped `recovery.img` holds once its kernel is gunzipped.

## Not in this folder

The 104517-byte devicetree appended after U-Boot in the shipped image (sha256 `f0d9397e0f74d03d...`, model "Samsung
Galaxy Tab S5e (Wi-Fi)", `samsung,gts4lvwifi`) is the tablet's own devicetree from the kernel tree
[linux-gts4lv](https://github.com/pulpoff/linux-gts4lv) (`sdm670-samsung-gts4lvwifi`); the exact source state it was
compiled from has not been matched here, so that one file is **not** proven. The wrapper around U-Boot (LineageOS's
recovery image header, its recovery and Linux images) is not part of this change.

## Licence

U-Boot is GPL-2.0-or-later; these patches are under the same licence. Debian's swirl logo is Debian's, under the
licences Debian gives it (the Debian Open Use Logo: LGPL-3.0-or-later or, at your option, CC BY-SA 3.0 Unported). Credits: the U-Boot project and its contributors; the
sm7150-mainline maintainers (the Tauchgang tree and its Qualcomm phone support). The patches are by pulpoff.
