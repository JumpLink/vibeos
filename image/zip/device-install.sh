#!/system/bin/sh
# install.sh find|install|boot: a VibeOS device zip, run by updater-script in LineageOS recovery after the VibeOS
# system zip (the same session, or any later one: then it updates the device layer of the system installed).
#
#   find     where the system zip put the root (this session's /tmp/vibeos.prop, else a partition named linux or
#            vibeos, else the image file), and that this device boots that kind of root
#   (updater-script then unpacks payload.tar and the debs to /tmp)
#   install  the root checked (e2fsck), grown by what goes in, mounted; payload.tar unpacked into it (the device
#            package's files and the firmware packages': kernel modules, firmware, units, settings), the debs into
#            /var/lib/vibeos/device/debs for the first boot (vibeos-device-setup installs them, then restarts once),
#            each read back against the zip's sums; unmounted and checked again
#   (updater-script then unpacks the boot pieces to /tmp)
#   boot     the device's boot pieces, each read back (see the device's part below)
#
# Writes nothing to efs, sec_efs, cpefs, radio, modem or persist, and prints no IMEI, serial, MAC or BD address.
ACTION=${1:-}
. /tmp/vibeos-lib.sh
set -u
DEV=@DEV@
SUMS=$T-dsums.txt
ROOT=$T-mnt
case $DEV in
s10e) KINDS=part ;;		# U-Boot boots the partition named linux
poco) KINDS=nested ;;		# U-Boot (in BOOT) boots the GPT inside the partition named linux
s5e)  KINDS=image ;;		# the initramfs maps the image file in Android's data by its extents
s20)  KINDS=part ;;		# the initramfs (in the kernel U-Boot starts from BOOT) boots the partition named vibeos
esac
# the power-on logo (s10e-logo: up_param's logo.jpg and the two unlocked-bootloader warnings from boot-logo/, every
# other picture kept; never a reason to fail): the S10e's and the S20's
put_logo() {
	chmod 755 $T-logo
	[ -b $BY/up_param ] || return 0
	OUT=$($T-logo apply $BY/up_param $T-boot-logo 2> $T-logo.err)
	case $?:$OUT in
	0:same) say "  the power-on logo is Debian's already" ;;
	0:written*) say "  Debian's power-on logo" ;;
	1:*) say "  power-on logo left as it was: $(cat $T-logo.err)" ;;
	*) say "  WARNING: the power-on logo did not write: $(cat $T-logo.err)" ;;
	esac
}

