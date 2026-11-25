# U-Boot for the Galaxy S20 FE (r8s, Exynos 990), VibeOS 0.1.4.3 and 0.1.5

The U-Boot in the S20 FE's boot image: S-Boot starts it in the boot image's kernel slot, through a small shim
(`shim/`), and it boots Linux from a FIT inside the image. Five patches to
[chiffathefox/u-boot](https://github.com/chiffathefox/u-boot), branch `exynos9820`, at commit `dec4927b4ad5`
("exynos9820: add a readme"), the Samsung Exynos U-Boot port the S10e's U-Boot is built on too. They are the five
commits of the branch `r8s-ub13` (commit `124997411d1f`) with their authorship.

| Patch | What it does |
| --- | --- |
| `0001-exynos-mobile-Samsung-Galaxy-S20-FE-r8s-Exynos-990.patch` | the S20 FE: Exynos 990 pin controller banks, the board entry (DRAM banks, DECON0 refresh), a console ring appended to the debug log, `exynos990_defconfig`, the environment and its menu |
| `0002-exynos990-r8s-no-autoboot-countdown-menu-only-on-Vol.patch` | no autoboot countdown; the menu opens on Volume Down only |
| `0003-exynos990-r8s-a-crash-comes-back-to-U-Boot-watchdog-.patch` | a crash comes back to U-Boot, watchdog-guarded |
| `0004-exynos990-r8s-ub13-the-boot-image-s-U-Boot-since-202.patch` | "ub13": the U-Boot of the boot image since 2026-11-20 |
| `0005-exynos990-r8s-the-rescue-entry-passes-r8s.debug.patch` | the "Rescue shell (USB)" entry passes `r8s.debug` |

`series` lists them in order. `shim/` holds `r8s-ubshim.S` and `build-ubshim.sh`, which are in no U-Boot tree:
the code S-Boot jumps to, 4 KiB in front of U-Boot. Before any U-Boot code runs it starts the display refresh,
keeps S-Boot from choosing charging mode on the next reset, counts boots in a PMU register that survives resets
(the third pass in a row without Linux getting far enough gives up into recovery), sets the reset reason so a crash
comes back here, arms the watchdog, and jumps to U-Boot with S-Boot's x0-x3.

## What ships

In the boot image's kernel slot (`boot.img` of the S20 device zip), two files:

| File | sha256 | Bytes |
| --- | --- | --- |
| `ubshim-ub13d.bin`: the shim with U-Boot after it (the whole kernel slot) | `46d5484da82615bc...` | 632008 |
| `u-boot-r8s-ub13d.bin`: U-Boot ("U-Boot 2026.01-g124997411d1f"), at +0x1000 in the shim file | `477d81e302113777...` | 627912 |

Both are in 0.1.4.3 (read back from its `boot.img`) and recorded as the 0.1.5 inputs.

## Build

    git clone https://github.com/chiffathefox/u-boot -b exynos9820 && cd u-boot
    git checkout dec4927b4ad5 && git am PATCHES/u-boot-s20/*.patch
    make O=build CC=aarch64-linux-gnu-gcc CROSS_COMPILE=aarch64-linux-gnu- exynos990_defconfig
    make O=build CC=aarch64-linux-gnu-gcc CROSS_COMPILE=aarch64-linux-gnu- -j"$(nproc)"       # build/u-boot.bin
    PATCHES/u-boot-s20/shim/build-ubshim.sh build/u-boot.bin ubshim.bin                        # the shim + U-Boot

The shim needs `aarch64-linux-gnu-gcc`, `-ld` and `-objcopy`; its source is read from beside the script. The rest of
the boot image (S-Boot's header, device-tree table and the FIT that carries the kernel) is not in this folder: the
header and table are the stock image's, which is not ours to publish, and the FIT is described by
`image/s20/make-boot.sh` of the VibeOS repository.
To get the shipped bytes the release build pins the version string (`.scmversion` holding `-g124997411d1f`), its
time (a `date` in `PATH` answering `date -d @1795532197` for the `+FORMAT` call, `TZ=UTC`: Nov 24 2026 - 14:56:37
+0000) and, for the FIT of devicetrees U-Boot carries (`fit-dtb.blob`), the same time as `SOURCE_DATE_EPOCH`.

## How this set was checked

- The shipped U-Boot is the branch `r8s-ub13` at `124997411d1f`, clean. After `git am` of these five patches on the
  base, the tree is identical to that commit's tree (same git tree object, `18abe9ebc2b2...`).
- `exynos990_defconfig` gives exactly the `.config` of the shipped build.
- Rebuilt from a fresh clone of the base with the patches, nice 19: `u-boot.bin` is **byte for byte**
  `u-boot-r8s-ub13d.bin` (sha256 `477d81e3021137778f701ff8883c03b2b004e343b561680e28927f3dd6fe97a2`); the
  U-Boot proper (`u-boot-nodtb.bin`) came out identical from the plain build, and the FIT of devicetrees only after
  its time stamp was pinned as above (it is the only other part that records the time).
- `shim/build-ubshim.sh` on that `u-boot.bin` gives `ubshim.bin` byte for byte (sha256
  `46d5484da82615bcde7b8e08b8b7a2bbee210fd775ec8b2aa02135013e8577fc`), and the kernel slot of the 0.1.4.3 `boot.img`
  is that file. For 0.1.5 the same two hashes are what the release record names; its `boot.img` was not read back here.

## Licence

U-Boot is GPL-2.0-or-later as a whole; each patch is under the licence of the files it changes, as their SPDX lines
say: `exynos-mobile.c`, the board Makefile and `exynos990-r8s-u-boot.dtsi` GPL-2.0, the rest GPL-2.0-or-later. The
shim is GPL-2.0-or-later (its SPDX lines, which change no byte of `ubshim.bin`). Credits: the U-Boot project;
chiffathefox, whose Exynos 9820/990 U-Boot port this is built on. The patches and the shim are by pulpoff.
