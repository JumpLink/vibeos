#!/system/bin/sh
# install.sh choose|prepare|chunks|finish: the VibeOS system zip (one root file system for every device), run by
# updater-script in LineageOS recovery. The device's own zip comes right after it, in the same recovery session.
#
#   choose   two questions, each a short menu (Volume Down / Up move the ">", Power chooses), answers to /tmp/vibeos.prop:
#            1. where:  existing  the partition named "linux" (or "vibeos") an earlier install made: only the system
#                                 in it is replaced (its users' files too); Android and its data stay
#                       new       a NEW partition "linux" carved from the end of userdata; Android stays, its data
#                                 is erased (a shrunk userdata cannot be kept)
#                       image     a file in Android's data, /data/lost+found/vibeos/rootfs.img; Android and its
#                                 data stay (an earlier Linux image file there is replaced)
#                       whole     all of userdata becomes "linux": Android's data is gone (asks for a second Power)
#            2. size:   new and image only (GB)
#            Only what this device's zip can boot is offered (devices this zip knows; any other: everything).
#            Nothing pressed for two minutes: cancel, nothing written.
#            The POCO X3 NFC asks nothing: a partition "linux" that carries its own GPT (an earlier install's) is used
#            as it is (existing: only the root in it is replaced, the table and Android's data are not touched); with
#            none, all of userdata becomes "linux" (whole) and gets that GPT, as the POCO's U-Boot boots it: Android
#            and its data are gone. A "linux" without a GPT of its own (an install cut short between the two) gets it.
#   prepare  new, whole: unmount Android's data (refused while something reads from it, such as this zip on the
#            internal storage), then carve or take over the partition; image: make room for the file, the Tab S5e's
#            extent list at the end of recovery cleared first (clear_extents: it names the old file's blocks). Points
#            /tmp/vibeos-root at where the root goes (a partition, or the root partition inside a "linux" that
#            carries its own GPT, through a loop device) and clears its first and last MiB (wipe_ends: an earlier
#            system's GPT or file system there must not outlive the install). The POCO's whole: the GPT tool writes
#            the partition's own GPT (nest: the ESP of ESP_MIB, the root behind it to the end) first
#   chunks   maps each part of rootfs.img (rootfs.img.0, .1, ...: LineageOS's updater cannot extract a zip entry
#            of 4 GiB or more) to /tmp/vibeos-root.N, a loop device over its bytes of the root's place (the
#            partition, or the image file, made to the root's size first)
#   (updater-script then writes each part there)
#   abort    after a part could not be written: the loop devices released, SELinux as prepare found it
#   finish   read the root back against the zip's sums, e2fsck it, record where it went (/etc/vibeos/installed-to
#            in it, /tmp/vibeos.prop for the device zip); image: the file grown to its size and pinned (chattr +i);
#            new: Android's data and /metadata formatted as LineageOS's "Format data" does.
#
# Writes nothing to efs, sec_efs, cpefs, radio, modem or persist, and prints no IMEI, serial, MAC or BD address.
ACTION=${1:-}
. /tmp/vibeos-lib.sh
set -u
SUMS=$T-sums.txt
ROOT=$T-mnt
SIZES_NEW="10 20 30 50"
SIZES_IMG="10 20 30 40 50 100"
DEVICE=$(getprop ro.product.device); [ -n "$DEVICE" ] || DEVICE=$(getprop ro.build.product)
# what each device's zip boots: the S10e's U-Boot a partition named linux; the POCO's U-Boot the GPT inside a
# linux partition (which only the POCO's own zip makes); the Tab S5e's initramfs a file in Android's data, mapped
# by its extents; the S20 FE's U-Boot the partition named vibeos (it has no zip of its own: no LineageOS recovery
# runs on it, s20-install-from-pc.sh installs it). Any other device: everything is offered.
AUTO=
case $DEVICE in
beyond0lte|beyond0) KINDS="existing new whole" ;;
surya|karna)        KINDS="nested"; AUTO=1 ;;	# no questions (owner, 2026-11-23): see choose
gts4lvwifi|gts4lv)  KINDS="image" ;;
r8s|r8sxx)          KINDS="existing" ;;	# the S20 FE: its partition named vibeos only (userdata is Lineage's)
*)                  KINDS="existing nested new image whole" ;;
esac
can() { case " $KINDS " in *" $1 "*) return 0 ;; esac; return 1; }
ESP_MIB=480	# the POCO's ESP, as its k22 image has it (blocks 2048-124927 of 4 KiB): the device zip writes esp.img there
VP=$T-part
# the root file system's size in the zip, as a string (over 2 GiB)
RB=$(grep '^rootfs ' $SUMS 2>/dev/null | cut -d' ' -f2)
fits() { [ "$(expr "$1" \>= "$RB")" = 1 ]; }	# fits BYTES
# partition N of DISK: sda31, mmcblk0p31, loop0p31
pdev() { case $DISK in *[0-9]) echo ${DISK}p$1 ;; *) echo $DISK$1 ;; esac; }
unmount_android() {
	for m in $(cut -d' ' -f2 /proc/mounts | grep -E '^/(data|sdcard|storage|metadata)(/|$)' | sort -r); do
		umount $m 2>/dev/null && continue
		case $m in /metadata) umount -l $m 2>/dev/null && continue ;; esac
		die "cannot unmount $m: something reads from it. Is this zip on the internal storage ($ZIP)? This install erases it: use adb sideload"
	done
	! cut -d' ' -f2 /proc/mounts | grep -q -E '^/(data|metadata)(/|$)' || die "Android's data is still mounted"
}
# an existing linux/vibeos partition: EX=name, EXK=part|nested, EXB=bytes the root gets there
existing() {
	EX= EXK= EXB=
	for n in linux vibeos; do
		d=$(part $n) || continue
		EX=$n
		set -- $(nested "$d")
		if [ $# = 3 ]; then EXK=nested EXB=$(expr \( $3 - $2 + 1 \) \* $1)
		else EXK=part EXB=$(expr $(sectors512 "$d") \* 512); fi
		return 0
	done
}
# Android's data free for an image file, in GB (an earlier image counts as free: it is replaced)
image_room() {
	FREE=0 OLDIMG=
	mount_data && data_ext4 && [ -d /data/lost+found ] || return 1
	FREE=$(($(stat -f -c %a /data) / (1073741824 / $(stat -f -c %S /data))))
	for f in $OLD_IMGS; do [ -f $f ] && { OLDIMG="$OLDIMG $f"; FREE=$((FREE + $(stat -c %b $f) / 2097152)); }; done
	return 0
}
# clear_extents: the Tab S5e's device zip writes where the image file lies on userdata (its extents) into the last
# 512 KiB of the recovery partition, after a "GTS4LV-EXTENTS 1" line, and the tablet's initramfs maps its root by that
# list. The new image file lies elsewhere, and the old one's blocks go back to Android: the list goes first, before
# the old file, so that until the device zip writes the new one the initramfs finds no list and stays, instead of
# mounting (or checking with e2fsck -fy) blocks that are now Android's. A recovery partition without the list (any
# other device) is not touched.
clear_extents() {
	r=$(part recovery) || return 0
	n=$(sectors512 $r) && [ "$n" -gt 1024 ] || return 0
	[ "$(dd if=$r bs=512 skip=$((n - 1024)) count=1 2>/dev/null | head -c 14)" = GTS4LV-EXTENTS ] || return 0
	guard $r
	dd if=/dev/zero of=$r bs=512 seek=$((n - 1024)) count=1024 conv=notrunc 2>/dev/null || die "cannot clear the old extent list at the end of recovery"
	sync
	[ -z "$(dd if=$r bs=512 skip=$((n - 1024)) count=1024 2>/dev/null | tr -d '\000' | head -c 1)" ] ||
		die "the old extent list at the end of recovery was not cleared"
	say "  the old image's extent list cleared: VibeOS starts again once the device zip has run"
}
# auto_choose: the POCO's choice, made without a question (see the top)
other() { if [ -e $BY/userdata ]; then say "No other partition is changed: Android's data stays as it is."; else say "No other partition is changed."; fi; }
auto_choose() {
	existing
	[ -z "$EX" ] || [ $EX = linux ] || die "a partition named '$EX' is there: the POCO boots only one named linux. Nothing was changed"
	if [ -n "$EX" ] && [ $EXK = nested ]; then
		fits $EXB || die "the partition $EX has room for $(expr $EXB / 1048576) MiB, the system needs $(expr $RB / 1048576) MiB"
		printf 'mode=existing\nkind=nested\nname=%s\n' $EX >> $P
		say "VibeOS goes into the partition '$EX' of an earlier install ($(expr $EXB / 1073741824) GB): its system is replaced."
		other
	elif [ -n "$EX" ]; then
		fits $(expr $EXB - 536870912) || die "the partition $EX has room for $(expr $EXB / 1048576) MiB, the system needs $(expr $RB / 1048576) MiB and the boot partition 480"
		printf 'mode=existing\nkind=nested\nname=%s\nnest=1\n' $EX >> $P
		say "VibeOS goes into the partition '$EX' of an earlier install: what it holds is ERASED."
		other
	else
		[ -e $BY/userdata ] || die "no userdata and no linux partition"
		DISK=$(disk_of $BY/userdata)
		$VP plan-whole $DISK > $T-whole.txt 2> $T-whole.err || die "VibeOS cannot take this phone's userdata: $(cat $T-whole.err | tr '\n' ' ')"
		printf 'mode=whole\nkind=nested\nname=linux\n' >> $P
		say "VibeOS takes all of Android's data partition, as a new partition 'linux'."
		say "Android, its apps and its data are ERASED. With the device zip, every power-on starts VibeOS."
	fi
	say "$SEP"
}
# nest_root PARTITION: the POCO's layout in PARTITION (a GPT of its own: the ESP, then the root to its end, as the k22
# image has it), its old start and end cleared first; RDEV the root inside it (a loop device), ODEV the partition
nest_root() {
	wipe_ends "$1"
	OUT=$($VP nest "$1" $ESP_MIB 2> $T-nest.err) || die "cannot write the partition table inside '$NAME': $(cat $T-nest.err)"
	set -- $1 $OUT
	[ "${2:-}" = nest ] || die "the partition tool said: $OUT"
	blockdev --flushbufs "$1" 2>/dev/null
	KIND=nested DEVP=$1
	open_root
}
gbytes() {	# literal byte counts (mksh math is 32-bit)
	case $1 in
	10) echo 10737418240 ;; 20) echo 21474836480 ;; 30) echo 32212254720 ;; 40) echo 42949672960 ;;
	50) echo 53687091200 ;; 100) echo 107374182400 ;; *) die "bad size $1" ;;
	esac
}

