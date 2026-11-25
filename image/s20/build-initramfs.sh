#!/bin/sh
# s20/build-initramfs.sh OUT: the Galaxy S20 FE's initramfs, as a tree for the kernel to build in
# (CONFIG_INITRAMFS_SOURCE="OUT/root OUT/extra.list", with INITRAMFS_ROOT_UID/GID 0 squashing the owner to root).
# The S20's S-Boot needs the initramfs inside the kernel: a boot image's ramdisk is the FIT U-Boot boots from.
#   OUT/root        /init (s20/init), busybox (static), dropbear, kexec, reboot2, e2fsck/resize2fs/dumpe2fs/mke2fs and
#                   every library they need (readelf NEEDED, recursively); root's passwd/shadow (no password: the
#                   shells start only with r8s.debug, see s20/init)
#   OUT/extra.list  the device nodes the kernel creates in it (console, null)
# The binaries come from Debian's arm64 packages in S20_INITRAMFS_DEBS (a directory of .debs: busybox-static,
# dropbear-bin, kexec-tools, e2fsprogs and their libraries), unpacked into OUT/debs-root. No SSH key unless DEV=1:
# then root/.ssh/authorized_keys = SSH_PUBKEY (a developer's own boot image, never a published one).
# REGDB_FROM=SYSTEM.img: wireless-regdb's upstream-signed regulatory.db and .p7s from that system image (read with
# debugfs, checked against the package's md5sums) as /lib/firmware/regulatory.db{,.p7s}: the built-in cfg80211 reads
# them before the root is there (without them Wi-Fi starts in the world domain).
# Paths in OUT are relative to nothing on this machine: point CONFIG_INITRAMFS_SOURCE at OUT relative to the kernel's
# build directory (the build's IKCONFIG embeds the line).
set -eu
T=$(cd "$(dirname "$0")/.." && pwd)
[ ! -f "$T/local.conf" ] || . "$T/local.conf"
OUT=${1:?OUT}
DEBS=${S20_INITRAMFS_DEBS:?set S20_INITRAMFS_DEBS: a directory of the arm64 .debs (busybox-static, dropbear-bin, kexec-tools, e2fsprogs, libraries)}
DEV=${DEV:-0}
case $DEV in 0) ;; 1) [ -s "${SSH_PUBKEY:-}" ] || { echo "DEV=1 needs SSH_PUBKEY=FILE" >&2; exit 1; } ;; *) echo "DEV is 0 or 1" >&2; exit 1 ;; esac
CC=${CROSS_COMPILE:-aarch64-linux-gnu-}
mkdir -p "$OUT"; OUT=$(cd "$OUT" && pwd)
D=$OUT/debs-root R=$OUT/root
rm -rf "${D:?}" "${R:?}"; mkdir -p "$D"
for p in busybox-static dropbear-bin kexec-tools e2fsprogs; do
	ls "$DEBS"/${p}_*_arm64.deb > /dev/null 2>&1 || { echo "no ${p}_*_arm64.deb in $DEBS" >&2; exit 1; }
