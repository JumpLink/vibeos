#!/bin/sh
# build-base.sh BASE.img: the base image build-system.sh makes the system image from, built from the Debian and
# Mobian archives alone (mmdebstrap; arm64 through qemu-user binfmt on a PC). It is what the 0.1.3 system image's
# base had without that base's S10e device layer: Debian sid + Mobian staging, Mobian's phone base, GNOME as Debian
# ships it (gnome-core, GDM, GNOME Web), the packages of base/packages with their Recommends, and
#   - the apt sources of the 0.1.3 image (Debian sid, Mobian staging, deb822) and its Phosh pin (base/overlay)
#   - the user "user" (uid 1000, password 147147, the groups a phone session needs); GDM logs it in
#   - host name vibeos, root by the label vibeos, time zone BASE_TZ, language BASE_LANG
#   - the graphical target; ssh installed but off; no SSH host keys, no machine-id
# It carries nothing of any device: no kernel, no modules, no firmware but Debian's, no device units, and Debian's
# own ModemManager, libcamera and WebKitGTK (HOLD_DEBS installs other builds of such packages over them and holds
# them). /etc/vibeos/base says what it is; build-system.sh reads it and skips the S10e layer removal.
#
#   sudo image/build-base.sh BASE.img                # ~1 h under qemu-user, a 4.2 GB image, needs the network
#
# Settings (image/local.conf or the environment): VIBEOS_OUT (logs: OUT/logs/base-TIME, work: OUT/work), the
# archives of base/archive.sh (DEBIAN_MIRROR, SNAPSHOT, MOBIAN_MIRROR), BASE_TZ (Europe/Berlin), BASE_LANG
# (en_GB.UTF-8), HOLD_DEBS (a directory of .debs, optional), KEEP_TAR=1 (keep the root's tar in OUT/work).
set -eu
IMG=${1:?BASE.img}
[ "$(id -u)" = 0 ] || { echo "run as root (sudo)" >&2; exit 1; }
# its mounts are its own (a private mount namespace): a service that starts during the build copies the host's mounts
# into a namespace of its own, where the image would stay mounted, and its loop device busy, until that service exits
[ -n "${VIBEOS_BUILD_NS:-}" ] || exec env VIBEOS_BUILD_NS=1 unshare --mount --propagation private sh "$0" "$@"
T=$(cd "$(dirname "$0")" && pwd)
[ ! -f "$T/local.conf" ] || . "$T/local.conf"
. "$T/base/archive.sh"
O=${VIBEOS_OUT:?set VIBEOS_OUT (image/local.conf): the directory builds, work files and logs go to}
BASE_TZ=${BASE_TZ:-Europe/Berlin}
BASE_LANG=${BASE_LANG:-en_GB.UTF-8}
HOLD_DEBS=${HOLD_DEBS:-}
[ ! -e "$IMG" ] || { echo "$IMG exists" >&2; exit 1; }
[ -e /proc/sys/fs/binfmt_misc/qemu-aarch64 ] || { echo "no qemu-aarch64 binfmt (apt install qemu-user-static)" >&2; exit 1; }
[ -e "/usr/share/zoneinfo/$BASE_TZ" ] || { echo "BASE_TZ=$BASE_TZ: no such time zone" >&2; exit 1; }
[ -z "$HOLD_DEBS" ] || ls "$HOLD_DEBS"/*.deb > /dev/null || { echo "HOLD_DEBS=$HOLD_DEBS: no .debs" >&2; exit 1; }
for c in mmdebstrap unshare mke2fs resize2fs e2fsck gpg python3; do command -v $c > /dev/null || { echo "needs $c" >&2; exit 1; }; done
STAMP=$(date +%Y%m%d-%H%M%S)
L=$O/logs/base-$STAMP; W=$O/work/base-$STAMP; M=$W/mnt
mkdir -p "$L" "$W" "$M"
log() { echo "build-base: $*" | tee -a "$L/build.txt"; }

PKGS=$(sed -e 's/#.*//' "$T/base/packages" | tr -s ' \t\n' '\n' | sed '/^$/d' | tr '\n' ',' | sed 's/,$//')
log "$(echo "$PKGS" | tr ',' '\n' | wc -l) packages asked for; Debian $DEB_URI $SUITE, Mobian $MOBIAN_MIRROR $MOBIAN_SUITE"

