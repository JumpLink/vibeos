#!/bin/sh
# init-test.sh DEVDIR...: the initramfs /init that a device folder (build-device.sh's output) ships, run under
# qemu-user (binfmt) in user, mount and network namespaces (unshare -rmn: no privileges on the host, no network), with
# stubs for everything that would touch hardware; init-case.py runs each case. The S10e's comes from the payload's
# boot/s10e/initramfs.cpio.gz, the Tab S5e's from the slot image at 34 MiB of boot/recovery.img. Cases:
#   plain   a normal boot: it must start Mobian (move its mounts onto the root and exec its systemd)
#   marker  what a boot that died left behind (the S10e: /var/lib/s10e/boot-attempt on the root; the S5e: a pstore
#           console record without GTS4LV-MOBIAN-OK): it must still start Mobian. Since 2026-11-23 only U-Boot's
#           Rescue entry or a root that cannot be started keeps the device in the initramfs
#   rescue  the same, with the Rescue entry's s10e.stay or gts4lv.stay: it must stay
# The Tab S5e's root as an image file in Android's data (no gts4lv_root label on userdata; the list of where the
# file lies at the end of recovery, mapped with dmlinear: a stub here that copies the mapped bytes to /dev/dm-0):
#   imgok    a list that holds, mapping an ext4 superblock that fits it: it must start Mobian
#   nolist   no list, as after a system zip without the device zip (INIT_RECOVERY_TAIL=FILE: recovery's last 512 KiB
#            from zip-test.sh's TAIL_OUT; zeros without it)
#   outside  a list that runs past the end of userdata
#   stale    a list that holds, mapping blocks with no file system (the old file's, now Android's)
#   Blocks no list vouches for may be Android's: these three must stay, say to install the device zip, and never
#   check (e2fsck), mount or grow anything; outside must not even map
# The Galaxy S20 FE's initramfs is built into its kernel: its tree (s20/build-initramfs.sh's OUT, with root/init) is
# tested here, the same tree the kernel builds in. Its cases (s20/init; U-Boot's entries as in its environment):
#   plain     VibeOS: it must start VibeOS's root (r8s.root=vibeos), and start no shell
#   debugroot the same with r8s.debug: the shells start, then the root
#   rescue    "Rescue shell (USB)", which passes r8s.debug: it must stay, with the shells up
#   oldrescue that entry without r8s.debug (a U-Boot from before it): it must stay, start no shell, and say how to
#             get one
#   noroot    VibeOS with no partition named vibeos: it must stay, start no shell, and say so
# Any other folder is skipped: the POCO's initramfs is initramfs-tools', the k22 ROM's byte for byte. Exit 1 on
# any failure. Work files in TMPDIR.
set -u
H=$(cd "$(dirname "$0")" && pwd)
FAILS=0
ok() { echo "  ok: $*"; }
bad() { echo "  FAIL: $*"; FAILS=$((FAILS + 1)); }
S10E_NORMAL='console=tty0 quiet splash loglevel=3 vt.global_cursor_default=0 systemd.show_status=false udev.log_level=3 max77705.dp_four_lanes=0 panic=10 s10e.argsfile=2 s10e.root=PARTLABEL=linux swiotlb=8192 mem_sleep_default=s2idle'
S10E_RESCUE='console=tty0 loglevel=7 phy_exynos5_usbdrd.exynos9820_ss=0 panic=0 s10e.root=PARTLABEL=linux s10e.stay'
S5E_NORMAL='console=tty0 fbcon=vc:2-63 logo.nologo vt.global_cursor_default=0 splash quiet loglevel=3 systemd.show_status=auto rd.udev.log_level=3 printk.devkmsg=on clk_ignore_unused regulator_ignore_unused fw_devlink.sync_state=timeout panic=10'
S5E_RESCUE="$S5E_NORMAL gts4lv.stay gts4lv.debug"
# the S20's U-Boot (ub13): r8s_args, and r8s_args_rescue with the public image's r8s.debug
S20_NORMAL='console=tty0 quiet loglevel=3 fbcon=vc:2-63 vt.global_cursor_default=0 splash systemd.show_status=error rd.udev.log_level=3 udev.log_level=3 r8s.root=vibeos clk_ignore_unused pd_ignore_unused panic=5 printk.devkmsg=on s3c2410_wdt.tmr_atboot=1 s3c2410_wdt.tmr_margin=60 sysctl.kernel.panic_on_rcu_stall=1 spidev.bufsiz=131072 r8s.uboot'
S20_OLDRESCUE='console=tty0 loglevel=7 ignore_loglevel fbcon=font:TER16x32 clk_ignore_unused pd_ignore_unused panic=5 printk.devkmsg=on s3c2410_wdt.tmr_atboot=1 s3c2410_wdt.tmr_margin=60 sysctl.kernel.panic_on_rcu_stall=1 r8s.uboot'
S20_RESCUE="$S20_OLDRESCUE r8s.debug"

