#!/bin/bash
# [KEYS="DOWN POWER"] [EXPECT=refused] zip-test.sh DEVZIP LAYOUT KIND [SLOT] [RUNS]: a nura-to-vibeos device zip run by its own update-binary in a chroot of
# LineageOS's recovery ramdisk (mksh/toybox under qemu-user; work/test/r, from test/recovery-chroot.py), on a fake
# UFS LUN (a loop device with 4096-byte sectors) holding what the VibeOS system zip would have left there:
#   LAYOUT  ab (boot_a, boot_b, no recovery: the OnePlus 6/6T, Pixel 3a) | aonly (boot + recovery: the POCO)
#   KIND    part   a partition "linux" that is the root (ext4, label vibeos)
#           nested a partition "linux" carrying a GPT of its own: ESP + root (the POCO's layout)
#           image  Android's data (ext4) with /lost+found/vibeos/rootfs.img
#   SLOT    _a | _b (ro.boot.slot_suffix, A/B only; default _a)
#   RUNS    1 | 2: the zip run twice (a second install over the first: the boot backup must stay the first's)
# The root is a stand-in for the system zip's (merged /usr, /etc/vibeos/system-image), not the real system image.
# The test copy of the zip differs from the real one in one line of vibeos-lib.sh (BY: the fake by-name links) and the
# updater-script's device check. Touches only work/test and its own loop devices.
set -u
export PATH=$PATH:/usr/sbin:/sbin
N=${NURA_WORK:?set NURA_WORK: a work directory for the tests (needs about 20 GB)}
W=$N/work/test
R=$W/r
DEVZIP=$(realpath $1) LAYOUT=$2 KIND=$3 SLOT=${4:-_a} RUNS=${5:-1}
[ $LAYOUT = aonly ] && SLOT=
D=$W/disk.img
F=$W/t-$LAYOUT-$KIND; rm -rf $F; mkdir -p $F
FAILS=0
ok() { echo "  ok: $*"; }
bad() { echo "  FAIL: $*"; FAILS=$((FAILS + 1)); }
L= L2=
cleanup() {
	set +e
	for p in /proc/[0-9]*; do case "$(sudo -n readlink $p/root 2>/dev/null)" in "$R"*) sudo -n kill -9 ${p#/proc/} 2>/dev/null ;; esac; done
	for m in $R/tmp/vibeos-mnt $R/data $F/m $F/u; do mountpoint -q $m && sudo -n umount $m; done
	for l in $(sudo -n losetup -a 2>/dev/null | grep -F "($W/" | cut -d: -f1); do sudo -n losetup -d $l 2>/dev/null; done
	sudo -n umount $R/proc/cmdline 2>/dev/null
	for m in dev sys proc; do mountpoint -q $R/$m && sudo -n umount $R/$m; done
	mountpoint -q $R/tmp && { sudo -n cp $R/tmp/vibeos.prop $F/ 2>/dev/null; sudo -n umount $R/tmp; }
	[ -n "$L" ] && sudo -n losetup -d $L 2>/dev/null
	sudo -n chown -R pulp:pulp $R/t $F 2>/dev/null
	rm -f $D $W/root.img
}
trap cleanup EXIT
echo "==== $(basename $DEVZIP) layout=$LAYOUT kind=$KIND slot=${SLOT:--} runs=$RUNS"
# the disk: 4096-byte sectors, the partitions a phone has around the ones the zips touch
rm -f $D; truncate -s 12G $D
L=$(sudo -n losetup -f --show --sector-size 4096 $D) || exit 1
if [ $LAYOUT = ab ]; then B1=boot_a B2=boot_b; else B1=boot B2=recovery; fi
sudo -n sgdisk -Z $L > /dev/null 2>&1
sudo -n sgdisk -n1:0:+8M -c1:efs -n2:0:+16M -c2:modem -n3:0:+8M -c3:persist -n4:0:+1M -c4:misc \
	-n5:0:+96M -c5:$B1 -n6:0:+96M -c6:$B2 -n7:0:+16M -c7:metadata -n8:0:+3G -c8:userdata -n9:0:+4G -c9:linux $L > /dev/null || exit 1
