#!/bin/bash
# zip-test-poco.sh SYSZIP DEVZIP CASE: the VibeOS system zip, then the POCO X3 NFC's device zip, as adb sideload runs
# them (one recovery session, one /tmp), in a chroot of the POCO's own LineageOS recovery ramdisk (work/test/rpoco:
# nura/test/recovery-chroot.py with LineageOS 22.2 surya 20261102's recovery.img; mksh and toybox under qemu-user),
# each zip run by its own update-binary and updater-script, on two fake UFS LUNs shaped like a POCO's (sparse files,
# loop devices with 4096-byte sectors, every partition at the phone's own blocks):
#   sda  ssd persist misc keystore frp minidump cust recovery vbmeta_system metadata cache exaid gsort ffu super
#        userdata (userdata last, to the last usable LBA)
#   sde  vbmeta dtbo boot
# getprop answers ro.product.device=surya. /data (ext4 here; f2fs on the phone) and /metadata are mounted from dm
# devices over their partitions, as recovery has them. The installers must read no key on a POCO.
#   CASE  stock     LineageOS as installed, no "linux": userdata becomes "linux", with its own GPT (ESP + root)
#                   and the root; then the ESP, dtbo and vbmeta (LineageOS 20261102's: here they start as other
#                   bytes) and U-Boot to boot. Recovery is not touched
#         existing  an earlier VibeOS's layout: userdata (Android's data, kept) and a "linux" of 40 GiB behind it
#                   carrying the k22 GPT (ESP 480 MiB, root 10 GiB) with an old root; dtbo and vbmeta LineageOS
#                   20261102's already, recovery the k22 zip's U-Boot. The table and userdata stay as they are, the
#                   root and the ESP are replaced, U-Boot goes to boot, and the device zip says recovery needs
#                   LineageOS's again
#         again     stock, then a restart (both LUNs attached again: by-name from the new table) and both zips
#                   again: the existing path on the layout stock made, nothing in the tables changes, dtbo, vbmeta
#                   and boot hold the zip's already
#         rc3       stock, but the GPT tool's whole ends with exit 3 after writing the table (what it does when
#                   BLKPG fails): the system zip stops with the restart message; a restart, both zips again: the
#                   "linux" without a GPT of its own gets one, then as stock
#         abort     existing, but part 1's loop device is read-only, so its write fails (as every loop device's did
#                   on a POCO while SELinux was enforcing): the system zip stops at part 1 with its loop devices let
#                   go and SELinux as it found it; a restart, both zips again: as existing
# SELinux as on the phone: LineageOS recovery's kernel domain is enforcing, and a loop device then fails every read and
# write ("avc: denied { use } ... tclass=fd"). Here /t/selinux-enforce stands for /sys/fs/selinux/enforce (1 at
# every session start), and the test copies' losetup refuses to set up a loop device while it says 1: every such
# attempt is a failure. After each zip, SELinux must be enforcing again.
# The test copies differ from the real zips in: vibeos-part (a wrapper around an x86-64 static build of the same
# source, image/src/s10e-part.c, logging every call: qemu-user does not pass BLKPG), two lines of vibeos-lib.sh (BY:
# the fake by-name links; SEF: the fake SELinux switch), and in the device zip's updater-script the device check and
# the final restart. The chroot's PATH starts with /t/bin: getprop, getevent and losetup there. Run it as
# root in a mount namespace of its own (sudo unshare -m --propagation private bash zip-test-poco.sh ...), so that no
# service starting meanwhile copies its mounts. Touches only work/test/poco-*, work/test/rpoco-CASE and its own loop
# and dm devices. One case at a time: the installers pick a free loop device by themselves (as on a phone, where
# nothing else does), and two cases at once can pick the same one ("losetup: ... Device or resource busy").
set -u
export PATH=$PATH:/usr/sbin:/sbin
T=$(cd "$(dirname "$0")/.." && pwd)
[ ! -f "$T/local.conf" ] || . "$T/local.conf"
O=${VIBEOS_OUT:?set VIBEOS_OUT (image/local.conf): the directory builds, work files and logs go to}
W=$O/work/test
[ $# = 3 ] || { echo "usage: zip-test-poco.sh SYSZIP DEVZIP stock|existing|again|rc3|abort" >&2; exit 2; }
SYSZIP=$(realpath $1) DEVZIP=$(realpath $2) CASE=$3
case $CASE in stock|existing|again|rc3|abort) ;; *) echo "CASE: stock | existing | again | rc3 | abort" >&2; exit 2 ;; esac
PRE=$CASE; [ $CASE != abort ] || PRE=existing	# the layout it starts from and ends with
[ "$(id -u)" = 0 ] || { echo "run it as root, in a mount namespace of its own" >&2; exit 2; }
[ -d $W/rpoco/system/bin ] || { echo "no $W/rpoco: nura/test/recovery-chroot.py LINEAGEOS-SURYA-recovery.img $W/rpoco" >&2; exit 1; }
F=$W/poco-$CASE
R=$W/rpoco-$CASE
[ -d $R ] || { cp -a $W/rpoco $R.new && rm -rf $R.new/tmp/* $R.new/sideload $R.new/t && mv $R.new $R; } || exit 1
DA=$F/sda.img DE=$F/sde.img
FAILS=0
ok() { echo "  ok: $*"; }
bad() { echo "  FAIL: $*"; FAILS=$((FAILS + 1)); }
LA= LE= WATCH=
VTD=vpdata$$ VTM=vpmeta$$	# dm names of this run only
inchroot() { for p in /proc/[0-9]*; do case "$(readlink $p/root 2>/dev/null)" in "$R"|"$R"/*) echo ${p#/proc/} ;; esac; done; }
pa() { echo ${LA}p$1; }
pe() { echo ${LE}p$1; }
# NUMBER:NAME:START:SIZE, in 4 KiB blocks: a POCO X3 NFC's sda and sde
SDA="1:ssd:6:2 2:persist:8:8192 3:misc:8200:256 4:keystore:8456:128 5:frp:8584:128 6:minidump:8712:32768
7:cust:41480:262144 8:recovery:303624:32768 9:vbmeta_system:336392:16 10:metadata:336408:4096 11:cache:340504:98304
12:exaid:438808:98304 13:gsort:537112:4096 14:ffu:541208:2048 15:super:543256:2097152 16:userdata:2640408:27479523"
SDE="1:vbmeta:6:16 2:dtbo:22:6144 3:boot:6166:32768"
UD=16 MD=10 RC=8
end_session() {
	set +e
	[ -n "$WATCH" ] && kill $WATCH 2>/dev/null; WATCH=
	for k in $(inchroot); do kill -9 $k 2>/dev/null; done
	sleep 1
	for m in $R/tmp/vibeos-mnt $R/data $R/metadata; do mountpoint -q $m && umount $m; done
	for m in $VTD $VTM; do dmsetup remove $m 2>/dev/null; done
	for l in $(losetup -a 2>/dev/null | grep -F -e "($R/" -e "($F/" -e "(${LA:-/none}p" -e "(${LE:-/none}p" | cut -d: -f1); do
		[ $l = "$LA" ] || [ $l = "$LE" ] || losetup -d $l 2>/dev/null	# not the fake LUNs themselves
	done
	for m in dev sys proc; do mountpoint -q $R/$m && umount $R/$m; done
	mountpoint -q $R/tmp && { cp $R/tmp/vibeos.prop $F/vibeos.prop.$1 2>/dev/null; umount $R/tmp; }
	set -u
}
detach() {
	[ -n "$LA" ] && losetup -d $LA; [ -n "$LE" ] && losetup -d $LE
	LA= LE=
}
cleanup() {
	end_session last
	detach
	rm -f $DA $DE $R/sideload/sys.zip $R/sideload/dev.zip
	[ -z "${SUDO_UID:-}" ] || chown -R $SUDO_UID:${SUDO_GID:-$SUDO_UID} $F $R/t 2>/dev/null
}
trap cleanup EXIT
echo "==== $(basename $SYSZIP) + $(basename $DEVZIP) case=$CASE (POCO X3 NFC-shaped disks)"
rm -rf $F; mkdir -p $F
# ---- the GPT tool, built here from the source the zip's was built from
gcc -static -Os -std=gnu11 -o $F/vibeos-part-x86 $T/src/s10e-part.c || { echo "cannot build the GPT tool" >&2; exit 1; }
# ---- the disks
table_in() {	# SPEC: an sfdisk script
	echo "label: gpt"; echo "first-lba: 6"
	for x in $1; do IFS=: read n name start size <<< "$x"
		t=0FC63DAF-8483-4772-8E79-3D69D8477DE4; [ $name = userdata ] && t=1B81E7E6-F50D-419B-A739-AAD6D89E2AB4
		echo "start=$start, size=$size, type=$t, name=$name"
	done
}
truncate -s $((30119936 * 4096)) $DA; truncate -s $((39003 * 4096)) $DE
table_in "$SDA" | sfdisk -q --sector-size 4096 $DA > /dev/null 2>&1 || { echo "sfdisk sda failed" >&2; exit 1; }
table_in "$SDE" | sfdisk -q --sector-size 4096 $DE > /dev/null 2>&1 || { echo "sfdisk sde failed" >&2; exit 1; }
LAST=$(sfdisk --sector-size 4096 -d $DA | sed -n 's/^last-lba: *//p')
[ "$LAST" = 30119930 ] && [ "$(sfdisk --sector-size 4096 -d $DA | grep -c 'name=')" = 16 ] || { echo "sda: last usable $LAST, not the POCO's" >&2; exit 1; }
[ $PRE = existing ] && { $F/vibeos-part-x86 --sector-size 4096 apply $DA 40 | tail -n1 | sed 's/^/  pre: /'; }
attach() {
	LA=$(losetup -f --show -P --sector-size 4096 $DA) && LE=$(losetup -f --show -P --sector-size 4096 $DE) || exit 1
	case $LA$LE in /dev/loop[0-9]*/dev/loop[0-9]*) ;; *) echo "losetup gave $LA $LE" >&2; exit 1 ;; esac
	udevadm settle; sleep 1
}
attach
echo "  (disks: sda $LA, sde $LE)"
U0=$(sfdisk -d $LA | sed -n "s|^$(pa $UD) : start= *\([0-9]*\), size= *\([0-9]*\).*|\1 \2|p")
case $PRE:$U0 in existing:2640408\ *|*:"2640408 27479523") ;; *) echo "sda: userdata at $U0, not the POCO's blocks" >&2; exit 1 ;; esac
for x in $SDA; do n=${x%%:*}; case $n in $UD|$MD) continue ;; esac; dd if=/dev/urandom of=$(pa $n) bs=1M count=4 status=none 2>/dev/null; done
for x in $SDE; do dd if=/dev/urandom of=$(pe ${x%%:*}) bs=1M status=none 2>/dev/null; done
X=$F/x; mkdir -p $X
if [ $PRE = existing ]; then
	# what the 0.1.1 zip left: the k22 image's GPT in linux (#17), its old root and ESP; dtbo and vbmeta LineageOS
	# 20261102's (vbmeta padded with zeros, as fastboot leaves it), recovery the k22 zip's U-Boot
	LX=17
	printf 'label: gpt\nstart=2048, size=122880, type=C12A7328-F81F-11D2-BA4B-00A0C93EC93B, name=primary\nstart=124928, size=2621440, type=B921B045-1DF0-41C3-AF44-4C6F280D3FAE, name=primary\n' |
		sfdisk -q $(pa $LX) > /dev/null 2>&1 || { echo "sfdisk linux failed" >&2; exit 1; }
	dd if=/dev/urandom of=$(pa $LX) bs=4096 seek=2048 count=122880 conv=notrunc status=none
	OR=$(losetup -f --show -o $((124928 * 4096)) --sizelimit $((2621440 * 4096)) $(pa $LX)) &&
		mkfs.ext4 -q -F -L vibeos $OR && mkdir -p $X/m && mount $OR $X/m && echo old > $X/m/old-root-marker && umount $X/m &&
		losetup -d $OR || { echo "no old root" >&2; exit 1; }
	mkfs.ext4 -q -F -E nodiscard $(pa $UD) && mount $(pa $UD) $X/m && mkdir -p $X/m/media/0 && echo android > $X/m/android-marker &&
		umount $X/m || { echo "no Android data" >&2; exit 1; }
	unzip -p $DEVZIP dtbo.img | dd of=$(pe 2) bs=1M conv=fsync status=none
	dd if=/dev/zero of=$(pe 1) bs=4096 count=16 status=none; unzip -p $DEVZIP vbmeta.img | dd of=$(pe 1) conv=notrunc,fsync status=none
	unzip -p ${POCO_K22_ZIP:?set POCO_K22_ZIP: the 0.1.1 zip, its recovery-uboot.img} recovery-uboot.img | dd of=$(pa $RC) bs=1M conv=fsync status=none
	sfdisk -d $(pa $LX) | grep -v -E '^(label-id|device|unit|sector-size):' | sed "s|^$(pa $LX)p|n|" | sed -E 's/, uuid=[0-9A-F-]+//' > $F/nested-before.txt
