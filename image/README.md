# VibeOS installer images

The tools that make the installer images on [vibeos.top/releases](https://vibeos.top/releases): one **system zip**
for every phone, and a **device zip** for each phone. LineageOS's recovery installs them one after the other, in the
same session, with `adb sideload`.

| | |
|---|---|
| `build-base.sh` | the base image the system image starts from: Debian sid and Mobian staging with `mmdebstrap` (`base/`: the package list, the apt pin) |
| `build-chroot.sh`, `build-mutter.sh`, `build-mobile.sh` | an arm64 build chroot; mutter 51.0-3+gp1 in it (`MUTTER`); GNOME Shell mobile in `/opt/gnome-mobile` (`MOBILE`) |
| `build-system.sh` | the system image (root file system): Debian sid, GNOME Shell mobile, VibeOS's packages, the first-boot units |
| `build-zips.sh` | the zips: `system`, or a device (`s10e`, `s5e`, `poco`) from what `build-device.sh` made |
| `build-device.sh` | the device half of our four devices (S10e, POCO X3 NFC, Tab S5e, S20 FE): device package, payload, boot images |
| `nura/` | `nura-to-vibeos`: a device zip for any phone [Nura](https://nura.eco) (postmarketOS) has a build for |
| `zip/` | the installers that run in recovery: `system-install.sh`, `device-install.sh`, `vibeos-lib.sh` (shared), the updater-scripts |
| `rootfs/` | what every system image starts with: `vibeos-growroot`, `vibeos-device-setup`, the SSH host-key and snakeoil-certificate units |
| `kit/` | the home screen (`home-layout.json`, `90-rom-home.gschema.override`), the GNOME apps removed, the shell drop-in |
| `src/` | `s10e-part` (the zips' GPT tool, `vibeos-part` in the zip) and `s10e-logo` (the S10e's power-on logo): `make -C image/src` |
| `get-update-binary.sh` | LineageOS's updater out of a LineageOS build zip (not in this repository: it is LineageOS's) |
| `test/` | `zip-test.sh`: the zips run by LineageOS's real updater on fake disks; `setup-test.sh`: the first boot's device setup in a chroot; `compare-rootfs.sh`: two system images' packages, /etc and /opt side by side; `qemu-boot.sh`: a system image booted in qemu (virt) up to the session; `device-audit.sh`: what a device folder would publish of the build machine, kernels decompressed first |
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

`build-zips.sh` from this tree gives the system zip and the S10e zip on Releases byte for byte (the same sha256) from
their rootfs.img and device folders: the zip stores fixed timestamps and, with its own `umask 022`, the same modes.

### Building the system image from sources

The whole system image can be built from the Debian and Mobian archives, the mobile shell project's git tag and
this repository, on any PC with an arm64 qemu-user (or on an arm64 machine). Nothing of a phone goes in: the base is
made with `mmdebstrap`, mutter and GNOME Shell mobile are compiled in an arm64 build chroot.

```sh
O=$VIBEOS_OUT
sudo image/build-base.sh "$O/base.img"           # Debian sid + Mobian staging: GNOME, the phone base, the user
sudo image/build-chroot.sh "$O/work/build-chroot" # the arm64 build chroot: mutter's and gnome-shell's build-deps
sudo image/build-mutter.sh "$O/mutter"           # MUTTER: Debian's mutter 51.0-3 + patches/mutter = 51.0-3+gp1
sudo image/build-mobile.sh "$O/gnome-mobile.tar" # MOBILE: mutter-mobile + gnome-shell-mobile 51.1.mobile.22 + patches
# VDEBS: this repository's packages (dpkg-buildpackage -b -uc -us, see "Build from source" in the top README),
# SPEECH: tools/vibeos-dictate-speech/build-debs.sh "$O/speech"
mkdir -p "$O/system" && cp --sparse=always "$O/base.img" "$O/system/rootfs.img"
sudo env MUTTER="$O/mutter" MOBILE="$O/gnome-mobile.tar" image/build-system.sh "$O/system/rootfs.img"
image/build-zips.sh system                       # -> $O/system/vibeos-system-0.1.3-arm64.zip
```

`build-mutter.sh` and `build-mobile.sh` can run at the same time (`JOBS=4` each); both use the one build chroot,
each with its own mounts. Times on an 8-core PC under qemu-user, with the two builds side by side: base 1 h, build
chroot 20 min, mutter 3 h (it compiles mutter's tests too, but does not run them), GNOME Shell mobile 2 h,
`build-system.sh` 20 min, `build-zips.sh system` 10 min. Disk: about 25 GB in `$O`.

Needs, on a Debian 13 host: `mmdebstrap`, `qemu-user-static` (or `qemu-user` with its binfmt), `debian-archive-keyring`,
`gpg`, `git`, `patch`, `e2fsprogs`, `python3`, root and the network. Settings (`local.conf.example`):
`SNAPSHOT` builds Debian as snapshot.debian.org had it at that time (Mobian has no snapshots: its four packages come
as they are today); `DEBIAN_MIRROR`, `MOBIAN_MIRROR`; `BASE_TZ` and `BASE_LANG` (the release's: Europe/Berlin,
en_GB.UTF-8); `JOBS`; `BUILD_CHROOT`.

- **The package set** (`base/packages`): the 0.1.3 base's own choice (`apt-mark showmanual` of the S10e clean
  image, less what Debian's required and important priorities bring anyway, less the S10e's device packages): GNOME
  core, Mobian's phone base, ModemManager, libcamera, iio-sensor-proxy, feedbackd, and the typelibs GNOME Shell
  mobile and VibeOS's apps load. `base/overlay/etc/apt/preferences.d/no-phosh` keeps Phosh, phoc, squeekboard and
  Debian's mobile shell packages out, as in the release.
- **Set by `build-base.sh`**: the user `user` (uid 1000, password 147147, groups sudo audio video render input plugdev
  netdev bluetooth dialout feedbackd), GDM's automatic login, host name vibeos, `LABEL=vibeos` in fstab, the time
  zone and language, graphical.target, SSH off and no host keys, an empty machine-id, `/etc/vibeos/base`. It checks
  that apt and dpkg are consistent and that nothing of the build machine (its name, its DNS search domain and name
  servers, the build paths) is in the image.
- **Not as in the 0.1.3 image**: Debian's own ModemManager, libcamera and WebKitGTK. The release holds builds of
  ours (`+surya3`, `+surya20`, `+neo1`) that came with the S10e base, and whose changes are not in this repository;
  `HOLD_DEBS=DIR` installs and holds such builds instead. The boot splash is Debian's default (a device package
  sets its own). And the versions: Debian sid moves, so a build today is newer than 0.1.3's of 11-17.