# 1. the root, as a tar. The build runs in its own UTS namespace named vibeos: maintainer scripts write the host
#    name into the image (exim4's mailname and configuration, ssl-cert's certificate), and this machine's name has
#    no place there. systemd-resolved's postinst keeps a copy of the resolv.conf it found, which is this machine's
#    (its search domain and name servers): it goes.
KT=$(mktemp -d /tmp/vibeos-keys.XXXXXX)
cleanup() {
	for d in dev/pts dev proc sys run; do umount "$M/$d" 2>/dev/null || true; done
	umount "$M" 2>/dev/null || true
	rm -rf "${KT:?}"
}
trap cleanup EXIT
make_keyrings "$KT"
DS='Types: deb\nURIs: '"$DEBIAN_MIRROR"'\nSuites: '"$SUITE"'\nComponents: '"$COMPONENTS"'\nSigned-By: /usr/share/keyrings/debian-archive-keyring.gpg\n'
MS='Types: deb\nURIs: '"$MOBIAN_MIRROR"'\nSuites: '"$MOBIAN_SUITE"'\nComponents: '"$MOBIAN_COMPONENTS"'\nSigned-By: /usr/share/keyrings/mobian-archive-keyring.gpg\n'
TMPDIR=$W nice -n 19 unshare --uts sh -c 'hostname vibeos && exec "$@"' sh \
	mmdebstrap --arch=arm64 --variant=important --mode=root --format=tar \
	--components="$COMPONENTS" --keyring="$KT" \
	--aptopt='APT::Install-Recommends "true"' \
	--setup-hook='mkdir -p "$1/etc/apt/preferences.d" "$1/usr/share/keyrings"' \
	--setup-hook='cp "'"$T"'/base/overlay/etc/apt/preferences.d/no-phosh" "$1/etc/apt/preferences.d/no-phosh"' \
	--setup-hook='cp "'"$KT"'/mobian-archive-keyring.gpg" "$1/usr/share/keyrings/"' \
	--include="$PKGS" \
	--customize-hook='rm -f "$1/etc/apt/sources.list" "$1/etc/.resolv.conf.systemd-resolved.bak"' \
	--customize-hook='printf "'"$DS"'" > "$1/etc/apt/sources.list.d/debian.sources"' \
	--customize-hook='printf "'"$MS"'" > "$1/etc/apt/sources.list.d/mobian.sources"' \
	$SUITE "$W/root.tar" "$(deb_line)" "$(mobian_line)" > "$L/mmdebstrap.txt" 2>&1 ||
	{ tail -30 "$L/mmdebstrap.txt" >&2; exit 1; }
log "root: $(du -h "$W/root.tar" | cut -f1) ($(grep -o 'success in [0-9.]* seconds' "$L/mmdebstrap.txt"))"

# 2. the ext4 image: the features LineageOS recovery's e2fsck knows (it checks the image after writing it)
truncate -s 16G "$IMG"
mke2fs -q -t ext4 -L vibeos -b 4096 -I 256 -O ^orphan_file,^metadata_csum_seed "$IMG"
mount -o loop,noatime "$IMG" "$M"
tar -C "$M" --numeric-owner --xattrs --xattrs-include='*' -xpf "$W/root.tar"
for d in proc sys dev dev/pts run; do mkdir -p "$M/$d"; done
mount -t proc proc "$M/proc"; mount -t sysfs sys "$M/sys"; mount --bind /dev "$M/dev"; mount --bind /dev/pts "$M/dev/pts"
mount -t tmpfs tmpfs "$M/run"
printf '#!/bin/sh\n# build-base.sh: no service starts in the build chroot\nexit 101\n' > "$M/usr/sbin/policy-rc.d"; chmod 755 "$M/usr/sbin/policy-rc.d"
CH="chroot $M /usr/bin/env -i PATH=/usr/sbin:/usr/bin:/sbin:/bin HOME=/root LC_ALL=C.UTF-8 DEBIAN_FRONTEND=noninteractive"

