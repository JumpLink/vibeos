#!/bin/sh
# build-mobile.sh OUT.tar: GNOME Shell mobile in /opt/gnome-mobile, the MOBILE of build-system.sh: the mobile shell
# project's mutter-mobile and gnome-shell-mobile at tag 51.1.mobile.22 (branch mobile-51), with this repository's
# patches/mutter (0001-0005) and patches/gnome-shell (0001-0005), built for arm64 in the build chroot
# (build-chroot.sh; qemu-user on a PC) and installed to /opt/gnome-mobile. OUT.tar holds opt/gnome-mobile.
#
#   sudo image/build-mobile.sh OUT.tar           # ~2 h under qemu-user on 8 cores; needs git and the network
#
# The VibeOS session's shell runs from there (kit/50-mobile.conf); GDM keeps Debian's gnome-shell. The binaries and
# libraries get a RUNPATH to /opt/gnome-mobile/lib*: the soname is Debian's mutter's, which is installed too.
# Settings: BUILD_CHROOT (default OUT/work/build-chroot, made by build-chroot.sh when missing), JOBS (8).
set -eu
OUT=${1:?OUT.tar}
[ "$(id -u)" = 0 ] || { echo "run as root (sudo)" >&2; exit 1; }
T=$(cd "$(dirname "$0")" && pwd)
[ ! -f "$T/local.conf" ] || . "$T/local.conf"
O=${VIBEOS_OUT:?set VIBEOS_OUT (image/local.conf): the directory builds, work files and logs go to}
CHROOT=${BUILD_CHROOT:-$O/work/build-chroot}
JOBS=${JOBS:-8}
TAG=51.1.mobile.22
MUTTER_URL=https://gitlab.gnome.org/World/MobileShell/mutter-mobile.git
MUTTER_COMMIT=7e7b25f68c7719ebf3b651808824e0c8bd33020d
SHELL_URL=https://gitlab.gnome.org/World/MobileShell/gnome-shell-mobile.git
SHELL_COMMIT=c602286053f9e06b3493decb4a9dabe8ba20ad9c
PREFIX=/opt/gnome-mobile
RP=$PREFIX/lib/mutter-51:$PREFIX/lib/gnome-shell:$PREFIX/lib
[ ! -e "$OUT" ] || { echo "$OUT exists" >&2; exit 1; }
command -v git > /dev/null || { echo "needs git" >&2; exit 1; }
for p in "$T"/../patches/mutter/0*.patch "$T"/../patches/gnome-shell/0*.patch; do [ -s "$p" ] || { echo "no patches in $T/../patches" >&2; exit 1; }; done
[ -d "$CHROOT" ] || "$T/build-chroot.sh" "$CHROOT"
[ -f "$CHROOT/build-chroot-stamp" ] || { echo "$CHROOT: not a build-chroot.sh chroot" >&2; exit 1; }
# every mount below is this build's own (a private mount namespace): nothing stays mounted when the build ends, and
# the chroot's own /opt/gnome-mobile is never written (this build's tree is mounted over it)
[ -n "${VIBEOS_BUILD_NS:-}" ] || exec env VIBEOS_BUILD_NS=1 unshare -m --propagation private sh "$0" "$@"

STAMP=$(date +%Y%m%d-%H%M%S)
W=$O/work/mobile-$STAMP; L=$O/logs/mobile-$STAMP
mkdir -p "$W/src" "$W/root$PREFIX" "$L"
log() { echo "build-mobile: $*" | tee -a "$L/build.txt"; }

