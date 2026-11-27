#!/bin/sh
# dtbo-test.sh: dtbo_for and zero of zip/vibeos-lib.sh on the host (no root), as nura-install.sh runs them on a device
# whose table row says dtbo clear: a by-name directory of files stands in for /dev/block/by-name. dtbo_for must name
# the boot slot's dtbo (boot_a: dtbo_a) and nothing for recovery, none, or a device without dtbo; zero must leave the
# partition all zeros and its neighbour untouched. Also: nura-to-vibeos reads the column (fajita clear, others not),
# and the watchdog column (fajita only) with the two drop-ins it writes.
set -u
T=$(cd "$(dirname "$0")/.." && pwd)
W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT
FAILS=0
ok() { echo "  ok: $*"; }
bad() { echo "  FAIL: $*"; FAILS=$((FAILS + 1)); }

B=$W/by-name
mkdir -p $B $W/tmp
for n in boot_a boot_b dtbo_a dtbo_b; do head -c 1048576 /dev/urandom > $B/$n; done
SUM_B=$(sha256sum < $B/dtbo_b)
lib() {
	. $T/zip/vibeos-lib.sh
	BY=$B P=$W/vibeos.prop T=$W/tmp/vibeos
	sectors512() { echo $(($(stat -c %s "$1") / 512)); }
	blockdev() { :; }
}

echo "dtbo_for:"
for c in boot_a:dtbo_a boot_b:dtbo_b recovery: none: boot:; do
	X=$( lib; dtbo_for ${c%%:*} )
	[ "$X" = "${c#*:}" ] && ok "${c%%:*} -> '${X}'" || bad "${c%%:*} -> '$X', want '${c#*:}'"
done
X=$( lib; BY=$W/none; dtbo_for boot_a )
[ -z "$X" ] && ok "no dtbo partition -> ''" || bad "no dtbo partition -> '$X'"

echo "zero:"
( lib; zero $(part dtbo_a) ) > $W/out 2>&1 && ok "zero dtbo_a" || bad "zero: $(cat $W/out)"
[ "$(tr -d '\0' < $B/dtbo_a | wc -c)" = 0 ] && ok "dtbo_a all zeros" || bad "dtbo_a not zero"
[ "$(stat -c %s $B/dtbo_a)" = 1048576 ] && ok "its size kept" || bad "dtbo_a's size changed"
[ "$(sha256sum < $B/dtbo_b)" = "$SUM_B" ] && ok "dtbo_b untouched" || bad "dtbo_b changed"

echo "the device table:"
python3 -I - $T/nura/nura-to-vibeos > $W/out 2>&1 <<'EOF' && ok "$(cat $W/out)" || bad "$(cat $W/out)"
import importlib.machinery, importlib.util, sys
spec = importlib.util.spec_from_loader('n', importlib.machinery.SourceFileLoader('n', sys.argv[1]))
m = importlib.util.module_from_spec(spec); spec.loader.exec_module(m)
got = {d: m.device_row(d)['dtbo'] for d in ('oneplus-fajita', 'oneplus-enchilada', 'xiaomi-surya')}
assert got == {'oneplus-fajita': 'clear', 'oneplus-enchilada': None, 'xiaomi-surya': None}, got
assert m.device_row('xiaomi-surya')['panels'] == ['huaxing', 'tianma']
print('dtbo column: %s' % got)
EOF

echo "the watchdog drop-ins (fajita only):"
python3 -I - $T/nura/nura-to-vibeos $W > $W/out 2>&1 <<'EOF' && ok "$(cat $W/out)" || bad "$(cat $W/out)"
import importlib.machinery, importlib.util, os, sys
spec = importlib.util.spec_from_loader('n', importlib.machinery.SourceFileLoader('n', sys.argv[1]))
m = importlib.util.module_from_spec(spec); spec.loader.exec_module(m)
got = {d: m.device_row(d)['watchdog'] for d in ('oneplus-fajita', 'oneplus-enchilada', 'xiaomi-surya')}
assert got == {'oneplus-fajita': True, 'oneplus-enchilada': False, 'xiaomi-surya': False}, got
P = os.path.join(sys.argv[2], 'pkg')
m.watchdog_dropins(P)
a = open(os.path.join(P, 'etc/systemd/system.conf.d/90-fajita-watchdog.conf')).read().splitlines()
b = open(os.path.join(P, 'etc/systemd/system/rmtfs.service.d/90-fajita-stop.conf')).read().splitlines()
assert '[Manager]' in a and 'RuntimeWatchdogSec=30s' in a and 'RebootWatchdogSec=30s' in a, a
assert '[Service]' in b and 'TimeoutStopSec=15s' in b, b
print('watchdog column %s, both drop-ins written' % got)
EOF

echo "the modem wake hook (fajita only):"
python3 -I - $T/nura/nura-to-vibeos $W > $W/out 2>&1 <<'EOF' && ok "$(cat $W/out)" || bad "$(cat $W/out)"
import importlib.machinery, importlib.util, os, sys
spec = importlib.util.spec_from_loader('n', importlib.machinery.SourceFileLoader('n', sys.argv[1]))
m = importlib.util.module_from_spec(spec); spec.loader.exec_module(m)
got = {d: m.device_row(d)['modemwake'] for d in ('oneplus-fajita', 'oneplus-enchilada', 'xiaomi-surya')}
assert got == {'oneplus-fajita': True, 'oneplus-enchilada': False, 'xiaomi-surya': False}, got
P = os.path.join(sys.argv[2], 'pkg')
m.modem_wake_hook(P)
h = os.path.join(P, 'usr/lib/systemd/system-sleep/50-modem-wake-settle')
assert os.access(h, os.X_OK), h
b = open(h).read()
assert 'glink-edge.IPCRTR' in b and 'echo enabled' in b and 'phy80211' in b, 'hook body'
print('modemwake column %s, hook written and executable' % got)
EOF
[ $FAILS = 0 ] && echo "all ok" || { echo "$FAILS failed"; exit 1; }
