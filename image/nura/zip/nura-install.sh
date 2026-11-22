#!/system/bin/sh
# install.sh find|install|boot: a VibeOS device zip made by nura-to-vibeos from a Nura (postmarketOS) build, run by
# updater-script in LineageOS recovery after the VibeOS system zip (the same session, or any later one: then it
# replaces the device layer of the system installed). VibeOS 0.1.3's device-install.sh, for any Nura device:
#
#   find     where the system zip put the root (this session's /tmp/vibeos.prop, else a partition named linux or
#            vibeos, nested when it carries a GPT, else the image file), and where the boot image goes
#            (nura-device.prop's boot_target: auto = the running slot's boot on A/B devices, recovery where there
#            is one, else boot; ask = a menu of that or nowhere; boot, recovery, other-slot, none)
#   (updater-script then unpacks payload.tar and the debs to /tmp)
#   install  the root checked (e2fsck), grown by what goes in, mounted; payload.tar unpacked into it (Nura's kernel
#            modules and firmware, the device's settings), the debs into /var/lib/vibeos/device/debs for the first
#            boot (vibeos-device-setup installs them, then restarts once), each read back against the zip's sums;
#            the boot partition about to be written saved first into the root, once:
#            /var/lib/vibeos/device/backup/<partition>.img; unmounted and checked again
#   (updater-script then unpacks boot.img to /tmp)
#   boot     boot.img (Nura's kernel and DTB, VibeOS's initramfs, which finds the root the same way find does)
#            to the boot target, the rest of the partition cleared, read back
#
# Writes nothing to efs, sec_efs, cpefs, radio, modem or persist, and prints no IMEI, serial, MAC or BD address.
ACTION=${1:-}
. /tmp/vibeos-lib.sh
set -u
NP=/tmp/vibeos-nura.prop
nget() { grep "^$1=" $NP 2>/dev/null | tail -n 1 | cut -d= -f2-; }
DEV=$(nget device)
SUMS=$T-dsums.txt
ROOT=$T-mnt
KINDS="part nested image"
SLOT=$(getprop ro.boot.slot_suffix)

# the partition the boot image goes to, by the zip's boot_target
boot_target() {
	case ${1:-$(nget boot_target)} in
	auto|ask|'')
		if [ -n "$SLOT" ] && part boot$SLOT > /dev/null; then echo boot$SLOT
		elif part recovery > /dev/null; then echo recovery
		else echo boot; fi ;;
	boot) if [ -n "$SLOT" ]; then echo boot$SLOT; else echo boot; fi ;;
	recovery) echo recovery ;;
	other-slot)
		case $SLOT in _a) echo boot_b ;; _b) echo boot_a ;; *) echo "-" ;; esac ;;
	none) echo none ;;
	*) echo "-" ;;
	esac
}
# raw userdata is a plain ext4 (no metadata encryption): the initramfs can mount it to reach the image file
userdata_plain_ext4() {
	u=$(part userdata) || return 1
	[ "$(od -An -tx1 -j 1080 -N 2 $u 2>/dev/null | tr -d ' \n')" = 53ef ]
}