sudo -n losetup -d $L; L=$(sudo -n losetup -f --show -P --sector-size 4096 $D) || exit 1
sudo -n udevadm settle; sleep 1
# the partition nodes can lag (or a loop device be taken by someone else meanwhile): wait for them, or stop here
for i in $(seq 20); do [ -b ${L}p9 ] && break; sudo -n partx -u $L 2>/dev/null; sleep 0.5; done
[ -b ${L}p9 ] || { echo "  FAIL: the test disk's partitions did not appear on $L"; exit 1; }
p() { echo ${L}p$1; }
for n in 1 2 3 4 7; do sudo -n dd if=/dev/urandom of=$(p $n) bs=1M count=1 status=none; done
# the boot partitions: something Android-like to save and to keep
for n in 5 6; do { printf 'ANDROID!'; head -c 30000000 /dev/urandom; } | sudo -n dd of=$(p $n) bs=1M status=none; done
sumof() { sudo -n dd if=$1 bs=1M status=none | sha256sum | cut -d' ' -f1; }
declare -A BEFORE; for n in 1 2 3 4 5 6 7; do BEFORE[$n]=$(sumof $(p $n)); done
# the stand-in root: merged /usr, owned by root
S=$F/stage; mkdir -p $S/usr/bin $S/usr/lib $S/usr/sbin $S/etc/vibeos $S/var/lib/vibeos/device $S/usr/lib/modules/0.0.0-old $S/boot
ln -s usr/bin $S/bin; ln -s usr/lib $S/lib; ln -s usr/sbin $S/sbin
echo 'VERSION=1:0.1.3' > $S/etc/vibeos/system-image; echo vibeos > $S/etc/hostname
printf 'LABEL=vibeos / ext4 defaults,noatime 0 1\n' > $S/etc/fstab
mkdir -p $S/var/lib/dpkg
if [ -n "${OTHER_DEVICE:-}" ]; then
	printf 'Package: base-files\nStatus: install ok installed\n\nPackage: vibeos-device-poco\nStatus: install ok installed\nVersion: 0.1.3\n\n' > $S/var/lib/dpkg/status
else
	printf 'Package: base-files\nStatus: install ok installed\n\n' > $S/var/lib/dpkg/status
fi
find $S -type d -exec chmod 755 {} +; find $S -type f -exec chmod 644 {} +
rm -f $W/root.img; truncate -s 1500M $W/root.img
fakeroot mke2fs -q -t ext4 -b 4096 -I 256 -O ^orphan_file,^metadata_csum_seed -L vibeos -E root_owner=0:0 -d $S $W/root.img || exit 1
case $KIND in
part) sudo -n dd if=$W/root.img of=$(p 9) bs=4M conv=fsync status=none ;;
nested)
	L2=$(sudo -n losetup -f --show --sector-size 4096 $(p 9))
	printf 'label: gpt\nstart=256, size=30720, type=C12A7328-F81F-11D2-BA4B-00A0C93EC93B, name=esp\nstart=30976, size=655360, type=B921B045-1DF0-41C3-AF44-4C6F280D3FAE, name=root\n' |
		sudo -n sfdisk -q $L2 > /dev/null || exit 1
	sudo -n losetup -d $L2
	sudo -n dd if=$W/root.img of=$(p 9) bs=4096 seek=30976 conv=notrunc,fsync status=none ;;
image)
	sudo -n mkfs.ext4 -q -F $(p 8)
	mkdir -p $F/u; sudo -n mount $(p 8) $F/u
	sudo -n mkdir -p $F/u/lost+found/vibeos $F/u/media/0
	sudo -n cp --sparse=always $W/root.img $F/u/lost+found/vibeos/rootfs.img
	sudo -n truncate -s 3G $F/u/lost+found/vibeos/rootfs.img
	sudo -n umount $F/u ;;
