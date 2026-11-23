#!/bin/sh
# build-zips.sh system|s10e|poco|s5e: the LineageOS recovery zips of VibeOS 0.1.3, from the built pieces:
#   system  OUT/system/vibeos-system-0.1.3-arm64.zip: rootfs.img (OUT/system/rootfs.img) + the generic installer
#   DEVICE  OUT/DEVICE/vibeos-device-DEVICE-0.1.3.zip: payload.tar, debs/, the boot pieces + the device installer
# Every file the installers write is listed with its size and sha256 in the zip's sums; the installers read each
# one back against it. Stored (zip -0) for the big images: recovery streams them to the device.
set -eu
umask 022	# the modes stored in the zip, whoever builds it (the 0.1.3 zips were built as root)
WHAT=${1:?system|s10e|poco|s5e}
T=$(cd "$(dirname "$0")" && pwd)
[ ! -f "$T/local.conf" ] || . "$T/local.conf"	# this machine's settings (local.conf.example)
O=${VIBEOS_OUT:?set VIBEOS_OUT (image/local.conf): the directory builds, work files and logs go to}
FORBID="gsm2sip waydroid ${FORBID_EXTRA:-}"	# never in an image: these package and file names
Z=$T/zip
# STRICT=1: only the exact binaries the 0.1.3 zips have (LineageOS 22.2's update-binary 3748b295, vibeos-part d108d33d:
# make -C src with Debian's aarch64-linux-gnu-gcc 14.2.0, the source with "nest" (the POCO's own GPT, 2026-11-23); the
# source before it gives ead00bed, the 11-22 and 11-23 system zips', and 1370fac's fe0cd129, the first 0.1.3 one's)
same() {	# FILE SUM16 WHAT
	[ "$(sha256sum < "$1" | cut -c1-16)" = "$2" ] && return 0
	[ -z "${STRICT:-}" ] || { echo "$3 is not the 0.1.3 zips' ($2)" >&2; exit 1; }
	echo "build-zips: note: $3 differs from the 0.1.3 zips' ($2)" >&2
}
UB=${UPDATE_BINARY:-$T/zip/update-binary}
[ -s "$UB" ] || { echo "no $UB: image/get-update-binary.sh LINEAGEOS.zip makes it" >&2; exit 1; }
same "$UB" 3748b295ec2a0340 update-binary
W=$(mktemp -d "$O/tmp/zip-$WHAT.XXXXXX")
mkdir -p "$W/META-INF/com/google/android"
cp "$UB" "$W/META-INF/com/google/android/update-binary"
cp "$Z/vibeos-lib.sh" "$W/vibeos-lib.sh"
sum() { set -- "$1" "$2" "$(stat -L -c %s "$2")" "$(sha256sum < "$2" | cut -d' ' -f1)"; echo "$1 $3 $4"; }
case $WHAT in
system)
	S=${SYS_DIR:-$O/system}
	[ -s "$S/rootfs.img" ] || { echo "no $S/rootfs.img" >&2; exit 1; }
	cp "$Z/system-install.sh" "$W/install.sh"
	cp "$Z/system/updater-script" "$W/META-INF/com/google/android/updater-script"
	# the GPT tool, from src/ (make -C src)
	VP=${VIBEOS_PART:-$T/src/s10e-part}
	[ -s "$VP" ] || { echo "no $VP: make -C image/src" >&2; exit 1; }
	cp "$VP" "$W/vibeos-part"
	same "$W/vibeos-part" d108d33dd19bfd05 vibeos-part
	# rootfs.img in parts of CHUNK_MB (1 GiB): LineageOS's updater cannot extract an entry of 4 GiB or more (32-bit
	# ZipEntry), and the image is bigger than that. install.sh chunks maps each part's place; the updater writes it
	CB=$((${CHUNK_MB:-1024} * 1048576))
	sum rootfs "$S/rootfs.img" > "$W/vibeos-sums.txt"
	RBYTES=$(stat -L -c %s "$S/rootfs.img")
	[ $((RBYTES % 4096)) = 0 ] || { echo "rootfs.img is not whole 4 KiB blocks" >&2; exit 1; }
	split -b $CB -d -a 1 "$S/rootfs.img" "$W/rootfs.img."
	NCH=$(ls "$W" | grep -c '^rootfs\.img\.[0-9]$')
	[ "$NCH" = $(( (RBYTES + CB - 1) / CB )) ] && [ $NCH -le 10 ] || { echo "split made $NCH parts" >&2; exit 1; }
	[ "$(cat $(i=0; while [ $i -lt $NCH ]; do echo "$W/rootfs.img.$i"; i=$((i + 1)); done) | sha256sum | cut -d' ' -f1)" = \
		"$(cut -d' ' -f3 "$W/vibeos-sums.txt")" ] || { echo "the parts do not make rootfs.img" >&2; exit 1; }
	echo "chunks $NCH $CB" >> "$W/vibeos-sums.txt"
	WRITE=$(i=0; while [ $i -lt $NCH ]; do
		printf 'ui_print("  part %d of %d");\npackage_extract_file("rootfs.img.%d", "/tmp/vibeos-root.%d") ||\n    abort("ERROR: could not write part %d of %d of the system");\n' \
			$((i + 1)) $NCH $i $i $((i + 1)) $NCH
		[ $((i + 1)) = $NCH ] || printf 'set_progress(%s);\n' "$(awk -v i=$i -v n=$NCH 'BEGIN {printf "%.6f", 0.02 + 0.88 * (i + 1) / n}')"
		i=$((i + 1)); done)
	U=$W/META-INF/com/google/android/updater-script
	awk -v w="$WRITE" '$0 == "@WRITE@" {print w; next} {print}' "$U" > "$U.new" && mv "$U.new" "$U"
	! grep -q '@WRITE@' "$U" || exit 1
	OUTZ=$S/vibeos-system-0.1.3-arm64.zip
	FILES="META-INF vibeos-lib.sh install.sh vibeos-part vibeos-sums.txt $(cd "$W" && ls rootfs.img.[0-9] | tr '\n' ' ')"
	;;
