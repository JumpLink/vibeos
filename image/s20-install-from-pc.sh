#!/bin/sh
# s20-install-from-pc.sh SYSTEM_ROOTFS_IMG S20_DIR --erase-the-vibeos-partition [root@172.16.43.1]
# VibeOS 0.1.3 on the Galaxy S20 FE (r8s), from the PC, over the USB network of the S20's rescue shell. The S20 has
# no LineageOS recovery any more (its RECOVERY holds Lineage 24 itself), so the zips do not apply: this does what
# they do, by ssh.
#
# Before: the S20 in U-Boot's "Rescue shell (USB)" (hold Volume Down at the logo, then pick it), the cable in, and
# `ssh root@172.16.43.1` working. Wait 2 minutes after it comes up: the rescue guard keeps every UFS node read-only
# for its first 120 s.
#
# It ERASES the partition named "vibeos" (the system and every user's files in it) and writes:
#   1. the system image (rootfs.img of the VibeOS system zip) to the partition named vibeos, read back
#   2. e2fsck, the file system grown by 1 GiB (the initramfs grows it to the partition on the next boot)
#   3. S20_DIR/payload.tar into it (kernel #51's modules, Wi-Fi/BT firmware, the r8s settings), S20_DIR/debs into
#      /var/lib/vibeos/device/debs (the first boot installs them, then restarts once), this PC's ssh key as root's
#      (the S20's only way in: OpenSSH, root by key)
#   4. BOOT is NOT written: it already holds ub13 (U-Boot + kernel #51, 4a82c5e3, since 11-20 19:52), which is
#      checked by reading it back; BOOT is only ever written from VibeOS (G780F/kit/flash-boot-linux.sh)
# Never touches efs, sec_efs, cpefs, radio or any other partition; sets the vibeos partition read-only again.
set -eu
IMG=${1:?rootfs.img} D=${2:?S20 device dir} OK=${3:-} H=${4:-root@172.16.43.1}
[ "$OK" = --erase-the-vibeos-partition ] || { echo "this erases the S20's vibeos partition: say --erase-the-vibeos-partition" >&2; exit 1; }
KEY=${SSH_PUBKEY:-$HOME/.ssh/id_ed25519.pub}
[ -s "$IMG" ] && [ -s "$D/payload.tar" ] && [ -s "$KEY" ] || { echo "missing $IMG, $D/payload.tar or $KEY" >&2; exit 1; }
O="-o UserKnownHostsFile=/dev/null -o StrictHostKeyChecking=no -o LogLevel=ERROR -o ConnectTimeout=8"
S() { ssh $O "$H" "$@"; }
# the rescue shell, not VibeOS: no /newroot mounted, the init's busybox
S 'grep -q "r8s.root" /proc/cmdline && [ -d /newroot/etc ] && exit 1; [ -e /bin/busybox ]' || { echo "$H is not the S20's rescue shell" >&2; exit 1; }
P=$(S 'for u in /sys/class/block/sd*/uevent; do grep -qx PARTNAME=vibeos $u && sed -n "s/^DEVNAME=//p" $u; done')
B=$(S 'for u in /sys/class/block/sd*/uevent; do grep -qx PARTNAME=boot $u && sed -n "s/^DEVNAME=//p" $u; done')
case $P in sd[a-z][0-9]*) ;; *) echo "no single partition named vibeos ($P)" >&2; exit 1 ;; esac
echo "vibeos is /dev/$P ($(S "echo \$((\$(cat /sys/class/block/$P/size) / 2097152))") GiB); BOOT is /dev/$B"
# BOOT: ub13, read back, not written
UB=$(stat -c %s "$(dirname "$0")/../s20/boot/boot-r8s-ub13.img" 2>/dev/null || echo 61865984)
[ "$(S "head -c $UB /dev/$B | sha256sum" | cut -c1-16)" = 4a82c5e3c5db7ab4 ] && echo "BOOT = ub13 (4a82c5e3): kept" ||
	echo "WARNING: BOOT is not ub13; not changed here (flash it from VibeOS with flash-boot-linux.sh, if at all)"
# room for the system and the 1 GiB it grows by below, before anything is written
PB=$(S "echo \$((\$(cat /sys/class/block/$P/size) * 512))")
[ "$PB" -ge $(($(stat -c %s "$IMG") + 1073741824)) ] 2>/dev/null ||
	{ echo "the vibeos partition ($PB bytes) cannot hold the system ($(stat -c %s "$IMG") bytes) and 1 GiB more" >&2; exit 1; }
S "grep -q '^/dev/$P ' /proc/mounts && umount /dev/$P; blockdev --setrw /dev/$P"
echo "writing the system ($(($(stat -c %s "$IMG") / 1048576)) MiB)"
dd if="$IMG" bs=4M status=progress | S "dd of=/dev/$P bs=4M && sync"
N=$(stat -c %s "$IMG")
[ "$(S "head -c $N /dev/$P | sha256sum" | cut -d' ' -f1)" = "$(sha256sum < "$IMG" | cut -d' ' -f1)" ] || { echo "read-back differs" >&2; exit 1; }
echo "read back: ok"
S "e2fsck -fy /dev/$P > /tmp/e2fsck.txt 2>&1; [ \$? -lt 4 ] && bc=\$(dumpe2fs -h /dev/$P 2>/dev/null | sed -n 's/^Block count: *//p') && resize2fs /dev/$P \$((bc + 262144)) > /tmp/resize.txt 2>&1"
S "mkdir -p /mnt/v && mount -t ext4 /dev/$P /mnt/v && [ -s /mnt/v/etc/vibeos/system-image ]"
echo "device files"
S "tar -C /mnt/v -xf -" < "$D/payload.tar"
S "rm -rf /mnt/v/var/lib/vibeos/device/debs && mkdir -p /mnt/v/var/lib/vibeos/device/debs"
for f in "$D"/debs/*.deb; do S "cat > /mnt/v/var/lib/vibeos/device/debs/$(basename "$f")" < "$f"; done
S "mkdir -p /mnt/v/root/.ssh && chmod 700 /mnt/v/root/.ssh && cat > /mnt/v/root/.ssh/authorized_keys && chmod 600 /mnt/v/root/.ssh/authorized_keys" < "$KEY"
S "printf 'kind=part\nname=vibeos\nmode=existing\nsize=\nimage=\ndevice=r8s\n' > /mnt/v/etc/vibeos/installed-to; echo device=s20 > /mnt/v/etc/vibeos/device; sync; umount /mnt/v && e2fsck -fn /dev/$P > /dev/null 2>&1; echo e2fsck \$?; blockdev --setro /dev/$P"
echo "done: on the S20, 'reboot2' starts U-Boot and VibeOS (r8s.root=vibeos); its first boot installs the device packages and restarts once"