# 1. the sources, each download in its own directory; the subprojects meson would fetch, at their wraps' revisions
clone() {	# URL COMMIT DIR [TAG]
	git init -q "$3"
	git -C "$3" fetch -q --depth 1 "$1" "${4:-$2}" 2>> "$L/git.txt"
	git -C "$3" -c advice.detachedHead=false checkout -q FETCH_HEAD
	[ "$(git -C "$3" rev-parse HEAD)" = "$2" ] || { echo "$1 ${4:-}: $(git -C "$3" rev-parse HEAD), not $2" >&2; exit 1; }
	echo "$1 $2${4:+ ($4)}" >> "$L/sources.txt"
}
wrap() {	# PROJECT NAME: the subproject NAME of PROJECT, at its wrap file's revision
	f=$W/src/$1/subprojects/$2.wrap
	u=$(sed -n 's/^url *= *//p' "$f"); r=$(sed -n 's/^revision *= *//p' "$f")
	case $r in [0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f]*) ;; *) echo "$f: revision $r is not a commit" >&2; exit 1 ;; esac
	clone "$u" "$r" "$W/src/$1/subprojects/$2"
}
clone "$MUTTER_URL" "$MUTTER_COMMIT" "$W/src/mutter-mobile" "refs/tags/$TAG"
clone "$SHELL_URL" "$SHELL_COMMIT" "$W/src/gnome-shell-mobile" "refs/tags/$TAG"
wrap mutter-mobile gvdb
wrap gnome-shell-mobile gvc
wrap gnome-shell-mobile libshew
log "sources: mutter-mobile and gnome-shell-mobile $TAG, subprojects gvdb gvc libshew ($L/sources.txt)"

# 2. VibeOS's patches
for p in mutter:mutter-mobile gnome-shell:gnome-shell-mobile; do
	for f in "$T/../patches/${p%%:*}"/0*.patch; do
		patch -d "$W/src/${p#*:}" -p1 -N --no-backup-if-mismatch < "$f" >> "$L/patches.txt" 2>&1 ||
			{ cat "$L/patches.txt" >&2; echo "${f##*/} does not apply" >&2; exit 1; }
		echo "${p#*:}: ${f##*/}" >> "$L/patches.txt"
	done
done
! grep -q -E 'fuzz|offset' "$L/patches.txt" || log "patches applied with offsets or fuzz: $L/patches.txt"
log "patches: mutter $(ls "$T/../patches/mutter" | grep -c '^0.*\.patch$'), gnome-shell $(ls "$T/../patches/gnome-shell" | grep -c '^0.*\.patch$')"

# 3. build and install, in the chroot
mkdir -p "$CHROOT/build" "$CHROOT$PREFIX"
mount --bind "$W/src" "$CHROOT/build"
mount --bind "$W/root$PREFIX" "$CHROOT$PREFIX"
mount -t proc proc "$CHROOT/proc"; mount -t sysfs sys "$CHROOT/sys"
mount --bind /dev "$CHROOT/dev"; mount --bind /dev/pts "$CHROOT/dev/pts"
CH="chroot $CHROOT /usr/bin/env -i PATH=/usr/sbin:/usr/bin:/sbin:/bin HOME=/root LC_ALL=C.UTF-8"
s=$(date +%s)
# the 0.1.3 tree's options: its libmutter has no libwacom (tablets and touchscreens are mapped to outputs as on our
# devices, without libwacom's database), no profiler, no tests
nice -n 19 $CH sh -c "cd /build/mutter-mobile && meson setup /build/b-mutter --prefix=$PREFIX --libdir=lib --buildtype=release \
	--wrap-mode=nodownload -Dtests=disabled -Dprofiler=false -Ddevkit=disabled -Ddocs=false -Dinstalled_tests=false -Dlibwacom=false \
	&& ninja -C /build/b-mutter -j$JOBS && meson install -C /build/b-mutter --no-rebuild" > "$L/mutter.txt" 2>&1 ||
	{ tail -40 "$L/mutter.txt" >&2; exit 1; }
