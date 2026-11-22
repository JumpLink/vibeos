#!/bin/sh
# setup-test.sh DEVICE [DEVDIR]: the first boot's vibeos-device-setup, run for real on a copy of the system image
# with DEVICE's payload.tar unpacked into it and its debs in /var/lib/vibeos/device/debs, as the device zip leaves
# them: in a chroot (qemu-user), offline, NOREBOOT=1. Checks that every deb installs, that nothing was removed or
# downloaded, the holds, the device's name, and that no initramfs was made (the POCO's modem dies with one built
# here). Works on a copy in work/test; the system image itself is only read.
set -eu
DEV=${1:?s10e|poco|s5e|s20}
T=$(cd "$(dirname "$0")/.." && pwd)
[ ! -f "$T/local.conf" ] || . "$T/local.conf"
O=${VIBEOS_OUT:?set VIBEOS_OUT (image/local.conf): the directory builds, work files and logs go to}
D=${2:-$O/$DEV}
SYS=${SYS:-$O/system/rootfs.img}	# SYS: another image to try it on (an earlier stage of the system build)
W=$O/work/test/setup-$DEV${TAG:+-$TAG}
[ "$(id -u)" = 0 ] || { echo "run as root" >&2; exit 1; }
[ -s "$SYS" ] && [ -s "$D/payload.tar" ] || { echo "no $SYS or $D/payload.tar" >&2; exit 1; }
[ ! -e "$W" ] || { echo "$W exists: move it aside" >&2; exit 1; }
mkdir -p "$W/m"
M=$W/m
cp --sparse=always "$SYS" "$W/root.img"
truncate -s +3G "$W/root.img"
e2fsck -fy "$W/root.img" > /dev/null 2>&1 || [ $? -lt 4 ]
resize2fs "$W/root.img" > /dev/null 2>&1
mount -o loop "$W/root.img" "$M"
cleanup() { for m in dev/pts dev sys proc; do mountpoint -q "$M/$m" && umount "$M/$m"; done; mountpoint -q "$M" && umount "$M"; }
trap cleanup EXIT
ls "$M/boot" > "$W/boot-before.txt" 2>/dev/null || true
tar -C "$M" -xf "$D/payload.tar"
mkdir -p "$M/var/lib/vibeos/device/debs"
cp "$D"/debs/*.deb "$M/var/lib/vibeos/device/debs/"
dpkg-query --admindir="$M/var/lib/dpkg" -W -f '${db:Status-Abbrev} ${Package} ${Version}\n' > "$W/pkgs-before.txt"
ls "$M/boot" > "$W/boot-after-payload.txt"
for m in proc sys dev dev/pts; do mount --bind /$m "$M/$m"; done
chroot "$M" /usr/bin/env -i PATH=/usr/sbin:/usr/bin:/sbin:/bin NOREBOOT=1 /usr/local/sbin/vibeos-device-setup > "$W/setup.out" 2>&1 || true
for m in dev/pts dev sys proc; do umount "$M/$m"; done
F=0
bad() { echo "  FAIL: $*"; F=$((F + 1)); }
ok() { echo "  ok: $*"; }
echo "== $DEV: vibeos-device-setup"
sed 's/^/  | /' "$M/var/log/vibeos-device-setup.log" | tail -n 25
[ "$(cat "$M/var/lib/vibeos/device/result" 2>/dev/null)" = ok ] && ok "result: ok" || bad "result: $(cat "$M/var/lib/vibeos/device/result" 2>/dev/null)"
dpkg-query --admindir="$M/var/lib/dpkg" -W -f '${db:Status-Abbrev} ${Package} ${Version}\n' > "$W/pkgs-after.txt"
for f in "$D"/debs/*.deb; do
	p=$(dpkg-deb -f "$f" Package) v=$(dpkg-deb -f "$f" Version)
	# exact strings (a version's "+" is no regex)
	awk -v p="$p" -v v="$v" '($1 == "ii" || $1 == "hi") && ($2 == p || $2 == p ":arm64") && $3 == v {f = 1} END {exit !f}' "$W/pkgs-after.txt" || bad "$p $v not installed: $(grep " $p " "$W/pkgs-after.txt")"
done
ok "$(ls "$D"/debs/*.deb | wc -l) packages checked"
GONE=$(awk '{print $2}' "$W/pkgs-before.txt" | while read -r p; do grep -q " $p " "$W/pkgs-after.txt" || echo "$p"; done)
[ -z "$GONE" ] && ok "nothing removed" || bad "removed: $GONE"
grep -q "^hi" "$W/pkgs-after.txt" && ok "held: $(grep '^hi' "$W/pkgs-after.txt" | cut -d' ' -f3 | tr '\n' ' ')"
[ "$(cat "$M/etc/hostname")" != vibeos ] && ok "hostname $(cat "$M/etc/hostname")" || bad "hostname still vibeos"
ls "$M/var/lib/initramfs-tools" > /dev/null 2>&1 && bad "an initramfs-tools state appeared: $(ls "$M/var/lib/initramfs-tools")" || ok "no initramfs state"
ls "$M"/boot/initrd.img-* > /dev/null 2>&1 && bad "an initrd in /boot: update-initramfs would rebuild it on the device" || ok "no initrd in /boot (update-initramfs has nothing to rebuild)"
grep -q "update-initramfs: Generating" "$W/setup.out" && bad "update-initramfs generated an initramfs" || true
ls "$M/boot" > "$W/boot-after.txt"
cmp -s "$W/boot-after-payload.txt" "$W/boot-after.txt" && ok "/boot as the payload left it" || bad "/boot changed: $(diff "$W/boot-after-payload.txt" "$W/boot-after.txt" | tr '\n' ' ')"
[ -z "$(ls -A "$M/var/lib/vibeos/device/debs")" ] && ok "debs moved to installed/" || bad "debs left in debs/"
# the regulatory database these kernels accept: wireless-regdb's upstream-signed copy, chosen by hand
RL=$(readlink "$M/etc/alternatives/regulatory.db" || true)
[ "$RL" = /lib/firmware/regulatory.db-upstream ] && [ "$(head -n 1 "$M/var/lib/dpkg/alternatives/regulatory.db")" = manual ] &&
	ok "regulatory.db -> $RL (manual)" || bad "regulatory.db -> $RL ($(head -n 1 "$M/var/lib/dpkg/alternatives/regulatory.db"))"
grep -E -i "error|warning" "$W/setup.out" | grep -v -E "^$" | head -n 10 | sed 's/^/  (setup.out) /'
echo "== $DEV: $F failures"
[ $F = 0 ]
