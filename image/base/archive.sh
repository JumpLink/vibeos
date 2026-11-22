# base/archive.sh, sourced by build-base.sh and build-chroot.sh (build-mutter.sh and build-mobile.sh use the chroot's
# apt): the archives the system image comes from (the 0.1.3 image's /etc/apt: Debian sid and Mobian staging), their keys.
#
#   DEBIAN_MIRROR   Debian's archive (default http://deb.debian.org/debian)
#   SNAPSHOT        a time such as 20261117T000000Z: Debian as snapshot.debian.org has it at that time, for a build
#                   that resolves the way an earlier one did (Mobian keeps no snapshots: its staging suite is used as
#                   it is now). The image itself always gets DEBIAN_MIRROR in its sources.
#   MOBIAN_MIRROR   Mobian's archive (default http://repo.mobian.org/)
#   DEBIAN_KEYRING  Debian's archive keyring on this machine (package debian-archive-keyring)
DEBIAN_MIRROR=${DEBIAN_MIRROR:-http://deb.debian.org/debian}
MOBIAN_MIRROR=${MOBIAN_MIRROR:-http://repo.mobian.org/}
DEBIAN_KEYRING=${DEBIAN_KEYRING:-/usr/share/keyrings/debian-archive-keyring.gpg}
SNAPSHOT=${SNAPSHOT:-}
SUITE=sid
MOBIAN_SUITE=staging
COMPONENTS="main contrib non-free non-free-firmware"
MOBIAN_COMPONENTS="main non-free-firmware"
# the key Mobian signs staging with (checked in the keyring Debian's mobian-archive-keyring package carries)
MOBIAN_STAGING_FPR=320660101B990C009FC0DC5AACB9BCCBED53C8BC
if [ -n "$SNAPSHOT" ]; then
	case $SNAPSHOT in [12][0-9][0-9][0-9][01][0-9][0-3][0-9]T[0-9][0-9][0-9][0-9][0-9][0-9]Z) ;;
	*) echo "SNAPSHOT=$SNAPSHOT: expected YYYYMMDDTHHMMSSZ" >&2; exit 1 ;; esac
	DEB_URI=http://snapshot.debian.org/archive/debian/$SNAPSHOT
	DEB_OPT="check-valid-until=no"
else
	DEB_URI=$DEBIAN_MIRROR
	DEB_OPT=
fi
[ -s "$DEBIAN_KEYRING" ] || { echo "no Debian archive keyring at $DEBIAN_KEYRING (apt install debian-archive-keyring)" >&2; exit 1; }

# the build's apt lines (mmdebstrap's arguments): Debian (or its snapshot) and Mobian, binary and source
deb_line() { echo "deb${DEB_OPT:+ [$DEB_OPT]} $DEB_URI $SUITE $COMPONENTS"; }
deb_src_line() { echo "deb-src${DEB_OPT:+ [$DEB_OPT]} $DEB_URI $SUITE $COMPONENTS"; }
mobian_line() { echo "deb $MOBIAN_MIRROR $MOBIAN_SUITE $MOBIAN_COMPONENTS"; }

# make_keyrings DIR: Debian's keyring, and Mobian's taken from Debian's mobian-archive-keyring package (fetched by
# this machine's apt from Debian, so Debian's signature vouches for it), in DIR, readable by apt's sandbox user
make_keyrings() {
	k=$1
	mkdir -p "$k/apt/lists/partial" "$k/apt/cache/archives/partial" "$k/dl" "$k/x"
	chmod 755 "$k" "$k/apt" "$k/apt/lists" "$k/apt/lists/partial" "$k/apt/cache" "$k/apt/cache/archives" \
		"$k/apt/cache/archives/partial" "$k/dl"
	cp "$DEBIAN_KEYRING" "$k/debian-archive-keyring.gpg"
	echo "deb [arch=arm64 signed-by=$k/debian-archive-keyring.gpg${DEB_OPT:+ $DEB_OPT}] $DEB_URI $SUITE main" > "$k/apt/sources.list"
	: > "$k/apt/status"
	chmod 644 "$k"/*.gpg "$k/apt/sources.list" "$k/apt/status"
	set -- -q -o Dir::Etc::SourceList="$k/apt/sources.list" -o Dir::Etc::SourceParts=/nonexistent \
		-o Dir::Etc::Preferences=/nonexistent -o Dir::Etc::PreferencesParts=/nonexistent \
		-o Dir::Etc::Trusted=/nonexistent -o Dir::Etc::TrustedParts=/nonexistent \
		-o Dir::State::Lists="$k/apt/lists" -o Dir::Cache="$k/apt/cache" -o Dir::State::status="$k/apt/status" \
		-o APT::Architecture=arm64 -o APT::Architectures::=arm64 -o Debug::NoLocking=1
	apt-get "$@" update > "$k/apt.log" 2>&1 || { cat "$k/apt.log" >&2; return 1; }
	(cd "$k/dl" && apt-get "$@" download mobian-archive-keyring) >> "$k/apt.log" 2>&1 || { cat "$k/apt.log" >&2; return 1; }
	dpkg-deb -x "$k"/dl/mobian-archive-keyring_*.deb "$k/x"
	cp "$k/x/usr/share/keyrings/mobian-archive-keyring.gpg" "$k/mobian-archive-keyring.gpg"
	chmod 644 "$k/mobian-archive-keyring.gpg"
	gpg --show-keys --with-colons "$k/mobian-archive-keyring.gpg" 2>/dev/null | awk -F: '$1 == "fpr" { print $10 }' | grep -qx "$MOBIAN_STAGING_FPR" ||
		{ echo "Mobian's keyring ($(ls "$k"/dl)) lacks the staging key $MOBIAN_STAGING_FPR" >&2; return 1; }
	rm -rf "${k:?}/apt" "${k:?}/dl" "${k:?}/x" "${k:?}/apt.log"
}