- **What `build-system.sh` removes from either base**: systemd-resolved's copy of the build machine's resolv.conf,
  ssl-cert's snakeoil key and certificate (every device makes its own on the first boot, `vibeos-ssl-cert`), and the
  build machine's name in exim4's settings.

How close it comes, checked on 2026-11-22 with `test/compare-rootfs.sh` against the 0.1.3 rootfs.img:

- the base has the 0.1.3 base's package set: the same 160 manually installed packages; the rest differs by sid's
  newer versions and four library transitions
- the system image's `/etc` differs in the three items above, the boot splash, the S10e builds held in 0.1.3 and
  what sid changed
- mutter 51.0-3+gp1 has the release's file lists, dependencies and exported symbols, and all but its six ELF files
  are the release's byte for byte; two builds of it gave the same debs byte for byte
- `/opt/gnome-mobile` has the release's file list, `DT_NEEDED` and exported symbols, and GNOME Shell's JavaScript is
  the same but for one comment

The zips pass `test/zip-test.sh` (S10e new partition, S5e image file), every device package installs on the image
(`test/setup-test.sh`), and `test/qemu-boot.sh` boots it on qemu's virt machine up to GDM's automatic login into the
vibeos session (GNOME Shell mobile from `/opt`, the home screen drawing). The image without `ART` passes the same
tests. From the S10e clean r7 base, `build-system.sh` still makes the 0.1.3 image: the same packages and `/opt`, and
an `/etc` that differs only in the three items above.

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

`test/zip-test-poco.sh SYSZIP DEVZIP CASE` does the same for the POCO X3 NFC, as root, in the POCO's own LineageOS
recovery (`OUT/work/test/rpoco`, made the same way from LineageOS surya's `recovery.img`), on two LUNs with the
POCO's partition layout: `stock` (userdata becomes `linux`), `existing` (an earlier install's `linux` is reused),
`again` (stock, then both zips once more), `rc3` (the GPT change stopped by a failed BLKPG, then a restart) and
`abort` (existing, but part 1 of the root cannot be written, then a restart). It checks that no key is read, the
tables and the GPT inside `linux`, that only `linux`, `boot`, `dtbo` and `vbmeta` change, and the root and the boot
pieces byte for byte. One case at a time.

