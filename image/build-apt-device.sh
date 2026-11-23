#!/bin/sh
# build-apt-device.sh DEVICE DEVDIR OUTDIR: a device folder's vibeos-device-DEVICE package (build-device.sh, as the
# device zips install it at the first boot) split for the apt archive, version APT_VERSION (default 1:0.1.4):
#   vibeos-device-DEVICE    firmware, units, settings, vibeos-kernel-install; no kernel, no modules
#   vibeos-kernel-KVER      the modules of the kernel the device zip installed (/usr/lib/modules/KVER), one
#                           package per uname -r, so that a later kernel's modules never replace them
#   vibeos-kernel-DEVICE    depends on the current vibeos-kernel-KVER
# s10e, s5e and poco only: the S20's kernel goes only through kexec-tested BOOT writes, never through apt.
#
# An upgrade from the zip's package (0.1.3, 0.1.3+1; no epoch, so older than any 1:) moves the modules to
# vibeos-kernel-KVER, which Replaces vibeos-device-DEVICE << 1:0.1.4~: unpacked first, it takes them over, and the
# device package's upgrade then has none of them to delete. vibeos-device-DEVICE Pre-Depends on vibeos-kernel-DEVICE
# for that order: with Breaks instead, apt unpacked the device package first, which deleted the modules until the
# kernel package put them back (no modules on a phone stopped in between). Nothing here writes where a bootloader reads a kernel; the kernel itself stays the device zip's:
#   S10e  /boot/s10e/{Image,Image.prev,initramfs.cpio.gz,bootargs.env} were files of the zip's package. A file
#         that is not in a package's new version is deleted when it is upgraded, so the new preinst diverts each
#         one (local, not renamed) for the length of the upgrade: dpkg deletes the diverted name, which does not
#         exist, and the file stays byte for byte; the postinst removes the diversions. After it no package owns
#         them: U-Boot's two menu entries (Image, Image.prev) are not dpkg's to rewrite.
#   POCO  the kernel, its initrd and DTBs are on the ESP inside "linux", not in /boot. The ESP hooks
#         (/etc/kernel/postinst.d and /etc/initramfs/post-update.d zz-surya-esp, which would copy an initrd built
#         on the phone to the ESP: the modem dies with one) are left out, so the upgrade removes them, and the
#         postinst sets update_initramfs=no in initramfs-tools' update-initramfs.conf (update-initramfs -u, which
#         the firmware packages' triggers run, then does nothing).
#   S5e   the kernel is in the recovery partition's slot (34 MiB), written only by the device zip.
set -eu
DEV=${1:?s10e|s5e|poco} SRC=${2:?DEVDIR} OUTD=${3:?OUTDIR}
case $DEV in s10e|s5e|poco) ;; *) echo "DEVICE: s10e, s5e or poco (the S20 never goes through apt)" >&2; exit 2 ;; esac
V=${APT_VERSION:-1:0.1.4}
case $V in 1:*) ;; *) echo "APT_VERSION $V: the archive's versions have epoch 1, which the zips' (0.1.3+1) are below" >&2; exit 2 ;; esac
VF=${V#1:}
set -- "$SRC"/debs/vibeos-device-${DEV}_*.deb
[ $# = 1 ] && [ -f "$1" ] || { echo "$SRC/debs: not one vibeos-device-${DEV}_*.deb" >&2; exit 1; }
SD=$1
OV=$(dpkg-deb -f "$SD" Version)
dpkg --compare-versions "$OV" lt "$V" || { echo "$SD is $OV, not below $V" >&2; exit 1; }
[ ! -e "$OUTD" ] || { echo "$OUTD exists: move it aside first" >&2; exit 1; }
mkdir -p "$OUTD"
W=$OUTD/work; mkdir "$W"
dpkg-deb -R "$SD" "$W/dev"
D=$W/dev
set -- "$D"/usr/lib/modules/*
[ $# = 1 ] && [ -d "$1" ] || { echo "$SD: not one kernel in /usr/lib/modules" >&2; exit 1; }
KV=${1##*/}
case $KV in *[!a-z0-9.+-]*) echo "kernel $KV: no package name" >&2; exit 1 ;; esac
KP=vibeos-kernel-$KV
echo "== $DEV: $(basename "$SD") ($OV) -> $V: vibeos-device-$DEV, $KP, vibeos-kernel-$DEV"
endscript() { sed '$d' "$1" > "$1.new" && [ "$(tail -n 1 "$1")" = "exit 0" ] && mv "$1.new" "$1" || { echo "$1 does not end with exit 0" >&2; exit 1; }; }
md5() { (cd "$1" && find . -path ./DEBIAN -prune -o -type f -print | sed 's|^\./||' | LC_ALL=C sort | xargs -r -d '\n' md5sum) > "$1/DEBIAN/md5sums"; }
build() {	# DIR NAME
	md5 "$1"
	for s in preinst postinst prerm postrm; do [ ! -f "$1/DEBIAN/$s" ] || { chmod 755 "$1/DEBIAN/$s"; sh -n "$1/DEBIAN/$s"; }; done
	dpkg-deb --root-owner-group -Zxz -b "$1" "$OUTD/${2}_${VF}_arm64.deb" > /dev/null
	echo "  $(basename "$OUTD/${2}_${VF}_arm64.deb"): $(du -h "$OUTD/${2}_${VF}_arm64.deb" | cut -f1)"
}