case "$ACTION" in
find)
	[ -n "$BY" ] || die "no partitions by name"
	[ -s $NP ] || die "nura-device.prop was not unpacked"
	find_root
	[ -n "$KIND" ] || die "no VibeOS system on this device: install the VibeOS system zip first"
	case " $KINDS " in *" $KIND "*) ;; *) die "the system is in a $KIND root; this zip boots $KINDS" ;; esac
	if [ $KIND = image ] && ! userdata_plain_ext4; then
		die "the system is an image file in Android's data, and that data is encrypted below the file system (metadata encryption) or not ext4: Linux cannot reach the file. Install the VibeOS system zip again and choose a partition"
	fi
	# a phone with more than one panel: the DTB in boot.img is for one of them, and the bootloader names this
	# phone's on the command line it gives every boot image, recovery's too (msm_drm.dsi_display0=...)
	PN=$(nget panel)
	if [ -n "$PN" ]; then
		DSI=$(tr ' ' '\n' < /proc/cmdline | sed -n 's/^msm_drm\.dsi_display0=//p' | head -n 1 | sed 's/:*$//')
		case "$DSI" in
		'') say "  (the bootloader names no panel: this zip is for the $PN one)" ;;
		*"$PN"*) say "  panel: $PN" ;;
		*) die "this zip's kernel is for the $PN panel; this phone has $DSI: make the zip again with --panel for it. Nothing was changed" ;;
		esac
	fi
	BT=$(boot_target)
	if [ "$(nget boot_target)" = ask ]; then
		label() {
			case $1 in
			none) echo "Nowhere: I start it once with fastboot boot" ;;
			recovery) echo "recovery (what it holds now is saved first)" ;;
			*) echo "$1, this slot's boot (saved first)" ;;
			esac
		}
		menu "Where does the VibeOS boot image go?" $BT none
		BT=$o
	fi
	case $BT in
	boot|boot_a|boot_b|recovery) b=$(part $BT) || die "no partition $BT (boot target $(nget boot_target))"; guard $b ;;
	none) ;;
	*) die "boot target $(nget boot_target): not possible here (slot '$SLOT')" ;;
	esac
	# the boot slot's dtbo, on a device whose table row says clear (nura-device.prop's dtbo)
	DT=; [ "$(nget dtbo)" != clear ] || DT=$(dtbo_for $BT)
	[ -z "$DT" ] || guard $(part $DT)
	put kind $KIND; put name "$NAME"; put image "$IMGF"; put dev "$DEVP"; put boot_target $BT; put dtbo_target "$DT"
	say "  the system: $(case $KIND in image) echo "$IMGF" ;; nested) echo "the root inside '$NAME'" ;; *) echo "partition '$NAME'" ;; esac)"
	say "  the boot image goes to: $(case $BT in none) echo "nowhere (fastboot boot it)" ;; *) echo "$BT" ;; esac)"
	[ -z "$DT" ] || say "  $DT is cleared after it (Android's overlays stop this kernel from booting)"
	;;
install)
	KIND=$(get kind) NAME=$(get name) IMGF=$(get image) DEVP=$(get dev) BT=$(get boot_target) DT=$(get dtbo_target)
	[ -s $T-payload.tar ] || die "payload.tar was not unpacked"
	check $T-payload.tar payload $SUMS
	open_root
	[ $KIND = image ] || guard "$ODEV"
	say "  checking the system"
	fsck_root $RDEV
	mkdir -p $ROOT
	# -D: toybox umount would otherwise send LOOP_CLR_FD to the device
	fail() { cd /; umount -D $ROOT; close_root; die "$@"; }
	# read-only first: a system this zip must not go onto is left exactly as it was
	mount -t ext4 -o ro $RDEV $ROOT || die "cannot mount the system"
	[ -s $ROOT/etc/vibeos/system-image ] || fail "this is not a VibeOS system. Nothing was changed"
	# another phone's device layer, installed: its files and this one's would collide at the first boot's dpkg
	OTHER=$(sed -n '/^Package: /h; /^Status: install ok installed/{x;s/^Package: //p;}' $ROOT/var/lib/dpkg/status 2>/dev/null |
		grep '^vibeos-device-' | grep -v -x "vibeos-device-$DEV" | head -n 1)
	[ -z "$OTHER" ] || fail "this system has another device layer installed ($OTHER): install the VibeOS system zip again first. Nothing was changed"
	umount -D $ROOT || die "cannot unmount the system"
	# room for the payload, the debs, the copies of a boot partition and its dtbo, and 256 MiB (the first boot grows
	# the root)
	K=0; for k in $(du -sk $T-payload.tar $T-debs | cut -f1); do K=$((K + k)); done
	BB=0; for x in $BT $DT; do [ $x = none ] || BB=$((BB + $(sectors512 $(part $x)) / 2)); done
	grow $RDEV $(((K + BB) / 4 + 65536))
	mount -t ext4 -o noatime $RDEV $ROOT || die "cannot mount the system"
	say "  kernel modules, firmware, device settings"
	tar -C $ROOT -xf $T-payload.tar || fail "cannot unpack the device files"
	[ -L $ROOT/lib ] || fail "the payload replaced /lib (the root's symlink)"
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
	# the boot partition as it is now (Android's, or LineageOS recovery's), and the dtbo cleared after it, kept
	# once: the first install's copy is the one to go back to
	for x in $BT $DT; do
		[ $x != none ] || continue
		mkdir -p $D/backup && chmod 700 $D/backup
		if [ ! -s $D/backup/$x.img ]; then
			say "  saving $x (to /var/lib/vibeos/device/backup/$x.img in the system)"
			dd if=$(part $x) of=$D/backup/$x.img bs=1048576 2>/dev/null || fail "cannot save $x"
			sha256sum < $D/backup/$x.img | cut -d' ' -f1 > $D/backup/$x.img.sha256
			[ "$(sha256sum < $(part $x) | cut -d' ' -f1)" = "$(cat $D/backup/$x.img.sha256)" ] || fail "the copy of $x differs"
		else
			say "  $x was saved by an earlier install: kept"
		fi
	done
	echo "device=$DEV" > $ROOT/etc/vibeos/device || fail "cannot write /etc/vibeos/device"
	{ echo "boot_target=$BT"; echo "slot=$SLOT"; grep -v '^boot_target=' $NP; } > $ROOT/etc/vibeos/device-nura || fail "cannot write /etc/vibeos/device-nura"
	sync
	umount -D $ROOT || die "cannot unmount the system"
	fsck_root $RDEV
	close_root
	say "  $n packages for the first boot"
	;;
