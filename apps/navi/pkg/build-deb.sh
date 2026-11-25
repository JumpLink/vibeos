#!/bin/sh
# vibeos-navi's Debian package. Navi is cross-built (MapLibre's stage is: maplibre/build-maplibre.sh), so this takes
# the built trees, puts the package together, and works out its dependencies inside the arm64 root it was built
# against, with that root's dpkg-shlibdeps, dpkg-gencontrol and dpkg-deb.
#
#   pkg/build-deb.sh NAVI_BUILD LIBVIBEOS_BUILD MAPLIBRE_SRC ROOT OUTDIR
#
#   NAVI_BUILD       apps/navi configured for the package:
#                      meson setup --cross-file ROOT.ini --cross-file cross/aarch64-linux-gnu-clang.ini \
#                        -Dprefix=/usr -Dmaplibre_stage=... -Dvoice_src=.../apps/dictate/src NAVI_BUILD apps/navi
#   LIBVIBEOS_BUILD  the same tree's libvibeos, built (Navi carries it privately: it may be newer than the system's)
#   MAPLIBRE_SRC     the maplibre-native checkout the stage was built from (its licence notices go into the package)
#   ROOT             the arm64 root filesystem (the sysroot of both builds), entered with sudo chroot
#   OUTDIR           where the .deb goes; the package is put together in OUTDIR/vibeos-navi.work
#
# NAVI_VERSION=VERSION: the package's version, for a release (1:0.1.4) instead of the project's and the commit's; the
# release's VibeOS libraries are then required at exactly that version.
#
# LIBDIRS=DIR[:DIR…]: libraries ROOT does not have (libtonal, from the same tree's top-level build), for
# dpkg-shlibdeps to find; their packages' versions are in pkg/shlibs.local.
#
# Everything private goes to /usr/lib/vibeos-navi, found through the RUNPATH: no system library is replaced.
set -eu
[ $# -eq 5 ] || { sed -n "2,20p" "$0"; exit 2; }
navi_build=$(realpath "$1"); vibeos_build=$(realpath "$2"); maplibre_src=$(realpath "$3"); root=$(realpath "$4")
outdir=$(realpath "$5")
pkgdir=$(cd "$(dirname "$0")" && pwd)
srcdir=$(cd "$pkgdir/.." && pwd)
umask 022

# the version: the project's, then the commit it was built from
git -C "$srcdir" diff --quiet HEAD -- . ../../libvibeos || [ -n "${ALLOW_DIRTY:-}" ] ||
    { echo "apps/navi or libvibeos has changes that are not committed (ALLOW_DIRTY=1 to build anyway)"; exit 1; }
project=$(meson introspect --projectinfo "$navi_build" | python3 -c 'import json,sys; print(json.load(sys.stdin)["version"])')
stamp=$(git -C "$srcdir" log -1 --format=%cd --date=format:%Y%m%d%H%M)
commit=$(git -C "$srcdir" rev-parse --short=7 HEAD)
version=${NAVI_VERSION:-$project~git$stamp.$commit}
prefix=$(meson introspect --buildoptions "$navi_build" |
         python3 -c 'import json,sys; print([o["value"] for o in json.load(sys.stdin) if o["name"] == "prefix"][0])')
[ "$prefix" = /usr ] || { echo "$navi_build is configured for $prefix, not /usr"; exit 1; }

nice -n 19 ninja -C "$vibeos_build" >/dev/null
nice -n 19 ninja -C "$navi_build" >/dev/null

work=$outdir/vibeos-navi.work
[ ! -e "$work" ] || { echo "$work is left from an earlier build: move it away first"; exit 1; }
mkdir -p "$work/navi" "$work/libvibeos"
DESTDIR=$work/navi meson install -C "$navi_build" --no-rebuild --strip >/dev/null
DESTDIR=$work/libvibeos meson install -C "$vibeos_build" --no-rebuild --strip >/dev/null

# the package's tree: the app and what it reads, without the demo, its data and the map widget's header
p=$work/debian/vibeos-navi
i=$work/navi/usr
lib=$(find "$work/libvibeos" -name "libvibeos-1.so.0.*" -type f | head -1)
[ -n "$lib" ] || { echo "no libvibeos-1.so.0.* in $vibeos_build's install"; exit 1; }
install -d "$p/usr/bin" "$p/usr/lib/vibeos-navi" "$p/usr/share/vibeos-navi" "$p/usr/share/applications" \
           "$p/usr/share/dbus-1/services" "$p/usr/share/icons/hicolor/scalable/apps" "$p/usr/share/doc/vibeos-navi"
install -m 755 "$i/bin/vibeos-navi" "$p/usr/bin/"
install -m 644 "$i/lib/vibeos-navi/libvibeos-navi-map.so.0" "$p/usr/lib/vibeos-navi/"
install -m 644 "$lib" "$p/usr/lib/vibeos-navi/"
ln -s "$(basename "$lib")" "$p/usr/lib/vibeos-navi/libvibeos-1.so.0"
install -m 644 "$i"/share/vibeos-navi/style-*.json "$p/usr/share/vibeos-navi/"
cp -R "$i/share/vibeos-navi/poi" "$p/usr/share/vibeos-navi/"
install -m 644 "$i/share/applications/vibeos.Navi.desktop" "$p/usr/share/applications/"
install -m 644 "$i/share/dbus-1/services/vibeos.Navi.service" "$p/usr/share/dbus-1/services/"
install -m 644 "$i/share/icons/hicolor/scalable/apps/vibeos.Navi.svg" "$p/usr/share/icons/hicolor/scalable/apps/"
for mo in "$i"/share/locale/*/LC_MESSAGES/vibeos-navi.mo; do
    d=$p/usr/share/locale/$(basename "$(dirname "$(dirname "$mo")")")/LC_MESSAGES
    install -d "$d" && install -m 644 "$mo" "$d/"
done
grep -q '^Exec=/usr/bin/vibeos-navi --gapplication-service$' "$p/usr/share/dbus-1/services/vibeos.Navi.service"

# the documents: the licences, MapLibre's notices, and a changelog naming the commit
install -m 644 "$pkgdir/copyright" "$p/usr/share/doc/vibeos-navi/"
gzip -9n < "$maplibre_src/LICENSES.core.md" > "$p/usr/share/doc/vibeos-navi/LICENSES.maplibre.md.gz"
cp "$pkgdir/control" "$work/debian/control"
cp "$pkgdir/shlibs.local" "$work/debian/shlibs.local"
# a release's package needs that release's libraries, exactly, as debian/rules has every other package of it pin
# them (dh_makeshlibs -V): pkg/'s (>= 1:0.1.3) is for a build between releases. An image refuses a relaxed pin.
if [ -n "${NAVI_VERSION:-}" ]; then
    sed -i -E "/lib(vibeos|spongy|spongy-gtk|tonal)-1-0/ s/\(>= [^)]*\)/(= $version)/" \
        "$work/debian/control" "$work/debian/shlibs.local"
    ! grep -E 'lib(vibeos|spongy|spongy-gtk|tonal)-1-0 \(>=' "$work/debian/control" "$work/debian/shlibs.local" ||
        { echo "a VibeOS library is still pinned with >="; exit 1; }
fi
subject=$(git -C "$srcdir" log -1 --format=%s)
# the entry wrapped as Debian's changelog lines are (at most 80 columns), the subject being as long as it is
entry=$(printf 'Built from VibeOS commit %s ("%s").\n' "$commit" "$subject" | fold -s -w 74 | sed -e 's/ *$//' -e '1s/^/  * /' -e '2,$s/^/    /')
cat > "$work/debian/changelog" <<EOF
vibeos-navi ($version) unstable; urgency=medium

$entry

 -- pulpoff <pashkovsky@gmail.com>  $(date -R)
EOF
gzip -9n < "$work/debian/changelog" > "$p/usr/share/doc/vibeos-navi/changelog.gz"
chmod -R u+rwX,go+rX,go-w "$p"
mkdir -p "$work/libs"
for d in $(echo "${LIBDIRS:-}" | tr ':' ' '); do
    for f in "$d"/*.so.*; do [ -d "$f" ] || cp -P "$f" "$work/libs/"; done
done

# the dependencies and the package, inside the arm64 root (where its libraries and their shlibs are)
inside=/build/vibeos-navi-deb
mounted=
cleanup () { [ -z "$mounted" ] || sudo -n umount "$root$inside"; }
trap cleanup EXIT
case "$work" in
"$root"/*) inside=${work#"$root"} ;;
*) mkdir -p "$root$inside" && sudo -n mount --bind "$work" "$root$inside" && mounted=1 ;;
esac
sudo -n chroot --userspec="$(id -u):$(id -g)" "$root" env -i PATH=/usr/sbin:/usr/bin:/sbin:/bin LC_ALL=C.UTF-8 \
    HOME=/tmp sh -euc '
cd "$1"
mkdir -p debian/vibeos-navi/DEBIAN
dpkg-shlibdeps -Tdebian/vibeos-navi.substvars -ldebian/vibeos-navi/usr/lib/vibeos-navi -llibs debian/vibeos-navi/usr/bin/vibeos-navi \
    debian/vibeos-navi/usr/lib/vibeos-navi/libvibeos-navi-map.so.0 debian/vibeos-navi/usr/lib/vibeos-navi/libvibeos-1.so.0.*
dpkg-gencontrol -pvibeos-navi -Pdebian/vibeos-navi -Tdebian/vibeos-navi.substvars
(cd debian/vibeos-navi && find usr -type f -print0 | sort -z | xargs -0 md5sum > DEBIAN/md5sums)
chmod 644 debian/vibeos-navi/DEBIAN/md5sums
dpkg-deb --root-owner-group -Zxz --build debian/vibeos-navi .
' sh "$inside"
mv "$work"/vibeos-navi_*.deb "$outdir/"
echo "$outdir/vibeos-navi_${version#*:}_arm64.deb"