case "$ACTION" in
choose)
	: > $P
	chmod 755 $VP
	[ -n "$BY" ] || die "no partitions by name (no userdata, linux or recovery)"
	[ -s $SUMS ] && [ -n "$RB" ] || die "the zip's sums are missing"
	say " "
	say "VibeOS system for: ${DEVICE:-an unknown device}"
	if [ -n "$AUTO" ]; then auto_choose; exit 0; fi
	OPTS= GB= OLD= DISK=
	existing
	if [ -n "$EX" ]; then
		if { [ $EXK = part ] && can existing; } || { [ $EXK = nested ] && can nested; }; then
			fits $EXB || die "the partition $EX has room for $(expr $EXB / 1048576) MiB, the system needs $(expr $RB / 1048576) MiB"
			OPTS=existing GB=$(expr $EXB / 1073741824)
		fi
	elif can new && [ -e $BY/userdata ]; then
		DISK=$(disk_of $BY/userdata)
		for s in $SIZES_NEW; do $VP plan $DISK $s > /dev/null 2> $T-part.err && OPTS="$OPTS new"; [ -z "$OPTS" ] || break; done
	fi
	if can image && image_room; then
		for s in $SIZES_IMG; do [ $FREE -ge $((s + 1)) ] && { OPTS="$OPTS image"; break; }; done
	fi
	if can whole && [ -e $BY/userdata ] && [ -z "$EX" ]; then
		DISK=$(disk_of $BY/userdata)
		$VP plan-whole $DISK > $T-whole.txt 2> $T-whole.err && OPTS="$OPTS whole"
	fi
	if [ -z "$OPTS" ]; then
		case $DEVICE in
		surya|karna) die "no linux partition with its own GPT: install the POCO's VibeOS 0.1.1 zip once first (it makes it, keeping Android's data), then this one" ;;
		esac
		die "this device offers no place for the system ($KINDS): $(cat $T-part.err $T-whole.err 2>/dev/null | tr '\n' ' ')"
	fi
	label() {
		case $1 in
		existing) echo "Replace VibeOS in '$EX'${GB:+ - $GB GB}" ;;
		new)      echo "New partition (ERASES Android data)" ;;
		image)    echo "Image file in Android data (all kept)" ;;
		whole)    echo "Whole device (ERASES Android data)" ;;
		[0-9]*)   echo "$1 GB" ;;
		esac
	}
	warn() { echo "Android's data and apps, and its internal storage."; }
	CONFIRM=whole
	say "Volume Up/Down: move.  Power: choose."
	say "No key for 2 minutes: cancel, no change."
	set -- $OPTS
	menu "1/2  Where to install?" "$@"
	W=$o
	case $W in
	existing) printf 'mode=existing\nkind=%s\nname=%s\n' $EXK $EX >> $P ;;
	whole)    printf 'mode=whole\nkind=part\nname=linux\n' >> $P ;;
	new)
		S=
		for s in $SIZES_NEW; do $VP plan $DISK $s > /dev/null 2>&1 && S="$S $s"; done
		CONFIRM=
		set -- $S
		menu "2/2  Size of the new partition?" "$@"
		printf 'mode=new\nkind=part\nname=linux\nsize=%s\n' $o >> $P ;;
	image)
		S=
		for s in $SIZES_IMG; do [ $FREE -ge $((s + 1)) ] && S="$S $s"; done
		[ -z "$OLDIMG" ] || say "The Linux image file there now is replaced."
		CONFIRM=
		set -- $S
		menu "2/2  Size of the image file? (1 GB stays free)" "$@"
		printf 'mode=image\nkind=image\nimage=%s\nsize=%s\n' $IMG $o >> $P ;;
	esac
	data_mounted && [ $W != image ] && umount /data 2>/dev/null
	say "$SEP"
	say "Installing: $(label $W)$(get size | sed 's/^\(..*\)$/, \1 GB/')"
	;;