log "mutter-mobile built and installed in $(( ($(date +%s) - s) / 60 )) min"
s=$(date +%s)
nice -n 19 $CH sh -c "cd /build/gnome-shell-mobile && PKG_CONFIG_PATH=$PREFIX/lib/pkgconfig meson setup /build/b-shell --prefix=$PREFIX \
	--libdir=lib --buildtype=release --wrap-mode=nodownload -Dtests=false -Dman=false -Dgtk_doc=false" > "$L/shell-setup.txt" 2>&1 ||
	{ tail -40 "$L/shell-setup.txt" >&2; exit 1; }
# against mutter-mobile in /opt, not Debian's libmutter-51-dev (gnome-shell's build dependencies put it in the chroot)
grep -q -E "dependency libmutter-51 found: YES $TAG" "$L/shell-setup.txt" || { grep -i libmutter "$L/shell-setup.txt" >&2; echo "the shell is not set up against mutter-mobile $TAG" >&2; exit 1; }
nice -n 19 $CH sh -c "ninja -C /build/b-shell -j$JOBS && meson install -C /build/b-shell --no-rebuild" > "$L/shell.txt" 2>&1 ||
	{ tail -40 "$L/shell.txt" >&2; exit 1; }
log "gnome-shell-mobile built and installed in $(( ($(date +%s) - s) / 60 )) min"

# 4. RUNPATHs (every ELF file that links a library of the tree finds it there, not Debian's of the same soname),
#    stripped; then each must resolve its tree libraries to /opt
$CH sh -c 'cd '"$PREFIX"' && find . -type f | while read -r f; do
	head -c 4 "$f" | grep -q ELF || continue
	if readelf -d "$f" | grep -q -E "NEEDED.*\[lib(mutter-(clutter-|cogl-|mtk-)?51|shell-51|st-51|gvc|shew-0)\.so"; then
		patchelf --set-rpath '"$RP"' "$f"; echo "runpath ${f#./}"
	fi
	strip --strip-unneeded "$f"
done' > "$L/runpath.txt" 2>&1 || { cat "$L/runpath.txt" >&2; exit 1; }
$CH sh -c 'cd '"$PREFIX"' && for f in bin/gnome-shell bin/mutter lib/gnome-shell/libshell-51.so lib/gnome-shell/libst-51.so lib/mutter-51/plugins/libdefault.so; do
	echo "== $f"; ldd "$f" | grep -E "lib(mutter-[a-z-]*51|shell-51|st-51|gvc|shew-0)\.so|not found"; done' > "$L/ldd.txt" 2>&1
! grep -E "=> /usr/|not found" "$L/ldd.txt" || { echo "a tree library resolves outside $PREFIX ($L/ldd.txt)" >&2; exit 1; }
for f in bin/gnome-shell lib/libmutter-51.so.0 lib/gnome-shell/libshell-51.so share/glib-2.0/schemas/gschemas.compiled \
	share/gnome-shell/gnome-shell-theme.gresource lib/mutter-51/Meta-51.typelib lib/gnome-shell/Shell-51.typelib; do
	[ -e "$W/root$PREFIX/$f" ] || { echo "missing $PREFIX/$f" >&2; exit 1; }
done
grep -q "^Version: $TAG$" "$W/root$PREFIX/lib/pkgconfig/libmutter-51.pc" || { echo "libmutter-51.pc is not $TAG" >&2; exit 1; }
log "$(grep -c '^runpath' "$L/runpath.txt") files with RUNPATH $RP; ldd resolves them to the tree"

# 5. the tar
(cd "$W/root" && find opt -type f | LC_ALL=C sort | xargs sha256sum) > "$L/files.sha256"
tar -C "$W/root" --owner=0 --group=0 --numeric-owner --sort=name -cf "$OUT" opt
[ -z "${SUDO_USER:-}" ] || chown "$SUDO_USER": "$OUT"
log "done: $OUT ($(du -h "$OUT" | cut -f1), libshell-51.so $(sha256sum "$W/root$PREFIX/lib/gnome-shell/libshell-51.so" | cut -c1-16)); logs in $L"
