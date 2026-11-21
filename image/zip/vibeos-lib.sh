# vibeos-lib.sh: what the VibeOS system zip and the device zips share, sourced by their install.sh in LineageOS
# recovery (mksh and toybox: arithmetic is 32-bit, so byte counts over 2 GiB go through expr or stay strings).
#
# The contract between the two zips: the system zip writes the root file system (ext4, label vibeos, shrunk to its
# content) to one of
#   part    a GPT partition named "linux" or "vibeos" (an earlier install's, or a new one carved from the end of
#           userdata, or userdata itself for a whole-device install)
#   nested  the Linux root partition inside the GPT a partition named "linux" carries (the POCO's layout: ESP +
#           root, 4096-byte sectors)
#   image   a file in Android's data, /data/lost+found/vibeos/rootfs.img (ext4 data only)
# and records where in /etc/vibeos/installed-to inside the root itself (kind, name, mode, size) and, for this
# recovery session, in /tmp/vibeos.prop. A device zip finds the root the same way (find_root), checks it is a VibeOS
# system (/etc/vibeos/system-image), puts the device's files in and makes the device boot it.
#
# Never written by either zip: efs, sec_efs, cpefs, radio, modem, persist, or any partition but the root's, the
# boot partitions a device zip names, and (new partition) userdata's table entry. Nothing prints IMEI, serial
# numbers, MAC or Bluetooth addresses.
P=/tmp/vibeos.prop
T=/tmp/vibeos
IMG=/data/lost+found/vibeos/rootfs.img
OLD_IMGS="/data/lost+found/vibeos/rootfs.img /data/lost+found/mobian/rootfs.img"
NEVER="efs sec_efs cpefs radio modem persist modemst1 modemst2 fsg fsc"

# the updater's pipe to the screen: update-binary's arguments are API FD ZIP, and it is this script's parent
OUTFD= ZIP=
set -- $(tr '\0' ' ' < /proc/$PPID/cmdline 2>/dev/null)
if [ $# -ge 3 ]; then eval OUTFD=\${$(($# - 1))}; eval ZIP=\${$#}; fi
say() {
	case "$OUTFD" in ''|*[!0-9]*) ;; *) echo "ui_print $*" > /proc/$PPID/fd/$OUTFD 2>/dev/null ;; esac
	echo "$*" >&2
}
die() { echo "error=$(echo "$*" | tr '\n' ' ')" >> $P; echo "vibeos: $*" >&2; exit 1; }
get() { grep "^$1=" $P 2>/dev/null | tail -n 1 | cut -d= -f2-; }
put() { echo "$1=$2" >> $P; }

# the by-name directory of the disk that holds userdata (or, after a whole-device install, the "linux" it became)
BY=
for n in userdata linux vibeos recovery; do
	for b in /dev/block/by-name /dev/block/bootdevice/by-name /dev/block/platform/*/by-name /dev/block/platform/*/*/by-name; do
		[ -e "$b/$n" ] && { BY=$b; break 2; }
	done
done
part() { [ -n "$BY" ] && [ -e "$BY/$1" ] && readlink -f "$BY/$1"; }
# sysname DEV: its name in /sys/class/block (by major:minor for a node of our own, such as /tmp/vibeos-loop4)
sysname() {
	d=$(readlink -f "$1"); n=$(basename "$d")
	[ -e /sys/class/block/$n ] && { echo $n; return; }
	set -- $(stat -c '%t %T' "$d")
	basename "$(readlink -f /sys/dev/block/$(printf %d 0x$1):$(printf %d 0x$2))"
}
disk_of() { x=$(basename "$(readlink -f /sys/class/block/$(sysname "$1")/..)"); if [ -b /dev/block/$x ]; then echo /dev/block/$x; else echo /dev/$x; fi; }
sectors512() { cat /sys/class/block/$(sysname "$1")/size; }
gib() { echo $(($(sectors512 "$1") / 2097152)); }
blocks4k() { echo $(($(sectors512 "$1") / 8)); }
# a partition the zips must never write: refuse the device if it is one of them (by name)
guard() {
	for n in $NEVER; do
		x=$(part $n) || continue
		[ "$(readlink -f "$1")" != "$x" ] || die "refusing to write $n"
	done
}