prepare)
	chmod 755 $VP
	MODE=$(get mode)
	loops_on	# before anything is written: the root's loop devices, here and in chunks and finish, need it
	case $MODE in
	existing)
		existing
		if [ "$(get nest)" = 1 ]; then
			# the POCO's "linux" without its own GPT: it gets one
			[ "$EX" = "$(get name)" ] && [ "$EXK" = part ] || die "the partition chosen is gone"
			NAME=$EX
			guard "$(part $EX)"
			nest_root "$(part $EX)"
		else
			[ "$EX" = "$(get name)" ] && [ "$EXK" = "$(get kind)" ] || die "the partition chosen is gone"
			NAME=$EX KIND=$EXK
			open_root
			guard "$ODEV"
		fi
		;;
	new)
		DISK=$(disk_of $BY/userdata)
		unmount_android
		OUT=$($VP apply $DISK $(get size) 2> $T-part.err)
		RC=$?
		[ $RC = 3 ] && die "$(cat $T-part.err). The new partition is in the table: restart into recovery, Format data, then install again and choose the existing partition"
		[ $RC = 0 ] || die "partitioning failed ($RC): $(cat $T-part.err)"
		set -- $(echo "$OUT" | tail -n 1)
		[ "${1:-}" = created ] || die "the partition tool said: $OUT"
		RDEV=$(pdev $2); put new 1
		;;
	whole)
		DISK=$(disk_of $BY/userdata)
		unmount_android
		OUT=$($VP whole $DISK 2> $T-part.err)
		RC=$?
		[ $RC = 3 ] && [ -n "$AUTO" ] && die "$(cat $T-part.err). userdata is \"linux\" in the table now: restart into recovery and install again"
		[ $RC = 3 ] && die "$(cat $T-part.err). userdata is \"linux\" in the table now: restart into recovery, install again and choose the existing partition"
		[ $RC = 0 ] || die "partitioning failed ($RC): $(cat $T-part.err)"
		set -- $(echo "$OUT" | grep '^whole ')
		[ "${1:-}" = whole ] || die "the partition tool said: $OUT"
		RDEV=$(pdev $2)
		if [ "$(get kind)" = nested ]; then
			# the POCO: the partition (no by-name link in this session) gets its own GPT, the root goes inside it
			i=0; while [ ! -b $RDEV ] && [ $i -lt 10 ]; do sleep 1; i=$((i + 1)); done
			[ -b $RDEV ] || die "no $RDEV"
			NAME=linux
			guard "$RDEV"
			nest_root "$RDEV"
		fi
		;;
	image)
		image_room || die "Android's data cannot be mounted, is not ext4 or has no lost+found"
		SIZE=$(get size)
		[ $FREE -ge $((SIZE + 1)) ] || die "Android's data has $FREE GB free; a $SIZE GB image needs $((SIZE + 1))"
		clear_extents
		for f in $OLDIMG; do
			chattr -i $f 2>/dev/null
			rm -f $f || die "cannot remove the earlier $f"
			say "  removed the earlier $f"
		done
		mkdir -p ${IMG%/*} && chmod 700 ${IMG%/*} || die "cannot create ${IMG%/*}"
		RDEV=
		;;
	*) die "nothing chosen" ;;
	esac
	if [ -n "$RDEV" ]; then
		# a new partition: the kernel knows it (BLKPG); ueventd may be slow to make its node
		i=0; while [ ! -b $RDEV ] && [ $i -lt 10 ]; do sleep 1; i=$((i + 1)); done
		if [ ! -b $RDEV ]; then
			MM=$(cat /sys/class/block/$(basename $RDEV)/dev 2>/dev/null) || die "no /sys/class/block/$(basename $RDEV)"
			mknod $RDEV b ${MM%%:*} ${MM##*:} || die "cannot make $RDEV"
		fi
		guard "$RDEV"
		wipe_ends "$RDEV"
		ln -sf $RDEV $T-root || die "cannot link $RDEV"
		put dev "${ODEV:-$RDEV}"; put loop "$LOOP"
	fi
	;;
chunks)
	# "chunks COUNT BYTES" in the sums: COUNT parts of BYTES each (the last one the rest)
	set -- $(grep '^chunks ' $SUMS)
	[ $# = 3 ] || die "no chunks line in the sums"
	N=$2 CB=$3
	if [ "$(get kind)" = image ]; then
		TGT=$IMG
		rm -f $TGT && fallocate -l $RB $TGT || die "cannot make $TGT ($RB bytes)"
	else
		TGT=$(readlink $T-root)
		[ -b "$TGT" ] || die "no root device"
	fi
	: > $T-chunkloops
	i=0
	while [ $i -lt $N ]; do
		OFF=$(expr $i \* $CB)
		if [ $i = $((N - 1)) ]; then SZ=$(expr $RB - $OFF); else SZ=$CB; fi
		L=$(newloop) && losetup -o $OFF -S $SZ $L "$TGT" || die "cannot map part $((i + 1)) of the root"
		echo $L >> $T-chunkloops
		[ "$(expr $(sectors512 $L) \* 512)" = "$SZ" ] || die "part $((i + 1)): the loop device has the wrong size"
		ln -sf $L $T-root.$i || die "cannot link $L"
		i=$((i + 1))
	done
	;;
abort)
	# a part of the root could not be written, and updater-script stops after this: the loop devices let go, and
	# SELinux as it was
	for L in $(cat $T-chunkloops 2>/dev/null); do losetup -d $L 2>/dev/null; done
	rm -f $T-chunkloops $T-root.[0-9]*
	LOOP=$(get loop); close_root
	loops_off
	;;
finish)
	KIND=$(get kind) NAME=$(get name) MODE=$(get mode)
	# the parts' loop devices: flushed and gone before the root is read back from its own device or file
	sync
	for L in $(cat $T-chunkloops 2>/dev/null); do blockdev --flushbufs $L 2>/dev/null; losetup -d $L || die "cannot release $L"; done
	rm -f $T-chunkloops $T-root.[0-9]*
	case $KIND in
	image)
		[ -s $IMG ] || die "rootfs.img was not written"
		[ "$(head -c $RB $IMG | sha256sum | cut -d' ' -f1)" = "$(grep '^rootfs ' $SUMS | cut -d' ' -f3)" ] ||
			die "rootfs.img was not written correctly"
		B=$(gbytes $(get size))
		fallocate -l $B $IMG || die "cannot grow rootfs.img to $(get size) GB"
		sync
		IMGF=$IMG; open_root ;;
	*)
		RDEV=$(readlink $T-root); LOOP=$(get loop)
		[ -b "$RDEV" ] || die "no root device"
		check $RDEV rootfs $SUMS ;;
	esac
	say "  checking the file system"
	fsck_root $RDEV
	grow $RDEV 65536
	mkdir -p $ROOT
	# noinit_itable: no lazy zeroing of inode tables not zeroed yet (the block groups the grow adds). Through a loop device
	# over the image file it would punch holes into the file (the loop driver turns write-zeroes into
	# FALLOC_FL_PUNCH_HOLE), and a hole has no blocks for the Tab S5e's dm-linear map; the first boot zeroes them
	mount -t ext4 -o noatime,noinit_itable $RDEV $ROOT || die "cannot mount the root file system"
	fail() { cd /; umount -D $ROOT; close_root; die "$@"; }
	[ -s $ROOT/etc/vibeos/system-image ] || fail "this is not a VibeOS system image"
	{ echo "kind=$KIND"; echo "name=$NAME"; echo "mode=$MODE"; echo "size=$(get size)"
	  echo "image=$( [ $KIND = image ] && echo $IMG)"; echo "device=$DEVICE"; } > $ROOT/etc/vibeos/installed-to ||
		fail "cannot write /etc/vibeos/installed-to"
	sync
	umount -D $ROOT || die "cannot unmount the root file system"
	fsck_root $RDEV
	close_root
	if [ $KIND = image ]; then
		sync
		# pinned: Android may not move or delete it (the Tab S5e boots it by its extents)
		chattr +i $IMG || die "chattr +i $IMG failed"
		umount /data 2>/dev/null
	fi
	if [ "$(get new)" = 1 ]; then
		say "  formatting Android's data"
		U=$BY/userdata
		if [ "$(od -An -tx1 -j 1024 -N 4 $U | tr -d ' \n')" = 1020f5f2 ]; then
			CF=
			[ "$(getprop external_storage.casefold.enabled)" = 1 ] && CF="-O casefold -C utf8"
			/system/bin/make_f2fs -f -g android -O project_quota,extra_attr $CF -O compression -O extra_attr \
				$U > $T-f2fs.log 2>&1 || die "cannot format userdata: $(tail -n 2 $T-f2fs.log)"
		else
			/system/bin/mke2fs -F -t ext4 -b 4096 $U > $T-ext4.log 2>&1 || die "cannot format userdata: $(tail -n 2 $T-ext4.log)"
		fi
		for m in metadata omr; do
			[ -e $BY/$m ] || continue
			/system/bin/mke2fs -F -t ext4 -b 4096 $BY/$m > $T-meta.log 2>&1 || die "cannot format $m: $(tail -n 2 $T-meta.log)"
			break
		done
	fi
	loops_off
	put done 1
	say "  the system is in: $(case $KIND in image) echo "$IMG" ;; nested) echo "the root inside '$NAME'" ;; *) echo "partition '$NAME'" ;; esac)"
	;;
*) die "usage: install.sh choose|prepare|chunks|finish|abort" ;;
esac
exit 0