# the Tab S5e's root, an image file in Android's data, is opened the way its initramfs opens it: dm-linear over the
# file's extents on userdata, never through Android's file system (Linux writes to those blocks directly, and the
# file system still calls the ones fallocate gave it unwritten: read through it they are zeros). The map holds
# userdata: while it exists /data cannot be mounted in this recovery session (this zip again, the system zip's image
# choice, recovery itself), so close_image_raw removes it at the end and on every way out
DMN=
close_image_raw() {
	[ -n "$DMN" ] || return 0
	cd /
	$T-dmlinear -r $DMN 2> $T-dmr.err || return 1
	DMN=; rm -f $T-dm
}
open_image_raw() {
	mount_data || die "cannot mount Android's data"
	[ -s "$IMGF" ] || die "no $IMGF"
	lsattr "$IMGF" 2>/dev/null | cut -d' ' -f1 | grep -q i || die "$IMGF is not pinned (chattr +i): install the VibeOS system zip again"
	chmod 755 $T-extents $T-dmlinear
	B=$(stat -c %s "$IMGF")
	$T-extents "$IMGF" $B > $T-extents.txt 2> $T-extents.err || die "extents: $(cat $T-extents.err)"
	[ $(stat -c %s $T-extents.txt) -lt 524288 ] || die "$(($(wc -l < $T-extents.txt) - 2)) extents do not fit in 512 KiB"
	sed '1d;/^END$/d' $T-extents.txt > $T-extents.map
	sync
	umount /data 2>/dev/null; umount /data 2>/dev/null
	! data_mounted || die "Android's data is still mounted"
	MM=$($T-dmlinear vibeos$$ "$(readlink -f $BY/userdata)" $T-extents.map 2> $T-dm.err) || die "dmlinear: $(cat $T-dm.err)"
	DMN=vibeos$$
	trap close_image_raw EXIT
	rm -f $T-dm; mknod $T-dm b ${MM%%:*} ${MM##*:} || die "cannot make the root's device node"
	RDEV=$T-dm
}

case "$ACTION" in
find)
	[ -n "$BY" ] || die "no partitions by name"
	# "ready" is only this run's (updater-script reads it: the S20's first run below)
	[ ! -f $P ] || { grep -v '^ready=' $P > $T-prop.new; cat $T-prop.new > $P; rm -f $T-prop.new; }
	find_root
	if [ -z "$KIND" ] && [ $DEV = s20 ]; then
		# The S20 FE's first run, before the system zip: the phone checked (userdata, a BOOT that holds the boot
		# image, up_param) and nothing written to it; S20READY lets the system zip offer the whole phone (see
		# system-install.sh), and the second run of this zip, after the system zip, installs
		[ -e $BY/userdata ] || die "no VibeOS system and no userdata: nothing this zip can install onto"
		b=$(part boot) || die "no boot partition"
		IB=$(grep '^boot ' $SUMS | cut -d' ' -f2)
		[ -n "$IB" ] && [ $(($(blocks4k $b) * 4096)) -ge $IB ] || die "the boot partition is smaller than the boot image"
		[ -b $BY/up_param ] || say "  (no up_param: the power-on logo will stay as it is)"
		echo device=s20 > /tmp/vibeos-ready-s20 || die "cannot write /tmp/vibeos-ready-s20"
		say "  this phone can take VibeOS; nothing was changed"
		put ready "Now: Apply update, the VibeOS system zip (Whole device: it ERASES Android data), then this zip again."
		exit 0
	fi
	[ -n "$KIND" ] || die "no VibeOS system on this device: install the VibeOS system zip first"
	what() { case $1 in image) echo "an image file" ;; nested) echo "the root inside a partition's own GPT" ;; *) echo "a partition" ;; esac; }
	[ "$KIND" = "$KINDS" ] || die "the system is in $(what $KIND) ($NAME$IMGF); this device boots only $(what $KINDS): install the VibeOS system zip again and choose that. Nothing was changed"
	[ $DEV != s10e ] || [ "$NAME" = linux ] || die "the system is in '$NAME'; the S10e boots only the partition named linux"
	[ $DEV != s20 ] || [ "$NAME" = vibeos ] || die "the system is in '$NAME'; the S20 FE boots only the partition named vibeos"
	put kind $KIND; put name "$NAME"; put image "$IMGF"; put dev "$DEVP"
	say "  the system: $(case $KIND in image) echo "$IMGF" ;; nested) echo "the root inside '$NAME'" ;; *) echo "partition '$NAME'" ;; esac)"
	;;
