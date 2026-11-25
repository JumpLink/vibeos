#!/bin/sh
# s20/make-boot.sh OUT.img: the Galaxy S20 FE's boot image (BOOT), the one its device zip writes (S20_BOOT):
#   header, DT table  the stock BOOT's (S20_STOCK_BOOT): S-Boot lays the dtbo partition's overlay over that table and
#                     starts the "kernel"
#   kernel            the shim with U-Boot (S20_UBSHIM: S20_DIR/kit/uboot/build-ubshim.sh of the u-boot.bin of the
#                     branch r8s-ub13: ub13, with r8s.debug on "Rescue shell (USB)")
#   ramdisk           a FIT (S20_DIR/kit/uboot/r8s-fit.its), which U-Boot boots: Image.gz of S20_KBUILD's Image (its
#                     initramfs, s20/build-initramfs.sh's, built in) and the devicetree S20_DTB
# zero-padded to BOOT's 59 MiB by S20_DIR/kit/mkboot-r8s.py. S20_MKIMAGE: U-Boot's tools/mkimage; SOURCE_DATE_EPOCH
# (default: the Image's mtime) is the FIT's timestamp. Then s20/check-boot.py holds it to what a public image must
# start; it prints what it checked, and a failure fails this.
set -eu
T=$(cd "$(dirname "$0")/.." && pwd)
[ ! -f "$T/local.conf" ] || . "$T/local.conf"
OUT=${1:?OUT.img}
G=${S20_DIR:?set S20_DIR}
KB=${S20_KBUILD:?set S20_KBUILD: the build directory of the release kernel}
DTB=${S20_DTB:?set S20_DTB: the devicetree the FIT carries}
UBSHIM=${S20_UBSHIM:?set S20_UBSHIM: the shim with U-Boot}
STOCK=${S20_STOCK_BOOT:?set S20_STOCK_BOOT: the stock BOOT image}
MK=${S20_MKIMAGE:?set S20_MKIMAGE: mkimage}
IMG=$KB/arch/arm64/boot/Image
[ ! -e "$OUT" ] || { echo "$OUT exists: move it aside first" >&2; exit 1; }
export SOURCE_DATE_EPOCH=${SOURCE_DATE_EPOCH:-$(stat -c %Y "$IMG")}
W=$(mktemp -d "${TMPDIR:-/tmp}/s20-boot.XXXXXX")
trap 'rm -rf "$W"' EXIT
gzip -9 -n -c "$IMG" > "$W/Image.gz"
cp "$DTB" "$W/exynos990-r8s.dtb"
cp "$G/kit/uboot/r8s-fit.its" "$W/r8s-fit.its"
(cd "$W" && "$MK" -f r8s-fit.its r8s.itb > mkimage.log) || { cat "$W/mkimage.log" >&2; exit 1; }
python3 -I "$G/kit/mkboot-r8s.py" "$STOCK" "$UBSHIM" "$W/r8s.itb" "$OUT.part"
python3 -I "$T/s20/check-boot.py" "$OUT.part" "$IMG" "$T/s20/init" || { echo "$OUT.part: not a boot image to publish" >&2; exit 1; }
mv "$OUT.part" "$OUT"
echo "$OUT: $(sha256sum < "$OUT" | cut -c1-16), kernel $(cat "$KB/include/config/kernel.release"), FIT $(stat -c %s "$W/r8s.itb") bytes"