# the next key pressed: UP, DOWN (the volume keys) or POWER; nothing within $1 seconds: empty. Releases and touches
# are passed over. Power is safe to press in recovery: only seven presses in a row reboot it.
key() {
	end=$((SECONDS + $1))
	while [ $SECONDS -lt $end ]; do
		getevent -lqc 2 > $T-key 2>/dev/null &
		g=$!
		while kill -0 $g 2>/dev/null && [ $SECONDS -lt $end ]; do sleep 0.1; done
		kill $g 2>/dev/null
		wait $g 2>/dev/null
		if grep -q 'KEY_VOLUMEUP *DOWN' $T-key; then echo UP; return; fi
		if grep -q 'KEY_VOLUMEDOWN *DOWN' $T-key; then echo DOWN; return; fi
		if grep -q 'KEY_POWER *DOWN' $T-key; then echo POWER; return; fi
	done
}
# menu TITLE CHOICES...: the S10e kit's menu (recovery scrolls its lines and prints ASCII only): the title, the
# choices one per line with ">" at the current one, drawn again after every key; Volume Down/Up move, Power chooses,
# sets o. A choice "label X" prints is the caller's label(). A choice listed in $CONFIRM asks for a second Power.
# Two minutes without a key: cancel, nothing written.
SEP="------------------------------------"
CONFIRM=
menu() {
	t=$1; shift
	i=1
	while :; do
		say "$SEP"
		say "$t"
		j=0
		for x; do
			j=$((j + 1))
			if [ $j -eq $i ]; then o=$x; say "> $(label $x)"; else say "  $(label $x)"; fi
		done
		say "Volume Up/Down: move.  Power: choose."
		k=$(key 120)
		case "$k" in
		DOWN) i=$((i + 1)); [ $i -le $# ] || i=1 ;;
		UP)   i=$((i - 1)); [ $i -ge 1 ] || i=$# ;;
		POWER)
			case " $CONFIRM " in *" $o "*) ;; *) return 0 ;; esac
			say "$SEP"
			say "$(label $o): this ERASES"
			say "$(warn $o)"
			say "Power again: erase and install."
			say "Volume: back."
			[ "$(key 60)" = POWER ] && return 0
			;;
		*) die "nothing chosen in two minutes: nothing was changed" ;;
		esac
	done
}

# u32 DEV OFFSET: the little-endian 32-bit number at OFFSET (bytes, below 2 GiB) of DEV
u32() { set -- $(od -An -tu4 -j "$2" -N 4 "$1" 2>/dev/null); echo "${1:-}"; }
# nested DEV: "SS FIRST LAST" (sector size, first and last sector) of the Linux root partition in the GPT that DEV
# itself carries, or nothing. The root: the first entry of type Linux root (arm64) or Linux file system.
nested() {
	for ss in 4096 512; do
		[ "$(dd if="$1" bs=$ss skip=1 count=1 2>/dev/null | head -c 8)" = "EFI PART" ] || continue
		dd if="$1" bs=$ss skip=1 count=1 of=$T-gpt-hdr 2>/dev/null
		lba=$(u32 $T-gpt-hdr 72); n=$(u32 $T-gpt-hdr 80); esz=$(u32 $T-gpt-hdr 84)
		[ -n "$lba" ] && [ "$n" -gt 0 ] && [ "$n" -le 256 ] && [ "$esz" -ge 128 ] || return 0
		dd if="$1" bs=$ss skip=$lba count=$(((n * esz + ss - 1) / ss)) of=$T-gpt-ent 2>/dev/null
		i=0
		while [ $i -lt $n ]; do
			o=$((i * esz))
			ty=$(od -An -tx1 -j $o -N 16 $T-gpt-ent | tr -d ' \n')
			case $ty in
			45b021b9f01dc341af444c6f280d3fae|af3dc60f838472478e793d69d8477de4)
				f=$(u32 $T-gpt-ent $((o + 32))); l=$(u32 $T-gpt-ent $((o + 40)))
				[ "$(u32 $T-gpt-ent $((o + 36)))" = 0 ] && [ "$(u32 $T-gpt-ent $((o + 44)))" = 0 ] || return 0
				echo "$ss $f $l"; return 0 ;;
			esac
			i=$((i + 1))
		done
		return 0
	done
}
# nested_esp DEV: "SS FIRST LAST" of the EFI system partition in DEV's own GPT, or nothing
nested_esp() {
	for ss in 4096 512; do
		[ "$(dd if="$1" bs=$ss skip=1 count=1 2>/dev/null | head -c 8)" = "EFI PART" ] || continue
		dd if="$1" bs=$ss skip=1 count=1 of=$T-gpt-hdr 2>/dev/null
		lba=$(u32 $T-gpt-hdr 72); n=$(u32 $T-gpt-hdr 80); esz=$(u32 $T-gpt-hdr 84)
		dd if="$1" bs=$ss skip=$lba count=$(((n * esz + ss - 1) / ss)) of=$T-gpt-ent 2>/dev/null
		i=0
		while [ $i -lt $n ]; do
			o=$((i * esz))
			[ "$(od -An -tx1 -j $o -N 16 $T-gpt-ent | tr -d ' \n')" = 28732ac11ff8d211ba4b00a0c93ec93b ] &&
				{ echo "$ss $(u32 $T-gpt-ent $((o + 32))) $(u32 $T-gpt-ent $((o + 40)))"; return 0; }
			i=$((i + 1))
		done
		return 0
	done
}

