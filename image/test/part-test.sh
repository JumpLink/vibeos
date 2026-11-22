#!/bin/sh
# part-test.sh [S10E_PART]: s10e-part (the host build, src/s10e-part-host by default) on sparse disk images in a
# temporary directory: a GPT of boot + userdata made by sgdisk, plain and with two leftover entries that have a type
# GUID but no name and LBA 0-0 (an earlier multiboot layout left them on a OnePlus 6T; Linux ignores them). show,
# plan, plan-whole and whole must work on both, and whole must keep the leftovers as they are. Needs sgdisk and
# python3; no root, no devices.
set -u
T=$(cd "$(dirname "$0")/.." && pwd)
P=${1:-$T/src/s10e-part-host}
[ -x "$P" ] || { echo "no $P: make -C image/src s10e-part-host" >&2; exit 1; }
W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT
FAILS=0
ok() { echo "  ok: $*"; }
bad() { echo "  FAIL: $*"; FAILS=$((FAILS + 1)); }

# gpt IMG leftovers: entries 18 (ESP type) and 19 (Linux root, arm64) set to LBA 0-0 without a name, in both
# tables, their CRCs made right again. gpt IMG check: "ok" when both headers and entry arrays have the right CRCs and
# the arrays are the same, then the numbers of the entries with a type and LBA 0-0
cat > $W/gpt.py <<'EOF'
import struct, sys, uuid, zlib
ss = 512
f = open(sys.argv[1], 'r+b')
def load(lba):
    f.seek(lba * ss); h = bytearray(f.read(ss))
    hsz, = struct.unpack_from('<I', h, 12)
    ent, n, esz = struct.unpack_from('<QII', h, 72)
    f.seek(ent * ss); e = bytearray(f.read(n * esz))
    return h, hsz, ent, esz, e
def leftovers(lba):
    h, hsz, ent, esz, e = load(lba)
    for i, t in ((17, 'c12a7328-f81f-11d2-ba4b-00a0c93ec93b'), (18, 'b921b045-1df0-41c3-af44-4c6f280d3fae')):
        o = i * esz
        e[o:o + esz] = bytes(esz)
        e[o:o + 16] = uuid.UUID(t).bytes_le
        e[o + 16:o + 32] = uuid.uuid5(uuid.NAMESPACE_OID, str(i)).bytes_le
    f.seek(ent * ss); f.write(e)
    struct.pack_into('<I', h, 88, zlib.crc32(e))
    struct.pack_into('<I', h, 16, 0)
    struct.pack_into('<I', h, 16, zlib.crc32(h[:hsz]))
    f.seek(lba * ss); f.write(h)
    return struct.unpack_from('<Q', h, 32)[0]
def good(lba):
    h, hsz, ent, esz, e = load(lba)
    c = bytearray(h[:hsz]); struct.pack_into('<I', c, 16, 0)
    ok = h[:8] == b'EFI PART' and struct.unpack_from('<I', h, 16)[0] == zlib.crc32(c) and \
        struct.unpack_from('<I', h, 88)[0] == zlib.crc32(e)
    return ok, e, esz, struct.unpack_from('<Q', h, 32)[0]
if sys.argv[2] == 'leftovers':
    leftovers(leftovers(1))
else:
    g1, e1, esz, alt = good(1)
    g2, e2, _, _ = good(alt)
    print('ok' if g1 and g2 and e1 == e2 else 'bad', *[i // esz + 1 for i in range(0, len(e1), esz)
          if e1[i:i + 16] != bytes(16) and e1[i + 32:i + 48] == bytes(16)])
EOF
gpt() { python3 -I $W/gpt.py "$1" "$2"; }

for kind in plain leftovers; do
	echo "$kind:"
	D=$W/$kind.img
	truncate -s 64G $D
	sgdisk -o -n 1:0:+64M -c 1:boot -n 2:0:0 -c 2:userdata -t 2:8300 $D > /dev/null || { bad "sgdisk"; continue; }
	[ $kind = plain ] || gpt $D leftovers || { bad "cannot add the leftover entries"; continue; }
	[ "$(gpt $D check)" = "$( [ $kind = plain ] && echo ok || echo ok 18 19)" ] || bad "the test disk: $(gpt $D check)"
	$P --sector-size 512 show $D > $W/out 2>&1 && ok show || bad "show: $(cat $W/out)"
	$P --sector-size 512 plan $D 10 > $W/out 2>&1 && ok "plan 10" || bad "plan 10: $(cat $W/out)"
	$P --sector-size 512 plan-whole $D > $W/out 2>&1 && grep -q '^userdata (#2).*-> linux' $W/out && ok plan-whole ||
		bad "plan-whole: $(cat $W/out)"
	cp --sparse=always $D $W/w.img
	$P --sector-size 512 whole $W/w.img > $W/out 2>&1 && ok whole || bad "whole: $(cat $W/out)"
	X=$(gpt $W/w.img check)
	[ "${X%% *}" = ok ] && ok "both tables right after whole" || bad "after whole: $X"
	sgdisk -i 2 $W/w.img | grep -q "Partition name: 'linux'" && ok "#2 is linux" || bad "#2 is not linux after whole"
	if [ $kind = leftovers ]; then
		[ "$X" = "ok 18 19" ] && ok "the leftovers kept" || bad "the leftovers: $X"
	fi
	rm -f $D $W/w.img
done
[ $FAILS = 0 ] && echo "all ok" || { echo "$FAILS failed"; exit 1; }