# ---- vibeos-kernel-KVER: the modules
K=$W/kernel; mkdir -p "$K/DEBIAN" "$K/usr/lib/modules"
mv "$D/usr/lib/modules/$KV" "$K/usr/lib/modules/"
rmdir "$D/usr/lib/modules"
cat > "$K/DEBIAN/control" <<EOF
Package: $KP
Version: $V
Architecture: arm64
Maintainer: pulpoff <pashkovsky@gmail.com>
Section: kernel
Priority: optional
Replaces: vibeos-device-$DEV (<< 1:0.1.4~)
Description: VibeOS kernel $KV: its modules
 The modules of kernel $KV, which VibeOS's device zip for this device installs.
 The kernel itself is where the device's boot loader reads it, put there by the
 device zip.
EOF
cat > "$K/DEBIAN/postinst" <<EOF
#!/bin/sh
set -e
if [ "\$1" = configure ]; then
	if [ -x /usr/sbin/vibeos-kernel-install ]; then vibeos-kernel-install $KV; else depmod -a $KV; fi
fi
exit 0
EOF
cat > "$K/DEBIAN/prerm" <<EOF
#!/bin/sh
set -e
# the running kernel's modules stay
if [ "\$1" = remove ] && [ "\$(uname -r)" = $KV ] && [ -z "\${VIBEOS_REMOVE_RUNNING_KERNEL:-}" ]; then
	echo "$KP: kernel $KV is running; its modules stay (VIBEOS_REMOVE_RUNNING_KERNEL=1 to remove them anyway)" >&2
	exit 1
fi
exit 0
EOF
build "$K" "$KP"

# ---- vibeos-kernel-DEVICE: the current kernel
M=$W/meta; mkdir -p "$M/DEBIAN"
cat > "$M/DEBIAN/control" <<EOF
Package: vibeos-kernel-$DEV
Version: $V
Architecture: arm64
Maintainer: pulpoff <pashkovsky@gmail.com>
Section: metapackages
Priority: optional
Depends: $KP (>= $V)
Description: VibeOS kernel for this device (metapackage)
 Depends on the package of the kernel VibeOS currently ships for this device.
EOF
build "$M" "vibeos-kernel-$DEV"