else
	LX=$UD
	dd if=/dev/urandom of=$(pa $RC) bs=1M status=none 2>/dev/null	# LineageOS recovery, here random
fi
sync; blockdev --flushbufs $LA; blockdev --flushbufs $LE
sumof() { head -c ${2:-8388608} $1 | sha256sum | cut -d' ' -f1; }
declare -A BEFORE
for x in $SDA; do n=${x%%:*}; case $n in $UD|$MD) continue ;; esac; BEFORE[a$n]=$(sumof $(pa $n)); done
BEFORE[rc]=$(sumof $(pa $RC) 134217728)
for x in $SDE; do n=${x%%:*}; BEFORE[e$n]=$(sumof $(pe $n) 134217728); done
table() { sfdisk -d $LA | grep -v -E '^(label-id|device|unit|first-lba|last-lba|sector-size):' | sed "s|^${LA}p|sda|"; }
table > $F/table-before.txt
# ---- the test copies of the zips (made once per zip, kept beside the work dirs)
redirect_lib() {
	python3 -I - $1 <<'EOF'
import sys
p=sys.argv[1]; s=open(p).read()
a='\t\t[ -e "$b/$n" ] && { BY=$b; break 2; }\n\tdone\ndone\n'
assert s.count(a)==1; s=s.replace(a,a+'BY=/t/by-name\n')
a='SEF=/sys/fs/selinux/enforce\n'
assert s.count(a)==1; s=s.replace(a,'SEF=/t/selinux-enforce\n'); open(p,'w').write(s)
EOF
}
TZ=$W/poco-zips-$(echo "$SYSZIP $(stat -c '%s %Y' $SYSZIP)" | md5sum | cut -c1-8)
TD=$W/poco-zips-dev-$(echo "$DEVZIP $(stat -c '%s %Y' $DEVZIP)" | md5sum | cut -c1-8)
mkdir -p $TZ $TD
exec 9> $TZ.lock; flock 9
if [ ! -s $TZ/sys.zip ]; then
	echo "  (making the test copy of the system zip)"
	rm -rf $TZ/sys; mkdir -p $TZ/sys
	(cd $TZ/sys && unzip -q -o $SYSZIP vibeos-lib.sh) && redirect_lib $TZ/sys/vibeos-lib.sh || exit 1
	cat > $TZ/sys/vibeos-part <<'EOF'
