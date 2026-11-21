# VibeOS installer images

The tools that make the installer images on [vibeos.top/releases](https://vibeos.top/releases): one **system zip**
for every phone, and a **device zip** for each phone. LineageOS's recovery installs them one after the other, in the
same session, with `adb sideload`.

| | |
|---|---|
| `build-system.sh` | the system image (root file system): Debian sid, GNOME Shell mobile, VibeOS's packages, the first-boot units |
| `build-zips.sh` | the zips: `system`, or a device (`s10e`, `s5e`, `poco`) from what `build-device.sh` made |
| `build-device.sh` | the device half of our four devices (S10e, POCO X3 NFC, Tab S5e, S20 FE): device package, payload, boot images |
| `nura/` | `nura-to-vibeos`: a device zip for any phone [Nura](https://nura.eco) (postmarketOS) has a build for |
| `zip/` | the installers that run in recovery: `system-install.sh`, `device-install.sh`, `vibeos-lib.sh` (shared), the updater-scripts |
| `rootfs/` | what every system image starts with: `vibeos-growroot`, `vibeos-device-setup`, the SSH host-key unit |
| `kit/` | the home screen (`home-layout.json`, `90-rom-home.gschema.override`), the GNOME apps removed, the shell drop-in |
| `src/` | `s10e-part` (the zips' GPT tool, `vibeos-part` in the zip) and `s10e-logo` (the S10e's power-on logo): `make -C image/src` |
| `get-update-binary.sh` | LineageOS's updater out of a LineageOS build zip (not in this repository: it is LineageOS's) |
| `test/` | `zip-test.sh`: the zips run by LineageOS's real updater on fake disks; `setup-test.sh`: the first boot's device setup in a chroot |
| `local.conf.example` | the settings every script reads from `image/local.conf` (git ignores it) |

## How an installation works

1. **The system zip** (`zip/system-install.sh`) asks where VibeOS goes, with the volume keys and the power key:
   a partition named `linux` from an earlier install (only its system is replaced), a new partition cut from Android's
   data (10–50 GB, erases Android's data), the whole data partition (renamed `linux`, a second press to confirm), or
   an image file in Android's data (10–100 GB, Android and its data stay). It offers only what the phone's device zip
   can boot. The root file system is in the zip in 1 GiB parts, because LineageOS's updater cannot extract an entry of
   4 GiB or more; each part is checked against `vibeos-sums.txt` after it is written. It leaves `/tmp/vibeos.prop`
   (where it wrote the system) for the device zip, and says to install the device zip next.
2. **The device zip** (`zip/device-install.sh`, or `nura/zip/nura-install.sh` for a converted one) finds that root,
   unpacks its payload into it (the kernel's modules, all the firmware, the device's settings, its Debian packages
   for the first boot), and writes the boot image where the phone starts it.
3. **The first start**: `vibeos-growroot` grows the file system to its partition or image file;
   `vibeos-device-setup` installs the device's packages offline, before GDM, and restarts once. Login: `user`,
   password `147147`.

What the installers never write: `efs`, `sec_efs`, `cpefs`, `radio`, `modem`, `persist`, `modemst1`, `modemst2`,
`fsg`, `fsc` (`NEVER` in `zip/vibeos-lib.sh`), nor any partition but the root's, the boot partitions the device zip
names and, for a new partition, userdata's table entry. They print no IMEI, serial number, MAC or Bluetooth address.

## Setting up

```sh
cp image/local.conf.example image/local.conf      # then fill it in
image/get-update-binary.sh lineage-22.2-…-signed.zip
make -C image/src s10e-part                         # gcc-aarch64-linux-gnu, libc6-dev-arm64-cross
```

The 0.1.3 zips carry the `update-binary` of `lineage-22.2-20261102-nightly-surya-signed.zip` (sha256
`3748b295…`). Another LineageOS 22 build's should do the same, but only that one is tested: `build-zips.sh` says
when it is another, and `STRICT=1` refuses it. Built on Debian 13 with its `gcc-aarch64-linux-gnu` (14.2),
`src/s10e-part` comes out byte for byte the 0.1.3 zips' binary.

## The system zip

```sh
sudo image/build-system.sh WORK.img                 # WORK.img: a copy of the base image
cp WORK.img "$VIBEOS_OUT/system/rootfs.img"
image/build-zips.sh system                          # -> $VIBEOS_OUT/system/vibeos-system-0.1.3-arm64.zip
```

`build-system.sh` installs VibeOS's packages (`VDEBS`: `dpkg-buildpackage` of this repository), mutter with
`patches/mutter` (`MUTTER`), the speech packages (`SPEECH`: `tools/vibeos-dictate-speech/build-debs.sh`) and GNOME
Shell mobile with `patches/gnome-shell` (`MOBILE`) in an arm64 chroot (qemu-user), removes GNOME Settings, Showtime
and the GNOME apps in `kit/ourapps-remove`, sets the home screen from `kit/`, and checks, labels (`vibeos`) and
shrinks the image. It refuses any development version, a relaxed version pin on a VibeOS library, and any package or
file named in `FORBID`. The third-party icon packs and parallax sets the 0.1.3 zip carries (`ART`) are not in this
repository; left unset, the image has VibeOS's own icons and the NASA solar-system wallpaper only.

**Not yet from sources alone.** The 0.1.3 image was made from a copy of the clean image of our own S10e:
`build-system.sh` removes that phone's device layer (`STAGE`: every file its device stage added) before it installs
VibeOS. A base builder that starts from the Debian and Mobian archives is the next step; until it is here, the
system zip on [Releases](https://vibeos.top/releases) is the one to use, and a new phone only needs a device zip.

## Device zips

**A phone Nura has a build for**, whichever flavour (Phosh, GNOME Mobile, Plasma Mobile): `nura/nura-to-vibeos`, see
[nura/README.md](nura/README.md).

**Our four devices**: `sudo image/build-device.sh s10e|poco|s5e|s20`, then `image/build-zips.sh s10e|poco|s5e`.
Its inputs are device stages of our own phones (a kernel build and its modules, firmware read from the phone's own
vendor and modem partitions, U-Boot builds, the device's units and settings), named in `local.conf.example`. They are
not in this repository, so the script is a record of how those zips are put together rather than something to run
elsewhere. The S20 FE has no device zip: it installs from a PC over its rescue shell (`s20-install-from-pc.sh`).

## Tests

`test/zip-test.sh SYSZIP DEVZIP PRE SYSKEYS -- DEVICE` runs the system zip and then a device zip in one chroot of
LineageOS recovery's ramdisk (mksh and toybox under qemu-user), each with its own update-binary and updater-script as
recovery runs them, on a loop device with 4096-byte sectors holding an S10e-shaped GPT. It checks every partition
the zips may write and that the others are untouched, the root's file system and its sums, the boot pieces byte for
byte, and that nothing identifying was printed. It needs `OUT/work/test/sda-real.img` (a sparse copy of an S10e's
`sda` with only its GPT) and `OUT/work/test/r` (`nura/test/recovery-chroot.py RECOVERY.img OUT/work/test/r` from a
LineageOS recovery image), qemu-user-static with binfmt, and `sudo -n`.

`test/setup-test.sh DEVICE` runs the first boot's `vibeos-device-setup` on a copy of the system image with the
device's payload in it, offline in a chroot, and checks that every package installs and nothing is removed or
downloaded.