esac
# the chroot: recovery's /tmp a tmpfs, by-name as ueventd made it, getprop's answers
sudo -n mount -t tmpfs -o size=2g,mode=1777 tmpfs $R/tmp
sudo -n mkdir -p $R/t/bin $R/t/by-name $R/data; sudo -n chown -R pulp:pulp $R/t $R/tmp
rm -rf $R/t/by-name/*
for n in 1:efs 2:modem 3:persist 4:misc 5:$B1 6:$B2 7:metadata 8:userdata; do ln -sfn $(p ${n%%:*}) $R/t/by-name/${n#*:}; done
[ $KIND = image ] || ln -sfn $(p 9) $R/t/by-name/linux
printf '#!/system/bin/sh\ncase "$1" in ro.boot.slot_suffix) echo "%s" ;; ro.product.device|ro.build.product) echo testdevice ;; esac\n' "$SLOT" > $R/t/bin/getprop
chmod 755 $R/t/bin/getprop
# the volume keys and power, for the zip's menu (KEYS="POWER" takes its first choice, "DOWN POWER" the second)
cat > $R/t/bin/getevent <<'KEYEOF'
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
KEYEOF
chmod 755 $R/t/bin/getevent
Y=$F/zip; mkdir -p $Y
(cd $Y && unzip -q -o $DEVZIP vibeos-lib.sh META-INF/com/google/android/updater-script)
python3 -I - $Y/vibeos-lib.sh <<'EOF'
import sys
p=sys.argv[1]; s=open(p).read()
a='\t\t[ -e "$b/$n" ] && { BY=$b; break 2; }\n\tdone\ndone\n'
assert s.count(a)==1; s=s.replace(a,a+'BY=/t/by-name\n'); open(p,'w').write(s)
EOF
U=$Y/META-INF/com/google/android/updater-script
sed -i -e 's#^assert(.*#assert("x" == "x" ||#' $U
cp $DEVZIP $R/tmp/dev.zip; chmod 644 $R/tmp/dev.zip
EXTRA=
# CURRENT_INSTALL=1: the converter's current install.sh instead of the zip's (to test it without a new conversion)
if [ -n "${CURRENT_INSTALL:-}" ]; then cp $N/tool/zip/nura-install.sh $Y/install.sh; EXTRA="$EXTRA install.sh"; fi
# BOOT_TARGET=...: the zip's boot target replaced (nura-device.prop)
if [ -n "${BOOT_TARGET:-}" ]; then
	unzip -p $DEVZIP nura-device.prop | sed "s/^boot_target=.*/boot_target=$BOOT_TARGET/" > $Y/nura-device.prop; EXTRA="$EXTRA nura-device.prop"
fi
(cd $Y && zip -q $R/tmp/dev.zip vibeos-lib.sh META-INF/com/google/android/updater-script $EXTRA)
unzip -p $DEVZIP META-INF/com/google/android/update-binary > $R/tmp/update-binary; chmod 755 $R/tmp/update-binary
for m in proc sys dev; do sudo -n mount --bind /$m $R/$m; done
# CMDLINE="...": what the bootloader passed, as recovery's /proc/cmdline shows it (the panel check)
if [ -n "${CMDLINE:-}" ]; then echo "$CMDLINE" > $F/cmdline; sudo -n mount --bind $F/cmdline $R/proc/cmdline; fi
# OTHER_DEVICE=1: the root has another phone's device layer installed (the zip must refuse it)
for run in $(seq 1 $RUNS); do
	T0=$SECONDS
	: > $R/t/keys; for k in ${KEYS:-POWER}; do echo $k >> $R/t/keys; done
	sudo -n chroot $R /system/bin/sh -c "cd /tmp && PATH=/t/bin:/system/bin exec /tmp/update-binary 3 1 /tmp/dev.zip" 2>$F/run$run.err | cat > $F/run$run.screen
	RC=${PIPESTATUS[0]}; [ $run = 1 ] && RC1=$RC
	echo "  run $run: update-binary exit $RC after $((SECONDS - T0)) s; screen:"
	sed -n 's/^ui_print \?//p' $F/run$run.screen | sed 's/^/  | /' | tail -n 20
	grep -v -E 'linker|realpath|^$' $F/run$run.err | tail -n 4 | sed 's/^/  stderr: /'
	if [ "${EXPECT:-}" = refused ]; then echo "  run $run: stopped ($RC), as it should"
	else [ $RC = 0 ] && ok "run $run: the zip succeeded" || bad "run $run failed: $(grep '^error=' $R/tmp/vibeos.prop | tail -n1)"; fi
	[ $run = 1 ] && sudo -n cp $R/tmp/vibeos.prop $F/prop1
	sudo -n rm -f $R/tmp/vibeos.prop