s10e|poco|s5e)
	D=${DEV_DIR:-$O/$WHAT}
	[ -s "$D/payload.tar" ] || { echo "no $D/payload.tar: build-device.sh $WHAT first" >&2; exit 1; }
	sed "s/@DEV@/$WHAT/" "$Z/device-install.sh" > "$W/install.sh"
	cp "$D/payload.tar" "$W/payload.tar"; cp -r "$D/debs" "$W/debs"
	{ sum payload "$W/payload.tar"; for f in "$W"/debs/*.deb; do echo "deb $(basename "$f") $(sha256sum < "$f" | cut -d' ' -f1)"; done; } > "$W/device-sums.txt"
	case $WHAT in
	s10e)
		NAME="Galaxy S10e" CODES=beyond0lte
		ASSERT='getprop("ro.product.device") == "beyond0lte" || getprop("ro.build.product") == "beyond0lte"'
		cp "$D/boot/uboot.img" "$D/boot/s10e-logo" "$W/"; cp -r "$D/boot/boot-logo" "$W/boot-logo"
		sum uboot "$W/uboot.img" >> "$W/device-sums.txt"
		TOOLS=
		BOOTFILES='package_extract_file("uboot.img", "/tmp/vibeos-uboot.img") || abort("ERROR: could not unpack U-Boot");
package_extract_file("s10e-logo", "/tmp/vibeos-logo");
package_extract_dir("boot-logo", "/tmp/vibeos-boot-logo");'
		DONE='ui_print("Power on starts Android (or Linux, with no Android).");
ui_print("Restart to recovery starts Linux. Restarting into it.");
run_program("/system/bin/setprop", "sys.powerctl", "reboot,recovery");'
		BOOTF="uboot.img s10e-logo boot-logo" ;;
	poco)
		NAME="POCO X3 NFC" CODES="surya, karna"
		ASSERT='getprop("ro.product.device") == "surya" || getprop("ro.build.product") == "surya" || getprop("ro.product.device") == "karna" || getprop("ro.build.product") == "karna"'
		# U-Boot in BOOT (boot.img), with LineageOS's dtbo and vbmeta of its build; the recovery partition stays
		# LineageOS's (oldrecovery: only the sum of the U-Boot an earlier install put there)
		cp "$D/boot/esp.img" "$D/boot/boot.img" "$D/boot/dtbo.img" "$D/boot/vbmeta.img" "$W/"
		for f in esp boot dtbo vbmeta; do sum $f "$W/$f.img" >> "$W/device-sums.txt"; done
		grep -E '^oldrecovery [0-9]+ [0-9a-f]{64}$' "$D/boot/oldrecovery.sum" >> "$W/device-sums.txt"
		TOOLS=
		BOOTFILES='package_extract_file("esp.img", "/tmp/vibeos-esp.img") || abort("ERROR: could not unpack the ESP");
package_extract_file("boot.img", "/tmp/vibeos-boot.img") || abort("ERROR: could not unpack U-Boot");
package_extract_file("dtbo.img", "/tmp/vibeos-dtbo.img") || abort("ERROR: could not unpack dtbo");
package_extract_file("vbmeta.img", "/tmp/vibeos-vbmeta.img") || abort("ERROR: could not unpack vbmeta");'
		DONE='ui_print("Power on starts VibeOS; Volume Up + Power starts LineageOS recovery. Restarting.");
run_program("/system/bin/setprop", "sys.powerctl", "reboot");'
		BOOTF="esp.img boot.img dtbo.img vbmeta.img" ;;
	s5e)
		NAME="Galaxy Tab S5e Wi-Fi" CODES=gts4lvwifi
		ASSERT='getprop("ro.product.device") == "gts4lvwifi" || getprop("ro.build.product") == "gts4lvwifi"'
		cp "$D/boot/recovery.img" "$D/boot/extents" "$D/boot/keepmisc" "$D/boot/dmlinear" "$W/"
		sum recovery "$W/recovery.img" >> "$W/device-sums.txt"
		TOOLS='package_extract_file("extents", "/tmp/vibeos-extents");
package_extract_file("dmlinear", "/tmp/vibeos-dmlinear");
package_extract_file("keepmisc", "/tmp/vibeos-km");'
		BOOTFILES='package_extract_file("recovery.img", "/tmp/vibeos-recovery.img") || abort("ERROR: could not unpack the recovery image");'
		DONE='ui_print("Reboot system now starts Linux; the boot menu also offers Android.");'
		BOOTF="recovery.img extents keepmisc dmlinear" ;;
	esac
	python3 -I - "$Z/device/updater-script.in" "$W/META-INF/com/google/android/updater-script" "$NAME" "$CODES" "$ASSERT" "$TOOLS" "$BOOTFILES" "$DONE" <<'EOF'
import sys
src,dst,name,codes,a,tools,bootf,done=sys.argv[1:]
s=open(src).read()
for k,v in (("@NAME@",name),("@CODES@",codes),("@ASSERT@",a),("@TOOLS@",tools),("@BOOTFILES@",bootf),("@DONE@",done)):
    assert k in s,k; s=s.replace(k,v)
open(dst,'w').write(s)
EOF
	OUTZ=$D/vibeos-device-$WHAT-0.1.3.zip
	FILES="META-INF vibeos-lib.sh install.sh device-sums.txt payload.tar debs $BOOTF"
	;;
*) echo "unknown $WHAT" >&2; exit 1 ;;
esac
[ ! -e "$OUTZ" ] || { echo "$OUTZ exists: move it aside first" >&2; exit 1; }
grep -rl -i -E "$(echo $FORBID | tr ' ' '|')" "$W/install.sh" "$W/vibeos-lib.sh" "$W/META-INF/com/google/android/updater-script" && { echo "forbidden name in the installer" >&2; exit 1; }
sh -n "$W/install.sh" && sh -n "$W/vibeos-lib.sh"
(cd "$W" && find $FILES -exec touch -h -d '2026-11-21 12:00:00' {} + && zip -q -0 -r -X "$OUTZ.part" $FILES)
mv "$OUTZ.part" "$OUTZ"
rm -rf "${W:?}"
sha256sum "$OUTZ" | sed "s|$O/||"