# stubs DIR EXTRA...: DIR/stubs.sh (sourced before the init): mount binds fakedisk for an ext4 mount and logs the
# rest; every command named after it only logs
stubs() {
	D=$1; shift
	cat > "$D/stubs.sh" <<-'EOF'
	mount() {
		echo "mount $*" >> /stublog
		case " $* " in
		*" -t ext4 "*) eval last=\${$#}; /bin/busybox mount --bind /fakedisk "$last"; return $? ;;
		esac
		return 0
	}
	umount() { echo "umount $*" >> /stublog; /bin/busybox umount "$@"; }
	EOF
	for s in e2fsck "$@"; do echo "$s() { echo \"$s \$*\" >> /stublog; return 0; }" >> "$D/stubs.sh"; done
	printf '#!/bin/busybox sh\n. /stubs.sh\n. /init\n' > "$D/test-init"; chmod 755 "$D/test-init"
	mkdir -p "$D/proc" "$D/dev" "$D/mnt/root" "$D/sys/kernel/config" "$D/sys/fs/pstore" "$D/fakedisk/lib/systemd" "$D/fakedisk/var/lib"
	printf '#!/bin/sh\n' > "$D/fakedisk/lib/systemd/systemd"; chmod 755 "$D/fakedisk/lib/systemd/systemd"
	: > "$D/stublog"
}
prep_s10e() {	# DIR CASE
	stubs "$1" resize2fs telnetd dropbear udhcpd sync
	mkdir -p "$1/sys/class/udc/10c00000.dwc3" "$1/sys/class/block/sda32" "$1/sys/class/block/sda31"
	printf 'MAJOR=8\nMINOR=32\nDEVNAME=sda32\nDEVTYPE=partition\nPARTN=50\nPARTNAME=linux\n' > "$1/sys/class/block/sda32/uevent"
	printf 'MAJOR=8\nMINOR=31\nDEVNAME=sda31\nDEVTYPE=partition\nPARTN=49\nPARTNAME=userdata\n' > "$1/sys/class/block/sda31/uevent"
	[ $2 = plain ] || { mkdir -p "$1/fakedisk/var/lib/s10e"; echo "Mon Nov 23 14:25:17 CET 2026" > "$1/fakedisk/var/lib/s10e/boot-attempt"; }
}
prep_s5e() {	# DIR CASE
	stubs "$1" resize2fs telnetd dropbear udhcpd sync watchdog i2cset i2cget reboot chvt killall wifi-up
	# dmlinear NAME DEV LIST: /dev/dm-0 a copy of what LIST maps of DEV (LOGICAL OFFSET LENGTH FLAGS, bytes)
	cat >> "$1/stubs.sh" <<-'SH'
	dmlinear() {
		echo "dmlinear $*" >> /stublog; : > /dev/dm-0
		while read -r l o n f; do dd if=$2 bs=512 skip=$((o / 512)) count=$((n / 512)) >> /dev/dm-0 2>/dev/null; done < $3
		echo 253:0
	}
	SH
	mkdir -p "$1/sys/class/udc/a600000.usb" "$1/sys/class/block/sda17" "$1/sys/class/block/sda15"
	printf 'MAJOR=8\nMINOR=17\nDEVNAME=sda17\nDEVTYPE=partition\nPARTNAME=userdata\n' > "$1/sys/class/block/sda17/uevent"
	printf 'MAJOR=8\nMINOR=15\nDEVNAME=sda15\nDEVTYPE=partition\nPARTNAME=recovery\n' > "$1/sys/class/block/sda15/uevent"
	# the recovery partition: 1 MiB here, its last 512 KiB where the device zip writes the extent list
	echo 2048 > "$1/sys/class/block/sda15/size"
	# userdata 16 MiB here. imgok, stale: a list of two extents (4 MiB at 1 MiB, 4 MiB at 8 MiB); imgok puts an
	# ext4 superblock of 8 MiB (2048 blocks of 4 KiB) where it starts, stale has zeros there
	echo 32768 > "$1/sys/class/block/sda17/size"
	python3 -I - "$1/dev/sda15" "$1/dev/sda17" "$2" "${INIT_RECOVERY_TAIL:-}" <<-'PY'
	import struct, sys
	rec, ud, case, tailf = sys.argv[1:5]
	M = 1 << 20
	lists = {'imgok': [(0, M, 4 * M), (4 * M, 8 * M, 4 * M)], 'stale': [(0, M, 4 * M), (4 * M, 8 * M, 4 * M)],
	         'outside': [(0, M, 4 * M), (4 * M, 14 * M, 4 * M)]}
	if case == 'nolist' and tailf:
	    t = open(tailf, 'rb').read()
	    assert len(t) == 524288, "INIT_RECOVERY_TAIL: not 512 KiB"
	elif case in lists:
	    t = ('GTS4LV-EXTENTS 1\n' + ''.join('%d %d %d 0x0\n' % e for e in lists[case]) + 'END\n').encode()
	else:
	    t = b''
	open(rec, 'wb').write(bytes(524288) + t.ljust(524288, b'\0'))	# recovery: 1 MiB, the list in its last 512 KiB
	if case in ('imgok', 'stale', 'outside', 'nolist'):
	    u = bytearray(16 * M)   # Android's data: no label of ours
	    if case == 'imgok':
	        sb = M + 1024
	        struct.pack_into('<I', u, sb + 4, 2048)       # s_blocks_count_lo
	        struct.pack_into('<I', u, sb + 24, 2)         # s_log_block_size: 4 KiB
	        struct.pack_into('<H', u, sb + 56, 0xEF53)    # s_magic
	        u[sb + 120:sb + 126] = b'vibeos'
	    open(ud, 'wb').write(u)
	PY
	if [ -s "$1/dev/sda17" ]; then
		:	# the image layout, written above
	else
		# the whole-tablet layout: userdata carries the ext4 label gts4lv_root (1024 + 120)
		python3 -I -c "import sys; b=bytearray(4096); b[1144:1155]=b'gts4lv_root'; open(sys.argv[1],'wb').write(b)" "$1/dev/sda17"
	fi
	case $2 in plain|imgok|nolist|outside|stale) ;; *) false ;; esac || printf 'gts4lv-init: up\ngts4lv-init: GTS4LV-SWITCHROOT\n' > "$1/sys/fs/pstore/console-ramoops-0"
}
prep_s20() {	# DIR CASE
	# devmem (PMU registers), blockdev (the read-only guard), watchdog and killall (which would reach this user's
	# own processes of those names) only log here
	stubs "$1" resize2fs dumpe2fs telnetd dropbear udhcpd watchdog devmem blockdev killall
	mkdir -p "$1/sys/class/udc/10e00000.dwc3" "$1/sys/class/block/sda32" "$1/fakedisk/sbin"
	printf '#!/bin/sh\n' > "$1/fakedisk/sbin/init"; chmod 755 "$1/fakedisk/sbin/init"
	echo "5.00 5.00" > "$1/proc/uptime"
	printf 'MAJOR=8\nMINOR=32\nDEVNAME=sda32\nDEVTYPE=partition\nPARTN=32\nPARTNAME=userdata\n' > "$1/sys/class/block/sda32/uevent"
	echo 1048576 > "$1/sys/class/block/sda32/size"
	if [ $2 != noroot ]; then
		mkdir -p "$1/sys/class/block/sda30"
		printf 'MAJOR=8\nMINOR=30\nDEVNAME=sda30\nDEVTYPE=partition\nPARTN=30\nPARTNAME=vibeos\n' > "$1/sys/class/block/sda30/uevent"
		echo 2097152 > "$1/sys/class/block/sda30/size"; : > "$1/dev/sda30"
	fi
}
# verdict_s20 JSON CASE: plain and debugroot must start VibeOS, the rest stay; shells only with r8s.debug
verdict_s20() {
	python3 -I - "$1" "$2" <<'EOF'
import json, sys
d = json.load(open(sys.argv[1])); case = sys.argv[2]
moved = [l for l in d['stublog'] if l.startswith('mount --move ') and '/newroot/' in l]
booted = d.get('init_exit') is not None and moved and not d['left_running']
stayed = d.get('init_exit') is None and d['running_at_end'] and any('staying' in l for l in d['kmsg'])
shells = sorted({l.split()[0] for l in d['stublog'] if l.split()[:1] in (['telnetd'], ['dropbear'])})
nologin = any('no login here' in l for l in d['kmsg'])
last = (d['kmsg'] or ['(no kernel log)'])[-1].strip()
want_boot = case in ('plain', 'debugroot')
want_shells = case in ('debugroot', 'rescue')
bad = []
if want_boot and not booted:
    bad.append('did not start VibeOS: ' + last)
if not want_boot and not stayed:
    bad.append('did not stay: ' + last)
if want_shells and shells != ['dropbear', 'telnetd']:
    bad.append('shells: %s, not telnetd and dropbear' % (' '.join(shells) or 'none'))
if not want_shells and shells:
    bad.append('started %s without r8s.debug' % ' '.join(shells))
if not want_boot and not want_shells and not nologin:
    bad.append('stays without a word of how to get a shell')
if bad:
    print('bad', '; '.join(bad))
elif want_boot:
    print('ok', '%sstarts VibeOS (%s)' % ('telnet + ssh up, then ' if want_shells else 'no shell, ', moved[-1]))
else:
    print('ok', 'stays, ' + ('telnet + ssh up' if want_shells else 'no shell, says how to get one'))
EOF
}
# verdict JSON CASE: plain and marker must start Mobian, rescue must stay
verdict() {
	python3 -I - "$1" "$2" <<'EOF'
import json, sys
d = json.load(open(sys.argv[1])); case = sys.argv[2]
moved = [l for l in d['stublog'] if l.startswith('mount --move ') and '/mnt/root/' in l]
booted = d.get('init_exit') is not None and moved and not d['left_running']
stayed = d.get('init_exit') is None and d['running_at_end'] and any('staying' in l for l in d['kmsg'])
last = (d['kmsg'] or ['(no kernel log)'])[-1].strip()
if case in ('nolist', 'outside', 'stale'):
    touched = [l for l in d['stublog'] if l.startswith(('e2fsck', 'resize2fs')) or (l.startswith('mount ') and ' ext4 ' in l)
               or (case != 'stale' and l.startswith('dmlinear'))]
    said = any('device zip' in l.lower() for l in d['kmsg'])
    if stayed and said and not touched:
        print('ok stays, says to sideload the device zip, maps and checks nothing')
    else:
        print('bad', 'did not stay: ' + last if not stayed else 'touched the disk: ' + '; '.join(touched[:3]) if touched else 'stays, but no word of the device zip: ' + last)
elif case == 'rescue':
    print('ok' if stayed else 'bad', 'stays in the initramfs' if stayed else 'did not stay: ' + last)
else:
    print('ok' if booted else 'bad', 'starts Mobian (%s)' % moved[-1] if booted else 'did not start Mobian: ' + last)
EOF
}