done
mountpoint -q $R/proc/cmdline 2>/dev/null && sudo -n umount $R/proc/cmdline
for m in dev sys proc; do sudo -n umount $R/$m; done
# ---- checks from the host
prop() { grep "^$1=" $F/prop1 2>/dev/null | tail -n1 | cut -d= -f2-; }
BT=$(prop boot_target)
echo "  boot target: $BT"
for n in 1 2 3 7; do [ "$(sumof $(p $n))" = "${BEFORE[$n]}" ] || bad "partition $n (efs/modem/persist/metadata) changed"; done
ok "efs, modem, persist, metadata untouched"
grep -i -E '([0-9a-f]{2}:){5}[0-9a-f]{2}|imei|serial' $F/run*.screen && bad "an identifier on the screen" || ok "no identifiers on the screen"
BI=$(unzip -p $DEVZIP boot.img | wc -c)
case $BT in $B1) TN=5 ON=6 ;; $B2) TN=6 ON=5 ;; *) TN= ON= ;; esac
[ "${EXPECT:-}" = refused ] && TN= ON=
if [ -n "$TN" ]; then
	[ "$(sudo -n head -c $BI $(p $TN) | sha256sum)" = "$(unzip -p $DEVZIP boot.img | sha256sum)" ] && ok "$BT = boot.img ($BI bytes)" || bad "$BT is not boot.img"
	PS=$(( $(sudo -n blockdev --getsize64 $(p $TN)) ))
	[ "$(sudo -n tail -c $((PS - BI)) $(p $TN) | tr -d '\000' | wc -c)" = 0 ] && ok "the rest of $BT is zeros" || bad "the rest of $BT is not cleared"
	[ "$(sumof $(p $ON))" = "${BEFORE[$ON]}" ] && ok "the other boot partition untouched" || bad "the other boot partition changed"
else
	[ "$(sumof $(p 5))" = "${BEFORE[5]}" ] && [ "$(sumof $(p 6))" = "${BEFORE[6]}" ] && ok "$B1 and $B2 untouched" || bad "$B1 or $B2 changed"
fi
# the root, as the initramfs would map it
RD=
case $KIND in
part) RD=$(p 9) ;;
nested) RD=$(sudo -n losetup -f --show -o $((30976 * 4096)) --sizelimit $((655360 * 4096)) $(p 9)) ;;
image)
	# the zip leaves /data mounted, as recovery does: let go of it first
	mountpoint -q $R/data && sudo -n umount $R/data
	mkdir -p $F/u; sudo -n mount -o ro $(p 8) $F/u && RD=$(sudo -n losetup -f --show -r $F/u/lost+found/vibeos/rootfs.img)
	[ -n "$RD" ] || bad "cannot reach rootfs.img in userdata to check it" ;;
esac
dfs() { sudo -n debugfs -R "$1" $RD 2>/dev/null; }
blocks() { sudo -n dumpe2fs -h $1 2>/dev/null | sed -n 's/^Block count: *//p'; }
KV=$(unzip -p $DEVZIP nura-device.prop | sed -n 's/^kver=//p')
if [ -n "$RD" ] && [ "${EXPECT:-}" = refused ]; then
	# EXPECT=refused: the zip must stop with an error and leave the system as the system zip left it
	[ "$RC1" != 0 ] && ok "refused: $(prop error)" || bad "the zip went through: it should have refused"
	sudo -n e2fsck -fn $RD > $F/fsck.log 2>&1 && ok "e2fsck -fn clean" || bad "e2fsck: $(tail -n 3 $F/fsck.log)"
	[ "$(blocks $RD)" = "$(blocks $W/root.img)" ] && ok "the root was not grown ($(blocks $RD) blocks)" || bad "the root was grown"
	for x in /etc/vibeos/device /etc/vibeos/device-nura /var/lib/vibeos/device/debs /var/lib/vibeos/device/backup /usr/lib/modules/$KV; do
		dfs "stat $x" | grep -q '^Inode:' && bad "$x exists"
	done
	dfs "stat /usr/lib/modules/$KV" | grep -q '^Inode:' || ok "no device files, debs, backup or modules written"
