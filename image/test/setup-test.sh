#!/bin/sh
# setup-test.sh DEVICE [DEVDIR]: the first boot's vibeos-device-setup, run for real on a copy of the system image
# with DEVICE's payload.tar unpacked into it and its debs in /var/lib/vibeos/device/debs, as the device zip leaves
# them: in a chroot (qemu-user), offline, NOREBOOT=1. Checks that every deb installs, that nothing was removed or
# downloaded, the holds, the device's name, and that no initramfs was made (the POCO's modem dies with one built
# here). Works on a copy in work/test; the system image itself is only read.
# OLD=DIR: an upgrade, as when a device zip is sideloaded alone onto a phone already on VibeOS: DIR's device folder
# (an earlier build) is set up first, as its own first boot did it, then DEVDIR's payload and debs go in as its zip
# puts them, and the setup runs for real but for the restart (a systemctl wrapper records it): the new versions
# installed, dpkg's state clean (dpkg --audit), one restart, and the files only the old device package had gone.
# APT=DIR (with OLD): the upgrade from the archive instead, after OLD's first boot: DIR's packages (build-apt-device.sh:
# vibeos-device-DEVICE, vibeos-kernel-KVER, vibeos-kernel-DEVICE) installed by apt-get in one run, offline (apt
# orders them; the modules' new package must be unpacked before the device package that gives them up). Their
# versions installed, dpkg --audit clean, the modules and depmod's files byte for byte as before and now
# vibeos-kernel-KVER's, modinfo finding KEYMODS for KVER, every file of the old package still there but those that
# go (the S10e's /boot/s10e files stay byte for byte, unowned, no diversion left; the POCO's ESP hooks go,
# update_initramfs=no), no initramfs made, nothing removed. DEVDIR is not read.
# HANDKV=KVER (with APT, S10e): a second kernel installed by hand before the upgrade, as one is tried on a phone
# before its package exists: KVER's modules (a copy of the packaged ones) beside the packaged kernel's, and
# /boot/s10e rotated (Image to Image.prev, a stand-in new Image). The upgrade must leave them byte for byte, unowned,
# modinfo still finding KEYMODS for KVER: a later vibeos-kernel-KVER takes them over from there.
# INTERRUPT=1 (with APT): the upgrade killed where a phone could lose power, the kernel package set up and the
# device package not yet unpacked (apt-get's dpkg is a wrapper that kills apt-get there): the modules must all be
# there as before, the old device package still installed; then apt-get runs again and everything above holds.
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
F=0
bad() { echo "  FAIL: $*"; F=$((F + 1)); }
ok() { echo "  ok: $*"; }
# ---- APT: the archive's packages over OLD's first boot
apt_upgrade() {
	set -- "$APT"/*.deb
	[ -f "$1" ] || { bad "no packages in $APT"; return 1; }
	KV=$(ls "$M/usr/lib/modules")
	[ "$(echo "$KV" | wc -l)" = 1 ] || { bad "not one kernel in /usr/lib/modules: $KV"; return 1; }
	case $DEV in
	s10e) KM=${KEYMODS:-brcmfmac exynos9820-decon-kms panel-samsung-s6e3fa7-beyond0 btbcm} ;;
	s5e)  KM=${KEYMODS:-msm btqca qcom_q6v5_pas q6asm} ;;
	poco) KM=${KEYMODS:-ath10k_snoc btqca qcom_q6v5_pas} ;;	# msm is built in
	*)    KM=${KEYMODS:-} ;;
	esac
	OP=$(ls "$OLD"/debs/vibeos-device-*.deb)
	dpkg-deb -c "$OP" | awk '$1 !~ /^d/ {n = $6; sub(/^\./, "", n); print n}' | LC_ALL=C sort > "$W/old-files.txt"
	for f in "$@"; do dpkg-deb -c "$f" | awk '$1 !~ /^d/ {n = $6; sub(/^\./, "", n); print n}'; done | LC_ALL=C sort > "$W/new-files.txt"
	LC_ALL=C comm -23 "$W/old-files.txt" "$W/new-files.txt" > "$W/old-only.txt"
	sums() { (cd "$M" && find "$@" -type f -print0 2>/dev/null | LC_ALL=C sort -z | xargs -0 -r sha256sum); }
	if [ -n "${HANDKV:-}" ]; then
		[ "$DEV" = s10e ] && [ "$HANDKV" != "$KV" ] && [ ! -e "$M/usr/lib/modules/$HANDKV" ] || { bad "HANDKV=$HANDKV: S10e only, a new name"; return 1; }
		cp -a "$M/usr/lib/modules/$KV" "$M/usr/lib/modules/$HANDKV"
		cp -p "$M/boot/s10e/Image" "$M/boot/s10e/Image.prev"
		{ cat "$M/boot/s10e/Image"; echo "a stand-in for $HANDKV, installed by hand"; } > "$M/boot/s10e/Image.hand"
		mv "$M/boot/s10e/Image.hand" "$M/boot/s10e/Image"
		echo "== $DEV: $HANDKV installed by hand: its modules beside $KV's, /boot/s10e/Image rotated to Image.prev"
		sums "usr/lib/modules/$HANDKV" > "$W/handkv-before.txt"
	fi
	sums "usr/lib/modules/$KV" > "$W/modules-before.txt"
	sums boot > "$W/boot-before-apt.txt"
	(cd "$M" && while IFS= read -r x; do [ -e ".$x" ] || [ -L ".$x" ] || echo "$x"; done < "$W/old-files.txt") > "$W/missing-before.txt"
	dpkg-query --admindir="$M/var/lib/dpkg" -W -f '${db:Status-Abbrev} ${Package} ${Version}\n' > "$W/pkgs-before.txt"
	mkdir -p "$M/run/setup-test-apt"; cp "$@" "$M/run/setup-test-apt/"
	echo "== $DEV: the archive's packages over it: $(for f in "$@"; do echo "$(dpkg-deb -f "$f" Package) $(dpkg-deb -f "$f" Version)"; done | tr '\n' ' ')"
	for m in proc sys dev dev/pts; do mount --bind /$m "$M/$m"; done
	if [ -n "${INTERRUPT:-}" ]; then
		mkdir -p "$M/run/setup-test-bin"; rm -f "$M/run/setup-test-stopped"
		cat > "$M/run/setup-test-bin/dpkg-stop" <<EOF
#!/bin/sh
# setup-test INTERRUPT: apt-get killed where a phone could lose power, the device package's unpack next
case " \$* " in *" --unpack "*)
	for a; do case \$a in *vibeos-device-${DEV}_*.deb)
		case "\$(dpkg-query -W -f '\${db:Status-Abbrev}' vibeos-kernel-$KV 2>/dev/null)" in ii*)
			echo "\$(basename \$a)" > /run/setup-test-stopped; kill -9 \$PPID; exit 137 ;;
		esac ;;
	esac; done ;;
esac
exec /usr/bin/dpkg "\$@"
EOF
		chmod 755 "$M/run/setup-test-bin/dpkg-stop"
		chroot "$M" /usr/bin/env -i PATH=/usr/sbin:/usr/bin:/sbin:/bin DEBIAN_FRONTEND=noninteractive sh -c \
			'apt-get install -y --no-install-recommends -o Acquire::Languages=none -o Dir::Bin::dpkg=/run/setup-test-bin/dpkg-stop /run/setup-test-apt/*.deb' > "$W/apt-1.out" 2>&1 && RC1=0 || RC1=$?
		sed 's/^/  | /' "$W/apt-1.out" | tail -n 6
		[ -s "$M/run/setup-test-stopped" ] && [ $RC1 != 0 ] && ok "apt-get killed before unpacking $(cat "$M/run/setup-test-stopped") (exit $RC1)" ||
			bad "apt-get not stopped where meant (exit $RC1, $(cat "$M/run/setup-test-stopped" 2>/dev/null))"
		sums "usr/lib/modules/$KV" > "$W/modules-stopped.txt"
		cmp -s "$W/modules-before.txt" "$W/modules-stopped.txt" && ok "stopped there: all of $KV's modules there as before ($(wc -l < "$W/modules-stopped.txt") files)" ||
			bad "stopped there: $KV's modules changed: $(diff "$W/modules-before.txt" "$W/modules-stopped.txt" | grep -c '^[<>]') lines differ"
		n=0; for x in $KM; do chroot "$M" modinfo -k "$KV" -F filename "$x" > /dev/null 2>&1 || { bad "stopped there: modinfo -k $KV $x fails"; n=1; }; done
		[ $n = 0 ] && ok "stopped there: modinfo finds $KM"
		dpkg-query --admindir="$M/var/lib/dpkg" -W -f '${db:Status-Abbrev} ${Package} ${Version}\n' > "$W/pkgs-stopped.txt"
		awk -v p="vibeos-kernel-$KV" '$1 == "ii" && $2 == p {f = 1} END {exit !f}' "$W/pkgs-stopped.txt" &&
			ok "stopped there: vibeos-kernel-$KV set up" || bad "stopped there: vibeos-kernel-$KV $(grep " vibeos-kernel-$KV " "$W/pkgs-stopped.txt")"
		d0=$(grep " vibeos-device-$DEV " "$W/pkgs-before.txt" || true) d1=$(grep " vibeos-device-$DEV " "$W/pkgs-stopped.txt" || true)
		[ -n "$d0" ] && [ "$d0" = "$d1" ] && ok "stopped there: the old device package as it was ($d1)" || bad "stopped there: the device package '$d1', before '$d0'"
		echo "== $DEV: apt-get again, as after the restart"
		rm -f "$M/run/setup-test-stopped"
	fi
	chroot "$M" /usr/bin/env -i PATH=/usr/sbin:/usr/bin:/sbin:/bin DEBIAN_FRONTEND=noninteractive sh -c \
		'apt-get install -y --no-install-recommends -o Acquire::Languages=none /run/setup-test-apt/*.deb' > "$W/apt.out" 2>&1 && RC=0 || RC=$?
	[ -z "${INTERRUPT:-}" ] || { cat "$W/apt-1.out" "$W/apt.out" > "$W/apt-both.out"; }
	chroot "$M" dpkg --audit > "$W/audit.txt" 2>&1 || true
	for x in $KM; do chroot "$M" modinfo -k "$KV" -F filename "$x" 2>&1 | head -n 1; done > "$W/modinfo.txt"
	[ -z "${HANDKV:-}" ] || for x in $KM; do chroot "$M" modinfo -k "$HANDKV" -F filename "$x" 2>&1 | head -n 1; done > "$W/modinfo-hand.txt"
	chroot "$M" dpkg-divert --list > "$W/diversions.txt" 2>&1 || true
	for m in dev/pts dev sys proc; do umount "$M/$m"; done
	sed 's/^/  | /' "$W/apt.out" | tail -n 12
	[ $RC = 0 ] && ok "apt-get install of them all: exit 0" || bad "apt-get install: exit $RC"
	AO=$W/apt.out; [ -z "${INTERRUPT:-}" ] || AO=$W/apt-both.out
	UO=$(sed -n 's/^Unpacking \([^ ]*\) .*/\1/p' "$AO" | tr '\n' ' ')
	echo "  unpacked in this order: $UO"
	case " $UO" in *" vibeos-kernel-$KV "*" vibeos-device-$DEV "*) ok "the modules' package unpacked before the device package" ;;
	*) bad "the device package unpacked before vibeos-kernel-$KV: its modules were deleted and put back" ;; esac
	dpkg-query --admindir="$M/var/lib/dpkg" -W -f '${db:Status-Abbrev} ${Package} ${Version}\n' > "$W/pkgs-after.txt"
	for f in "$@"; do
		p=$(dpkg-deb -f "$f" Package) v=$(dpkg-deb -f "$f" Version)
		awk -v p="$p" -v v="$v" '$1 == "ii" && ($2 == p || $2 == p ":arm64") && $3 == v {f = 1} END {exit !f}' "$W/pkgs-after.txt" &&
			ok "$p $v installed" || bad "$p $v not installed: $(grep " $p " "$W/pkgs-after.txt")"
	done
	[ ! -s "$W/audit.txt" ] && ok "dpkg --audit: nothing half-installed or unconfigured" || bad "dpkg --audit: $(head -n 5 "$W/audit.txt" | tr '\n' ' ')"
	GONE=$(awk '{print $2}' "$W/pkgs-before.txt" | while read -r p; do grep -q " $p " "$W/pkgs-after.txt" || echo "$p"; done)
	[ -z "$GONE" ] && ok "no package removed" || bad "removed: $GONE"
	sums "usr/lib/modules/$KV" > "$W/modules-after.txt"
	# the modules as the kernel package has them, and depmod's files as it ran them (the package carries them too)
	rm -rf "$W/kx"; mkdir "$W/kx"; dpkg-deb -x "$APT/vibeos-kernel-${KV}_"*.deb "$W/kx"
	(cd "$W/kx" && find "usr/lib/modules/$KV" -type f -print0 | LC_ALL=C sort -z | xargs -0 -r sha256sum) > "$W/modules-deb.txt"
	rm -rf "$W/kx"
	cmp -s "$W/modules-deb.txt" "$W/modules-after.txt" &&
		ok "/usr/lib/modules/$KV = vibeos-kernel-$KV's files, depmod's output the same ($(wc -l < "$W/modules-after.txt") files)" ||
		bad "/usr/lib/modules/$KV is not the package's: $(diff "$W/modules-deb.txt" "$W/modules-after.txt" | grep '^[<>]' | head -n 4 | tr '\n' ' ')"
	cmp -s "$W/modules-before.txt" "$W/modules-after.txt" && echo "  (and byte for byte as before the upgrade)" ||
		echo "  (changed from before the upgrade: $(diff "$W/modules-before.txt" "$W/modules-after.txt" | grep -c '^>') files: an older build)"
	OW=$(chroot "$M" dpkg -S "/usr/lib/modules/$KV/modules.dep" 2>/dev/null | cut -d: -f1)
	[ "$OW" = "vibeos-kernel-$KV" ] && ok "its modules are vibeos-kernel-$KV's now" || bad "modules.dep belongs to '$OW'"
	n=0; for x in $KM; do
		fn=$(sed -n "$((n + 1))p" "$W/modinfo.txt"); n=$((n + 1))
		case $fn in /usr/lib/modules/$KV/*|/lib/modules/$KV/*) [ -f "$M$fn" ] && ok "modinfo -k $KV $x: $fn" || bad "modinfo $x: $fn is not there" ;;
		*) bad "modinfo -k $KV $x: $fn" ;; esac
	done
	[ -n "$KM" ] || bad "no key modules for $DEV"
	(cd "$M" && while IFS= read -r x; do [ -e ".$x" ] || [ -L ".$x" ] || echo "$x"; done < "$W/new-files.txt") > "$W/new-missing.txt"
	[ ! -s "$W/new-missing.txt" ] && ok "every file of the new packages is there ($(wc -l < "$W/new-files.txt"))" ||
		bad "files of the new packages missing: $(head -n 4 "$W/new-missing.txt" | tr '\n' ' ')"
	[ ! -s "$W/missing-before.txt" ] || echo "  (missing before the upgrade already: $(head -n 4 "$W/missing-before.txt" | tr '\n' ' '))"
	sums boot > "$W/boot-after-apt.txt"
	case $DEV in
	s10e)
		[ "$(sed -n 's|.*  boot/s10e/||p' "$W/boot-before-apt.txt" | LC_ALL=C sort | tr '\n' ' ')" = "Image Image.prev bootargs.env initramfs.cpio.gz " ] ||
			bad "before: /boot/s10e has $(sed -n 's|.*  boot/s10e/||p' "$W/boot-before-apt.txt" | tr '\n' ' ')"
		cmp -s "$W/boot-before-apt.txt" "$W/boot-after-apt.txt" && ok "/boot byte for byte as before: $(cut -c1-12 "$W/boot-after-apt.txt" | tr '\n' ' ')" ||
			bad "/boot changed: $(diff "$W/boot-before-apt.txt" "$W/boot-after-apt.txt" | grep '^[<>]' | tr '\n' ' ')"
		n=0; for f in Image Image.prev initramfs.cpio.gz bootargs.env; do
			o=$(chroot "$M" dpkg -S "/boot/s10e/$f" 2>/dev/null) && { bad "/boot/s10e/$f still belongs to $o"; n=1; }
		done
		[ $n = 0 ] && ok "no package owns /boot/s10e's four files (U-Boot's; no dpkg upgrade rewrites them)"
		grep -q "/boot/s10e" "$W/diversions.txt" && bad "a diversion left: $(grep /boot/s10e "$W/diversions.txt" | head -n 2 | tr '\n' ' ')" ||
			ok "no diversion of /boot/s10e left"
		ls "$M"/boot/s10e/*.vibeos-keep > /dev/null 2>&1 && bad "a .vibeos-keep file in /boot/s10e" || true
		EXP="/boot/s10e/Image /boot/s10e/Image.prev /boot/s10e/bootargs.env /boot/s10e/initramfs.cpio.gz" ;;
	poco)
		cmp -s "$W/boot-before-apt.txt" "$W/boot-after-apt.txt" && [ ! -s "$W/boot-after-apt.txt" ] && ok "/boot: nothing in it, as before" ||
			bad "/boot: $(cat "$W/boot-after-apt.txt" | tr '\n' ' ')"
		grep -q '^update_initramfs=no$' "$M/etc/initramfs-tools/update-initramfs.conf" && ! grep -q '^update_initramfs=yes' "$M/etc/initramfs-tools/update-initramfs.conf" &&
			ok "update-initramfs.conf: update_initramfs=no" || bad "update-initramfs.conf: $(grep '^update_initramfs' "$M/etc/initramfs-tools/update-initramfs.conf")"
		[ ! -e "$M/etc/kernel/postinst.d/zz-surya-esp" ] && [ ! -e "$M/etc/initramfs/post-update.d/zz-surya-esp" ] &&
			ok "the ESP hooks (zz-surya-esp) gone" || bad "an ESP hook is still there"
		grep -q "zz-qcom-bootimg" "$W/diversions.txt" && ok "qcom-phone-utils' boot image hook still diverted" || bad "zz-qcom-bootimg not diverted any more"
		EXP="/etc/initramfs/post-update.d/zz-surya-esp /etc/kernel/postinst.d/zz-surya-esp" ;;
	*)	cmp -s "$W/boot-before-apt.txt" "$W/boot-after-apt.txt" && ok "/boot as before" || bad "/boot changed"
		EXP= ;;
	esac
	if [ -n "${HANDKV:-}" ]; then
		sums "usr/lib/modules/$HANDKV" > "$W/handkv-after.txt"
		cmp -s "$W/handkv-before.txt" "$W/handkv-after.txt" && ok "$HANDKV (by hand): its modules byte for byte as before ($(wc -l < "$W/handkv-after.txt") files)" ||
			bad "$HANDKV's modules changed: $(diff "$W/handkv-before.txt" "$W/handkv-after.txt" | grep '^[<>]' | head -n 4 | tr '\n' ' ')"
		o=$(chroot "$M" dpkg -S "/usr/lib/modules/$HANDKV/modules.dep" 2>/dev/null) && bad "$HANDKV's modules.dep belongs to $o" ||
			ok "$HANDKV's modules: no package's (a later vibeos-kernel-$HANDKV takes them over)"
		n=0; for x in $KM; do
			fn=$(sed -n "$((n + 1))p" "$W/modinfo-hand.txt"); n=$((n + 1))
			case $fn in /usr/lib/modules/$HANDKV/*|/lib/modules/$HANDKV/*) [ -f "$M$fn" ] && ok "modinfo -k $HANDKV $x: there" || bad "modinfo $HANDKV $x: $fn is not there" ;;
			*) bad "modinfo -k $HANDKV $x: $fn" ;; esac
		done
		[ "$(awk '$2 == "boot/s10e/Image" {print $1}' "$W/boot-after-apt.txt")" != "$(awk '$2 == "boot/s10e/Image.prev" {print $1}' "$W/boot-after-apt.txt")" ] &&
			ok "/boot/s10e: the hand-installed Image and the packaged kernel as Image.prev, both kept" || bad "/boot/s10e: Image = Image.prev"
	fi
	[ "$(tr '\n' ' ' < "$W/old-only.txt" | sed 's/ $//')" = "$EXP" ] && ok "the old package's files: all carried over but ${EXP:-none}" ||
		bad "files of the old package in no new package: $(tr '\n' ' ' < "$W/old-only.txt")"
	ls "$M/var/lib/initramfs-tools" > /dev/null 2>&1 && bad "an initramfs-tools state appeared" || ok "no initramfs made"
	grep -q "update-initramfs: Generating" "$W/apt.out" && bad "update-initramfs generated an initramfs" || true
	[ -x "$M/usr/sbin/vibeos-kernel-install" ] && ok "vibeos-kernel-install there" || bad "no vibeos-kernel-install"
	grep -E -i "error|warning" "$W/apt.out" | head -n 10 | sed 's/^/  (apt.out) /'
	rm -rf "$M/run/setup-test-apt"
	echo "== $DEV: $F failures"
	[ $F = 0 ]
}
if [ -n "${OLD:-}" ]; then
	[ -s "$OLD/payload.tar" ] || { echo "no $OLD/payload.tar" >&2; exit 1; }
	echo "== $DEV: the earlier device folder first (vibeos-device-$DEV $(dpkg-deb -f "$(ls "$OLD"/debs/vibeos-device-*.deb)" Version))"
	tar -C "$M" -xf "$OLD/payload.tar"
	mkdir -p "$M/var/lib/vibeos/device/debs"
	cp "$OLD"/debs/*.deb "$M/var/lib/vibeos/device/debs/"
	for m in proc sys dev dev/pts; do mount --bind /$m "$M/$m"; done
	chroot "$M" /usr/bin/env -i PATH=/usr/sbin:/usr/bin:/sbin:/bin NOREBOOT=1 /usr/local/sbin/vibeos-device-setup > "$W/setup-old.out" 2>&1 || true
	for m in dev/pts dev sys proc; do umount "$M/$m"; done
	[ "$(cat "$M/var/lib/vibeos/device/result" 2>/dev/null)" = ok ] && ok "the earlier device package set up: $(dpkg-query --admindir="$M/var/lib/dpkg" -W -f '${Version}' vibeos-device-$DEV)" ||
		bad "the earlier setup: $(cat "$M/var/lib/vibeos/device/result" 2>/dev/null)"
	if [ -n "${APT:-}" ]; then apt_upgrade; exit $?; fi
	# the files only the old device package had (not directories, not conffiles: dpkg keeps those)
	dpkg-deb -c "$(ls "$OLD"/debs/vibeos-device-*.deb)" | awk '$1 !~ /^d/ {print $6}' | sed 's|^\.||' | sort > "$W/old-files.txt"
	dpkg-deb -c "$(ls "$D"/debs/vibeos-device-*.deb)" | awk '$1 !~ /^d/ {print $6}' | sed 's|^\.||' | sort > "$W/new-files.txt"
	dpkg-deb -I "$(ls "$OLD"/debs/vibeos-device-*.deb)" conffiles 2>/dev/null | sort > "$W/old-conffiles.txt"
	comm -23 "$W/old-files.txt" "$W/new-files.txt" | comm -23 - "$W/old-conffiles.txt" > "$W/old-only.txt"
	# as the device zip does it: its payload over the root, the debs replaced, no result
	rm -rf "$M/var/lib/vibeos/device/debs"; rm -f "$M/var/lib/vibeos/device/result"
fi
tar -C "$M" -xf "$D/payload.tar"
mkdir -p "$M/var/lib/vibeos/device/debs"
cp "$D"/debs/*.deb "$M/var/lib/vibeos/device/debs/"
dpkg-query --admindir="$M/var/lib/dpkg" -W -f '${db:Status-Abbrev} ${Package} ${Version}\n' > "$W/pkgs-before.txt"
ls "$M/boot" > "$W/boot-after-payload.txt"
for m in proc sys dev dev/pts; do mount --bind /$m "$M/$m"; done
if [ -n "${OLD:-}" ]; then
	# the restart, recorded instead of done; every other systemctl call goes through
	mkdir -p "$M/run/setup-test-bin"
	printf '#!/bin/sh\nif [ "$*" = "--no-block reboot" ]; then echo "$*" >> /run/setup-test-reboots; exit 0; fi\nexec /usr/bin/systemctl "$@"\n' > "$M/run/setup-test-bin/systemctl"
	chmod 755 "$M/run/setup-test-bin/systemctl"; rm -f "$M/run/setup-test-reboots"
	chroot "$M" /usr/bin/env -i PATH=/run/setup-test-bin:/usr/sbin:/usr/bin:/sbin:/bin /usr/local/sbin/vibeos-device-setup > "$W/setup.out" 2>&1 || true
	chroot "$M" dpkg --audit > "$W/audit.txt" 2>&1
else
	chroot "$M" /usr/bin/env -i PATH=/usr/sbin:/usr/bin:/sbin:/bin NOREBOOT=1 /usr/local/sbin/vibeos-device-setup > "$W/setup.out" 2>&1 || true
fi
for m in dev/pts dev sys proc; do umount "$M/$m"; done
echo "== $DEV: vibeos-device-setup$( [ -z "${OLD:-}" ] || echo ', the upgrade')"
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
if [ -n "${OLD:-}" ]; then
	[ ! -s "$W/audit.txt" ] && ok "dpkg --audit: nothing half-installed or unconfigured" || bad "dpkg --audit: $(head -n 5 "$W/audit.txt" | tr '\n' ' ')"
	[ "$(cat "$M/run/setup-test-reboots" 2>/dev/null | wc -l)" = 1 ] && ok "restarts once" || bad "restarts: $(cat "$M/run/setup-test-reboots" 2>/dev/null | wc -l)"
	n=0; while IFS= read -r x; do [ -e "$M$x" ] || [ -L "$M$x" ] && { bad "$x (only the old package had it) is still there"; n=$((n + 1)); }; done < "$W/old-only.txt"
	[ $n = 0 ] && ok "the old package's own files gone ($(wc -l < "$W/old-only.txt"): $(head -n 4 "$W/old-only.txt" | tr '\n' ' '))"
	rm -rf "$M/run/setup-test-bin" "$M/run/setup-test-reboots"
fi
grep -E -i "error|warning" "$W/setup.out" | grep -v -E "^$" | head -n 10 | sed 's/^/  (setup.out) /'
echo "== $DEV: $F failures"
[ $F = 0 ]