boot)
	BT=$(get boot_target) DT=$(get dtbo_target)
	[ -s $T-boot.img ] || die "boot.img was not unpacked"
	check $T-boot.img bootimg $SUMS
	case $BT in
	none)
		put done "Nothing written to a boot partition: start VibeOS once with: fastboot boot (this zip's boot.img)"
		say "  boot image: not written (boot target none)"
		;;
	boot|boot_a|boot_b|recovery)
		b=$(part $BT) || die "no partition $BT"
		guard $b
		BI=$(grep '^bootimg ' $SUMS | cut -d' ' -f2)
		n=$(blocks4k $b)
		[ $n -ge $(((BI + 4095) / 4096)) ] || die "the $BT partition ($n blocks) is smaller than the boot image"
		say "  boot image to $BT"
		dd if=$T-boot.img of=$b bs=1048576 2>/dev/null || die "cannot write $BT"
		# the rest cleared: an old AVB footer at its end would describe the image that was there
		S=$(((BI + 4095) / 4096))
		dd if=/dev/zero of=$b bs=4096 seek=$S count=$((n - S)) 2>/dev/null || die "cannot clear the rest of $BT"
		check $b bootimg $SUMS
		# only now: until boot holds VibeOS's kernel, what is in it may need this dtbo (LineageOS recovery does)
		if [ -n "$DT" ]; then
			say "  clearing $DT"
			guard $(part $DT)
			zero $(part $DT) || die "cannot clear $DT: fastboot erase $DT, or VibeOS stops at the fastboot screen"
		fi
		case $BT in
		recovery) put done "Restart to recovery starts VibeOS; a normal start, Android. What recovery held is saved in VibeOS: /var/lib/vibeos/device/backup/recovery.img (to flash zips again: fastboot boot a LineageOS recovery)" ;;
		boot) put done "Reboot system now starts VibeOS. Android's boot is saved in VibeOS: /var/lib/vibeos/device/backup/boot.img" ;;
		*) if [ "boot$SLOT" = $BT ]; then
			put done "Reboot system now starts VibeOS (slot $SLOT). LineageOS (and its recovery) is saved in VibeOS: /var/lib/vibeos/device/backup/$BT.img${DT:+, and $DT, which it needs back too: $DT.img}"
		   else
			put done "VibeOS is in $BT, the other slot: fastboot --set-active=${BT#boot_} starts it; fastboot --set-active=${SLOT#_} goes back"
		   fi ;;
		esac
		;;
	*) die "boot target '$BT'" ;;
	esac
	sync
	;;
*) die "usage: install.sh find|install|boot" ;;
esac
exit 0
