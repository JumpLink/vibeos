#!/bin/sh
# build-chroot.sh DIR: the arm64 Debian sid chroot that build-mutter.sh and build-mobile.sh compile in (qemu-user
# binfmt on a PC): buildd essentials, mutter's and gnome-shell's build dependencies, patchelf, and Debian's source
# index (apt-get source). The archives and SNAPSHOT are base/archive.sh's. Run as root; about 20 minutes.
set -eu
D=${1:?DIR (new)}
[ "$(id -u)" = 0 ] || { echo "run as root (sudo)" >&2; exit 1; }
T=$(cd "$(dirname "$0")" && pwd)
[ ! -f "$T/local.conf" ] || . "$T/local.conf"
. "$T/base/archive.sh"
[ ! -e "$D" ] || { echo "$D exists" >&2; exit 1; }
[ -e /proc/sys/fs/binfmt_misc/qemu-aarch64 ] || { echo "no qemu-aarch64 binfmt (apt install qemu-user-static)" >&2; exit 1; }
KT=$(mktemp -d /tmp/vibeos-keys.XXXXXX); trap 'rm -rf "${KT:?}"' EXIT
make_keyrings "$KT"
mkdir -p "$(dirname "$D")"
# mmdebstrap's own temporary files go next to the chroot (TMPDIR), not to /tmp
TMPDIR=$(dirname "$D") nice -n 19 mmdebstrap --arch=arm64 --variant=buildd --mode=root --format=directory \
	--keyring="$KT" --include=fakeroot,patchelf,ca-certificates,dpkg-dev \
	--customize-hook='chroot "$1" env DEBIAN_FRONTEND=noninteractive apt-get -y -q build-dep mutter gnome-shell' \
	--customize-hook='chroot "$1" dpkg-query -W -f "\${Package} \${Version}\n" > "$1/build-chroot-packages.txt"' \
	$SUITE "$D" "$(deb_line)" "$(deb_src_line)"
echo "${SNAPSHOT:-$(date -u +%Y%m%dT%H%M%SZ)}" > "$D/build-chroot-stamp"
echo "build chroot: $D ($(wc -l < "$D/build-chroot-packages.txt") packages)"
