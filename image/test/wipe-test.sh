#!/bin/sh
# wipe-test.sh: wipe_ends of zip/vibeos-lib.sh on 64 MiB files in a temporary directory, as the system zip runs it on
# the root's partition before writing (new, whole, existing). Each file is random data carrying an old GPT the way a
# postmarketOS userdata does (boot + Linux root, primary at block 1, backup in the last block), once with 4096-byte
# and once with 512-byte sectors: nested() must find the root before, nothing after; the first and last MiB must be
# zero and everything between unchanged. Needs python3; no root, no devices.
set -u
T=$(cd "$(dirname "$0")/.." && pwd)
W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT
FAILS=0
ok() { echo "  ok: $*"; }
bad() { echo "  FAIL: $*"; FAILS=$((FAILS + 1)); }

# gpt.py IMG SS: an old GPT at sector size SS, primary at LBA 1 and backup in the last LBA (only what nested() reads)
cat > $W/gpt.py <<'EOF'
import struct, sys, uuid
img, ss = sys.argv[1], int(sys.argv[2])
f = open(img, 'r+b'); f.seek(0, 2); last = f.tell() // ss - 1
ent = bytearray(128 * 128)
for i, (t, a, b) in enumerate((('c12a7328-f81f-11d2-ba4b-00a0c93ec93b', 2048 // (ss // 512), 4095),
                               ('b921b045-1df0-41c3-af44-4c6f280d3fae', 4096, last - 40))):
    o = i * 128
    ent[o:o + 16] = uuid.UUID(t).bytes_le
    struct.pack_into('<QQ', ent, o + 32, a, b)
def hdr(my, alt, elba):
    h = bytearray(ss)
    h[:8] = b'EFI PART'
    struct.pack_into('<IIIIQQ', h, 8, 0x10000, 92, 0, 0, my, alt)
    struct.pack_into('<QII', h, 72, elba, 128, 128)
    return h
f.seek(510); f.write(b'\x55\xaa')
f.seek(ss); f.write(hdr(1, last, 2))
f.seek(2 * ss); f.write(ent)
f.seek((last - 32) * ss); f.write(ent)
f.seek(last * ss); f.write(hdr(last, 1, last - 32))
EOF

# the library as recovery sources it; sectors512 reads sysfs there, here the file's size
lib() {
	mkdir -p $W/tmp
	. $T/zip/vibeos-lib.sh
	P=$W/vibeos.prop T=$W/tmp/vibeos
	sectors512() { echo $(($(stat -c %s "$1") / 512)); }
}
mid() { dd if="$1" bs=1M skip=1 count=62 2>/dev/null | sha256sum | cut -d' ' -f1; }

for ss in 4096 512; do
	echo "sector size $ss:"
	D=$W/d$ss.img
	head -c 67108864 /dev/urandom > $D
	python3 -I $W/gpt.py $D $ss || { bad "cannot make the old GPT"; continue; }
	M=$(mid $D)
	X=$( lib; nested $D 2>/dev/null ) && [ "${X%% *}" = $ss ] && ok "nested finds the old root: $X" || bad "nested before: '$X'"
	( lib; wipe_ends $D ) > $W/out 2>&1 && ok wipe_ends || bad "wipe_ends: $(cat $W/out)"
	X=$( lib; nested $D 2>/dev/null ); [ -z "$X" ] && ok "nested finds nothing after" || bad "nested after: '$X'"
	[ "$(head -c 1048576 $D | tr -d '\0' | wc -c)" = 0 ] && ok "first MiB zero" || bad "first MiB not zero"
	[ "$(tail -c 1048576 $D | tr -d '\0' | wc -c)" = 0 ] && ok "last MiB zero" || bad "last MiB not zero"
	[ "$(mid $D)" = "$M" ] && ok "the rest unchanged" || bad "the rest changed"
done
echo "too small:"
head -c 1048576 /dev/zero > $W/small.img
if ( lib; wipe_ends $W/small.img ) > $W/out 2>&1; then bad "wipe_ends took a 1 MiB file"
elif grep -q 'too small' $W/out; then ok "a 1 MiB file refused"
else bad "small: $(cat $W/out)"; fi
[ $FAILS = 0 ] && echo "all ok" || { echo "$FAILS failed"; exit 1; }