install)
	KIND=$(get kind) NAME=$(get name) IMGF=$(get image) DEVP=$(get dev)
	[ -s $T-payload.tar ] || die "payload.tar was not unpacked"
	check $T-payload.tar payload $SUMS
	# nested: the root is a loop device over its place in the partition (loops_on: see vibeos-lib.sh)
	[ $KIND != nested ] || loops_on
	if [ $KIND = image ]; then open_image_raw; else open_root; fi
	[ $KIND = image ] || guard "$ODEV"
	say "  checking the system"
	fsck_root $RDEV
	# room for the payload and the debs, and 256 MiB (the first boot grows it to the whole partition or file)
	K=0; for k in $(du -sk $T-payload.tar $T-debs | cut -f1); do K=$((K + k)); done
	grow $RDEV $((K / 4 + 65536))
	mkdir -p $ROOT
	# noinit_itable: as in the system zip (no lazy inode-table zeroing while it is mounted here)
	mount -t ext4 -o noatime,noinit_itable $RDEV $ROOT || die "cannot mount the system"
	# -D: toybox umount would otherwise send LOOP_CLR_FD to the device
	fail() { cd /; umount -D $ROOT; close_root; die "$@"; }
	[ -s $ROOT/etc/vibeos/system-image ] || fail "this is not a VibeOS system"
	say "  kernel modules, firmware, device settings"
	unpack $T-payload.tar $ROOT || fail "cannot unpack the device files"
	D=$ROOT/var/lib/vibeos/device
	rm -rf $D/debs
	mkdir -p $D/debs || fail "cannot create /var/lib/vibeos/device"
	rm -f $D/result
	cp $T-debs/*.deb $D/debs/ || fail "cannot copy the device packages"
	sync
	n=0
	for f in $(grep '^deb ' $SUMS | cut -d' ' -f2); do
		[ "$(sha256sum < $D/debs/$f | cut -d' ' -f1)" = "$(grep "^deb $f " $SUMS | cut -d' ' -f3)" ] ||
			fail "$f was not copied correctly"
		n=$((n + 1))
	done
	[ $n = $(ls $D/debs | wc -l) ] || fail "the packages copied are not the zip's ($n in its sums)"
	echo "device=$DEV" > $ROOT/etc/vibeos/device || fail "cannot write /etc/vibeos/device"
	sync
	umount -D $ROOT || die "cannot unmount the system"
	fsck_root $RDEV
	if [ $KIND = image ]; then
		close_image_raw || die "cannot remove the map over Android's data ($(cat $T-dmr.err)): restart recovery before anything else uses /data"
	else
		close_root
	fi
	loops_off
	say "  $n packages for the first boot"
	;;
boot)
	KIND=$(get kind) NAME=$(get name)
	case $DEV in
	s10e)
		# The power-on logo (s10e-logo: up_param's logo.jpg and the two unlocked-bootloader warnings from
		# boot-logo/, every other picture kept; not a reason to fail). Then U-Boot to boot when Android is gone
		# (no userdata: a whole-phone install), and to recovery, last: until then a failure leaves LineageOS
		# recovery to try again. Each read back.
		put_logo
		UB=$(($(grep '^uboot ' $SUMS | cut -d' ' -f2) / 4096))
		[ $UB -gt 0 ] || die "no sum for uboot"
		# (in the session of a whole-device install userdata keeps its by-name link until the next boot)
		B=; { [ "$(get mode)" = whole ] || [ ! -e $BY/userdata ]; } && B=boot
		for p in $B recovery; do
			say "  U-Boot to $p"
			b=$(part $p) || die "no $p partition"
			guard $b
			n=$(blocks4k $b)
			[ $n -ge $UB ] || die "the $p partition ($n blocks) is smaller than U-Boot ($UB)"
			dd if=$T-uboot.img of=$b bs=1048576 2>/dev/null || die "cannot write U-Boot to $p"
			dd if=/dev/zero of=$b bs=4096 seek=$UB count=$((n - UB)) 2>/dev/null || die "cannot clear the rest of $p"
			check $b uboot $SUMS
		done
		;;
	poco)
		# The ESP inside "linux" (kernel 7.2.15-22surya, its initramfs, the DTBs, systemd-boot: the k22 ROM's,
		# booting root=LABEL=vibeos); then BOOT = U-Boot (in LineageOS 22.2 20261102's boot.img: its header and DTB,
		# U-Boot as the kernel), last, so that every power-on starts VibeOS. ABL lays the dtbo PARTITION's overlay
		# over that DTB, under vbmeta: both must be that build's (its vbmeta has flags 3, verification off), and are
		# written first where they are not already. The recovery partition is not written: LineageOS's recovery
		# stays there, for the next sideload (Volume Up + Power).
		d=$(get dev); [ -b "$d" ] || d=$(part $NAME) || die "no partition $NAME"
		guard $d
		set -- $(nested_esp $d)
		[ $# = 3 ] && [ $1 = 4096 ] || die "no EFI system partition inside $NAME"
		EB=$(grep '^esp ' $SUMS | cut -d' ' -f2)
		[ "$(expr \( $3 - $2 + 1 \) \* 4096 \>= $EB)" = 1 ] || die "the ESP inside $NAME is smaller than the zip's"
		say "  kernel and boot loader (the ESP inside '$NAME')"
		dd if=$T-esp.img of=$d bs=4096 seek=$2 conv=notrunc 2>/dev/null || die "cannot write the ESP"
		sync; blockdev --flushbufs $d 2>/dev/null
		[ "$(dd if=$d bs=4096 skip=$2 count=$(expr $EB / 4096) 2>/dev/null | sha256sum | cut -d' ' -f1)" = "$(grep '^esp ' $SUMS | cut -d' ' -f3)" ] ||
			die "the ESP was not written correctly"
		# put_img NAME PART WHAT: the zip's NAME image at the start of partition PART, the rest of it zeros, read
		# back; nothing written when the partition holds exactly that already
		put_img() {
			b=$(part $2) || die "no $2 partition"
			guard $b
			IB=$(grep "^$1 " $SUMS | cut -d' ' -f2)
			[ -n "$IB" ] || die "no sum for $1"
			n=$(blocks4k $b)
			[ $((n * 4096)) -ge $IB ] || die "the $2 partition is smaller than the zip's $1 ($IB bytes)"
			if [ "$(head -c $IB $b | sha256sum | cut -d' ' -f1)" = "$(grep "^$1 " $SUMS | cut -d' ' -f3)" ] &&
				[ "$(tail -c +$((IB + 1)) $b | tr -d '\000' | wc -c)" = 0 ]; then
				say "  $3 in $2: there already"
				return 0
			fi
			say "  $3 to $2"
			dd if=$T-$1.img of=$b bs=1048576 2>/dev/null || die "cannot write $2"
			[ $((n * 4096)) = $IB ] || dd if=/dev/zero of=$b bs=4096 seek=$((IB / 4096)) count=$((n - IB / 4096)) 2>/dev/null ||
				die "cannot clear the rest of $2"
			check $b $1 $SUMS
		}
		put_img dtbo dtbo "LineageOS 20261102's dtbo"
		put_img vbmeta vbmeta "LineageOS 20261102's vbmeta"
		put_img boot boot "U-Boot"
		# a recovery partition still holding the U-Boot an earlier VibeOS put there: LineageOS's recovery goes
		# back in by hand (only the earlier VibeOS can be started from it)
		r=$(part recovery) && OB=$(grep '^oldrecovery ' $SUMS | cut -d' ' -f2) && [ -n "$OB" ] &&
			[ "$(head -c $OB $r | sha256sum | cut -d' ' -f1)" = "$(grep '^oldrecovery ' $SUMS | cut -d' ' -f3)" ] &&
			say "  NOTE: recovery still holds the earlier U-Boot: flash LineageOS's recovery.img there with fastboot"
		;;
	s20)
		# The power-on logo, then BOOT, last (until then a failure leaves BOOT as it was, and LineageOS Recovery to
		# try again): the boot image (the stock header and DT table, the shim with U-Boot as its kernel, the FIT of
		# Linux, its initramfs built in, as its ramdisk), the rest of BOOT zeros, read back. RECOVERY is not written:
		# LineageOS Recovery stays there, the way back (U-Boot's menu: Volume Down at power-on). Nor dtbo: S-Boot
		# lays it over that DT table and hands U-Boot the memory and bootargs from the result
		put_logo
		b=$(part boot) || die "no boot partition"
		guard $b
		IB=$(grep '^boot ' $SUMS | cut -d' ' -f2)
		[ -n "$IB" ] && [ $((IB % 4096)) = 0 ] || die "no sum for boot"
		n=$(blocks4k $b)
		[ $((n * 4096)) -ge $IB ] || die "the boot partition ($n blocks) is smaller than the boot image ($IB bytes)"
		say "  U-Boot and Linux to boot"
		dd if=$T-boot.img of=$b bs=1048576 2>/dev/null || die "cannot write the boot partition"
		[ $((n * 4096)) = $IB ] || dd if=/dev/zero of=$b bs=4096 seek=$((IB / 4096)) count=$((n - IB / 4096)) 2>/dev/null ||
			die "cannot clear the rest of boot"
		check $b boot $SUMS
		;;
	s5e)
		# The recovery partition: U-Boot (dual boot) and LineageOS recovery in its first 34 MiB, Linux (kernel
		# 7.2.15-gts4lv-uc, its initramfs, the DT) at 34 MiB, the rest cleared, and the image file's extent list
		# in its last 512 KiB, where the initramfs reads it. Then keepmisc holds "boot-recovery" in misc until
		# the restart, so that "Reboot system now" starts U-Boot (and Linux) and not Android.
		b=$(part recovery) || die "no recovery partition"
		guard $b
		[ -s $T-extents.txt ] || die "no extent list (install step missing)"
		RB=$(grep '^recovery ' $SUMS | cut -d' ' -f2)
		S=$(($(sectors512 $b) - 1024))
		[ $((RB / 512)) -le $S ] || die "the recovery partition is too small"
		say "  U-Boot, LineageOS recovery and Linux to recovery"
		dd if=$T-recovery.img of=$b bs=1048576 2>/dev/null || die "cannot write the recovery partition"
		dd if=/dev/zero of=$b bs=512 seek=$((RB / 512)) count=$((S - RB / 512 + 1024)) 2>/dev/null || die "cannot clear recovery"
		dd if=$T-extents.txt of=$b bs=512 seek=$S conv=notrunc 2>/dev/null || die "cannot write the extent list"
		sync
		check $b recovery $SUMS
		[ "$(dd if=$b bs=512 skip=$S count=1024 2>/dev/null | tr -d '\000' | sha256sum | cut -d' ' -f1)" = "$(sha256sum < $T-extents.txt | cut -d' ' -f1)" ] ||
			die "the extent list was not written correctly"
		say "  $(($(wc -l < $T-extents.txt) - 2)) extents"
		# a keepmisc an earlier zip of this session left running stops first
		chmod 755 $T-km && $T-km stop $BY/misc 2>/dev/null
		rm -f $T-km-run && cp $T-km $T-km-run && chmod 755 $T-km-run || die "keepmisc was not unpacked"
		$T-km-run start $BY/misc $BY/userdata > /dev/null 2> $T-km.err || die "keepmisc: $(cat $T-km.err)"
		[ "$(dd if=$BY/misc bs=32 count=1 2>/dev/null | tr -d '\000')" = boot-recovery ] || die "misc does not say boot-recovery"
		;;
	esac
	sync
	;;
*) die "usage: install.sh find|install|boot" ;;
esac
exit 0