#!/system/bin/sh
# the harness's GPT tool: the x86-64 static build of vibeos-part; with /t/rc3, "whole" ends as it does when BLKPG
# fails (the table written, exit 3); every call's exit code goes to /t/vp.log
/t/bin/vibeos-part-x86 "$@"; rc=$?
if [ $rc = 0 ] && [ "$1" = whole ] && [ -e /t/rc3 ]; then
	rm -f /t/rc3
	echo "GPT written, but BLKPG resize of partition 16 failed: Device or resource busy (reboot to see the new table)" >&2
	rc=3
fi
echo "$* rc=$rc" >> /t/vp.log
exit $rc
EOF
	cp $SYSZIP $TZ/sys.zip.new && (cd $TZ/sys && zip -q $TZ/sys.zip.new vibeos-lib.sh vibeos-part) && mv $TZ/sys.zip.new $TZ/sys.zip || exit 1
fi
flock -u 9
exec 9> $TD.lock; flock 9
if [ ! -s $TD/dev.zip ]; then
	echo "  (making the test copy of the device zip)"
	rm -rf $TD/dev; mkdir -p $TD/dev
	(cd $TD/dev && unzip -q -o $DEVZIP vibeos-lib.sh META-INF/com/google/android/updater-script) && redirect_lib $TD/dev/vibeos-lib.sh || exit 1
	U=$TD/dev/META-INF/com/google/android/updater-script
	sed -i -e 's#^assert(.*#assert("x" == "x" ||#' \
		-e 's#^run_program("/system/bin/setprop", "sys.powerctl", "reboot");$#run_program("/system/bin/echo", "TEST: here recovery would restart");#' $U
	grep -q '^assert("x" == "x" ||' $U && ! grep -q setprop $U || { echo "updater-script not redirected" >&2; exit 1; }
	cp $DEVZIP $TD/dev.zip.new && (cd $TD/dev && zip -q -0 $TD/dev.zip.new vibeos-lib.sh META-INF/com/google/android/updater-script) &&
		mv $TD/dev.zip.new $TD/dev.zip || exit 1
