#!/bin/sh
# nm-own-bearer-test.sh: patches/network-manager/1.58.1-1+fajita1 applies to Debian's network-manager 1.58.1-1 after
# Debian's own series, and the patched disconnect() names the bearer NetworkManager connected instead of a NULL path
# (which makes ModemManager disconnect every bearer, the IMS bearer of a VoLTE daemon too). The same checks run on
# Debian's source first and must fail there, so they see the bug. The source comes from snapshot.debian.org,
# checked against the .dsc's sha256, into $NM_SRC_CACHE (default ~/.cache/vibeos-test/network-manager-1.58.1-1).
# Needs sh, curl, tar (bzip2, xz), patch, awk, grep and sha256sum; writes only the cache and a temporary directory.
set -u
T=$(cd "$(dirname "$0")/../.." && pwd)
P=$T/patches/network-manager/1.58.1-1+fajita1
C=${NM_SRC_CACHE:-${XDG_CACHE_HOME:-$HOME/.cache}/vibeos-test/network-manager-1.58.1-1}
DSC=network-manager_1.58.1-1.dsc DSC_SHA256=4019446cd3e731da0fe8e45aecde9da2113666072d4d6f80f080dd2cb5135ade
W=$(mktemp -d); trap 'rm -rf "$W"' EXIT
FAILS=0
mkdir -p "$C"
# get NAME SHA1: the file as snapshot.debian.org stores it, by its sha1
get() {
	[ -s "$C/$1" ] && return
	curl -fsSL -o "$C/$1.part" "https://snapshot.debian.org/file/$2" && mv "$C/$1.part" "$C/$1" ||
		{ echo "FAIL: cannot download $1"; exit 1; }
}
get $DSC 2cd168e87bf40cbc7ee2b630ddfeb735acd17606
get network-manager_1.58.1.orig.tar.bz2 53e8c870acd3c0ee88bd9af5cfab15ece9d1bd7f
get network-manager_1.58.1-1.debian.tar.xz 6847996f77bc7d7dd04f5b6e787c167d29e6f860
echo "$DSC_SHA256  $C/$DSC" | sha256sum -c --quiet - || { echo "FAIL: $DSC is not the pinned one"; exit 1; }
awk '/^Checksums-Sha256:/ { s = 1; next } /^[^ ]/ { s = 0 } s { print $1 "  " C "/" $3 }' C="$C" "$C/$DSC" |
	sha256sum -c --quiet - || { echo "FAIL: the source tarballs do not match $DSC"; exit 1; }

S=$W/network-manager-1.58.1
mkdir "$S" && tar -C "$S" --strip-components=1 -xjf "$C/network-manager_1.58.1.orig.tar.bz2" && tar -C "$S" -xJf "$C/network-manager_1.58.1-1.debian.tar.xz" ||
	{ echo "FAIL: cannot unpack the source"; exit 1; }
# series DIR LIST: apply the patches LIST names (quilt's format: one per line, # comments), from DIR, in order
series() {
	grep -v '^[[:space:]]*\(#\|$\)' "$2" | while read -r p opt; do
		(cd "$S" && patch -s -N -p1 ${opt:-} --no-backup-if-mismatch < "$1/$p") || { echo "FAIL: $p does not apply"; exit 1; }
	done
}
series "$S/debian/patches" "$S/debian/patches/series" || exit 1
F=$S/src/core/devices/wwan/nm-modem-broadband.c
[ -f "$F" ] || { echo "FAIL: no $F"; exit 1; }

# fn NAME: the body of the C function NAME in $F (from the line starting "NAME(" to the next "}" in column 0)
fn() { awk -v n="$1(" 'index($0, n) == 1 { s = 1 } s { print } s && /^}/ { exit }' "$F"; }
# check: the three properties of disconnecting only our own bearer, each line "yes NAME" or "no NAME"
check() {
	# the bearer path argument of mm_modem_simple_disconnect() is not a NULL constant
	fn disconnect | tr '\n' ' ' | grep -q 'mm_modem_simple_disconnect([^,]*, *NULL *[,/]' && echo "no names-a-bearer" || echo "yes names-a-bearer"
	# the path survives deactivate_cleanup(), which drops the bearer before disconnect() runs on sleep
	fn deactivate_cleanup | grep -q 'last_bearer_path' && echo "yes path-kept" || echo "no path-kept"
	# with no bearer of ours and no attempt, nothing is disconnected
	fn disconnect | grep -q '!connecting && !bearer_path' && echo "yes nothing-to-do" || echo "no nothing-to-do"
}
# expect STATE LABEL: every check gives "yes" (patched) or none does (Debian's)
expect() {
	r=$(check)
	if [ "$1" = patched ]; then bad=$(echo "$r" | grep -c '^no'); else bad=$(echo "$r" | grep -c '^yes'); fi
	if [ "$bad" = 0 ]; then echo "  ok: $2"
	else echo "  FAIL: $2:"; echo "$r" | sed 's/^/    /'; FAILS=$((FAILS + 1)); fi
}
expect debian "Debian's 1.58.1-1 disconnects all bearers (the checks see the bug)"
series "$P" "$P/series" || exit 1
echo "  ok: the set applies after Debian's series"
expect patched "1.58.1-1+fajita1 disconnects only its own bearer"
[ $FAILS = 0 ] && echo "all ok" || { echo "$FAILS failed"; exit 1; }
