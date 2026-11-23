#!/bin/bash
# zip-test.sh SYSZIP DEVZIP PRE SYSKEYS -- DEVICE: the VibeOS system zip, then a device zip, in ONE chroot of the
# S10e's own LineageOS recovery ramdisk (mksh/toybox, qemu-user; the copy in work/test/r), each run by its own
# update-binary and updater-script, as recovery runs two zips in one session (one /tmp), on a fake UFS LUN: a sparse
# copy of the S10e's real GPT (s10e-installer/gpt/sda-real.img), a loop device with 4096-byte sectors.
#   PRE      fresh | existing (an empty "linux" #32 of 40 GiB) | nested (#32 carries a GPT: ESP + root, the POCO's
#            layout) | wholed (userdata is "linux" already)
#   SYSKEYS  the keys the system zip's menus read: UP DOWN POWER
#   DEVICE   s10e | s5e | poco (DEVZIP's device; "-" runs no device zip)
# The test zips differ from the real ones in: vibeos-part (an x86-64 static build of the same source: qemu-user does
# not pass BLKPG_RESIZE_PARTITION), one line of vibeos-lib.sh (BY: the loop device's by-name links), and in the
# device zip's updater-script the device check and the final setprop. Touches only work/test and its loop/dm devices.
set -u
export PATH=$PATH:/usr/sbin:/sbin
T=$(cd "$(dirname "$0")/.." && pwd)
[ ! -f "$T/local.conf" ] || . "$T/local.conf"
O=${VIBEOS_OUT:?set VIBEOS_OUT (image/local.conf): the directory builds, work files and logs go to}
W=$O/work/test
R=$W/r
D=$W/disk.img
SYSZIP=$(realpath $1) DEVZIP=$2 PRE=$3; shift 3
KEYS=
while [ $# -gt 0 ] && [ "$1" != -- ]; do KEYS="$KEYS $1"; shift; done
shift; DEVICE=${1:--}
[ "$DEVZIP" = - ] || DEVZIP=$(realpath $DEVZIP)
HOST=$W/s10e-part-x86static
F=$W/t; rm -rf $F; mkdir -p $F
FAILS=0
ok() { echo "  ok: $*"; }
bad() { echo "  FAIL: $*"; FAILS=$((FAILS + 1)); }
L= WATCH=
VTD=vtdata$$ VTM=vtmeta$$ VTT=vibeostest$$	# dm names of this run only
inchroot() { sudo -n sh -c 'for p in /proc/[0-9]*; do case "$(readlink $p/root 2>/dev/null)" in '"$R"'*) echo ${p#/proc/} ;; esac; done'; }
cleanup() {
	set +e
	[ -n "$WATCH" ] && kill $WATCH 2>/dev/null
	for k in $(inchroot); do sudo -n kill -9 $k 2>/dev/null; done
	sleep 1
	for m in $R/tmp/vibeos-mnt $R/data $R/metadata; do mountpoint -q $m && sudo -n umount $m; done
	for m in $VTD $VTM; do sudo -n dmsetup remove $m 2>/dev/null; done
	# only this run's: the device zip's dm map over this fake disk, loops over files in this chroot or this disk
	# (other sessions run zip tests of their own on this host)
	for d in $(sudo -n dmsetup ls 2>/dev/null | awk '/^vibeos[0-9]/{print $1}'); do
		[ -n "$L" ] && sudo -n dmsetup deps -o devname $d 2>/dev/null | grep -q "(${L##*/}p" && sudo -n dmsetup remove $d
	done
	for l in $(sudo -n losetup -a 2>/dev/null | grep -F -e "($R/" -e "($F/" -e "(${L:-/none}p" | cut -d: -f1); do sudo -n losetup -d $l 2>/dev/null; done
	for m in dev sys proc; do mountpoint -q $R/$m && sudo -n umount $R/$m; done
	[ -d $F ] && mountpoint -q $R/tmp && sudo -n cp $R/tmp/vibeos.prop $F/ 2>/dev/null
	mountpoint -q $R/tmp && sudo -n umount $R/tmp
	[ -n "$L" ] && sudo -n losetup -d $L
	sudo -n chown -R $(id -u):$(id -g) $R/tmp $R/t 2>/dev/null
	rm -f $D $R/sideload/sys.zip $R/sideload/dev.zip
}
trap cleanup EXIT
echo "==== $(basename $SYSZIP) + $(basename $DEVZIP) pre=$PRE keys=[$KEYS ] device=$DEVICE"
rm -f $D; cp --sparse=always $W/sda-real.img $D
case $PRE in
fresh) ;;
existing|nested) $HOST apply $D 40 | tail -n1 | sed 's/^/  pre: /' ;;
wholed) $HOST whole $D | tail -n1 | sed 's/^/  pre: /' ;;
esac
L=$(sudo -n losetup -f --show -P --sector-size 4096 $D) || exit 1
case $L in /dev/loop[0-9]*) ;; *) echo "losetup gave $L" >&2; exit 1 ;; esac
sudo -n udevadm settle; sleep 1
p() { echo ${L}p$1; }
psize() { echo $(($(cat /sys/class/block/$(basename $(p $1))/size) * 512)); }
if [ $PRE = nested ]; then
	printf 'label: gpt\nstart=2048, size=122880, type=C12A7328-F81F-11D2-BA4B-00A0C93EC93B, name=esp\nstart=124928, size=2621440, type=B921B045-1DF0-41C3-AF44-4C6F280D3FAE, name=root\n' |
		sudo -n sfdisk -q $(p 32) > /dev/null || { echo "sfdisk failed" >&2; exit 1; }
	ok "pre: #32 carries a GPT (ESP 480 MiB at block 2048, root 10 GiB at block 124928)"