data_mounted() { grep -q " /data " /proc/mounts; }
mount_data() { data_mounted || mount /data 2>/dev/null || mount -t ext4 -o noatime "$BY/userdata" /data 2>/dev/null; }
data_ext4() { [ "$(grep " /data " /proc/mounts | head -n1 | cut -d' ' -f3)" = ext4 ]; }

# find_root: where the system zip put the root, as KIND NAME DEVP (the partition) or IMGF (an image file): this
# session's marker first (a partition made or renamed in this session has no by-name link before the next boot:
# DEVP is the device the system zip wrote), then a partition named linux or vibeos (nested when it carries a GPT),
# then an image file
DEVP=
find_root() {
	KIND=$(get kind) NAME=$(get name) IMGF=$(get image) DEVP=$(get dev)
	[ -z "$KIND" ] || return 0
	for n in linux vibeos; do
		d=$(part $n) || continue
		NAME=$n DEVP=$d
		if [ -n "$(nested "$d")" ]; then KIND=nested; else KIND=part; fi
		return 0
	done
	if mount_data && data_ext4; then
		for f in $IMG; do [ -s "$f" ] && { KIND=image IMGF=$f; return 0; }; done
	fi
	KIND=
}
# open_root: RDEV, a block device of exactly the root file system's partition or file (a loop device for nested
# and image), and ODEV, the partition it is in; close_root undoes it
LOOP= ODEV=
open_root() {
	case $KIND in
	part) ODEV=${DEVP:-$(part $NAME)}; RDEV=$ODEV ;;
	nested)
		ODEV=${DEVP:-$(part $NAME)}; d=$ODEV
		set -- $(nested "$d")
		[ $# = 3 ] || die "partition $NAME: no Linux root in its own GPT"
		OFF=$(expr $2 \* $1); SZ=$(expr \( $3 - $2 + 1 \) \* $1)
		LOOP=$(newloop) && losetup -o $OFF -S $SZ $LOOP "$d" || die "cannot map the root inside $NAME"
		RDEV=$LOOP ;;
	image)
		[ -s "$IMGF" ] || die "no $IMGF"
		LOOP=$(newloop) && losetup $LOOP "$IMGF" || die "cannot map $IMGF"
		RDEV=$LOOP ;;
	*) die "no VibeOS root found: install the VibeOS system zip first" ;;
	esac
	[ -b "$RDEV" ] || die "no block device for the root ($KIND $NAME)"
}
close_root() { [ -z "$LOOP" ] || { losetup -d $LOOP 2>/dev/null; LOOP=; }; }
# newloop: a free loop device (one with no file bound: no loop/ in sysfs), as a node of our own in /tmp: toybox's
# losetup -f fails when ueventd has not made /dev/block/loopN for a loop device the kernel added on demand
newloop() {
	for i in 1 2; do
		for s in /sys/class/block/loop[0-9]*; do
			n=${s##*/}
			case $n in loop*[0-9]p[0-9]*) continue ;; esac
			[ -r $s/dev ] && [ ! -e $s/loop ] || continue
			MM=$(cat $s/dev)
			rm -f $T-$n; mknod $T-$n b ${MM%%:*} ${MM##*:} && { echo $T-$n; return 0; }
		done
		# none free: losetup -f has the kernel add one (loop-control), whether or not it can open it
		losetup -f > /dev/null 2>&1
	done
	return 1
}
# fs_blocks DEV: the ext4 block count; grow DEV BLOCKS: the file system grown by BLOCKS 4 KiB blocks, or to the end
fs_blocks() { tune2fs -l "$1" 2>/dev/null | sed -n 's/^Block count: *//p'; }
grow() {
	[ "$(tune2fs -l "$1" | sed -n 's/^Block size: *//p')" = 4096 ] || die "the root file system's blocks are not 4 KiB"
	nb=$(($(fs_blocks "$1") + $2)); pb=$(blocks4k "$1")
	[ $nb -le $pb ] || nb=$pb
	resize2fs "$1" $nb > $T-resize.log 2>&1 || die "cannot grow the root file system: $(tail -n 2 $T-resize.log)"
}
fsck_root() {
	e2fsck -fy "$1" > $T-fsck.log 2>&1
	[ $? -lt 4 ] || die "the root file system does not check: $(tail -n 2 $T-fsck.log)"
}
# check DEV NAME SUMS: the first BYTES of DEV against "NAME BYTES SHA256" in SUMS, read from the device itself
check() {
	set -- "$1" "$2" $(grep "^$2 " "$3")
	[ $# = 5 ] || die "no sum for $2"
	sync
	blockdev --flushbufs "$1" 2>/dev/null
	[ "$(head -c $4 "$1" | sha256sum | cut -d' ' -f1)" = "$5" ] || die "$2 was not written correctly to $1"
}