fi
flock -u 9; exec 9>&-
mkdir -p $R/sideload
ln -f $TZ/sys.zip $R/sideload/sys.zip && ln -f $TD/dev.zip $R/sideload/dev.zip || exit 1
mkdir -p $R/t/bin $R/t/by-name
cp $F/vibeos-part-x86 $R/t/bin/vibeos-part-x86
cat > $R/t/bin/getprop <<'EOF'
#!/system/bin/sh
case "${1:-}" in
ro.product.device|ro.build.product) echo surya ;;
*) echo ;;
esac
EOF
# no key may be read on a POCO: every call is logged, and none is answered
cat > $R/t/bin/getevent <<'EOF'
#!/system/bin/sh
echo "getevent $*" >> /t/getevent.log
exec sleep 1000
EOF
# SELinux enforcing: no loop device (see the top); /t/abort1: part 1's loop device (offset 0) read-only
cat > $R/t/bin/losetup <<'EOF'
#!/system/bin/sh
case " $* " in *" -d "*|*" -a "*|" -f ") exec /system/bin/losetup "$@" ;; esac
if [ "$(cat /t/selinux-enforce 2>/dev/null)" != 0 ]; then
	echo "losetup $* (SELinux enforcing)" >> /t/se.log
	echo "losetup: harness: SELinux is enforcing, this loop device would fail every read and write" >&2
	exit 1
