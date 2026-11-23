#!/bin/sh
# device-audit.sh DEVDIR...: what a device folder (build-device.sh's output: payload.tar, debs/, boot/) would
# publish of the machine that built it. A leak is:
#   - a resolv.conf backup, or an ssl-cert snakeoil key or certificate
#   - this machine's user or host name as a word in a text file
#   - a /home/<user>/ path of this machine
#   - a kernel whose banner names this machine, wherever the kernel sits: kernel-names.py decompresses first (an
#     Android boot image, a recovery partition, an EFI zboot image on a FAT ESP, a FIT image, an initramfs)
# Binary files and compressed modules that hold one of the names as a word are listed to look at, not as leaks: a
# short host name can be part of a module alias. AUDIT_NAMES adds names (colon separated). Exit 1 on any leak.
set -u
TD=$(cd "$(dirname "$0")" && pwd)
U=$(id -un)
AUDIT_NAMES="$U:$(hostname -s):$(uname -n)${AUDIT_NAMES:+:$AUDIT_NAMES}"
export AUDIT_NAMES
rc=0
for D; do
	[ -s "$D/payload.tar" ] || { echo "$D: no payload.tar" >&2; rc=1; continue; }
	T=$(mktemp -d "${TMPDIR:-/tmp}/device-audit.XXXXXX")
	echo "$AUDIT_NAMES" | tr ':' '\n' | sort -u > "$T/names"
	mkdir -p "$T/x/payload" "$T/x/deb" "$T/x/boot"
	tar -C "$T/x/payload" -xf "$D/payload.tar"
	for d in "$D"/debs/vibeos-device-*.deb; do
		[ -f "$d" ] || continue
		dpkg-deb -x "$d" "$T/x/deb"; dpkg-deb -e "$d" "$T/x/deb/DEBIAN"
	done
	# boot/: FAT images as their files (an ESP's kernel and initrd), everything else as it is
	for f in "$D"/boot/*; do
		[ -f "$f" ] || continue
		if mdir -i "$f" :: > /dev/null 2>&1; then
			mkdir -p "$T/x/boot/${f##*/}.files"; mcopy -s -n -i "$f" '::*' "$T/x/boot/${f##*/}.files/"
		else
			ln -s "$(cd "${f%/*}" && pwd)/${f##*/}" "$T/x/boot/${f##*/}"
		fi
	done
	n=0; c=0
	leak() { echo "LEAK $*"; n=$((n + 1)); }
	echo "== $D"
	(cd "$T/x" && find . \( -name '.resolv.conf.systemd-resolved.bak' -o -name 'ssl-cert-snakeoil.*' \) | sed 's|^\./||') > "$T/l"
	while read -r f; do leak "$f"; done < "$T/l"
	grep -RIlwi -F -f "$T/names" "$T/x" | sed "s|^$T/x/||" > "$T/l"
	while read -r f; do leak "a build name in the text file $f"; done < "$T/l"
	grep -RlaF "/home/$U/" "$T/x" | sed "s|^$T/x/||" > "$T/l"
	while read -r f; do leak "/home/$U/ in $f"; done < "$T/l"
	grep -Rlawi -F -f "$T/names" "$T/x" | sed "s|^$T/x/||" > "$T/l"
	while read -r f; do
		grep -qIwi -F -f "$T/names" "$T/x/$f" 2> /dev/null && continue	# a text file: above
		echo "CHECK a build name as a word in the binary $f"; c=$((c + 1))
	done < "$T/l"
	(cd "$T/x" && find . -type f \( -name '*.ko.zst' -o -name '*.ko.xz' -o -name '*.ko.gz' \) | sed 's|^\./||') > "$T/l"
	while read -r f; do
		case $f in *.zst) z="zstd -dc" ;; *.xz) z="xz -dc" ;; *) z="gzip -dc" ;; esac
		$z "$T/x/$f" 2> /dev/null > "$T/ko"
		if grep -qaF "/home/$U/" "$T/ko"; then leak "/home/$U/ in the module $f"
		elif grep -qawi -F -f "$T/names" "$T/ko"; then echo "CHECK a build name as a word in the module $f"; c=$((c + 1)); fi
	done < "$T/l"
	# kernels: the boot pieces, and in the payload and the package the big files and anything named like a kernel,
	# an initramfs or a boot image
	find -L "$T/x" -type f \( -size +4M -o -name 'Image*' -o -name 'vmlinu*' -o -name 'zImage*' -o -name 'initr*' \
		-o -name '*.cpio*' -o -name '*.img' -o -name '*.efi' -o -name '*.itb' -o -name 'boot*' -o -path "$T/x/boot/*" \) \
		! -name '*.ko*' -print0 | sort -z | xargs -0 -r python3 -I "$TD/kernel-names.py" --base "$T/x" > "$T/kernels"
	kr=$?
	cat "$T/kernels"
	k=$(grep -cE '^(LEAK )?kernel ' "$T/kernels")
	[ "$k" -gt 0 ] || { echo "LEAK CHECK FAILED: no kernel found in $D"; n=$((n + 1)); }
	[ "$kr" -eq 0 ] || n=$((n + $(grep -c '^LEAK' "$T/kernels")))
	c=$((c + $(grep -c '^CHECK' "$T/kernels")))
	echo "== $D: $n leak(s), $c to look at, $k kernel banner(s) read"
	[ "$n" -eq 0 ] || rc=1
	rm -rf "${T:?}"
done
exit $rc