elif [ -n "$RD" ]; then
	sudo -n e2fsck -fn $RD > $F/fsck.log 2>&1 && ok "e2fsck -fn clean" || bad "e2fsck: $(tail -n 3 $F/fsck.log)"
	[ "$(sudo -n blkid -o value -s LABEL $RD)" = vibeos ] && ok "label vibeos" || bad "label"
	[ "$(dfs 'cat /etc/vibeos/device')" = "device=$(unzip -p $DEVZIP nura-device.prop | sed -n 's/^device=//p')" ] && ok "/etc/vibeos/device: $(dfs 'cat /etc/vibeos/device')" || bad "/etc/vibeos/device"
	dfs 'stat /lib' | grep -q 'Type: symlink' && ok "/lib is still the usr/lib symlink" || bad "/lib is no symlink any more"
	m0=$FAILS
	for d in / /usr /etc /var /usr/lib /var/lib; do o=$(dfs "stat $d" | sed -n 's/.*User: *\([0-9]*\) *Group: *\([0-9]*\).*/\1:\2/p'); m=$(dfs "stat $d" | sed -n 's/.*Mode: *\([0-7]*\).*/\1/p'); [ "$o" = 0:0 ] && [ "$m" = 0755 ] || bad "$d is $o mode $m"; done
	[ $FAILS = $m0 ] && ok "/, /usr, /etc, /var, /usr/lib, /var/lib root's, 0755"
	DD=$(dfs "ls /var/lib/vibeos/device/debs" | tr -s ' ' '\n' | grep '\.deb$' | sort | tr '\n' ' ')
	WANT=$(unzip -Z1 $DEVZIP | sed -n 's|^debs/\(.*\.deb\)$|\1|p' | sort | tr '\n' ' ')
	[ "$DD" = "$WANT" ] && ok "debs for the first boot: $(echo $DD | wc -w)" || bad "debs: '$DD' want '$WANT'"
	for d in $DD; do [ "$(dfs "cat /var/lib/vibeos/device/debs/$d" | sha256sum)" = "$(unzip -p $DEVZIP debs/$d | sha256sum)" ] || bad "$d differs"; done
	NP=$(unzip -p $DEVZIP payload.tar | tar -t | grep -v '/$' | wc -l)
	X=$(unzip -p $DEVZIP payload.tar | tar -t | grep -v '/$' | grep -v ' ' | shuf -n 25 --random-source=<(yes))
	miss=0; for x in $X; do dfs "stat /$x" | grep -q '^Inode:' || { miss=$((miss + 1)); echo "    missing /$x"; }; done
	[ $miss = 0 ] && ok "payload unpacked (25 of its $NP files looked up)" || bad "$miss payload files missing"
	dfs "stat /usr/lib/modules/$KV/modules.dep" | grep -q '^Inode:' && ok "modules for $KV" || bad "no modules.dep for $KV"
	dfs "stat /usr/lib/modules/0.0.0-old" | grep -q '^Inode:' && ok "an earlier kernel's module directory left alone" || bad "/usr/lib/modules/0.0.0-old is gone"
	if [ -n "$TN" ]; then
		[ "$(dfs "cat /var/lib/vibeos/device/backup/$BT.img" | sha256sum | cut -d' ' -f1)" = "${BEFORE[$TN]}" ] && ok "backup/$BT.img = $BT as it was before the first run" || bad "backup/$BT.img is not the original $BT"
	fi
	dfs "cat /etc/vibeos/device-nura" | sed 's/^/    device-nura: /'
fi
case $KIND in nested) [ -n "$RD" ] && sudo -n losetup -d $RD ;; image) [ -n "$RD" ] && sudo -n losetup -d $RD; mountpoint -q $F/u && sudo -n umount $F/u ;; esac
echo "== $FAILS failures"
[ $FAILS = 0 ]