done
for f in "$DEBS"/*_arm64.deb "$DEBS"/*_all.deb; do [ -f "$f" ] && dpkg-deb -x "$f" "$D"; done
(cd "$DEBS" && sha256sum *.deb) > "$OUT/debs.sha256"
mkdir -p "$R/bin" "$R/usr/sbin" "$R/usr/bin" "$R/usr/lib/aarch64-linux-gnu" "$R/lib" "$R/etc" "$R/dev"
cp "$D/usr/bin/busybox" "$R/bin/busybox"
file "$R/bin/busybox" | grep -q 'statically linked' || { echo "busybox is not the static one (busybox-static)" >&2; exit 1; }
install -m 755 "$T/s20/init" "$R/init"
cp "$D/usr/sbin/dropbear" "$D/usr/sbin/kexec" "$R/usr/sbin/"
${CC}gcc -static -Os -s -o "$R/usr/sbin/reboot2" "$T/s20/reboot2.c"
# r8s.root=vibeos checks the root file system and grows it to the whole partition before mounting it; mke2fs for
# formatting by hand in the rescue shell
for f in e2fsck resize2fs dumpe2fs mke2fs; do cp "$D/usr/sbin/$f" "$R/usr/sbin/"; done
[ ! -f "$D/etc/mke2fs.conf" ] || cp "$D/etc/mke2fs.conf" "$R/etc/"
need() { ${CC}readelf -d "$1" 2>/dev/null | sed -n 's/.*Shared library: \[\(.*\)\]/\1/p'; }
todo= done_=
for f in dropbear kexec e2fsck resize2fs dumpe2fs mke2fs; do todo="$todo $(need "$R/usr/sbin/$f")"; done
while [ -n "$(echo $todo)" ]; do
	set -- $todo; l=$1; shift; todo="$*"
	case " $done_ " in *" $l "*) continue ;; esac
	done_="$done_ $l"
	[ -e "$D/usr/lib/aarch64-linux-gnu/$l" ] || { echo "no $l in $DEBS's packages" >&2; exit 1; }
	cp -L "$D/usr/lib/aarch64-linux-gnu/$l" "$R/usr/lib/aarch64-linux-gnu/"
	todo="$todo $(need "$D/usr/lib/aarch64-linux-gnu/$l")"
done
echo "libraries:$done_"
ln -s ../usr/lib/aarch64-linux-gnu/ld-linux-aarch64.so.1 "$R/lib/ld-linux-aarch64.so.1"
ln -s ../usr/lib/aarch64-linux-gnu "$R/lib/aarch64-linux-gnu"
echo "root:x:0:0:root:/root:/bin/sh" > "$R/etc/passwd"
echo "root::0::::::" > "$R/etc/shadow"
echo "/bin/sh" > "$R/etc/shells"
ln -s /proc/mounts "$R/etc/mtab"
if [ "$DEV" = 1 ]; then
	mkdir -p "$R/root/.ssh"
	cp "$SSH_PUBKEY" "$R/root/.ssh/authorized_keys"
	chmod 700 "$R/root/.ssh"; chmod 600 "$R/root/.ssh/authorized_keys"
	echo "DEV=1: root/.ssh/authorized_keys from $SSH_PUBKEY (a developer's boot image: never publish it)"
fi
if [ -n "${REGDB_FROM:-}" ]; then
	DF=$(command -v debugfs || echo /sbin/debugfs)
	mkdir -p "$R/lib/firmware"
	"$DF" -R "cat /var/lib/dpkg/info/wireless-regdb.md5sums" "$REGDB_FROM" 2> /dev/null > "$OUT/regdb.md5"
	for f in regulatory.db regulatory.db.p7s; do
		"$DF" -R "dump /usr/lib/firmware/$f-upstream $R/lib/firmware/$f" "$REGDB_FROM" 2> /dev/null
		m=$(awk -v p="usr/lib/firmware/$f-upstream" '$2 == p {print $1}' "$OUT/regdb.md5")
		[ -n "$m" ] && [ "$(md5sum < "$R/lib/firmware/$f" | cut -d' ' -f1)" = "$m" ] || { echo "$f-upstream: not wireless-regdb's" >&2; exit 1; }
		chmod 644 "$R/lib/firmware/$f"
	done
	rm -f "$OUT/regdb.md5"
	echo "regulatory.db: wireless-regdb's upstream-signed copy, $(sha256sum < "$R/lib/firmware/regulatory.db" | cut -c1-16)"
fi
printf 'nod /dev/console 600 0 0 c 5 1\nnod /dev/null 666 0 0 c 1 3\n' > "$OUT/extra.list"
# what a published kernel must not carry: a key, or this machine's names and paths in a text file
if [ "$DEV" = 0 ]; then
	! find "$R" -name authorized_keys -o -name 'id_*' | grep . || { echo "an SSH key in the tree" >&2; exit 1; }
	for n in "$(id -un)" "$(hostname -s)"; do
		! grep -rIlw -e "$n" "$R" | grep . || { echo "the name $n in a text file of the tree" >&2; exit 1; }
	done
	! grep -rIl -e "/home/$(id -un)/" -e "/media/$(id -un)/" "$R" | grep . || { echo "a path of this machine in the tree" >&2; exit 1; }
fi
echo "init $(sha256sum < "$R/init" | cut -c1-16), $(du -sh "$R" | cut -f1)"
echo "CONFIG_INITRAMFS_SOURCE: \"$OUT/root $OUT/extra.list\" (relative to the kernel's build directory in a published build)"
