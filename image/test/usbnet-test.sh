#!/bin/sh
# usbnet-test.sh: rootfs/vibeos-usb-net on the host (no root, no hardware): a tempdir stands in for configfs,
# /sys/class/udc and /sys/class/net, and fake ip/dnsmasq/modprobe in PATH record what would run. Checks the gadget it
# builds (NCM+RNDIS, bound to the UDC), the address and DHCP range, that a second run changes nothing, and that it
# exits 0 and touches nothing with no UDC, with the opt-outs, with another gadget on the UDC, and in host mode.
set -u
T=$(cd "$(dirname "$0")/.." && pwd)
W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT
FAILS=0
ok() { echo "  ok: $*"; }
bad() { echo "  FAIL: $*"; FAILS=$((FAILS + 1)); }
mkdir -p $W/bin
for c in modprobe mount; do printf '#!/bin/sh\nexit 0\n' > $W/bin/$c; done
printf '#!/bin/sh\necho "ip $*" >> "$W/calls"\nexit 0\n' > $W/bin/ip
printf '#!/bin/sh\necho "dnsmasq $*" >> "$W/calls"\nexit 0\n' > $W/bin/dnsmasq
chmod +x $W/bin/*

fresh() {	# fresh [udc] [net]: a new fake tree
	rm -rf $W/t; mkdir -p $W/t/cfg/usb_gadget $W/t/udc $W/t/net $W/t/run
	[ -z "${1-}" ] || mkdir $W/t/udc/$1
	[ -z "${2-}" ] || mkdir $W/t/net/$2
	: > $W/calls; : > $W/t/cmdline
}
run() {
	W=$W PATH=$W/bin:$PATH VIBEOS_CONFIGFS=$W/t/cfg VIBEOS_UDC_DIR=$W/t/udc VIBEOS_NET_DIR=$W/t/net \
		VIBEOS_CMDLINE=$W/t/cmdline VIBEOS_NO_USBNET=$W/t/no-usbnet VIBEOS_RUN=$W/t/run VIBEOS_WAIT=0 \
		sh $T/rootfs/vibeos-usb-net "$@" > $W/out 2>&1
}
G=$W/t/cfg/usb_gadget/vibeos

echo "a phone with a controller:"
fresh a8f8800.usb usb0
run && ok "exit 0" || bad "exit $(cat $W/out)"
[ "$(cat $G/UDC 2>/dev/null)" = a8f8800.usb ] && ok "gadget bound to the UDC" || bad "UDC: $(cat $G/UDC 2>/dev/null)"
[ -d $G/functions/ncm.usb0 ] && [ -d $G/functions/rndis.usb0 ] && ok "NCM and RNDIS" || bad "functions"
[ -L $G/configs/c.1/ncm.usb0 ] && [ -L $G/configs/c.1/rndis.usb0 ] && ok "both in the configuration" || bad "config links"
grep -q 'ip addr replace 172.16.42.1/16 dev usb0' $W/calls && ok "172.16.42.1/16 on usb0" || bad "address: $(cat $W/calls)"
grep -q 'dhcp-range=172.16.42.2,172.16.42.2,255.255.0.0' $W/calls && ok "DHCP hands out 172.16.42.2" || bad "dhcp: $(cat $W/calls)"
grep -q -- '--port=0' $W/calls && ok "dnsmasq: DHCP only, no DNS" || bad "dnsmasq opens a DNS port"
grep -q 'up: usb0' $W/out && ok "says so" || bad "log: $(cat $W/out)"

echo "run twice:"
SUM=$(cd $W/t/cfg && find . | LC_ALL=C sort | xargs -r ls -ld 2>/dev/null | awk '{print $1,$NF}' | sha256sum)
run && ok "exit 0" || bad "second run: $(cat $W/out)"
SUM2=$(cd $W/t/cfg && find . | LC_ALL=C sort | xargs -r ls -ld 2>/dev/null | awk '{print $1,$NF}' | sha256sum)
[ "$SUM" = "$SUM2" ] && ok "the gadget is unchanged" || bad "the gadget changed"
grep -q 'already bound' $W/out && ok "reuses the bound gadget (the initramfs's)" || bad "log: $(cat $W/out)"

echo "usb1 instead of usb0:"
fresh a8f8800.usb usb1; run
grep -q 'dev usb1' $W/calls && ok "usb1 gets the address" || bad "$(cat $W/calls)"

echo "nothing to do (all exit 0, nothing written):"
check_idle() {	# check_idle name
	run && ok "$1: exit 0" || bad "$1: exit non-zero: $(cat $W/out)"
	[ ! -e $G ] && ok "$1: no gadget made" || bad "$1: gadget made"
	[ ! -s $W/calls ] && ok "$1: no ip, no dnsmasq" || bad "$1: ran $(cat $W/calls)"
}
fresh "" ""; check_idle "no UDC"
grep -q 'no USB device controller' $W/out && ok "no UDC: says so" || bad "log: $(cat $W/out)"
fresh a8f8800.usb usb0; echo "quiet vibeos.usbnet=0 splash" > $W/t/cmdline; check_idle "vibeos.usbnet=0"
fresh a8f8800.usb usb0; : > $W/t/no-usbnet; check_idle "no-usbnet file"
fresh a8f8800.usb usb0; mkdir -p $W/t/cfg/usb_gadget/g1; echo a8f8800.usb > $W/t/cfg/usb_gadget/g1/UDC
check_idle "another gadget on the UDC"
[ "$(cat $W/t/cfg/usb_gadget/g1/UDC)" = a8f8800.usb ] && ok "the other gadget is still bound" || bad "the other gadget was touched"
grep -q 'belongs to the gadget g1' $W/out && ok "says whose it is" || bad "log: $(cat $W/out)"

echo "no network device after binding:"
fresh a8f8800.usb ""; run && ok "exit 0" || bad "exit non-zero"
[ ! -s $W/calls ] && ok "no address, no DHCP" || bad "ran $(cat $W/calls)"

echo "stop:"
fresh a8f8800.usb usb0; echo 99999999 > $W/t/run/usb-net-dnsmasq.pid
run stop && ok "exit 0" || bad "stop failed"
[ ! -e $W/t/run/usb-net-dnsmasq.pid ] && ok "pid file gone" || bad "pid file left"

[ $FAILS = 0 ] && echo "all ok" || { echo "$FAILS failed"; exit 1; }
