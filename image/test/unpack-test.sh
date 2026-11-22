#!/bin/sh
# unpack-test.sh: unpack of zip/vibeos-lib.sh on the host (no root), as the device zips unpack payload.tar into the
# root. The root: usr/lib/firmware/regulatory.db{,.p7s} as absolute symlinks to /etc/alternatives/... (Debian's
# wireless-regdb), lib -> usr/lib, a firmware link the payload also ships as a link. The payload: python's tarfile,
# GNU format, as nura-to-vibeos writes it, with the two files as regular files. tar is a stand-in for recovery's
# toybox tar, which refuses to write a file through a symlink that leads out of -C (GNU tar would replace the link,
# and the test could not fail): unpack must remove those links first, keep lib a symlink, and unpack everything.
set -u
T=$(cd "$(dirname "$0")/.." && pwd)
W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT
FAILS=0
ok() { echo "  ok: $*"; }
bad() { echo "  FAIL: $*"; FAILS=$((FAILS + 1)); }

REAL=$(command -v tar)
mkdir -p $W/bin
cat > $W/bin/tar <<EOF
#!/bin/sh
# toybox's refusal: -x of a regular file whose place in -C is a symlink
case " \$* " in *" -xf "*)
	C=\$2 F=\$4
	$REAL -tvf "\$F" | sed -n 's/^-[^ ]*  *[^ ]*  *[^ ]*  *[^ ]*  *[^ ]*  *//p' | while IFS= read -r f; do
		[ ! -L "\$C/\$f" ] || { echo "tar: '\$f' \$(readlink "\$C/\$f") not under '\$C'" >&2; exit 1; }
	done || exit 1 ;;
esac
exec $REAL "\$@"
EOF
chmod +x $W/bin/tar

R=$W/root
mkdir -p $R/usr/lib/firmware/qcom $R/etc/alternatives
ln -s usr/lib $R/lib
for f in regulatory.db regulatory.db.p7s; do ln -s /etc/alternatives/$f $R/usr/lib/firmware/$f; done
ln -s qcom/old.mbn $R/usr/lib/firmware/a630.mbn
python3 -I - $W/payload.tar <<'EOF'
import io, sys, tarfile
with tarfile.open(sys.argv[1], 'w', format=tarfile.GNU_FORMAT) as t:
    def add(name, kind, data=b'', link=''):
        i = tarfile.TarInfo(name); i.type = kind; i.mode = 0o755 if kind == tarfile.DIRTYPE else 0o644
        i.size = len(data); i.linkname = link; i.mtime = 1763800000
        t.addfile(i, io.BytesIO(data) if data else None)
    for d in ('usr', 'usr/lib', 'usr/lib/firmware', 'usr/lib/firmware/qcom'):
        add(d, tarfile.DIRTYPE)
    add('usr/lib/firmware/regulatory.db', tarfile.REGTYPE, b'db\n')
    add('usr/lib/firmware/regulatory.db.p7s', tarfile.REGTYPE, b'sig\n')
    add('usr/lib/firmware/qcom/a630 gmu.bin', tarfile.REGTYPE, b'gmu\n')
    add('usr/lib/firmware/a630.mbn', tarfile.SYMTYPE, link='qcom/new.mbn')
EOF

# the library as recovery sources it (in a function: it sets the positional parameters), the stand-in tar first
lib() { mkdir -p $W/tmp; . $T/zip/vibeos-lib.sh; T=$W/tmp/vibeos P=$W/vibeos.prop; }
run() { ( PATH=$W/bin:$PATH; lib; "$@" ); }
echo "the stand-in tar:"
cp -a $R $W/plain
run sh -c "tar -C $W/plain -xf $W/payload.tar" > $W/out 2>&1 && bad "took the payload over the links" ||
	ok "refuses as toybox does: $(head -n1 $W/out)"
echo "unpack:"
run unpack $W/payload.tar $R > $W/out 2>&1 && ok unpack || bad "unpack: $(cat $W/out)"
for f in regulatory.db regulatory.db.p7s; do
	[ -f $R/usr/lib/firmware/$f ] && [ ! -L $R/usr/lib/firmware/$f ] && ok "$f is a file" || bad "$f: $(ls -l $R/usr/lib/firmware/$f)"
done
[ "$(cat $R/usr/lib/firmware/regulatory.db)" = db ] && ok "its content" || bad "regulatory.db's content"
[ -f "$R/usr/lib/firmware/qcom/a630 gmu.bin" ] && ok "a name with a space" || bad "no 'a630 gmu.bin'"
[ "$(readlink $R/usr/lib/firmware/a630.mbn)" = qcom/new.mbn ] && ok "a link replaced by the payload's link" ||
	bad "a630.mbn -> $(readlink $R/usr/lib/firmware/a630.mbn)"
[ -L $R/lib ] && ok "lib is still a symlink" || bad "lib was replaced"
[ -z "$(ls -A $R/etc/alternatives)" ] && ok "nothing written through a link" || bad "etc/alternatives: $(ls $R/etc/alternatives)"
[ $FAILS = 0 ] && echo "all ok" || { echo "$FAILS failed"; exit 1; }