fi
if [ -e /t/abort1 ] && [ "$1" = -o ] && [ "$2" = 0 ]; then rm -f /t/abort1; exec /system/bin/losetup -r "$@"; fi
exec /system/bin/losetup "$@"
EOF
chmod 755 $R/t/bin/*
: > $R/t/vp.log; : > $R/t/getevent.log; : > $R/t/se.log; rm -f $R/t/rc3 $R/t/abort1
# dm2 NAME DEV: a dm device over DEV in two linear halves, split at a 4 KiB boundary (the LUNs' logical block size)
dm2() { local n=$(blockdev --getsz $2) h; h=$((n / 16 * 8)); printf '0 %s linear %s 0\n%s %s linear %s %s\n' $h $2 $h $((n - h)) $2 $h | dmsetup create $1; }
# ---- a recovery session: by-name as ueventd made it at boot, /data and /metadata mounted, a fresh /tmp
start_session() {
	rm -rf $R/t/by-name/*
	for l in $LA $LE; do
		sfdisk -d $l | sed -n 's|^\(/dev/[^ ]*\) :.*name="\([^"]*\)".*|\1 \2|p' | while read d n; do ln -sfn $d $R/t/by-name/$n; done
	done
	if [ -e $R/t/by-name/userdata ]; then
		dm2 $VTD $(pa $UD) && udevadm settle && { [ -n "$(blkid -o value -s TYPE /dev/mapper/$VTD)" ] || mkfs.ext4 -q -F -E nodiscard /dev/mapper/$VTD; } &&
			mount /dev/mapper/$VTD $R/data && mkdir -p $R/data/media/0 || { echo "harness: no /data on $VTD" >&2; exit 1; }
		echo "  (harness: /data on $VTD)"
	fi
	dm2 $VTM $(pa $MD) && udevadm settle && { [ -n "$(blkid -o value -s TYPE /dev/mapper/$VTM)" ] || mkfs.ext4 -q -F /dev/mapper/$VTM; } &&
		mount /dev/mapper/$VTM $R/metadata || { echo "harness: no /metadata on $VTM" >&2; exit 1; }
	# a phone's unmount lets go of the partition; here the dm device would hold it, so it goes once the installer has
	# unmounted its file system
	( for m in data:$VTD metadata:$VTM; do
		dmsetup info ${m#*:} > /dev/null 2>&1 || continue
		until ! mountpoint -q $R/${m%%:*}; do sleep 0.2; done
		until dmsetup remove ${m#*:} 2>/dev/null; do sleep 0.2; done
		blockdev --flushbufs $(pa $( [ ${m%%:*} = data ] && echo $UD || echo $MD))
		echo "  (harness: ${m%%:*} unmounted by the installer, ${m#*:} released)"
	  done ) &
	WATCH=$!
	mount -t tmpfs -o size=3g,mode=1777 tmpfs $R/tmp
	echo 1 > $R/t/selinux-enforce	# a fresh boot of recovery: enforcing
	unzip -p $SYSZIP META-INF/com/google/android/update-binary > $R/tmp/update-binary; chmod 755 $R/tmp/update-binary
	for m in proc sys dev; do mount --bind /$m $R/$m; done
	echo "  (by-name: $(ls $R/t/by-name | tr '\n' ' '))"
}
run() {	# ZIP LABEL
	T0=$SECONDS
	chroot $R /system/bin/sh -c "cd /tmp && PATH=/t/bin:/system/bin exec /tmp/update-binary 3 1 /sideload/$1" 2>$F/$2.err | cat > $F/$2.screen
	RC1=${PIPESTATUS[0]}
	echo "  $2: update-binary exit $RC1 after $((SECONDS - T0)) s; screen:"
	sed -n 's/^ui_print \?//p' $F/$2.screen | grep -v -E '^-+$' | sed 's/^/  | /' | tail -n 24
	grep -v -E 'linker|realpath|^$' $F/$2.err | tail -n 6 | sed 's/^/  stderr: /'
	return $RC1
}
prop() { grep "^$1=" $R/tmp/vibeos.prop 2>/dev/null | tail -n1 | cut -d= -f2-; }
leftover() {
	for k in $(inchroot); do
		case "$(tr '\0' ' ' < /proc/$k/cmdline)" in *"sleep 1000"*) ;; *) bad "left a process: $(tr '\0' ' ' < /proc/$k/cmdline)" ;; esac
		kill -9 $k
	done
}
screen() { sed -n 's/^ui_print \?//p' $F/$1.screen; }
se_check() {	# LABEL: after a zip, SELinux enforcing again and no mode kept for later
	[ "$(cat $R/t/selinux-enforce)" = 1 ] && [ ! -e $R/tmp/vibeos-selinux ] && ok "$1: SELinux enforcing again" ||
		bad "$1: SELinux left at '$(cat $R/t/selinux-enforce)'$( [ ! -e $R/tmp/vibeos-selinux ] || echo ', its mode kept in /tmp')"
}
both() {	# LABEL: the system zip, then the device zip, in the session running
	run sys.zip system$1; SRC=$?
	echo "  prop: $(tr '\n' ' ' < $R/tmp/vibeos.prop)"
	[ $SRC = 0 ] && ok "system zip succeeded" || bad "system zip failed: $(grep '^error=' $R/tmp/vibeos.prop)"
	screen system$1 | grep -q '^  SELinux permissive while VibeOS is written' && ok "system zip: says SELinux is permissive meanwhile" ||
		bad "system zip: no SELinux line"
	se_check "system zip"
	DRC=-
	if [ $SRC = 0 ]; then
		run dev.zip device$1; DRC=$?
		[ $DRC = 0 ] && ok "device zip succeeded" || bad "device zip failed: $(grep '^error=' $R/tmp/vibeos.prop)"
		screen device$1 | grep -q '^  SELinux permissive while VibeOS is written' && ok "device zip: says SELinux is permissive meanwhile" ||
			bad "device zip: no SELinux line"
		se_check "device zip"
	fi
	echo "  vibeos-part calls: $(tr '\n' ';' < $R/t/vp.log)"
}
# ---- session 1
[ $CASE = rc3 ] && touch $R/t/rc3
start_session
if [ $CASE = rc3 ]; then
	run sys.zip system1; SRC=$?
	echo "  prop: $(tr '\n' ' ' < $R/tmp/vibeos.prop)"
	[ $SRC != 0 ] && grep -q 'restart into recovery and install again' $R/tmp/vibeos.prop &&
		ok "system zip stopped on exit 3 with the restart message" || bad "system zip on exit 3: rc $SRC, $(grep '^error=' $R/tmp/vibeos.prop)"
	grep -q '^whole .* rc=3$' $R/t/vp.log && ok "the GPT tool's whole exited 3 (forced)" || bad "no forced exit 3"
	se_check "stopped system zip"
	leftover
	echo "  -- restart: both LUNs attached again, by-name from the new table"
	end_session s1
	detach; attach
	: > $R/t/vp.log
	start_session
	both 2
	grep -q '^nest .* 480 rc=0$' $R/t/vp.log && ok "linux without its own GPT got one (nest)" || bad "no nest call"
	! grep -q -E '^(plan-whole|whole) ' $R/t/vp.log && ok "no second whole" || bad "whole ran again"
elif [ $CASE = abort ]; then
	touch $R/t/abort1
	run sys.zip system1; SRC=$?
	echo "  prop: $(tr '\n' ' ' < $R/tmp/vibeos.prop)"
	[ ! -e $R/t/abort1 ] && ok "part 1's loop device was made read-only" || bad "part 1's loop device was not set up as expected"
	[ $SRC != 0 ] && grep -q 'could not write part 1 of' $F/system1.screen $F/system1.err &&
		ok "system zip stopped: could not write part 1" || bad "system zip with part 1 unwritable: rc $SRC"
	se_check "stopped system zip"
	n=$(losetup -a | grep -c -F -e "($R/" -e "(${LA}p")
	[ $n = 0 ] && ok "its loop devices let go before it stopped" || bad "$n of its loop devices still attached"
	leftover
	echo "  -- restart: both LUNs attached again; both zips again"
	end_session s1
	detach; attach
	: > $R/t/vp.log
	start_session
	both 2
else
	both 1
fi
EXP_MODE=whole; case $PRE in existing|rc3) EXP_MODE=existing ;; esac
[ "$(prop mode)" = $EXP_MODE ] && [ "$(prop kind)" = nested ] && [ "$(prop name)" = linux ] && ok "mode $EXP_MODE, kind nested, name linux" ||
	bad "prop: mode '$(prop mode)' kind '$(prop kind)' name '$(prop name)'"
S1=system1; D1=device1; case $CASE in rc3|abort) S1=system2 D1=device2 ;; esac
case $PRE in
stock|again) grep -q '^plan-whole .* rc=0$' $R/t/vp.log && grep -q '^whole .* rc=0$' $R/t/vp.log && grep -q '^nest .* 480 rc=0$' $R/t/vp.log &&
		ok "GPT tool: plan-whole, whole, nest" || bad "GPT tool calls: $(tr '\n' ';' < $R/t/vp.log)"
	screen $S1 | grep -q 'ERASED' && ok "the system zip says Android's data is erased" || bad "no erase notice" ;;
existing) [ ! -s $R/t/vp.log ] && ok "the GPT tool did not run" || bad "GPT tool calls: $(tr '\n' ';' < $R/t/vp.log)"
	screen $S1 | grep -q "of an earlier install" && screen $S1 | grep -q "Android's data stays as it is" &&
		ok "the system zip says it replaces the earlier install's system, and Android's data stays" || bad "no notice" ;;
esac
if [ "${DRC:-}" = 0 ]; then
	case $PRE in
	existing)
		screen $D1 | grep -q "dtbo in dtbo: there already" && screen $D1 | grep -q "vbmeta in vbmeta: there already" &&
			ok "dtbo and vbmeta: there already, not written" || bad "dtbo/vbmeta written"
		screen $D1 | grep -q 'NOTE: recovery still holds the earlier U-Boot' && ok "the note: LineageOS's recovery goes back by hand" || bad "no recovery note" ;;
	*)
		screen $D1 | grep -q "dtbo to dtbo" && screen $D1 | grep -q "vbmeta to vbmeta" && ok "dtbo and vbmeta written" || bad "dtbo/vbmeta not written"
		! screen $D1 | grep -q NOTE && ok "no recovery note" || bad "a recovery note on a LineageOS recovery" ;;
	esac
	screen $D1 | grep -q "U-Boot to boot" && ok "U-Boot written to boot" || bad "U-Boot not written"
fi
leftover
if [ $CASE = again ]; then
	echo "  -- restart: both LUNs attached again, by-name from the new table; both zips again"
	end_session s1
	table > $F/table-mid.txt
	detach; attach
	: > $R/t/vp.log
	start_session
	both 2
	[ "$(prop mode)" = existing ] && ok "second run: the existing partition, no question" || bad "second run: mode '$(prop mode)'"
	EXP_MODE=existing	# what the root says now
	screen system2 | grep -qx "No other partition is changed." && ok "second run: no Android data named (there is none)" ||
		bad "second run: $(screen system2 | grep 'No other partition')"
	[ ! -s $R/t/vp.log ] && ok "second run: the GPT tool did not run" || bad "second run: GPT tool calls"
	if [ "${DRC:-}" = 0 ]; then
		[ "$(screen device2 | grep -c ': there already')" = 3 ] && ok "second run: dtbo, vbmeta and U-Boot there already" ||
			bad "second run wrote: $(screen device2 | grep ' to ' | tr '\n' ';')"
	fi
	leftover
fi
[ ! -s $R/t/getevent.log ] && ok "no key read" || bad "keys were read: $(tr '\n' ';' < $R/t/getevent.log)"
[ ! -s $R/t/se.log ] && ok "no loop device set up while SELinux was enforcing" || bad "loop devices while enforcing: $(tr '\n' ';' < $R/t/se.log)"
BYN=$(ls $R/t/by-name | tr '\n' ' ')
end_session end
# ---- checks from the host (each partition's own cache dropped: /data went through a dm device over it)
for p in $LA $LE ${LA}p* ${LE}p*; do blockdev --flushbufs $p; done
grep -i -E '([0-9a-f]{2}:){5}[0-9a-f]{2}|imei|serial' $F/*.screen && bad "an identifier on the screen" || ok "no identifiers on the screen"
grep -q '"sys.powerctl", "reboot");' <(unzip -p $DEVZIP META-INF/com/google/android/updater-script) &&
	! unzip -p $DEVZIP META-INF/com/google/android/updater-script | grep -q 'reboot,recovery' &&
	ok "the device zip restarts plainly (not into recovery)" || bad "the device zip's restart"
table > $F/table-after.txt
if [ $PRE = existing ]; then
	cmp -s $F/table-before.txt $F/table-after.txt && ok "sda table unchanged" || bad "sda table: $(diff $F/table-before.txt $F/table-after.txt | tr '\n' ' ')"
else
	D=$(diff $F/table-before.txt $F/table-after.txt | grep '^>' | cut -c3-)
	[ "$(diff $F/table-before.txt $F/table-after.txt | grep -c '^[<>]')" = 2 ] && echo "$D" | grep -q "^sda$UD .*name=\"linux\"" &&
		echo "$D" | grep -q 'type=0FC63DAF-8483-4772-8E79-3D69D8477DE4' && ok "sda: only #$UD changed, userdata -> linux (Linux type)" ||
		bad "sda table: $(diff $F/table-before.txt $F/table-after.txt | tr '\n' ' ')"
	set -- $(echo "$D" | sed -n 's/.*start= *\([0-9]*\), size= *\([0-9]*\).*/\1 \2/p') 0 0
	[ $1 = 2640408 ] && [ $(($1 + $2 - 1)) = $LAST ] && ok "linux: userdata's blocks, to the last usable LBA ($LAST)" || bad "linux at $1 +$2"
	[ $CASE != again ] || { cmp -s $F/table-mid.txt $F/table-after.txt && ok "second run: sda table unchanged" || bad "second run changed the table"; }