fi
for n in 14 15; do sudo -n dd if=/dev/urandom of=$(p $n) bs=1M count=1 status=none; done
sudo -n dd if=/dev/zero of=$(p 19) bs=1M count=1 status=none	# misc: no bootloader message (recovery cleared it)
for n in 3 7 16; do sudo -n dd if=/dev/urandom of=$(p $n) bs=1M count=1 status=none; done	# efs sec_efs radio
# up_param: a copy of an S10e's (UP_PARAM_IMG), or a stand-in tar with the three pictures the device zip replaces
UPP=${UP_PARAM_IMG:-}
if [ -z "$UPP" ]; then
	UPP=$W/up_param-standin.img; mkdir -p $W/upp
	for j in logo.jpg svb_orange.jpg booting_warning.jpg other.jpg; do head -c 4096 /dev/urandom > $W/upp/$j; done
	tar -C $W/upp -cf $UPP logo.jpg svb_orange.jpg booting_warning.jpg other.jpg
fi
sudo -n dd if=$UPP of=$(p 5) bs=1M status=none conv=fsync
sumof() { sudo -n dd if=$1 bs=1M count=${2:-8} status=none | sha256sum | cut -d' ' -f1; }
declare -A BEFORE
for n in $(seq 1 32); do [ -b $(p $n) ] && BEFORE[$n]=$(sumof $(p $n)); done
dm2() { local n=$(sudo -n blockdev --getsz $2) h; h=$((n / 2)); printf '0 %s linear %s 0\n%s %s linear %s %s\n' $h $2 $h $((n - h)) $2 $h | sudo -n dmsetup create $1; }
UDN=31; [ $PRE = wholed ] && UDN=
if [ -n "$UDN" ]; then
	# Android's data: ext4 here (the S10e's is f2fs; the Tab S5e's ext4, which its image file needs)
	dm2 $VTD $(p 31) && sudo -n udevadm settle && sudo -n mkfs.ext4 -q -F -E nodiscard /dev/mapper/$VTD &&
		sudo -n mount /dev/mapper/$VTD $R/data && sudo -n mkdir -p $R/data/media/0 || { echo "harness: no /data on vtdata" >&2; exit 1; }
	echo "  (harness: /data on $VTD)"