# ---- vibeos-device-DEVICE: the rest
case $DEV in
s10e)	BOOTF="Image Image.prev initramfs.cpio.gz bootargs.env"
	for f in $BOOTF; do [ -f "$D/boot/s10e/$f" ] || { echo "$SD: no /boot/s10e/$f" >&2; exit 1; }; rm "$D/boot/s10e/$f"; done
	[ -z "$(ls -A "$D/boot/s10e")" ] || { echo "$SD: more in /boot/s10e than U-Boot's four files" >&2; exit 1; } ;;	# the directory stays the package's
poco)	for f in etc/kernel/postinst.d/zz-surya-esp etc/initramfs/post-update.d/zz-surya-esp; do
		[ -f "$D/$f" ] || { echo "$SD: no /$f" >&2; exit 1; }; rm "$D/$f"
	done
	rmdir "$D/etc/kernel/postinst.d" "$D/etc/kernel" "$D/etc/initramfs/post-update.d" "$D/etc/initramfs" 2> /dev/null || true ;;
esac
[ ! -e "$D/boot" ] || [ "$DEV" = s10e ] || { echo "$SD: a /boot" >&2; exit 1; }
install -d "$D/usr/sbin"
cat > "$D/usr/sbin/vibeos-kernel-install" <<EOF
#!/bin/sh
# vibeos-kernel-install KVER: run by vibeos-kernel-KVER's postinst. Registers the kernel's modules (depmod).
# It writes no boot location: on the $DEV the kernel the boot loader starts is the device zip's.
set -e
KV=\${1:?usage: vibeos-kernel-install KVER}
[ -d /usr/lib/modules/\$KV ] || { echo "vibeos-kernel-install: no /usr/lib/modules/\$KV" >&2; exit 1; }
depmod -a "\$KV"
EOF
chmod 755 "$D/usr/sbin/vibeos-kernel-install"
CTL=$D/DEBIAN/control
# (>= 1:0.1.4, the version the modules moved in, whatever V is: a later device package must not pre-depend on the
# kernel package of its own upgrade, which may depend on it)
python3 -I - "$CTL" "$V" "vibeos-kernel-$DEV (>= 1:0.1.4)" "$KP" <<'PYEOF'
import sys
p, v, dep, kp = sys.argv[1:]
out = []
for line in open(p).read().splitlines():
    if line.startswith('Version: '):
        line = 'Version: ' + v
    assert not line.startswith('Pre-Depends: '), line
    out.append(line)
# the kernel package unpacked and set up first (it takes the modules over; see the top)
out.insert([i for i, l in enumerate(out) if l.startswith('Depends: ') or l.startswith('Description: ')][0], 'Pre-Depends: ' + dep)
out = [l for l in out if not (l.startswith(' The device zip unpacks') or l.startswith(' the first boot registers'))]
out.append(' The kernel\'s modules are in ' + kp + '.')
open(p, 'w').write('\n'.join(out) + '\n')
PYEOF
grep -q "^Version: $V\$" "$CTL" && grep -q "^Pre-Depends: vibeos-kernel-$DEV (>= 1:0.1.4)\$" "$CTL" || { echo "control not rewritten" >&2; exit 1; }
# the zip's package ran depmod for its kernel; that is the kernel package's job now
grep -q "^	depmod -a $KV || true\$" "$D/DEBIAN/postinst" || { echo "postinst: no depmod line" >&2; exit 1; }
sed -i "/^	depmod -a $KV || true\$/d" "$D/DEBIAN/postinst"
for s in preinst postinst postrm; do [ -f "$D/DEBIAN/$s" ] || printf '#!/bin/sh\nset -e\nexit 0\n' > "$D/DEBIAN/$s"; endscript "$D/DEBIAN/$s"; done
if [ $DEV = s10e ]; then
	{
		echo '# U-Boot'"'"'s files, in the zip'"'"'s package until 1:0.1.4: diverted (local, not renamed) while it is upgraded,'
		echo '# so that dpkg deletes a name that does not exist instead of them'
		echo 'if [ "$1" = upgrade ] && dpkg --compare-versions "$2" lt 1:0.1.4~; then'
		echo "	for f in $BOOTF; do"
		echo '		dpkg-divert --local --no-rename --add --divert /boot/s10e/$f.vibeos-keep /boot/s10e/$f > /dev/null'
		echo '	done'
		echo 'fi'
	} >> "$D/DEBIAN/preinst"
	BLOCK=$(cat <<EOF
for f in $BOOTF; do
	if [ "\$(dpkg-divert --truename /boot/s10e/\$f)" = /boot/s10e/\$f.vibeos-keep ]; then
		dpkg-divert --local --no-rename --remove --divert /boot/s10e/\$f.vibeos-keep /boot/s10e/\$f > /dev/null
	fi
done
EOF
)
	{ echo 'if [ "$1" = configure ]; then'; echo "$BLOCK" | sed 's/^/\t/'; echo 'fi'; } >> "$D/DEBIAN/postinst"
	{ echo 'if [ "$1" = abort-upgrade ] || [ "$1" = abort-install ]; then'; echo "$BLOCK" | sed 's/^/\t/'; echo 'fi'; } >> "$D/DEBIAN/postrm"