It also models the SELinux of a real LineageOS recovery, which the chroot does not have. There the recovery domain
is permissive but the kernel's is enforcing, so a loop device fails every read and write: its worker thread may not
use the backing file that recovery's `losetup` opened. The zips therefore make SELinux permissive while their loop
devices exist (`loops_on` and `loops_off` in `zip/vibeos-lib.sh`). In the harness, a file stands for
`/sys/fs/selinux/enforce`, and its `losetup` refuses to set up a loop device while that file says enforcing. After
each zip, SELinux must be enforcing again.

`test/setup-test.sh DEVICE` runs the first boot's `vibeos-device-setup` on a copy of the system image with the
device's payload in it, offline in a chroot, and checks that every package installs and nothing is removed or
downloaded.

`test/compare-rootfs.sh NEW.img REF.img OUTDIR` mounts two system images read-only and writes what differs to
OUTDIR: the packages and their versions, the manually installed ones, `/etc` and `/opt/gnome-mobile` file by file
(mode, owner, link target, sha256), and the files no package owns. A one-line summary of each goes to stdout.

`test/qemu-boot.sh SYSTEM.img OUTDIR` boots a copy of a system image with Debian's arm64 kernel on qemu's virt
machine (TCG, virtio-gpu, Mesa's software renderer) for `BOOT_WAIT` seconds (default 1200), and counts in its
journal what a first boot must show: the first-boot units, GDM, the vibeos session's GNOME Shell from `/opt`, the
home screen drawing, and no crash. It needs `qemu-system-aarch64` and the network (for the kernel).

`test/device-audit.sh DEVDIR...` lists what a device folder (`payload.tar`, every package in `debs/`, `boot/`)
would publish of the machine that built it: a resolv.conf backup or snakeoil key, the build user's or host's name in
a text file, a `/home/<user>/` or `/media/<user>/` path, an SSH key file (`authorized_keys`, a host key, a private
key) or a private key in any file, and every kernel's banner. `test/kernel-names.py` finds the kernels first,
decompressed wherever they sit: in an Android boot image or a recovery partition (gzip, xz or zstd at any offset),
in an EFI zboot image on the ESP, in a FIT image, and an initramfs inside a kernel; in every stream it opens it
looks for the same paths, `user@host` and key files in any cpio archive (an initramfs, an embedded config). A
kernel's banner names the user and host it was built as unless `KBUILD_BUILD_USER` and `KBUILD_BUILD_HOST` were
set. The names come from the machine it runs on (`AUDIT_NAMES` adds more). Binary files and compressed modules that
hold a name as a word are listed to look at, since a short host name can be part of a module alias, and so are
empty key files. It exits 1 on a leak, or when it finds no kernel.

`build-base.sh`, `build-system.sh`, `build-mutter.sh`, `build-mobile.sh`, `compare-rootfs.sh` and `qemu-boot.sh`
mount in a mount namespace of their own. A service that starts in the meantime (polkit, started for PackageKit's
refresh, say) copies the host's mounts into its own namespace, and an image mounted there stays mounted, with its
loop device busy, until that service exits. `zip-test.sh` and `setup-test.sh` mount on the host: a run that fails
on a busy device ("cannot mount Android's data") may have met such a service, and passes when run again.

## Before a release's images

The images are built from packages that have booted on a phone, and only then. A phone, not only the tablet: some
of the shell runs only on a phone. In 0.1.3 the lock screen read a name whose import had been lost, so the extension
failed to enable on every phone and the session fell back to GDM's login screen, while the tablet, which never runs
that line, was fine.

1. The packages' own tests pass (`dpkg-buildpackage` runs them), among them the extension's `undefined-names`,
   which reports any name a module uses without declaring or importing it. It needs TypeScript's checker
   (`node-typescript` in the build chroot) and is skipped without it: the build log must show
   `undefined-names ... OK`.
2. The packages go onto a phone (`dpkg -i`), and the phone restarts. Then, on the phone, as the user, in the
   session (`XDG_RUNTIME_DIR=/run/user/$(id -u)`):

   ```sh
   gnome-extensions info shell@vibeos                     # State: ACTIVE
   journalctl --user -b | grep 'Extension shell@vibeos:'  # nothing
   loginctl show-seat seat0 -p ActiveSession              # the user's session, not the greeter's:
   loginctl show-session "$(loginctl show-seat seat0 -p ActiveSession --value)" -p Class -p Name   # Class=user
   ```

3. Only then `build-system.sh`, the device folders, the zips and their tests.