fi
dm2 $VTM $(p 29) && sudo -n udevadm settle && sudo -n mkfs.ext4 -q -F /dev/mapper/$VTM &&
	sudo -n mount /dev/mapper/$VTM $R/metadata || { echo "harness: no /metadata on vtmeta" >&2; exit 1; }
# on the phone an unmount lets go of the partition; here the dm device would hold it (make_f2fs and mke2fs refuse
# a device in use), so it goes once the installer has unmounted its file system
( for m in data:$VTD metadata:$VTM; do
	sudo -n dmsetup info ${m#*:} > /dev/null 2>&1 || continue
	until ! mountpoint -q $R/${m%%:*}; do sleep 0.2; done
	until sudo -n dmsetup remove ${m#*:} 2>/dev/null; do sleep 0.2; done
	# what the dm device wrote went past the partition's own page cache, which still holds the blocks the sums
	# above read: dropped, as a phone (one block device for /data, no dm in between) never needs
	sudo -n blockdev --flushbufs $(p $( [ ${m%%:*} = data ] && echo 31 || echo 29))
	echo "  (harness: ${m%%:*} unmounted by the installer, ${m#*:} released)"
	# a later mount of /data (the S5e's device zip) sees userdata itself
  done ) &
WATCH=$!
# recovery's /tmp is a tmpfs (and VibeDev is nodev: the installers make device nodes in /tmp)
sudo -n mount -t tmpfs -o size=3g,mode=1777 tmpfs $R/tmp
sudo -n mkdir -p $R/t/bin $R/t/by-name; sudo -n chown -R $(id -u):$(id -g) $R/t $R/tmp
sudo -n sh -c "rm -rf $R/t/by-name/*"
# by-name as ueventd made it at boot: a partition the install makes or renames gets no link in this session
BYN="3:efs 5:up_param 7:sec_efs 14:boot 15:recovery 16:radio 19:misc 29:omr 31:userdata"
[ $DEVICE = poco ] && BYN="3:efs 5:up_param 7:sec_efs 14:boot 26:recovery 16:radio 19:misc 29:omr 31:userdata"
case $PRE in existing|nested) BYN="$BYN 32:linux" ;; wholed) BYN=$(echo $BYN | sed 's/31:userdata/31:linux/') ;; esac
for n in $BYN; do ln -sfn $(p ${n%%:*}) $R/t/by-name/${n#*:}; done
cat > $R/t/bin/getevent <<'EOF'
#!/system/bin/sh
k=$(head -n1 /t/keys 2>/dev/null)
[ -n "$k" ] || exec sleep 1000
sed -i 1d /t/keys
case $k in
UP)    echo "/dev/input/event2: EV_KEY       KEY_VOLUMEUP         DOWN" ;;
DOWN)  echo "/dev/input/event2: EV_KEY       KEY_VOLUMEDOWN       DOWN" ;;
POWER) echo "/dev/input/event0: EV_KEY       KEY_POWER            DOWN" ;;
esac
echo "/dev/input/event2: EV_SYN       SYN_REPORT           00000000"
EOF
chmod 755 $R/t/bin/getevent
: > $R/t/keys; for k in $KEYS; do echo $k >> $R/t/keys; done
# the test copies of the zips
redirect_lib() {
	python3 -I - $1 <<'EOF'
import sys
p=sys.argv[1]; s=open(p).read()
a='\t\t[ -e "$b/$n" ] && { BY=$b; break 2; }\n\tdone\ndone\n'
assert s.count(a)==1; s=s.replace(a,a+'BY=/t/by-name\n'); open(p,'w').write(s)
EOF
}
Z=$F/sys; mkdir -p $Z
(cd $Z && unzip -q -o $SYSZIP vibeos-lib.sh)
redirect_lib $Z/vibeos-lib.sh
cp $HOST $Z/vibeos-part
# the zips as adb sideload serves them (/sideload, not recovery's /tmp: a big zip and what it unpacks would not
# both fit in this tmpfs)
sudo -n mkdir -p $R/sideload; sudo -n chown $(id -u):$(id -g) $R/sideload
cp $SYSZIP $R/sideload/sys.zip; chmod 644 $R/sideload/sys.zip
(cd $Z && zip -q $R/sideload/sys.zip vibeos-lib.sh vibeos-part)
unzip -p $SYSZIP META-INF/com/google/android/update-binary > $R/tmp/update-binary; chmod 755 $R/tmp/update-binary
if [ "$DEVZIP" != - ]; then
	Y=$F/dev; mkdir -p $Y
	(cd $Y && unzip -q -o $DEVZIP vibeos-lib.sh META-INF/com/google/android/updater-script)
	redirect_lib $Y/vibeos-lib.sh
	U=$Y/META-INF/com/google/android/updater-script
	sed -i -e 's#^assert(.*#assert("x" == "x" ||#' -e 's#^run_program("/system/bin/setprop", "sys.powerctl", "reboot,recovery");$#run_program("/system/bin/echo", "TEST: here recovery would restart");#' $U
	grep -q '^assert("x" == "x" ||' $U && ! grep -q setprop $U || { echo "updater-script not redirected" >&2; exit 1; }
	# dmlinear: qemu-user garbles DM_TABLE_LOAD; an x86-64 static build of the same source runs here
	X=; unzip -Z1 $DEVZIP | grep -qx dmlinear && { cp $W/dmlinear-x86static $Y/dmlinear; X=dmlinear; }
	cp $DEVZIP $R/sideload/dev.zip; chmod 644 $R/sideload/dev.zip
	(cd $Y && zip -q $R/sideload/dev.zip vibeos-lib.sh META-INF/com/google/android/updater-script $X)
