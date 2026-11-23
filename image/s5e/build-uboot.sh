#!/bin/sh
# build-uboot.sh UBOOT_TREE OUT [ENV]: the S5e's dual-boot U-Boot, OUT/u-boot-nodtb.bin, with ENV (default: uboot.env
# beside this script) as its default environment. UBOOT_TREE is the gts4lv U-Boot tree, its build/.config the
# configuration; the build runs in a copy of it (OUT/src), so the tree is not changed. It carries the 11-11 build's
# version and time stamp: with the environment 0.1.3 shipped (git show 1f66f7c:image/s5e/uboot.env) it gives that
# zip's U-Boot back byte for byte.
set -eu
T=${1:?UBOOT_TREE}
OUT=${2:?OUT}
H=$(cd "$(dirname "$0")" && pwd)
ENV=${3:-$H/uboot.env}
[ -f "$T/build/.config" ] || { echo "$T/build/.config: none" >&2; exit 1; }
grep -qx 'CONFIG_ENV_DEFAULT_ENV_TEXT_FILE="board/qualcomm/gts4lv.env"' "$T/build/.config" ||
	{ echo "$T/build/.config: not the dual-boot configuration (gts4lv.env)" >&2; exit 1; }
mkdir -p "$OUT/src/build" "$OUT/bin"
rsync -a --exclude /build --exclude '/build-*' --exclude '/*.log' --exclude /.git "$T/" "$OUT/src/"
cp "$T/build/.config" "$OUT/src/build/.config"
cp "$ENV" "$OUT/src/board/qualcomm/gts4lv.env"
echo "-gf8c04469e828-dirty" > "$OUT/src/.scmversion"		# the tree's git describe at that build
# the 11-11 build's time stamp: U-Boot writes it with date +FORMAT, in local time (CET)
cat > "$OUT/bin/date" <<'EOF'
#!/bin/sh
case "$1" in +*) exec /usr/bin/date -d @1794364427 "$@" ;; esac
exec /usr/bin/date "$@"
EOF
chmod 755 "$OUT/bin/date"
(cd "$OUT/src" && PATH="$OUT/bin:$PATH" TZ=Europe/Berlin make O=build CROSS_COMPILE=aarch64-linux-gnu- -j"${JOBS:-8}") \
	> "$OUT/build.log" 2>&1 || { tail -20 "$OUT/build.log" >&2; exit 1; }
cp "$OUT/src/build/u-boot-nodtb.bin" "$OUT/u-boot-nodtb.bin"
echo "$OUT/u-boot-nodtb.bin $(sha256sum < "$OUT/u-boot-nodtb.bin" | cut -c1-16), environment $(sha256sum < "$ENV" | cut -c1-16)"
