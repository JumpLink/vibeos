# U-Boot for the POCO X3 NFC (surya), VibeOS 0.1.4.3 and 0.1.5

The U-Boot the POCO X3 NFC starts Linux from (in the recovery partition: Xiaomi's ABL starts it, it boots the ESP of
a "linux" partition). Four patches to the [sm7150-mainline U-Boot](https://github.com/sm7150-mainline/u-boot)
("Tauchgang", branch `tauchgang`) at commit `f8c04469e828` ("HACK: Hardcode text offset for sunfish to boot").

| Patch | What it does |
| --- | --- |
| `0001-snapdragon-choose-the-kernel-s-devicetree-by-the-pan.patch` | mach-snapdragon: read the panel ABL names (`msm_drm.dsi_display0=`, Huaxing or Tianma) into `panel`, and tell the OS what was detected in `/chosen` |
| `0002-video-the-splash-black-out-ABL-s-logos-and-centre-th.patch` | video: the splash blacks out ABL's padlock, POCO logo and "Powered by android" and centres the logo |
| `0003-video-the-Debian-swirl-as-the-logo.patch` | video: the logo is Debian's swirl (200x200, 8 bit; binary patch) |
| `0004-qualcomm-quiet-boot-and-a-Linux-partition-for-the-PO.patch` | qualcomm: `qcom-quiet.config`, `qcom-recovery.config`, `qcom-phone-linuxpart.env` (the environment the build uses) and `qcom-phone-quiet.env` |

`series` lists them in order. `qcom-phone-quiet.env` is named by `qcom-quiet.config` but overridden by
`qcom-recovery.config`: it is in the tree the shipped binary was built from, and not part of its environment.

## What ships

`u-boot.bin`, sha256 `c0f5e87bd44e4ab9...` (1390672 bytes, "U-Boot Tauchgang 2026.01-rc3-gf8c04469e828-dirty"), gzip-compressed
as the kernel of an Android boot image (`boot.img`, sha256 `0bda775a9724f8de...` in the 0.1.4.3 device zip); ABL inflates it.

## Build

    git clone https://github.com/sm7150-mainline/u-boot && cd u-boot
    git checkout f8c04469e828 && git am PATCHES/u-boot-poco/*.patch
    make O=.output CROSS_COMPILE=aarch64-linux-gnu- qcom_defconfig qcom-phone.config qcom-quiet.config qcom-recovery.config
    make O=.output CROSS_COMPILE=aarch64-linux-gnu- -j"$(nproc)"      # .output/u-boot.bin

Packaging: `image/poco/mkrecovery.py` of the VibeOS repository puts `u-boot.bin` into LineageOS's recovery or boot
image (their header, devicetree and overlay stay LineageOS's byte for byte; none of it is in this folder).
To get the shipped bytes the build pins the version string (`.scmversion` holding `-gf8c04469e828-dirty`) and its time
(a `date` in `PATH` answering `date -d @1794200319` for `+FORMAT`, `TZ=Europe/Berlin`: Nov 09 2026 - 05:58:39 +0100).

## How this set was checked

- The shipped build's tree was the base plus four edited files and four new ones; all eight are in the patches. After
  `git am` the tree equals that tree file for file, except `video-uclass.c`, whose comment lost the project's name
  ("poco:" for the original's prefix); no code differs.
- The four fragments and `qcom_defconfig` give exactly the `.config` of that build.
- Rebuilt from a fresh clone of the base with the patches, nice 19: `u-boot.bin` is **byte for byte** the shipped one
  (sha256 `c0f5e87bd44e4ab9...`): it equals the shipped `boot.img`'s kernel once gunzipped. The boot image itself
  (`0bda775a9724f8de`) is LineageOS's header and devicetree around it and was not rebuilt.

## Licence

U-Boot is GPL-2.0-or-later; the patches are under the same licence. Debian's swirl logo is Debian's, under the
licences Debian gives it (the Debian Open Use Logo: LGPL-3.0-or-later or, at your option, CC BY-SA 3.0 Unported). Credits: the U-Boot project; the sm7150-mainline maintainers (the
Tauchgang tree, SM7150 devicetrees and Qualcomm phone support). The patches are by pulpoff.