fi
for m in proc sys dev; do sudo -n mount --bind /$m $R/$m; done
run() {	# ZIP LABEL
	T0=$SECONDS
	sudo -n chroot $R /system/bin/sh -c "cd /tmp && PATH=/t/bin:/system/bin exec /tmp/update-binary 3 1 /sideload/$1" 2>$F/$2.err | cat > $F/$2.screen
	RC=${PIPESTATUS[0]}
	echo "  $2: update-binary exit $RC after $((SECONDS - T0)) s; screen:"
	sed -n 's/^ui_print \?//p' $F/$2.screen | grep -v -E '^-+$|Volume Up/Down|No key for 2' | sed 's/^/  | /' | tail -n 30
	grep -v -E 'linker|realpath|^$' $F/$2.err | tail -n 6 | sed 's/^/  stderr: /'
	return $RC
}
run sys.zip system; SRC=$?
echo "  prop: $(tr '\n' ' ' < $R/tmp/vibeos.prop)"
[ -z "$(cat $R/t/keys)" ] && ok "every key was read" || bad "keys left over: $(cat $R/t/keys | tr '\n' ' ')"
[ $SRC = 0 ] && ok "system zip succeeded" || bad "system zip failed: $(grep '^error=' $R/tmp/vibeos.prop)"
DRC=-
if [ $SRC = 0 ] && [ "$DEVZIP" != - ]; then run dev.zip device; DRC=$?; [ $DRC = 0 ] && ok "device zip succeeded" || bad "device zip failed: $(grep '^error=' $R/tmp/vibeos.prop)"; fi
for k in $(inchroot); do case "$(tr '\0' ' ' < /proc/$k/cmdline)" in *vibeos-km-run*) echo "  (keepmisc running: $k, killed)" ;; *) bad "left a process: $(tr '\0' ' ' < /proc/$k/cmdline)" ;; esac; sudo -n kill -9 $k; done
for m in dev sys proc; do sudo -n umount $R/$m; done
prop() { grep "^$1=" $R/tmp/vibeos.prop 2>/dev/null | tail -n1 | cut -d= -f2-; }
KIND=$(prop kind) MODE=$(prop mode)
# ---- checks from the host
sudo -n blockdev --flushbufs $L; sudo -n blockdev --rereadpt $L 2>/dev/null; sudo -n udevadm settle; sleep 1
SHOW=$($HOST show $D)
echo "  table: $(echo "$SHOW" | grep -E ' (userdata|linux) ' | awk '{printf "#%s %s %s %s; ", $1, $6, $4, $5}')"
for n in 3 7 16; do [ "$(sumof $(p $n))" = "${BEFORE[$n]}" ] || bad "partition $n (efs/sec_efs/radio) changed"; done
ok "efs, sec_efs, radio untouched"
grep -i -E '([0-9a-f]{2}:){5}[0-9a-f]{2}|imei|serial' $F/*.screen && bad "an identifier on the screen" || ok "no identifiers on the screen"
# the root, as the device would see it
RD=
case $KIND in
part) RD=$(p $(echo "$SHOW" | awk '$6=="linux"{print $1}')) ;;
nested) RD=$(sudo -n losetup -f --show -o $((124928 * 4096)) --sizelimit $((2621440 * 4096)) $(p 32)) ;;
image)
	# as the Tab S5e's initramfs: the extents the device zip wrote at the end of recovery, dm-linear over userdata
	if [ "$DRC" = 0 ]; then
		RP=$(p 15); n=$(cat /sys/class/block/$(basename $RP)/size)
		sudo -n dd if=$RP bs=512 skip=$((n - 1024)) count=1024 status=none | tr -d '\000' > $F/tail.txt
		[ "$(head -n1 $F/tail.txt)" = "GTS4LV-EXTENTS 1" ] && grep -qx END $F/tail.txt && ok "extent list at the end of recovery ($(($(wc -l < $F/tail.txt) - 2)) extents)" || bad "no extent list"
		sed '1d;/^END$/d' $F/tail.txt | awk -v d=$(p 31) 'BEGIN{s=0} {printf "%d %d linear %s %d\n", s, $3/512, d, $2/512; s+=$3/512}' | sudo -n dmsetup create $VTT && RD=/dev/mapper/$VTT
	else
		sudo -n mount -o ro $(p 31) $F/m 2>/dev/null || { mkdir -p $F/m; sudo -n mount -o ro $(p 31) $F/m; }
		RD=$(sudo -n losetup -f --show -r $F/m/lost+found/vibeos/rootfs.img)
	fi ;;
esac
if [ -n "$RD" ]; then
	LBL=$(sudo -n blkid -o value -s LABEL $RD); [ "$LBL" = vibeos ] && ok "root $RD: label vibeos" || bad "root label '$LBL'"
	sudo -n e2fsck -fn $RD > $F/fsck.log 2>&1 && ok "e2fsck -fn clean" || bad "e2fsck: $(tail -n 3 $F/fsck.log)"
	IT=$(sudo -n debugfs -R "cat /etc/vibeos/installed-to" $RD 2>/dev/null | tr '\n' ' ')
	echo "$IT" | grep -q "kind=$KIND " && ok "installed-to: $IT" || bad "installed-to: '$IT'"
	if [ "$DRC" = 0 ]; then
		DD=$(sudo -n debugfs -R "ls /var/lib/vibeos/device/debs" $RD 2>/dev/null | tr -s ' ' '\n' | grep '\.deb$' | sort | tr '\n' ' ')
		WANT=$(unzip -Z1 $DEVZIP | sed -n 's|^debs/\(.*\.deb\)$|\1|p' | sort | tr '\n' ' ')
		[ "$DD" = "$WANT" ] && ok "debs for the first boot: $(echo $DD | wc -w)" || bad "debs: '$DD' want '$WANT'"
		for d in $DD; do [ "$(sudo -n debugfs -R "cat /var/lib/vibeos/device/debs/$d" $RD 2>/dev/null | sha256sum)" = "$(unzip -p $DEVZIP debs/$d | sha256sum)" ] || bad "$d differs"; done
		NP=$(unzip -p $DEVZIP payload.tar | tar -t | grep -v '/$' | wc -l)
		X=$(unzip -p $DEVZIP payload.tar | tar -t | grep -v '/$' | shuf -n 20 --random-source=<(yes))
		miss=0; for x in $X; do sudo -n debugfs -R "stat ${x#.}" $RD 2>/dev/null | grep -q '^Inode:' || { miss=$((miss + 1)); echo "    missing ${x#.}"; }; done
		[ $miss = 0 ] && ok "payload unpacked (20 of its $NP files looked up)" || bad "$miss payload files missing"
		sudo -n debugfs -R "stat /lib" $RD 2>/dev/null | grep -q 'Type: symlink' && ok "/lib is still the usr/lib symlink" || bad "/lib is no symlink any more"
		[ "$(sudo -n debugfs -R "cat /etc/vibeos/device" $RD 2>/dev/null)" = "device=$DEVICE" ] && ok "/etc/vibeos/device" || bad "/etc/vibeos/device"
	fi
fi
case $KIND in nested) sudo -n losetup -d $RD ;; image) [ "$DRC" = 0 ] && sudo -n dmsetup remove $VTT || { sudo -n losetup -d $RD; sudo -n umount $F/m; } ;; esac
# what the system zip did to the table and Android's data
case $MODE in
new) echo "$SHOW" | grep -q ' 32 .* linux ' && ok "new linux #32" || bad "no linux #32"
     [ "$(sudo -n dd if=$(p 31) bs=1 skip=1080 count=2 status=none | od -An -tx1 | tr -d ' ')" = 53ef ] && ok "userdata formatted again (ext4, as it was)" || bad "userdata not formatted" ;;
whole) [ "$(echo "$SHOW" | awk '$6=="linux"{print $1}')" = 31 ] && ! echo "$SHOW" | grep -q ' userdata ' && ok "userdata became linux #31" || bad "table" ;;
existing|image) [ "$(echo "$SHOW" | grep -c ' linux ')" = "$( [ $PRE = fresh ] && echo 0 || echo 1)" ] && ok "table unchanged" || bad "table changed" ;;
esac
# the boot pieces
same() { [ "$(sudo -n head -c $3 $1 | sha256sum)" = "$(unzip -p $DEVZIP $2 | head -c $3 | sha256sum)" ]; }
if [ "$DRC" = 0 ]; then
	case $DEVICE in
	s10e)
		same $(p 15) uboot.img 4194304 && ok "recovery = U-Boot" || bad "recovery"
		if [ $MODE = whole ] || [ $PRE = wholed ]; then same $(p 14) uboot.img 4194304 && ok "boot = U-Boot (no Android)" || bad "boot"
		else [ "$(sumof $(p 14))" = "${BEFORE[14]}" ] && ok "boot (Android's) untouched" || bad "boot changed"; fi
		[ "$(sumof $(p 5))" != "${BEFORE[5]}" ] && ok "up_param: the logo written" || bad "up_param unchanged" ;;
	s5e)
		RB=$(unzip -p $DEVZIP recovery.img | wc -c)
		same $(p 15) recovery.img $RB && ok "recovery = U-Boot + LineageOS recovery + Linux ($RB bytes)" || bad "recovery"
		[ "$(sudo -n dd if=$(p 19) bs=32 count=1 status=none | tr -d '\000')" = boot-recovery ] && ok "misc: boot-recovery (keepmisc)" || bad "misc" ;;
	poco)
		EB=$(unzip -p $DEVZIP esp.img | wc -c)
		[ "$(sudo -n dd if=$(p 32) bs=4096 skip=2048 count=$((EB / 4096)) status=none | sha256sum)" = "$(unzip -p $DEVZIP esp.img | sha256sum)" ] && ok "the ESP inside linux = esp.img" || bad "ESP"
		same $(p 26) recovery-uboot.img 134217728 && ok "recovery (#26 here) = U-Boot" || bad "recovery" ;;
	esac
fi
echo "== $FAILS failures"
[ $FAILS = 0 ]
