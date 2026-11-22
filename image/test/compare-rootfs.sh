#!/bin/sh
# compare-rootfs.sh NEW.img REF.img OUTDIR: what differs between two system images (a build and the released
# rootfs.img, say), mounted read-only side by side (root, sudo -n):
#   packages.txt   packages only in one of them, and those at another version or state
#   manual.txt     apt-mark showmanual, the difference
#   etc.txt        /etc: files only in one, and files whose content, mode, owner or link target differs
#   opt.txt        /opt/gnome-mobile: the same, by file list and sha256
#   unowned.txt    files no package owns, outside /etc, /proc, /sys, /dev, /run, /tmp and the dpkg/apt databases
# The summary goes to stdout; OUTDIR (new) keeps the lists.
set -eu
NEW=${1:?NEW.img} REF=${2:?REF.img} OUT=${3:?OUTDIR}
[ ! -e "$OUT" ] || { echo "$OUT exists" >&2; exit 1; }
# its mounts are its own (a private mount namespace, entered as root and left again for this user): a service that
# starts meanwhile copies the host's mounts into a namespace of its own, where an image mounted here would stay
# mounted, and its loop device busy, until that service exits
[ -n "${VIBEOS_TEST_NS:-}" ] || exec sudo -n env VIBEOS_TEST_NS=1 PATH="$PATH" HOME="$HOME" TMPDIR="${TMPDIR:-/tmp}" \
	unshare --mount --propagation private setpriv --reuid="$(id -u)" --regid="$(id -g)" --init-groups sh "$0" "$@"
mkdir -p "$OUT/mnt-new" "$OUT/mnt-ref"
cleanup() { for m in "$OUT/mnt-new" "$OUT/mnt-ref"; do sudo -n umount "$m" 2>/dev/null || true; done; rmdir "$OUT/mnt-new" "$OUT/mnt-ref" 2>/dev/null || true; }
trap cleanup EXIT
sudo -n mount -o loop,ro,noload "$NEW" "$OUT/mnt-new"
sudo -n mount -o loop,ro,noload "$REF" "$OUT/mnt-ref"
N=$OUT/mnt-new R=$OUT/mnt-ref
pk() { dpkg-query --admindir="$1/var/lib/dpkg" -W -f '${Package}\t${Version}\t${db:Status-Abbrev}\n' | LC_ALL=C sort; }
pk "$N" > "$OUT/new-packages.tsv"; pk "$R" > "$OUT/ref-packages.tsv"
{
	echo "# only in NEW"; LC_ALL=C join -t "$(printf '\t')" -v1 "$OUT/new-packages.tsv" "$OUT/ref-packages.tsv"
	echo "# only in REF"; LC_ALL=C join -t "$(printf '\t')" -v2 "$OUT/new-packages.tsv" "$OUT/ref-packages.tsv"
	echo "# version or state differs (package, NEW, REF)"
	LC_ALL=C join -t "$(printf '\t')" "$OUT/new-packages.tsv" "$OUT/ref-packages.tsv" | awk -F'\t' '$2 != $4 || $3 != $5 { print $1 "\t" $2 " " $3 "\t" $4 " " $5 }'
} > "$OUT/packages.txt"
for i in N R; do eval d=\$$i; sudo -n chroot "$d" apt-mark showmanual 2>/dev/null | LC_ALL=C sort > "$OUT/$i-manual.txt"; done
{ echo "# manual only in NEW"; LC_ALL=C comm -23 "$OUT/N-manual.txt" "$OUT/R-manual.txt"; echo "# manual only in REF"; LC_ALL=C comm -13 "$OUT/N-manual.txt" "$OUT/R-manual.txt"; } > "$OUT/manual.txt"
# a tree, one line per path: type, mode, owner, link target or sha256
tree() {	# ROOT DIR
	sudo -n python3 -I - "$1" "$2" <<'EOF'
import hashlib, os, stat, sys
root, top = sys.argv[1], sys.argv[2]
for d, dirs, files in os.walk(os.path.join(root, top)):
    dirs.sort()
    for n in sorted(files) + sorted(x for x in dirs if os.path.islink(os.path.join(d, x))):
        p = os.path.join(d, n); st = os.lstat(p); rel = os.path.relpath(p, root)
        if stat.S_ISLNK(st.st_mode): what = "-> " + os.readlink(p)
        elif stat.S_ISREG(st.st_mode):
            h = hashlib.sha256()
            with open(p, "rb") as f:
                for b in iter(lambda: f.read(1 << 20), b""): h.update(b)
            what = h.hexdigest()[:16]
        else: what = "special"
        print(f"/{rel}\t{stat.filemode(st.st_mode)} {st.st_uid}:{st.st_gid}\t{what}")
EOF
}
cmp_tree() {	# DIR OUT
	tree "$N" "$1" | LC_ALL=C sort > "$OUT/new-$2.tsv"; tree "$R" "$1" | LC_ALL=C sort > "$OUT/ref-$2.tsv"
	{
		echo "# only in NEW"; LC_ALL=C join -t "$(printf '\t')" -v1 "$OUT/new-$2.tsv" "$OUT/ref-$2.tsv" | cut -f1
		echo "# only in REF"; LC_ALL=C join -t "$(printf '\t')" -v2 "$OUT/new-$2.tsv" "$OUT/ref-$2.tsv" | cut -f1
		echo "# differs (path, NEW, REF)"
		LC_ALL=C join -t "$(printf '\t')" "$OUT/new-$2.tsv" "$OUT/ref-$2.tsv" | awk -F'\t' '$2 != $4 || $3 != $5 { print $1 "\t" $2 " " $3 "\t" $4 " " $5 }'
	} > "$OUT/$2.txt"
}
cmp_tree etc etc
cmp_tree opt/gnome-mobile opt
for i in N R; do
	eval d=\$$i
	cat "$d"/var/lib/dpkg/info/*.list | LC_ALL=C sort -u > "$OUT/$i-owned.txt"
	sudo -n find "$d" -xdev \( -path "$d/proc" -o -path "$d/sys" -o -path "$d/dev" -o -path "$d/run" -o -path "$d/tmp" \
		-o -path "$d/etc" -o -path "$d/var/lib/dpkg" -o -path "$d/var/lib/apt" -o -path "$d/lost+found" \) -prune -o \
		! -type d -print | sed "s|^$d||" | grep -v -E '__pycache__|\.pyc$|^/usr/share/mime/|^/var/cache/|^/var/log/' |
		LC_ALL=C sort | LC_ALL=C comm -23 - "$OUT/$i-owned.txt" > "$OUT/$i-unowned.txt"
done
{ echo "# unowned only in NEW"; LC_ALL=C comm -23 "$OUT/N-unowned.txt" "$OUT/R-unowned.txt"; echo "# unowned only in REF"; LC_ALL=C comm -13 "$OUT/N-unowned.txt" "$OUT/R-unowned.txt"; } > "$OUT/unowned.txt"
for f in packages manual etc opt unowned; do
	printf '%-9s' "$f:"; awk '/^# /{if (h) printf "%s %d; ", h, n; h=substr($0, 3); n=0; next} {n++} END {printf "%s %d\n", h, n}' "$OUT/$f.txt"
done