fi
if [ $DEV = poco ]; then
	cat >> "$D/DEBIAN/postinst" <<'EOF'
# never an initramfs built on the phone (the modem dies with one): update-initramfs -u, which the firmware packages'
# triggers run, does nothing with update_initramfs=no; the ESP's initrd is the image's
if [ "$1" = configure ] && [ -f /etc/initramfs-tools/update-initramfs.conf ]; then
	sed -i 's/^update_initramfs=.*/update_initramfs=no/' /etc/initramfs-tools/update-initramfs.conf
	grep -q '^update_initramfs=no$' /etc/initramfs-tools/update-initramfs.conf ||
		echo 'update_initramfs=no' >> /etc/initramfs-tools/update-initramfs.conf
fi
EOF
fi
for s in preinst postinst postrm; do echo 'exit 0' >> "$D/DEBIAN/$s"; done
build "$D" "vibeos-device-$DEV"
# no path in two of them, and every file of the zip's package in one of them (but the S10e's boot files, the
# POCO's ESP hooks, which go, and the new helper)
for f in "$OUTD"/*.deb; do dpkg-deb -c "$f" | awk '$1 !~ /^d/ { n = $6; sub(/^\./, "", n); print n }'; done | LC_ALL=C sort > "$W/new-files.txt"
[ -z "$(uniq -d "$W/new-files.txt")" ] || { echo "paths in two of the packages: $(uniq -d "$W/new-files.txt" | head -n 3)" >&2; exit 1; }
dpkg-deb -c "$SD" | awk '$1 !~ /^d/ { n = $6; sub(/^\./, "", n); print n }' | LC_ALL=C sort > "$W/old-files.txt"
LC_ALL=C comm -23 "$W/old-files.txt" "$W/new-files.txt" > "$W/gone.txt"
LC_ALL=C comm -13 "$W/old-files.txt" "$W/new-files.txt" > "$W/added.txt"
case $DEV in
s10e) EXPGONE=$(for f in $BOOTF; do echo /boot/s10e/$f; done | LC_ALL=C sort) ;;
poco) EXPGONE=$(printf '%s\n' /etc/initramfs/post-update.d/zz-surya-esp /etc/kernel/postinst.d/zz-surya-esp | LC_ALL=C sort) ;;
*)    EXPGONE= ;;
esac
[ "$(cat "$W/gone.txt")" = "$EXPGONE" ] || { echo "not carried over: $(tr '\n' ' ' < "$W/gone.txt")" >&2; exit 1; }
[ "$(cat "$W/added.txt")" = /usr/sbin/vibeos-kernel-install ] || { echo "new files: $(tr '\n' ' ' < "$W/added.txt")" >&2; exit 1; }
rm -rf "$W"
(cd "$OUTD" && sha256sum *.deb > SHA256SUMS)
echo "== $OUTD: $(ls "$OUTD" | tr '\n' ' ')"
