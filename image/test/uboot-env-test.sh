#!/bin/bash
# uboot-env-test.sh DEVDIR...: what the U-Boot in a device folder (build-device.sh's output) will boot, read from its
# built-in default environment and checked against the folder's own files. Static: no U-Boot runs (there is no
# emulator for these boards). The zips' harness proves the right bytes reach the right partition; this proves those
# bytes look for what the zips install:
#   s10e  bootcmd boots s10e_boot, which loads /boot/s10e/Image (or Image.prev), initramfs.cpio.gz and bootargs.env
#         from the partition named linux, all of them in payload.tar; the root argument is PARTLABEL=linux
#   poco  preboot maps the partition named linux and bootcmd is bootefi bootmgr; the ESP has EFI/BOOT/BOOTAA64.EFI
#         and a loader.conf whose default entry boots /vmlinuz-mobian with root=LABEL=vibeos
#   s5e   linux reads the Android image at 34 MiB of recovery, lineage_recovery the one at 4 MiB, both Android
#         images in recovery.img; the rescue entry adds gts4lv.debug
#   s20   (BOOT = boot/boot.img: an Android image whose kernel is the shim with U-Boot, whose ramdisk is a FIT) bootcmd
#         and VibeOS's menu entry boot that FIT with r8s_args, which name the partition vibeos and carry no
#         r8s.debug; "Rescue shell (USB)" boots it with r8s_args_rescue, which carry r8s.debug; the menu keeps
#         LineageOS Recovery; the FIT holds a kernel, an fdt and the configuration r8s
# The folder's name says the device (s10e, poco, s5e, s20); any other folder is skipped.
set -u
F=0
ok() { echo "  ok: $*"; }
bad() { echo "  FAIL: $*"; F=$((F + 1)); }
# env IMAGE: the default environment of the U-Boot in IMAGE (an Android boot image whose kernel is U-Boot, gzip'd
# or not), one variable per line
env_of() {
	python3 -I - "$1" <<'EOF'
import sys, struct, zlib, re
d = open(sys.argv[1], 'rb').read()
if d[:8] == b'ANDROID!':
    ks, = struct.unpack_from('<I', d, 8); ps, = struct.unpack_from('<I', d, 36)
    d = d[ps:ps + ks]
    if d[:2] == b'\x1f\x8b':
        d = zlib.decompressobj(31).decompress(d)
i = d.find(b'\0bootcmd=')
if i < 0:
    sys.exit('no default environment (bootcmd=) in ' + sys.argv[1])
s = i
while True:
    j = d.rfind(b'\0', 0, s)
    if j < 0 or not re.fullmatch(rb'[A-Za-z0-9_.:-]+=[\x09\x20-\x7e]*', d[j + 1:s]):
        break
    s = j
for v in d[s + 1:d.find(b'\0\0', i + 1)].split(b'\0'):
    print(v.decode('latin-1'))
EOF
}
var() { sed -n "s|^$1=||p" "$E" | head -n 1; }
has() { case $(var "$1") in *"$2"*) return 0 ;; *) return 1 ;; esac; }
W=$(mktemp -d); trap 'rm -rf "$W"' EXIT
for D in "$@"; do
	DEV=$(basename "$D")
	E=$W/$DEV.env
	echo "== $DEV ($D)"
	case $DEV in
	s10e)
		env_of "$D/boot/uboot.img" > "$E" || { bad "no environment in boot/uboot.img"; continue; }
		has bootcmd 'run s10e_boot' && ok "bootcmd boots s10e_boot" || bad "bootcmd: $(var bootcmd)"
		has s10e_load 'part number scsi 0 linux' && ok "s10e_load finds the partition named linux" || bad "s10e_load: no linux"
		L=$(tar -tf "$D/payload.tar" | sed -n 's|^\./||p')
		for f in "$(var s10e_image)" Image.prev initramfs.cpio.gz bootargs.env; do
			case $f in Image.prev) w='s10e_image Image.prev' v=bootmenu_1 ;; *) w=/boot/s10e/ v=s10e_load ;; esac
			has $v "$w" && echo "$L" | grep -qx "boot/s10e/$f" && ok "/boot/s10e/$f: U-Boot loads it, payload.tar has it" ||
				bad "/boot/s10e/$f: not both in U-Boot ($v) and payload.tar"
		done
		has s10e_load '/boot/s10e/initramfs.cpio.gz' && has s10e_load '/boot/s10e/bootargs.env' || bad "s10e_load does not load initramfs.cpio.gz and bootargs.env"
		has s10e_args 's10e.root=PARTLABEL=linux' && ok "the root: PARTLABEL=linux" || bad "s10e_args: $(var s10e_args)"
		;;
	poco)
		env_of "$D/boot/boot.img" > "$E" || { bad "no environment in boot/boot.img"; continue; }
		has preboot 'part start scsi 0 linux' && has preboot 'blkmap map root' && ok "preboot maps the partition named linux" ||
			bad "preboot: $(var preboot)"
		has bootcmd 'bootefi bootmgr' && ok "bootcmd: bootefi bootmgr" || bad "bootcmd: $(var bootcmd)"
		for f in ::/EFI/BOOT/BOOTAA64.EFI ::/vmlinuz-mobian ::/initrd-mobian; do
			[ "$(mcopy -n -i "$D/boot/esp.img" $f - 2>/dev/null | head -c 1 | wc -c)" = 1 ] && ok "ESP: ${f#::/}" || bad "ESP: no ${f#::/}"
		done
		C=$(mcopy -n -i "$D/boot/esp.img" ::/loader/loader.conf - 2>/dev/null)
		DE=$(echo "$C" | sed -n 's/^default  *//p')
		N=$(mcopy -n -i "$D/boot/esp.img" "::/loader/entries/$DE" - 2>/dev/null)
		[ -n "$DE" ] && echo "$N" | grep -q '^linux */vmlinuz-mobian$' && echo "$N" | grep -q '^initrd */initrd-mobian$' &&
			ok "loader.conf's default $DE boots /vmlinuz-mobian with /initrd-mobian" || bad "loader.conf default '$DE': no such entry, or not vmlinuz-mobian"
		echo "$N" | grep -q '^options .*root=LABEL=vibeos' && ok "the root: LABEL=vibeos" || bad "entry options: $(echo "$N" | grep '^options' | cut -c1-120)"
		;;
	s5e)
		env_of "$D/boot/recovery.img" > "$E" || { bad "no environment in boot/recovery.img"; continue; }
		has bootcmd 'run linux' && ok "bootcmd boots linux" || bad "bootcmd: $(var bootcmd)"
		has load_aimg 'part start mmc 0 recovery' && ok "load_aimg reads the recovery partition" || bad "load_aimg: $(var load_aimg | cut -c1-100)"
		for x in linux:34 lineage_recovery:4; do
			v=${x%%:*} m=${x##*:}
			o=$(var $v | sed -n 's/.*img_off \(0x[0-9a-fA-F]*\).*/\1/p')
			[ -n "$o" ] && [ $((o * 512)) = $((m * 1048576)) ] &&
				[ "$(dd if="$D/boot/recovery.img" bs=1048576 skip=$m count=1 status=none | head -c 8)" = ANDROID! ] &&
				ok "$v: the Android image at $m MiB of recovery (img_off $o)" || bad "$v: img_off '$o', or no Android image at $m MiB"
		done
		has linux_rescue gts4lv.debug && ok "the rescue entry passes gts4lv.debug" || bad "linux_rescue: $(var linux_rescue)"
		;;
	s20)
		env_of "$D/boot/boot.img" > "$E" || { bad "no environment in boot/boot.img"; continue; }
		has bootcmd 'run r8s_boot' && ok "bootcmd boots r8s_boot" || bad "bootcmd: $(var bootcmd)"
		for x in r8s_boot:r8s_args r8s_rescue:r8s_args_rescue; do
			has ${x%%:*} "bootargs \"\${${x#*:}}\"" && has ${x%%:*} 'bootm ${prevbl_initrd_start_addr}' &&
				ok "${x%%:*} boots the boot image's FIT with ${x#*:}" || bad "${x%%:*}: $(var ${x%%:*} | cut -c1-160)"
		done
		has r8s_args r8s.root=vibeos && ! has r8s_args r8s.debug && ok "r8s_args: the partition named vibeos, no r8s.debug" ||
			bad "r8s_args: $(var r8s_args)"
		has r8s_args_rescue r8s.debug && ok "r8s_args_rescue: r8s.debug (the initramfs's shells)" || bad "r8s_args_rescue: $(var r8s_args_rescue)"
		grep -q '^bootmenu_[0-9]*=VibeOS=run r8s_boot$' "$E" && grep -q '^bootmenu_[0-9]*=Rescue shell (USB)=run r8s_rescue$' "$E" &&
			grep -q '^bootmenu_[0-9]*=Lineage Recovery=run reboot_recovery$' "$E" &&
			ok "the menu: VibeOS, Rescue shell (USB), Lineage Recovery" || bad "the menu: $(grep '^bootmenu_[0-9]' "$E" | tr '\n' ';')"
		python3 -I - "$D/boot/boot.img" <<'EOF' && ok "the ramdisk is a FIT: kernel, fdt, configuration r8s" || bad "the ramdisk is no FIT of a kernel and an fdt"
import struct, sys
d = open(sys.argv[1], 'rb').read()
assert d[:8] == b'ANDROID!'
ks, ka, rs = struct.unpack_from('<III', d, 8); ps, = struct.unpack_from('<I', d, 36)
r = d[ps + -(-ks // ps) * ps:][:rs]
assert r[:4] == bytes.fromhex('d00dfeed')
off_struct, off_strings = struct.unpack_from('>II', r, 8)
names, i = [], off_struct
while True:
    tok, = struct.unpack_from('>I', r, i); i += 4
    if tok == 1:
        e = r.index(b'\0', i); names.append(r[i:e].decode()); i = (e + 4) & ~3
    elif tok == 3:
        ln, = struct.unpack_from('>I', r, i); i = (i + 8 + ln + 3) & ~3
    elif tok in (2, 4):
        continue
    else:
        break
assert {'images', 'kernel', 'fdt', 'configurations', 'r8s'} <= set(names), names
EOF
		;;
	*) echo "  (skipped: no U-Boot check for $DEV)" ;;
	esac
done
echo "== $F failures"
[ $F = 0 ]
