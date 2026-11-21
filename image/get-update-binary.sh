#!/bin/sh
# get-update-binary.sh LINEAGEOS.zip [OUT]: LineageOS's recovery updater (META-INF/com/google/android/update-binary)
# out of a LineageOS build zip, for the VibeOS zips: build-zips.sh and nura/nura-to-vibeos take it from
# image/zip/update-binary (or UPDATE_BINARY). The 0.1.3 zips carry the one in
# lineage-22.2-20261102-nightly-surya-signed.zip; another LineageOS 22 arm64 build's should do the same, but only that
# one is tested. The file is LineageOS's (Apache-2.0), so it is not in this repository.
set -eu
Z=${1:?LINEAGEOS.zip}
OUT=${2:-$(cd "$(dirname "$0")" && pwd)/zip/update-binary}
TESTED=3748b295ec2a0340419b36d826dc72c9a1eb0316c0f1141f632d339d10cd2ff1
unzip -p "$Z" META-INF/com/google/android/update-binary > "$OUT.new" || true
[ -s "$OUT.new" ] || { rm -f "$OUT.new"; echo "$Z has no META-INF/com/google/android/update-binary" >&2; exit 1; }
# an ELF file for arm64 (e_machine 0xb7)
[ "$(od -An -tx1 -N4 "$OUT.new" | tr -d ' \n')" = 7f454c46 ] && [ "$(od -An -tx1 -j18 -N2 "$OUT.new" | tr -d ' \n')" = b700 ] ||
	{ rm -f "$OUT.new"; echo "not an arm64 ELF program" >&2; exit 1; }
chmod 644 "$OUT.new"; mv "$OUT.new" "$OUT"	# recovery makes it executable when it runs it
S=$(sha256sum < "$OUT" | cut -d' ' -f1)
if [ "$S" = "$TESTED" ]; then echo "$OUT: the tested update-binary"
else echo "$OUT: sha256 $S, not the tested one ($TESTED): build-zips.sh says so, and STRICT=1 refuses it"; fi