fi
sfdisk --verify $LA > $F/verify.txt 2>&1 && ok "sda GPT verifies (primary and backup)" || bad "sfdisk --verify: $(tr '\n' ' ' < $F/verify.txt)"
F0=$FAILS
for x in $SDA; do n=${x%%:*}; case $n in $UD|$MD|$LX) continue ;; esac
	[ "$(sumof $(pa $n))" = "${BEFORE[a$n]}" ] || bad "sda$n ($(echo $x | cut -d: -f2)) changed"; done
[ "$(sumof $(pa $RC) 134217728)" = "${BEFORE[rc]}" ] || bad "recovery changed"
[ $FAILS = $F0 ] && ok "sda1-15 untouched (persist, misc, recovery, super among them; metadata below)"
e2fsck -fn $(pa $MD) > /dev/null 2>&1 && ok "metadata: still a clean ext4 (mounted and unmounted, as recovery does)" || bad "metadata"
if [ $PRE = existing ]; then
	mkdir -p $X/m && mount -o ro $(pa $UD) $X/m && { [ "$(cat $X/m/android-marker)" = android ] && ok "userdata: Android's data still there" ||
		bad "userdata: the marker is gone"; umount $X/m; } || bad "userdata does not mount"
	e2fsck -fn $(pa $UD) > /dev/null 2>&1 && ok "userdata: a clean ext4" || bad "userdata does not check"