# 3. other builds of archive packages, held (HOLD_DEBS only)
if [ -n "$HOLD_DEBS" ]; then
	mkdir -p "$M/tmp/hold-debs"; cp "$HOLD_DEBS"/*.deb "$M/tmp/hold-debs/"
	$CH sh -c 'dpkg -i /tmp/hold-debs/*.deb' > "$L/hold-debs.txt" 2>&1 || { tail -20 "$L/hold-debs.txt" >&2; exit 1; }
	H=$(for f in "$HOLD_DEBS"/*.deb; do dpkg-deb -f "$f" Package; done)
	$CH apt-mark hold $H > /dev/null
	$CH apt-get -q check > /dev/null
	rm -rf "$M/tmp/hold-debs"
	log "held, from HOLD_DEBS: $(echo $H)"
fi

# 4. the user: "user", password 147147 (the lock screen asks for it; GDM logs the user in)
G=$(for g in sudo audio video render input plugdev netdev bluetooth dialout feedbackd; do
	$CH getent group $g > /dev/null && printf '%s,' $g; done | sed 's/,$//')
$CH useradd -m -u 1000 -s /bin/bash -c User -G "$G" user
echo 'user:147147' | $CH chpasswd
python3 -I - "$M/etc/gdm3/daemon.conf" <<'EOF'
import re, sys
p = sys.argv[1]
s = open(p).read()
s = re.sub(r'(?m)^#?\s*AutomaticLoginEnable\s*=.*$', 'AutomaticLoginEnable = true', s, count=1)
s = re.sub(r'(?m)^#?\s*AutomaticLogin\s*=.*$', 'AutomaticLogin = user', s, count=1)
open(p, 'w').write(s)
EOF
grep -q '^AutomaticLogin = user$' "$M/etc/gdm3/daemon.conf" || { echo "GDM's automatic login not set" >&2; exit 1; }
log "user: uid 1000, groups $G; GDM logs it in"

# 5. identity, place, language, services
echo vibeos > "$M/etc/hostname"
printf '127.0.0.1\tlocalhost\n127.0.1.1\tvibeos\n::1\tlocalhost ip6-localhost ip6-loopback\n' > "$M/etc/hosts"
printf '# the root file system, by its ext4 label (build-system.sh writes the final one)\nLABEL=vibeos  /  ext4  defaults,noatime  0 1\n' > "$M/etc/fstab"
ln -sf "/usr/share/zoneinfo/$BASE_TZ" "$M/etc/localtime"
echo "$BASE_TZ" > "$M/etc/timezone"
echo "LANG=$BASE_LANG" > "$M/etc/default/locale"
echo "LANG=$BASE_LANG" > "$M/etc/locale.conf"
$CH systemctl set-default graphical.target > /dev/null 2>&1
$CH systemctl disable ssh.service ssh.socket > /dev/null 2>&1 || true
rm -f "$M"/etc/ssh/ssh_host_*
$CH glib-compile-schemas /usr/share/glib-2.0/schemas
mkdir -p "$M/etc/vibeos"
{ echo "BASE=archives"; echo "BUILT=$(date -u +%Y-%m-%dT%H:%MZ)"; echo "DEBIAN=$SUITE${SNAPSHOT:+ snapshot $SNAPSHOT}"
  echo "MOBIAN=$MOBIAN_SUITE"; [ -z "$HOLD_DEBS" ] || echo "HELD=$(echo $H)"; } > "$M/etc/vibeos/base"
log "host name vibeos, LABEL=vibeos, $BASE_TZ, $BASE_LANG, graphical.target, ssh off"

# 6. checks
$CH dpkg-query -W -f '${Package} ${Version} ${db:Status-Abbrev}\n' | LC_ALL=C sort > "$L/installed.txt"
$CH apt-mark showmanual | LC_ALL=C sort > "$L/manual.txt"
$CH apt-get -q check > "$L/apt-check.txt" 2>&1 || { cat "$L/apt-check.txt" >&2; exit 1; }
$CH dpkg --audit > "$L/dpkg-audit.txt" 2>&1; [ ! -s "$L/dpkg-audit.txt" ] || { cat "$L/dpkg-audit.txt" >&2; exit 1; }
for p in $(echo "$PKGS" | tr ',' ' '); do
	grep -q "^$p [^ ]* [ih]i *$" "$L/installed.txt" || { echo "$p is not installed" >&2; exit 1; }
done
! grep -E '^(phosh|phoc|squeekboard|mutter-mobile|gnome-shell-mobile|linux-image-[^ ]*) ' "$L/installed.txt" ||
	{ echo "a pinned-out package or a kernel is in the base" >&2; exit 1; }
[ -z "$(ls -A "$M/usr/lib/modules" 2>/dev/null)" ] || { echo "kernel modules in the base" >&2; exit 1; }
# nothing of this machine: its host name, its DNS search domain and name servers, the paths of this build
HN=$(hostname); P=$L/privacy-patterns.txt; : > "$P"
case $HN in ""|localhost|debian|vibeos) ;; *) echo "$HN" >> "$P"; [ "$(hostname -f 2>/dev/null)" = "$HN" ] || hostname -f >> "$P" 2>/dev/null || true ;; esac
sed -n 's/^\(search\|domain\)[[:space:]]\{1,\}//p' /etc/resolv.conf | tr ' \t' '\n\n' | sed '/^$/d' >> "$P"
sed -n 's/^nameserver[[:space:]]\{1,\}//p' /etc/resolv.conf | grep -v -E '^(127\.|::1$)' >> "$P" || true
for d in "$T" "$O" ${SUDO_USER:+/home/$SUDO_USER}; do echo "$d" >> "$P"; done
LC_ALL=C sort -u -o "$P" "$P"
(cd "$M" && grep -r -l -I -F -w -f "$P" etc var/lib/apt var/lib/AccountsService var/lib/exim4 var/log root home usr/local opt srv 2>/dev/null) > "$L/privacy-hits.txt" || true
[ ! -s "$L/privacy-hits.txt" ] || { echo "this machine's name, DNS or paths in the base (patterns in $P):" >&2; cat "$L/privacy-hits.txt" >&2; exit 1; }
[ ! -e "$M/etc/.resolv.conf.systemd-resolved.bak" ] || { echo "a copy of this machine's resolv.conf in the base" >&2; exit 1; }
[ -z "$(ls -A "$M/etc/NetworkManager/system-connections" 2>/dev/null)" ] || { echo "network connections in the base" >&2; exit 1; }
log "checks: $(wc -l < "$L/installed.txt") packages ($(wc -l < "$L/manual.txt") manual), apt and dpkg consistent; nothing of this machine"

# 7. clean up, shrink
$CH apt-get -q clean
rm -rf "$M"/var/lib/apt/lists/*_* "$M/usr/sbin/policy-rc.d" "$M/var/log/journal/"* "$M/var/tmp/"* "$M/tmp/"*
: > "$M/etc/machine-id"; rm -f "$M/var/lib/dbus/machine-id" "$M/var/lib/systemd/random-seed" "$M/var/lib/systemd/credential.secret"
du -sh --exclude=proc --exclude=sys --exclude=dev --exclude=run "$M" | cut -f1 > "$L/size.txt"
cleanup; trap - EXIT
if losetup -a | grep -qF "($(readlink -f "$IMG"))"; then echo "a loop device still holds $IMG: not shrinking" >&2; exit 1; fi
e2fsck -fy "$IMG" > "$L/e2fsck.txt" 2>&1 || [ $? -le 1 ]
resize2fs -M "$IMG" > /dev/null 2>&1; resize2fs -M "$IMG" > /dev/null 2>&1
BC=$(dumpe2fs -h "$IMG" 2>/dev/null | sed -n 's/^Block count: *//p')
truncate -s $((BC * 4096)) "$IMG"
e2fsck -fn "$IMG" > "$L/e2fsck-final.txt" 2>&1
# the root's tar (this build's own temporary file) stays only with KEEP_TAR=1
if [ -n "${KEEP_TAR:-}" ]; then log "kept: $W/root.tar"; else rm -f "$W/root.tar"; rmdir "$M" "$W" 2>/dev/null || true; fi
[ -z "${SUDO_USER:-}" ] || chown "$SUDO_USER": "$IMG"
log "done: $IMG, $((BC * 4096)) bytes; logs in $L"
