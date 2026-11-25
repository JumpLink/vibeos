#!/bin/sh
# build-mutter.sh OUTDIR: mutter 51.0-3+gp1, the MUTTER of build-system.sh: Debian's mutter 51.0-3, unchanged but
# for this repository's patches/mutter (0001-0005, as debian/patches/vibeos/) and the two symbols patch 0004
# exports (in debian/libmutter-51-0.symbols), built for arm64 in the build chroot (build-chroot.sh; qemu-user on a
# PC). The .debs, .buildinfo and .changes go to OUTDIR (new).
#
#   sudo image/build-mutter.sh OUTDIR            # ~3 h under qemu-user on 8 cores (tests built, not run)
#
# Settings: BUILD_CHROOT (default OUT/work/build-chroot, made by build-chroot.sh when missing), JOBS (8),
# MUTTER_DEBIAN_VERSION (51.0-3: once Debian has moved on, give build-chroot.sh a SNAPSHOT of when it had it),
# MUTTER_CHANGELOG_DATE (the date of the +gp1 changelog entry: the 0.1.3 build's, so the packages come out the same).
set -eu
OUTD=${1:?OUTDIR (new)}
[ "$(id -u)" = 0 ] || { echo "run as root (sudo)" >&2; exit 1; }
T=$(cd "$(dirname "$0")" && pwd)
[ ! -f "$T/local.conf" ] || . "$T/local.conf"
O=${VIBEOS_OUT:?set VIBEOS_OUT (image/local.conf): the directory builds, work files and logs go to}
CHROOT=${BUILD_CHROOT:-$O/work/build-chroot}
JOBS=${JOBS:-8}
DV=${MUTTER_DEBIAN_VERSION:-51.0-3}
V=$DV+gp1
CDATE=${MUTTER_CHANGELOG_DATE:-Wed, 11 Nov 2026 06:56:22 +0100}
PD=$T/../patches/mutter
[ ! -e "$OUTD" ] || { echo "$OUTD exists" >&2; exit 1; }
ls "$PD"/0*.patch > /dev/null || { echo "no patches in $PD" >&2; exit 1; }
[ -d "$CHROOT" ] || "$T/build-chroot.sh" "$CHROOT"
[ -f "$CHROOT/build-chroot-stamp" ] || { echo "$CHROOT: not a build-chroot.sh chroot" >&2; exit 1; }
# every mount below is this build's own (a private mount namespace): two builds can share the chroot, and nothing
# stays mounted when the build ends, however it ends
[ -n "${VIBEOS_BUILD_NS:-}" ] || exec env VIBEOS_BUILD_NS=1 unshare -m --propagation private sh "$0" "$@"

STAMP=$(date +%Y%m%d-%H%M%S)
W=$O/work/mutter-$STAMP; L=$O/logs/mutter-$STAMP
mkdir -p "$W" "$L" "$CHROOT/build"
log() { echo "build-mutter: $*" | tee -a "$L/build.txt"; }
mount --bind "$W" "$CHROOT/build"
mount -t proc proc "$CHROOT/proc"; mount -t sysfs sys "$CHROOT/sys"
mount --bind /dev "$CHROOT/dev"; mount --bind /dev/pts "$CHROOT/dev/pts"
[ ! -L "$CHROOT/etc/resolv.conf" ] || { echo "$CHROOT/etc/resolv.conf is a symlink" >&2; exit 1; }
cp -L /etc/resolv.conf "$W/.resolv.conf"; touch "$CHROOT/etc/resolv.conf"; mount --bind "$W/.resolv.conf" "$CHROOT/etc/resolv.conf"
CH="chroot $CHROOT /usr/bin/env -i PATH=/usr/sbin:/usr/bin:/sbin:/bin HOME=/root LC_ALL=C.UTF-8 DEBIAN_FRONTEND=noninteractive"

# Debian's source, as its signed Sources index has it
$CH apt-get -q update > "$L/apt-update.txt" 2>&1 || { tail "$L/apt-update.txt" >&2; exit 1; }
$CH sh -c "cd /build && apt-get -q source --only-source mutter=$DV" > "$L/source.txt" 2>&1 ||
	{ tail "$L/source.txt" >&2; echo "no mutter $DV in the chroot's archive: build-chroot.sh with a SNAPSHOT of when Debian had it" >&2; exit 1; }