fi
same() {	# PARTITION FILE-IN-ZIP: the partition is that file, the rest zeros
	local b=$(unzip -p $DEVZIP $2 | wc -c)
	[ "$(head -c $b $1 | sha256sum)" = "$(unzip -p $DEVZIP $2 | sha256sum)" ] && [ "$(tail -c +$((b + 1)) $1 | tr -d '\000' | wc -c)" = 0 ]
}
if [ "${DRC:-}" = 0 ]; then
	same $(pe 1) vbmeta.img && ok "vbmeta = the zip's vbmeta.img (LineageOS 20261102's), the rest zeros" || bad "vbmeta"
	same $(pe 2) dtbo.img && ok "dtbo = the zip's dtbo.img" || bad "dtbo"
	same $(pe 3) boot.img && ok "boot = the zip's boot.img (U-Boot)" || bad "boot"
	[ $PRE != existing ] || { [ "$(sumof $(pe 1) 134217728)" = "${BEFORE[e1]}" ] && [ "$(sumof $(pe 2) 134217728)" = "${BEFORE[e2]}" ] &&
		ok "vbmeta and dtbo byte for byte as before" || bad "vbmeta/dtbo bytes changed"; }
fi
# the GPT inside linux, and what it holds
NL=$(losetup -f --show -r -b 4096 $(pa $LX)) || { bad "cannot map linux"; NL=; }
if [ -n "$NL" ]; then
	sfdisk -d $NL 2>/dev/null | grep -v -E '^(label-id|device|unit|sector-size):' | sed "s|^${NL}p|n|" | sed -E 's/, uuid=[0-9A-F-]+//' > $F/nested-after.txt
	NB=$(($(blockdev --getsz $NL) / 8))
	if [ $PRE = existing ]; then
		cmp -s $F/nested-before.txt $F/nested-after.txt && ok "the GPT inside linux unchanged" || bad "nested GPT: $(diff $F/nested-before.txt $F/nested-after.txt | tr '\n' ' ')"
	else
		grep -q '^n1 : start= *2048, size= *122880, type=C12A7328-F81F-11D2-BA4B-00A0C93EC93B, name="primary"$' $F/nested-after.txt &&
			grep -q "^n2 : start= *124928, size= *$((NB - 5 - 124928)), type=B921B045-1DF0-41C3-AF44-4C6F280D3FAE, name=\"primary\"$" $F/nested-after.txt &&
			[ "$(grep -c '^n[0-9]' $F/nested-after.txt)" = 2 ] && ok "inside linux: the k22 layout (ESP 480 MiB at block 2048, root behind it to the end)" ||
			bad "nested GPT: $(tr '\n' ' ' < $F/nested-after.txt)"
		sfdisk --verify $NL > $F/nverify.txt 2>&1 && ok "the GPT inside linux verifies" || bad "nested verify: $(tr '\n' ' ' < $F/nverify.txt)"
	fi
	set -- $(sed -n 's/^n2 : start= *\([0-9]*\), size= *\([0-9]*\).*/\1 \2/p' $F/nested-after.txt) 0 0
	RD=$(losetup -f --show -r -o $(($1 * 4096)) --sizelimit $(($2 * 4096)) $(pa $LX))
	if [ "${DRC:-}" = 0 ]; then
		EB=$(unzip -p $DEVZIP esp.img | wc -c)
		[ "$(dd if=$(pa $LX) bs=4096 skip=2048 count=$((EB / 4096)) status=none | sha256sum)" = "$(unzip -p $DEVZIP esp.img | sha256sum)" ] &&
			ok "the ESP inside linux = esp.img" || bad "ESP"
	fi
	LBL=$(blkid -o value -s LABEL $RD); [ "$LBL" = vibeos ] && ok "root: label vibeos" || bad "root label '$LBL'"
	e2fsck -fn $RD > $F/fsck.log 2>&1 && ok "root: e2fsck -fn clean" || bad "e2fsck: $(tail -n 3 $F/fsck.log)"
	dbg() { debugfs -R "$1" $RD 2>/dev/null; }
	IT=$(dbg "cat /etc/vibeos/installed-to" | tr '\n' ' ')
	echo "$IT" | grep -q "kind=nested name=linux mode=$EXP_MODE " && echo "$IT" | grep -q 'device=surya' && ok "installed-to: $IT" || bad "installed-to: '$IT'"
	dbg "cat /etc/vibeos/system-image" | grep -q '^VERSION=1:0.1.3' && ok "system-image 1:0.1.3" || bad "system-image"
	dbg "stat /old-root-marker" | grep -q '^Inode:' && bad "the old root's file is still there" || ok "no file of an earlier root"
	if [ "${DRC:-}" = 0 ]; then
		DD=$(dbg "ls /var/lib/vibeos/device/debs" | tr -s ' ' '\n' | grep '\.deb$' | sort | tr '\n' ' ')
		WANT=$(unzip -Z1 $DEVZIP | sed -n 's|^debs/\(.*\.deb\)$|\1|p' | sort | tr '\n' ' ')
		[ "$DD" = "$WANT" ] && ok "debs for the first boot: $(echo $DD | wc -w)" || bad "debs: '$DD' want '$WANT'"
		for d in $DD; do [ "$(dbg "cat /var/lib/vibeos/device/debs/$d" | sha256sum)" = "$(unzip -p $DEVZIP debs/$d | sha256sum)" ] || bad "$d differs"; done
		NP=$(unzip -p $DEVZIP payload.tar | tar -t | grep -v '/$' | wc -l)
		XS=$(unzip -p $DEVZIP payload.tar | tar -t | grep -v '/$' | shuf -n 40 --random-source=<(yes))
		miss=0; for x in $XS; do dbg "stat ${x#.}" | grep -q '^Inode:' || { miss=$((miss + 1)); echo "    missing ${x#.}"; }; done
		[ $miss = 0 ] && ok "payload unpacked (40 of its $NP files looked up)" || bad "$miss payload files missing"
		dbg "stat /lib" | grep -q 'Type: symlink' && ok "/lib is still the usr/lib symlink" || bad "/lib is no symlink any more"
		[ "$(dbg "cat /etc/vibeos/device")" = "device=poco" ] && ok "/etc/vibeos/device" || bad "/etc/vibeos/device: $(dbg "cat /etc/vibeos/device")"
		n=0; for f in /etc/vibeos/android /etc/tmpfiles.d/surya-reboot-linux.conf /usr/local/sbin/surya-boot-android \
			/usr/share/polkit-1/actions/vibeos.surya.boot-android.policy; do dbg "stat $f" | grep -q '^Inode:' && { bad "$f is in the root"; n=1; }; done
		[ $n = 0 ] && ok "no Android entry, no restart into recovery (none of the four files)"
		dbg "ls -l /var/lib/vibeos/device/result" | grep -q result && bad "a result file is there" || ok "no result yet (the first boot writes it)"
	fi
	losetup -d $RD; losetup -d $NL
fi
echo "  (session by-name at the end: $BYN)"
echo "== $FAILS failures"
[ $FAILS = 0 ]
