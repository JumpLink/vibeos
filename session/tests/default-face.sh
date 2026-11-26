#!/bin/sh
# vibeos-default-face on made-up homes: Debian's copy of the skeleton's picture, and no picture at all, become a link
# to the Trapu picture; the user's own picture, a link of the user's, a directory, and a home that has the link
# already are left as they are; with no Trapu picture nothing changes. $1: the session project's source directory
set -u
src=$1
t=$(mktemp -d)
trap 'rm -rf "$t"' EXIT
fail=0
printf 'trapu' > "$t/trapu.png"
printf 'a later skeleton picture' > "$t/skel"
# a file of desktop-base's sha (the Debian swirl) is not at hand here: the skeleton's own copy stands in for it
mkdir -p "$t/debian" "$t/none" "$t/own" "$t/link" "$t/dir/.face" "$t/again" "$t/nopic"
cp "$t/skel" "$t/debian/.face"
printf 'the user chose this' > "$t/own/.face"
printf 'x' > "$t/elsewhere"; ln -s "$t/elsewhere" "$t/link/.face"
ln -s "$t/trapu.png" "$t/again/.face"
cp "$t/skel" "$t/nopic/.face"
run() { env HOME="$t/$1" VIBEOS_FACE="${2:-$t/trapu.png}" VIBEOS_SKEL_FACE="$t/skel" sh "$src/vibeos-default-face"; }
check() {	# HOME WHAT EXPECTED
	if [ "$2" = "link" ]; then got=$(readlink "$t/$1/.face"); else got=$(cat "$t/$1/.face" 2>/dev/null || echo "(a directory)"); fi
	[ "$got" = "$3" ] && echo "ok: $1" || { echo "FAIL: $1: $got, not $3"; fail=1; }
}
for h in debian none own link dir again; do run $h; done
run nopic "$t/missing.png"
check debian link "$t/trapu.png"
check none link "$t/trapu.png"
check own file 'the user chose this'
check link link "$t/elsewhere"
check dir file '(a directory)'
check again link "$t/trapu.png"
check nopic file 'a later skeleton picture'
run debian; check debian link "$t/trapu.png"	# twice: the same
[ -z "$(find "$t" -name '.face.trapu-*')" ] || { echo "FAIL: a temporary link left behind"; fail=1; }
exit $fail