S=$(ls -d "$W"/mutter-*/ | head -1); S=${S%/}
[ "$(cd "$S" && dpkg-parsechangelog -S Version)" = "$DV" ] || { echo "$S is not mutter $DV" >&2; exit 1; }
(cd "$W" && sha256sum mutter_*.dsc mutter_*.orig.tar.* mutter_*.debian.tar.*) > "$L/source.sha256"
log "Debian's mutter $DV ($(head -c 16 "$L/source.sha256")... $(ls "$W"/mutter_*.dsc | xargs -n1 basename))"

# VibeOS's patches, after Debian's
mkdir -p "$S/debian/patches/vibeos"
for p in "$PD"/0*.patch; do
	cp "$p" "$S/debian/patches/vibeos/"
	echo "vibeos/${p##*/}" >> "$S/debian/patches/series"
done
MAINT=$(dpkg-parsechangelog -l "$T/../debian/changelog" -S Maintainer)
{ printf 'mutter (%s) unstable; urgency=medium\n\n' "$V"
  printf '  * VibeOS: patches 0001-0005 (keep outputs on, phone logical size,\n    orientation inhibit export, blur buffers, resize resync)\n\n'
  printf ' -- %s  %s\n\n' "$MAINT" "$CDATE"
  cat "$S/debian/changelog"; } > "$S/debian/changelog.new"
mv "$S/debian/changelog.new" "$S/debian/changelog"
# patch 0004 exports two functions (GNOME Shell mobile calls them): libmutter's symbols file lists them, as the 0.1.3
# build's does, or dpkg-gensymbols fails the build on the new symbols
SY=$S/debian/libmutter-51-0.symbols
grep -q '^ meta_orientation_manager_has_accelerometer@Base ' "$SY" || { echo "$SY: no meta_orientation_manager_has_accelerometer to add after" >&2; exit 1; }
sed -i "/^ meta_orientation_manager_has_accelerometer@Base /a\\
 meta_orientation_manager_inhibit_tracking@Base $V\\
 meta_orientation_manager_uninhibit_tracking@Base $V" "$SY"
[ "$(grep -c -E '^ meta_orientation_manager_(un)?inhibit_tracking@Base ' "$SY")" = 2 ] || { echo "$SY: the two symbols were not added" >&2; exit 1; }
$CH sh -c "cd /build/${S##*/} && dpkg-source --before-build ." > "$L/patches.txt" 2>&1 ||
	{ tail -20 "$L/patches.txt" >&2; exit 1; }
for p in "$PD"/0*.patch; do grep -q "vibeos/${p##*/}" "$L/patches.txt" || { echo "${p##*/} was not applied (see $L/patches.txt)" >&2; exit 1; }; done
log "patches: $(cd "$PD" && ls 0*.patch | tr '\n' ' ')"
$CH sh -c "cd /build/${S##*/} && dpkg-checkbuilddeps" > "$L/builddeps.txt" 2>&1 || { cat "$L/builddeps.txt" >&2; exit 1; }

# the build: Debian's rules, without the tests (they need a session; Debian runs them on its own builders)
s=$(date +%s)
nice -n 19 $CH sh -c "cd /build/${S##*/} && DEB_BUILD_OPTIONS='nocheck parallel=$JOBS' dpkg-buildpackage -b -uc -us" > "$L/dpkg-buildpackage.txt" 2>&1 ||
	{ tail -40 "$L/dpkg-buildpackage.txt" >&2; exit 1; }
log "built in $(( ($(date +%s) - s) / 60 )) min"
mkdir -p "$OUTD"
cp "$W"/*_"$V"_*.deb "$W"/mutter_"$V"_*.buildinfo "$W"/mutter_"$V"_*.changes "$OUTD/"
(cd "$OUTD" && sha256sum *.deb) > "$OUTD/SHA256SUMS"
for p in gir1.2-mutter-51 libmutter-51-0 mutter-common mutter-common-bin; do
	ls "$OUTD"/${p}_"$V"_*.deb > /dev/null || { echo "no $p $V" >&2; exit 1; }
done
[ -z "${SUDO_USER:-}" ] || chown -R "$SUDO_USER": "$OUTD"
log "done: $(ls "$OUTD"/*.deb | wc -l) packages in $OUTD; logs in $L"