for DD; do
	DD=$(cd "$DD" && pwd) || { bad "no $DD"; continue; }
	T=$(mktemp -d "${TMPDIR:-/tmp}/init-test.XXXXXX")
	mkdir -p "$T/rd"
	if tar -tf "$DD/payload.tar" ./boot/s10e/initramfs.cpio.gz > /dev/null 2>&1; then
		DEV=s10e
		# regulatory.db follows the gzip member as a second archive: gzip warns about it, cpio reads the first
		tar -xOf "$DD/payload.tar" ./boot/s10e/initramfs.cpio.gz | gzip -dc 2> /dev/null | (cd "$T/rd" && cpio -idm --quiet --no-absolute-filenames 2> /dev/null)
	elif [ -f "$DD/boot/recovery.img" ] && [ "$(dd if="$DD/boot/recovery.img" bs=1M skip=34 count=1 2> /dev/null | head -c 8)" = 'ANDROID!' ]; then
		DEV=s5e
		python3 -I - "$DD/boot/recovery.img" "$T/ramdisk" <<'EOF'
import struct, sys
b = open(sys.argv[1], 'rb').read()[34 << 20:]
assert b[:8] == b'ANDROID!'
ks, ka, rs = struct.unpack_from('<III', b, 8)
ps, = struct.unpack_from('<I', b, 36)
o = ps + -(-ks // ps) * ps
open(sys.argv[2], 'wb').write(b[o:o + rs])
EOF
		gzip -dc < "$T/ramdisk" 2> /dev/null | (cd "$T/rd" && cpio -idm --quiet --no-absolute-filenames 2> /dev/null)
	elif [ -f "$DD/root/init" ] && head -n 2 "$DD/root/init" | grep -q 'Galaxy S20 FE (r8s)'; then
		DEV=s20
		cp -a "$DD/root/." "$T/rd/"
	else
		echo "== $DD: no S10e, S5e or S20 initramfs, skipped"; rm -rf "${T:?}"; continue
	fi
	echo "== $DEV ($DD): /init $(sha256sum < "$T/rd/init" | cut -c1-16)"
	[ -x "$T/rd/bin/busybox" ] || { bad "no busybox in the initramfs"; rm -rf "${T:?}"; continue; }
	CASES="plain marker rescue"; [ $DEV != s5e ] || CASES="$CASES imgok nolist outside stale"
	[ $DEV != s20 ] || CASES="plain debugroot rescue oldrescue noroot"
	for c in $CASES; do
		D=$T/$c; cp -a "$T/rd" "$D"
		prep_$DEV "$D" $c
		case $DEV-$c in
		s10e-rescue) CL=$S10E_RESCUE ;; s10e-*) CL=$S10E_NORMAL ;;
		s5e-rescue) CL=$S5E_RESCUE ;; s5e-*) CL=$S5E_NORMAL ;;
		s20-debugroot) CL="$S20_NORMAL r8s.debug" ;; s20-rescue) CL=$S20_RESCUE ;; s20-oldrescue) CL=$S20_OLDRESCUE ;;
		s20-*) CL=$S20_NORMAL ;;
		esac
		# the S20's init looks for its root partition for 10 s before it gives up (noroot)
		WT=12; [ $DEV != s20 ] || WT=25
		unshare -rmn --propagation private python3 -I "$H/init-case.py" "$D" "$CL" $WT "$T/$c.json" 2> "$T/$c.err" ||
			{ bad "$c: the case did not run: $(tail -n 2 "$T/$c.err" | tr '\n' ' ')"; continue; }
		if [ $DEV = s20 ]; then R=$(verdict_s20 "$T/$c.json" $c); else R=$(verdict "$T/$c.json" $c); fi
		if [ "${R%% *}" = ok ]; then ok "$c: ${R#* }"; else bad "$c: ${R#* }"; fi
	done
	rm -rf "${T:?}"
done
echo "== $FAILS failures"
[ $FAILS = 0 ]
