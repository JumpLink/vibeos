#!/bin/sh
# build-system.sh: the generic VibeOS system image (arm64, no kernel, no modules, no device firmware), made
# from a COPY of a base image: VibeOS goes in.
#
#   sudo image/build-system.sh WORK.img
#
# WORK.img: a copy of a base image (never the original: BASE_ORIG names it, and is refused), either
#   - build-base.sh's, from the Debian and Mobian archives alone (/etc/vibeos/base says so): step 2 has nothing to do
#   - the S10e clean r7 image, the 0.1.3 release's base, the way the S20 FE's root was made
#     (G780F/kit/rootfs/build-rootfs.sh): its S10e device layer comes out in step 2 (STAGE needed)
# Inputs (environment, defaults below): SYSTEM_VERSION the release (default 0.1.4), VDEBS its .debs (1:SYSTEM_VERSION);
# MUTTER, SPEECH (mutter 51.0-3+gp1 and the speech models, carried since 0.1.1: build-mutter.sh,
# tools/vibeos-dictate-speech/build-debs.sh); ART (the
# third-party icon packs and parallax sets: ROM only, never in the repo or a release; optional); STAGE (the S10e
# device stage, whose every file is the device layer to remove: the r7 base only); MOBILE (GNOME Shell mobile with
# libshell patch 0005, as on the S10e and the Poco since 11-17: build-mobile.sh); KIT (the Poco kit: home layout,
# drawer override, the shell drop-in, the list of GNOME apps to remove).
#
#   1. grows the copy to 12 GiB (room for apt)
#   2. (the r7 base) removes the S10e device layer: every file of the device stage, the kernel and modules, the boot
#      glue, the S10e's first-boot units and the enablement links of all of them; the sleep-target masks (a device
#      decides)
#   3. generic identity: fstab LABEL=vibeos, hostname vibeos, no machine-info, no SSH host keys and no snakeoil
#      key (made on the first boot), nothing of the machine the base was built on (its resolv.conf, its name in
#      exim4's settings); the first-boot units every device shares (grow the root; install the device package);
#      a developer image only (VIBEOS_DEV=1): the USB network on the gadget port, which a release image never has
#   4. VibeOS SYSTEM_VERSION (apt in an arm64 chroot, local .debs + Debian for new dependencies: mpv, libmpv2): every
#      package of the build except -dev and -dbgsym; mutter gp1 held; GNOME's own
#      apps out (the ROM list), Showtime out, GNOME Settings out (vibeos-settings replaces it), Files and Vim
#      hidden; the home screen and the drawer list of the POCO ROM (Showtime's place: Video Player); user's
#      session vibeos
#   5. checks (a release image: no developer unit in it), cleans up, labels the file system vibeos (new UUID), shrinks it.
# Logs: LOGS/system-<time>/.
set -eu
IMG=${1:?WORK.img}
[ "$(id -u)" = 0 ] || { echo "run as root (sudo)" >&2; exit 1; }
# the build runs under the host name vibeos (its own UTS namespace): maintainer scripts write the host name they see
# into the image (exim4's mail name, ssl-cert), and the build machine's has no place there. Its mounts are its own (a
# private mount namespace): a service that starts during the build copies the host's mounts into a namespace of its
# own, where the image would stay mounted, and its loop device busy, until that service exits
[ -n "${VIBEOS_BUILD_UTS:-}" ] || exec env VIBEOS_BUILD_UTS=1 VIBEOS_BUILD_HOST="$(hostname -s 2>/dev/null || hostname)" \
	unshare --uts --mount --propagation private sh -c 'hostname vibeos && exec sh "$0" "$@"' "$0" "$@"
T=$(cd "$(dirname "$0")" && pwd)
[ ! -f "$T/local.conf" ] || . "$T/local.conf"	# this machine's settings (local.conf.example)
[ -z "${BASE_ORIG:-}" ] || [ "$(readlink -f "$IMG")" != "$(readlink -f "$BASE_ORIG")" ] || { echo "refusing: $IMG is BASE_ORIG (work on a copy)" >&2; exit 1; }
O=${VIBEOS_OUT:?set VIBEOS_OUT (image/local.conf): the directory builds, work files and logs go to}
SV=${SYSTEM_VERSION:-0.1.4}	# the VibeOS release the image carries: VDEBS holds its packages (1:SV)
case $SV in [0-9]*.[0-9]*.[0-9]*) ;; *) echo "SYSTEM_VERSION $SV: not a release number" >&2; exit 1 ;; esac
VDEBS=${VDEBS:?set VDEBS: the directory with the 1:$SV .debs}
MUTTER=${MUTTER:?set MUTTER: the directory with mutter 51.0-3+gp1}
SPEECH=${SPEECH:?set SPEECH: the directory with the vibeos-dictate-* .debs}
ART=${ART:-}	# the third-party icon packs and parallax sets (not in this repository): left out when unset
STAGE=${STAGE:-}	# the S10e device stage: needed only for the r7 base
MOBILE=${MOBILE:?set MOBILE: the GNOME Shell mobile tar (opt/gnome-mobile)}
KIT=${KIT:-$T/kit}
DEV=${VIBEOS_DEV:-0}	# 1: a developer image, with the debugging ways in (vibeos-usb-net); 0: a release image, without
case $DEV in 0) VARIANT=release ;; 1) VARIANT=dev ;; *) echo "VIBEOS_DEV is 0 or 1, not $DEV" >&2; exit 1 ;; esac
FORBID="gsm2sip waydroid ${FORBID_EXTRA:-}"	# never in an image: these package and file names
L=$O/logs/system-$(date +%Y%m%d-%H%M%S); mkdir -p "$L"
M=$O/work/mnt-rw
log() { echo "build-system: $*" | tee -a "$L/build.txt"; }
add() { echo "$*" >> "$L/added.txt"; }

for f in "$MOBILE" "$KIT/home-layout.json" "$KIT/90-rom-home.gschema.override" "$KIT/50-mobile.conf" "$KIT/ourapps-remove"; do
	[ -s "$f" ] || { echo "missing $f" >&2; exit 1; }
done
if losetup -a | grep -qF "($(readlink -f "$IMG"))"; then echo "a loop device already holds $IMG" >&2; exit 1; fi
mountpoint -q "$M" 2>/dev/null && { echo "$M is mounted already" >&2; exit 1; }

# the package set, before anything changes
P=$L/debs; mkdir -p "$P"
for f in "$VDEBS"/*_1:${SV}_*.deb "$VDEBS"/*_${SV}_*.deb; do
	[ -f "$f" ] || continue
	p=$(dpkg-deb -f "$f" Package); v=$(dpkg-deb -f "$f" Version)
	[ "$v" = 1:$SV ] || { echo "$f: version $v, not 1:$SV" >&2; exit 1; }
	case $p in *-dbgsym|*-dev) continue ;; esac
	cp "$f" "$P/"
done
for p in gir1.2-mutter-51 libmutter-51-0 mutter-common mutter-common-bin; do
	cp "$MUTTER"/${p}_51.0-3+gp1_*.deb "$P/"
done
cp "$SPEECH"/vibeos-dictate-*_*-1_*.deb "$P/"
[ -z "$ART" ] || for p in vibeos-iconpack-h2o vibeos-iconpack-oneui vibeos-parallax-sets; do cp "$ART"/${p}_0.1.0_all.deb "$P/"; done
# no development builds (0.1.3~next*, relaxed pins) and nothing that is not ours
for f in "$P"/*.deb; do
	v=$(dpkg-deb -f "$f" Version)
	case $v in *~next*|*~dev*|*+test*) echo "refusing $f: development version $v" >&2; exit 1 ;; esac
	if dpkg-deb -f "$f" Depends | tr ',' '\n' | grep -E '(vibeos|spongy|tonal)' | grep -q '>= '; then
		dpkg-deb -f "$f" Depends | tr ',' '\n' | grep -E '(libvibeos|libspongy|libtonal)-[0-9a-z-]* \(>=' &&
			{ echo "refusing $f: a relaxed pin on a VibeOS library" >&2; exit 1; } || true
	fi
done
(cd "$P" && sha256sum *.deb) > "$L/debs.sha256"
log "$(ls "$P" | wc -l) packages to install (logs/$(basename "$L")/debs.sha256)"

# 1. room for apt
e2fsck -fy "$IMG" > "$L/e2fsck-before.txt" 2>&1 || [ $? -le 1 ]
truncate -s 12G "$IMG"
resize2fs "$IMG" > /dev/null 2>&1
mkdir -p "$M"
mount -o loop,noatime "$IMG" "$M"
cleanup() {
	for d in dev/pts dev proc sys run; do umount "$M/$d" 2>/dev/null || true; done
	umount "$M" 2>/dev/null || true
}
trap cleanup EXIT
CH="chroot $M /usr/bin/env -i PATH=/usr/sbin:/usr/bin:/sbin:/bin HOME=/root LC_ALL=C.UTF-8 DEBIAN_FRONTEND=noninteractive"
rmv() { for p; do [ -e "$M$p" ] || [ -L "$M$p" ] || continue; rm -rf "$M$p" && echo "$p" >> "$L/removed.txt"; done; }
cat "$M"/var/lib/dpkg/info/*.list | LC_ALL=C sort -u > "$L/packaged.txt"
# which base: build-base.sh's (/etc/vibeos/base) or the S10e clean r7 image (hostname s10e, its stage in STAGE)
if grep -qx 'BASE=archives' "$M/etc/vibeos/base" 2>/dev/null; then
	BASEKIND=archives
	BASE="from the archives (build-base.sh, $(sed -n 's/^BUILT=//p' "$M/etc/vibeos/base"))"
elif [ "$(cat "$M/etc/hostname")" = s10e ]; then
	BASEKIND=s10e
	BASE="s10e-clean-20261117-r7 (device layer removed)"
	[ -d "$STAGE/root/boot/s10e" ] || { echo "the S10e clean image needs its device stage: no S10e stage at STAGE=$STAGE" >&2; exit 1; }
else
	echo "neither build-base.sh's base (/etc/vibeos/base) nor the S10e clean image (hostname)" >&2; exit 1
fi
log "base: $BASE"
log "variant: $VARIANT (VIBEOS_DEV=$DEV)"

# 2. the S10e's device layer (the r7 base; build-base.sh's has none)
if [ "$BASEKIND" = s10e ]; then
	# every file the device stage put in (device.sh: kernel, modules, firmware, units, udev, modprobe, UCM, splash...)
	# (a path some package of the image owns stays: the stage only ever adds device files)
	(cd "$STAGE/root" && find . ! -type d | sed 's/^\.//') | LC_ALL=C sort -u > "$L/stage-files.txt"
	LC_ALL=C comm -12 "$L/stage-files.txt" "$L/packaged.txt" > "$L/stage-packaged-kept.txt"
	LC_ALL=C comm -23 "$L/stage-files.txt" "$L/packaged.txt" | while read -r f; do rmv "$f"; done
	# and what build.sh added around it
	rmv /boot/s10e /usr/lib/modules/7.2.15-s10e /var/lib/initramfs-tools/7.2.15-s10e /var/lib/s10e /var/lib/exynos-cp \
		/etc/systemd/system/s10e-ssh-keygen.service /etc/environment.d/60-s10e-gpu.conf /etc/machine-info
	for f in "$M"/usr/local/sbin/s10e-* "$M"/usr/local/sbin/exynos-* "$M"/usr/local/bin/exynos-*; do rmv "${f#$M}"; done
	# enablement links whose unit is gone, and any S10e/Exynos unit or link left
	find "$M/etc/systemd" -xdev -type l | while read -r l; do
		n=${l##*/}
		case $n in s10e-*|exynos-*) rmv "${l#$M}"; continue ;; esac
		t=$(readlink "$l")
		case $t in /dev/null) continue ;; esac
		case $t in /*) [ -e "$M$t" ] || rmv "${l#$M}" ;; *) [ -e "${l%/*}/$t" ] || rmv "${l#$M}" ;; esac
	done
	# the sleep targets: the S10e masked them (s10e-sleepd sleeps the phone); a device package decides
	for u in sleep suspend hibernate hybrid-sleep suspend-then-hibernate; do
		[ "$(readlink "$M/etc/systemd/system/$u.target" 2>/dev/null)" = /dev/null ] && rmv /etc/systemd/system/$u.target
	done
	rmdir "$M"/usr/lib/firmware/brcm "$M"/usr/lib/firmware/samsung "$M"/boot/s10e 2>/dev/null || true
	# whatever still names the device outside the packages: listed, then removed
	find "$M/etc" "$M/usr/local" "$M/usr/lib/firmware" "$M/usr/share/alsa" "$M/var/lib" "$M/usr/share/glib-2.0/schemas" \
		-xdev \( -iname '*s10e*' -o -iname '*exynos*' -o -iname '*beyond0*' \) 2>/dev/null | sed "s|^$M||" > "$L/leftover.txt"
	LC_ALL=C sort -u "$L/leftover.txt" | LC_ALL=C comm -12 - "$L/packaged.txt" > "$L/leftover-kept.txt"
	LC_ALL=C sort -u "$L/leftover.txt" | LC_ALL=C comm -23 - "$L/packaged.txt" | while read -r f; do rmv "$f"; done
	log "S10e device layer removed: $(wc -l < "$L/removed.txt") paths"
else
	log "no device layer to remove (a base from the archives)"
fi
[ -z "$(ls -A "$M/usr/lib/modules" 2>/dev/null)" ] || { echo "modules left in /usr/lib/modules" >&2; exit 1; }
[ -z "$(ls -A "$M/boot" 2>/dev/null)" ] || { echo "/boot is not empty: $(ls "$M/boot")" >&2; exit 1; }

# 3. generic identity
printf '# the root file system, by its ext4 label: every VibeOS device boots the partition or image file the\n# system zip wrote, labelled vibeos (its kernel and initramfs come with the device package)\nLABEL=vibeos  /  ext4  defaults,noatime  0 1\n' > "$M/etc/fstab"
echo vibeos > "$M/etc/hostname"
printf '127.0.0.1\tlocalhost\n127.0.1.1\tvibeos\n::1\tlocalhost ip6-localhost ip6-loopback\n' > "$M/etc/hosts"
add "/etc/fstab LABEL=vibeos /; hostname vibeos (a device package may rename it)"
# nothing of the machine the base was built on: systemd-resolved's copy of the resolv.conf it found (that machine's
# search domain and name servers), ssl-cert's snakeoil key and certificate (one key in every image, with that
# machine's name in it: each device makes its own on the first boot, as with the SSH host keys); exim4's mail name
# is set again in step 4
rmv /etc/.resolv.conf.systemd-resolved.bak /etc/ssl/private/ssl-cert-snakeoil.key
for c in "$M"/etc/ssl/certs/*.0; do [ "$(readlink "$c" 2>/dev/null)" = ssl-cert-snakeoil.pem ] && rmv "${c#$M}"; done
rmv /etc/ssl/certs/ssl-cert-snakeoil.pem
# the boot splash is a device's (sized for its screen): the S10e's debian-swirl went with its stage
install -d "$M/usr/local/sbin" "$M/etc/vibeos" "$M/var/lib/vibeos/device"
install -m 755 "$T/rootfs/vibeos-growroot" "$M/usr/local/sbin/vibeos-growroot"
install -m 755 "$T/rootfs/vibeos-device-setup" "$M/usr/local/sbin/vibeos-device-setup"
UNITS="vibeos-growroot vibeos-device-setup vibeos-ssh-keygen vibeos-ssl-cert"
# a developer image only: the USB network (172.16.42.1 on the gadget port, a DHCP server for the PC) is a way in
# that needs no Wi-Fi and no unlocked screen, which a release image must not open
if [ "$DEV" = 1 ]; then
	install -m 755 "$T/rootfs/vibeos-usb-net" "$M/usr/local/sbin/vibeos-usb-net"
	install -D -m 644 "$T/rootfs/vibeos-usb-net.nm.conf" "$M/etc/NetworkManager/conf.d/vibeos-usb-net.conf"
	UNITS="$UNITS vibeos-usb-net"
fi
for u in $UNITS; do
	install -m 644 "$T/rootfs/$u.service" "$M/etc/systemd/system/$u.service"
	mkdir -p "$M/etc/systemd/system/multi-user.target.wants"
	ln -sf ../$u.service "$M/etc/systemd/system/multi-user.target.wants/$u.service"
done
add "first boot: vibeos-growroot (root fills its partition or file), vibeos-device-setup (the device package, offline, before GDM), vibeos-ssh-keygen, vibeos-ssl-cert"
[ "$DEV" = 0 ] || add "developer image: every boot vibeos-usb-net (USB network 172.16.42.1)"

# 4. VibeOS, in an arm64 chroot (qemu binfmt)
for d in proc sys dev dev/pts run; do mkdir -p "$M/$d"; done
mount -t proc proc "$M/proc"; mount -t sysfs sys "$M/sys"; mount --bind /dev "$M/dev"; mount --bind /dev/pts "$M/dev/pts"
mount -t tmpfs tmpfs "$M/run"
RESOLV=; if [ -e "$M/etc/resolv.conf" ] || [ -L "$M/etc/resolv.conf" ]; then mv "$M/etc/resolv.conf" "$M/etc/resolv.conf.vibeos-build"; RESOLV=1; fi
cp -L /etc/resolv.conf "$M/etc/resolv.conf"
printf '#!/bin/sh\n# build-system.sh: no service starts in the build chroot\nexit 101\n' > "$M/usr/sbin/policy-rc.d"; chmod 755 "$M/usr/sbin/policy-rc.d"
rm -rf "$M/tmp/vibeos-debs"; mkdir -p "$M/tmp/vibeos-debs"; cp "$P"/*.deb "$M/tmp/vibeos-debs/"
OURS=$(for f in "$P"/*.deb; do dpkg-deb -f "$f" Package; done)
LOCAL=$(for f in "$P"/*.deb; do echo "/tmp/vibeos-debs/${f##*/}"; done)
NEW="mpv libmpv2 gdisk cloud-guest-utils"
# exim4's mail name and local domains: the host name, vibeos (the base's say the build machine's when it was made
# under that machine's name, as the r7 base was). exim4-config's own settings file wins over debconf on a
# reconfigure, so it is set first; the reconfigure then writes debconf and config.autogenerated from it
if [ -f "$M/etc/exim4/update-exim4.conf.conf" ]; then
	sed -i "s/^dc_other_hostnames=.*/dc_other_hostnames='vibeos'/" "$M/etc/exim4/update-exim4.conf.conf"
	printf 'exim4-config exim4/mailname string vibeos\nexim4-config exim4/dc_other_hostnames string vibeos\n' | $CH debconf-set-selections
	echo vibeos > "$M/etc/mailname"
	$CH dpkg-reconfigure -f noninteractive exim4-config > "$L/exim4.txt" 2>&1 || { cat "$L/exim4.txt" >&2; exit 1; }
	rm -f "$M"/var/cache/debconf/*.dat-old
	add "exim4: mail name and local domain vibeos"
fi
$CH apt-get -q update > "$L/apt-update.txt" 2>&1 || { tail "$L/apt-update.txt" >&2; exit 1; }
# the Debian/Mobian packages the two Qualcomm devices run and this generic image does not carry, at the versions
# their verified images had (the POCO's k22 ROM of 11-17, the S5e's clean image): fetched here, through apt's
# signature checks, for their device folders. A version the archive no longer has is logged, not fatal.
DL_poco="droid-juicer=0.4.3-1 firmware-atheros=20260916-1 firmware-qcom-dsp=0~20261019-1 firmware-qcom-soc=20260916-1 hexagonrpcd=0.4.0-2 libdrm-freedreno1=2.4.134-3 libhexagonrpc0.4=0.4.0-2 libqrtr1=1.2-1 make-dynpart-mappings=10.2.4-2+b1 mkbootimg=1:34.0.5-13 mobile-usb-networking=0.0.4 protection-domain-mapper=1.1-1 qbootctl=0.2.2-2 qcom-phone-utils=0.4.5 qcom-support-common=0.3.5 qrtr-tools=1.2-1 rmtfs=1.3-1 systemd-boot-efi=262.2-1 tqftpserv=1.3-1"
DL_s5e="droid-juicer=0.4.3-1 hexagonrpcd=0.4.0-2 i2c-tools=4.4-3+b2 libhexagonrpc0.4=0.4.0-2 libi2c0=4.4-3+b2 libqrtr1=1.2-1 make-dynpart-mappings=10.2.4-2+b1 mkbootimg=1:34.0.5-13 qbootctl=0.2.2-2 qcom-phone-utils=0.4.5 qrtr-tools=1.2-1 read-edid=3.0.2-3+b1 rmtfs=1.3-1 tqftpserv=1.3-1"
# KEEP_DOWNLOADS=1: the downloads a DOWNLOAD_ONLY run made stay (the device folders were built from them);
# DOWNLOAD_ONLY=1 (on a throwaway copy of the r7 image): stop after them
for d in poco s5e; do
	eval list=\$DL_$d
	if [ -n "${KEEP_DOWNLOADS:-}" ] && [ -n "$(ls "$O/work/downloads/$d" 2>/dev/null)" ]; then
		log "downloads for $d: kept, $(ls "$O/work/downloads/$d" | wc -l) of $(echo $list | wc -w) ($(cd "$O/work/downloads/$d" && sha256sum *.deb | sha256sum | cut -c1-16))"
		continue
	fi
	rm -rf "$M/tmp/dl-$d" "$O/work/downloads/$d"; mkdir -p "$M/tmp/dl-$d" "$O/work/downloads/$d"
	for pv in $list; do
		$CH sh -c "cd /tmp/dl-$d && apt-get download -q $pv" >> "$L/download-$d.txt" 2>&1 || echo "$pv" >> "$L/download-$d-missing.txt"
	done
	cp "$M"/tmp/dl-$d/*.deb "$O/work/downloads/$d/" 2>/dev/null || true
	rm -rf "$M/tmp/dl-$d"
	log "downloads for $d: $(ls "$O/work/downloads/$d" | wc -l) of $(echo $list | wc -w)$( [ -s "$L/download-$d-missing.txt" ] && echo ", missing: $(tr '\n' ' ' < "$L/download-$d-missing.txt")")"
done
[ -z "${DOWNLOAD_ONLY:-}" ] || { log "DOWNLOAD_ONLY: stopping after the downloads"; exit 0; }
# Debian's evolution-data-server where it is (EDS_HOLD, the list releases.html gives for 0.1.4), for the releases built
# against EDS 3.56, every image's (0.1.4 and before): Debian unstable's 3.62 would take their apps with it. Exactly the
# installed part of the list, held before the plan: a dependency that needs a newer EDS then stops the build here
# instead of upgrading it. 0.1.4.1 and later are built against 3.62 and their notes lift the hold: nothing is held, and
# the plan takes 3.62 with them (ALLOW_UPGRADES)
case $SV in
0.1.[0-4]) EDS_HOLD=${EDS_HOLD-evolution-data-server evolution-data-server-common libebook-1.2-21t64 libecal-2.0-3 libedata-book-1.2-27t64 libedata-cal-2.0-2t64 libebackend-1.2-11t64 libedataserverui-1.2-4t64 libedataserverui4-1.0-0t64 gir1.2-camel-1.2 gir1.2-ecal-2.0 gir1.2-edataserver-1.2 libfolks26 libfolks-eds26 folks-common} ;;
*) EDS_HOLD=${EDS_HOLD-} ;;
esac
EH=$(for p in $EDS_HOLD; do [ "$($CH dpkg-query -W -f '${db:Status-Abbrev}' "$p" 2>/dev/null)" = "ii " ] && echo "$p"; done | tr '\n' ' ')
[ -z "$EH" ] || { $CH apt-mark hold $EH > /dev/null; add "held: $EH(Debian's EDS, which VibeOS $SV is built against)"; }
$CH apt-get -s -q --no-install-recommends install $LOCAL $NEW showtime- > "$L/apt-plan.txt" 2>&1 || { tail -30 "$L/apt-plan.txt" >&2; exit 1; }
# removals: exactly Showtime (the brief: out) and GNOME Settings (vibeos-settings Conflicts/Replaces/Provides it),
# and with it the gnome-core metapackage (it Depends on gnome-control-center >= 1:51; what it pulled in is marked
# manual after the install, so that no autoremove takes it)
RM=$(sed -n 's/^Remv \([^ :]*\).*/\1/p' "$L/apt-plan.txt" | LC_ALL=C sort -u | tr '\n' ' ')
case "$RM" in
"gnome-control-center showtime "|"gnome-control-center gnome-core showtime ") ;;
*) echo "apt would remove: $RM (only gnome-control-center showtime, and the gnome-core metapackage, allowed)" >&2; exit 1 ;;
esac
REPL=$(sed -n 's/^Inst \([^ :]*\) \[.*/\1/p' "$L/apt-plan.txt")
UPG=$(echo "$REPL" | sed '/^$/d' | grep -vxF "$OURS" || true)
EXTRA=$(sed -n 's/^Inst \([^ :]*\) (.*/\1/p' "$L/apt-plan.txt" | grep -vxF "$OURS" || true)
echo "$UPG" > "$L/apt-upgrades.txt"; echo "$EXTRA" > "$L/apt-new.txt"
log "apt: removes $RM; new from Debian: $(echo $EXTRA | wc -w); upgrades of installed packages not ours: $(echo $UPG | wc -w)"
if [ -n "$UPG" ] && [ -z "${ALLOW_UPGRADES:-}" ]; then echo "apt would upgrade (ALLOW_UPGRADES=1 to accept, see $L/apt-upgrades.txt):" $UPG >&2; exit 1; fi
[ -z "${PLAN_ONLY:-}" ] || { log "PLAN_ONLY: stopping before the install"; exit 0; }
$CH apt-get -y -q --no-install-recommends -o Dpkg::Options::=--force-confdef -o Dpkg::Options::=--force-confold \
	install $LOCAL $NEW showtime- > "$L/apt.txt" 2>&1 || { tail -30 "$L/apt.txt" >&2; exit 1; }
$CH apt-get -y -q purge showtime > /dev/null 2>&1 || true
bad=; for p in $OURS $NEW gnome-control-center-data; do
	s=$($CH dpkg-query -W -f '${db:Status-Abbrev}' "$p" 2>/dev/null || true)
	case "$s" in "ii "|"hi ") ;; *) bad="$bad $p" ;; esac
done
[ -z "$bad" ] || { echo "not installed:$bad" >&2; exit 1; }
for p in gnome-control-center showtime; do
	s=$($CH dpkg-query -W -f '${db:Status-Abbrev}' "$p" 2>/dev/null || true)
	case "$s" in ""|"un "|"rc "|"pn ") ;; *) echo "$p is still installed ($s)" >&2; exit 1 ;; esac
done
$CH apt-mark manual $NEW > /dev/null
AR=$($CH apt-get -s -q autoremove 2>/dev/null | sed -n 's/^Remv \([^ :]*\).*/\1/p' | tr '\n' ' ')
[ -z "$AR" ] || { $CH apt-mark manual $AR > /dev/null; echo "marked manual (autoremove would have taken them with gnome-core gone): $AR" >> "$L/removed.txt"; }
add "VibeOS $SV: $(echo $OURS | wc -w) packages (debs.sha256), plus $NEW from Debian; gnome-control-center, showtime (and the gnome-core metapackage) removed, gnome-control-center-data kept"
HOLD=$(echo "$OURS" | grep mutter | tr '\n' ' ')
$CH apt-mark hold $HOLD > /dev/null; add "held: $HOLD(VibeOS's mutter build)"
# GNOME's own apps out (the ROM's list, as the S10e/POCO/S20): exactly the installed part of it, or nothing
RL=$(for p in $(sed -e 's/#.*//' -e '/^[[:space:]]*$/d' "$KIT/ourapps-remove"); do
	[ "$($CH dpkg-query -W -f '${db:Status-Abbrev}' "$p" 2>/dev/null)" = "ii " ] && echo "$p"; done | LC_ALL=C sort -u)
if [ -n "$RL" ]; then
	RP=$($CH apt-get -s -q remove $RL 2>/dev/null | sed -n 's/^Remv \([^ :]*\).*/\1/p' | LC_ALL=C sort -u)
	if [ "$RL" = "$RP" ]; then
		$CH apt-get -y -q remove $RL > "$L/apt-remove.txt" 2>&1
		AR=$($CH apt-get -s -q autoremove 2>/dev/null | sed -n 's/^Remv \([^ :]*\).*/\1/p')
		[ -z "$AR" ] || $CH apt-mark manual $AR > /dev/null
		echo "GNOME apps (the ROM list):" $RL >> "$L/removed.txt"
	else
		echo "GNOME's apps: apt would remove more:" $(echo "$RP" | grep -vxF "$RL") >&2; exit 1
	fi
fi
mkdir -p "$M/usr/local/share/applications"
for a in org.gnome.Nautilus vim; do
	[ -f "$M/usr/share/applications/$a.desktop" ] || continue
	sed '/^NoDisplay=/d; /^\[Desktop Entry\]/a NoDisplay=true' "$M/usr/share/applications/$a.desktop" \
		> "$M/usr/local/share/applications/$a.desktop" && add "hidden from the launcher: $a"
done
$CH update-desktop-database -q /usr/local/share/applications 2>/dev/null || true
# GNOME Shell mobile with libshell patch 0005 (the S10e's and the POCO's since 11-17), for the vibeos session
rm -rf "$M/opt/gnome-mobile"
tar -C "$M" --no-same-owner -xpf "$MOBILE" opt/gnome-mobile
install -D -m 644 "$KIT/50-mobile.conf" "$M/etc/systemd/user/org.gnome.Shell@vibeos.service.d/50-mobile.conf"
add "/opt/gnome-mobile from $(basename "$MOBILE") (libshell-51.so $(sha256sum "$M/opt/gnome-mobile/lib/gnome-shell/libshell-51.so" | cut -c1-16))"
# the home screen: the POCO ROM's (11-18 kit), with Video Player where Showtime was
mkdir -p "$M/etc/xdg/vibeos-launcher"	# (the r7 base had one: the S10e kit's)
python3 -I - "$KIT/home-layout.json" "$M/etc/xdg/vibeos-launcher/layout.json" <<'EOF'
import json, sys
d = json.load(open(sys.argv[1]))
n = 0
for page in [d["dock"]] + d["pages"]:
    for i, x in enumerate(page):
        if x == "org.gnome.Showtime.desktop":
            page[i] = "vibeos.Video.desktop"; n += 1
assert n == 1, n
open(sys.argv[2], "w").write(json.dumps(d, indent=1) + "\n")
EOF
install -m 644 "$KIT/90-rom-home.gschema.override" "$M/usr/share/glib-2.0/schemas/90-rom-home.gschema.override"
for id in $(python3 -I -c 'import json,sys; d=json.load(open(sys.argv[1])); print(" ".join(x for p in [d["dock"]]+d["pages"] for x in p if x))' "$M/etc/xdg/vibeos-launcher/layout.json"); do
	[ -f "$M/usr/share/applications/$id" ] || { echo "home layout: no $id" >&2; exit 1; }
done
add "/etc/xdg/vibeos-launcher/layout.json (POCO 11-18, Showtime -> vibeos.Video.desktop) + 90-rom-home.gschema.override"
F=$M/var/lib/AccountsService/users/user; mkdir -p "${F%/*}"
if [ ! -e "$F" ] || ! grep -q '^\[User\]' "$F"; then printf '[User]\nSession=vibeos\n' >> "$F"
elif grep -q '^Session=' "$F"; then sed -i 's/^Session=.*/Session=vibeos/' "$F"
else sed -i '/^\[User\]/a Session=vibeos' "$F"; fi
chmod 600 "$F"; add "user's session: vibeos (AccountsService)"
$CH glib-compile-schemas /usr/share/glib-2.0/schemas
# what the image says about itself
{ echo "VERSION=1:$SV"; echo "BUILT=$(date -u +%Y-%m-%dT%H:%MZ)"; echo "BASE=$BASE"; echo "VARIANT=$VARIANT"; } > "$M/etc/vibeos/system-image"
$CH dpkg-query -W -f '${Package} ${Version} ${db:Status-Abbrev}\n' > "$L/installed.txt"
# every file the image's packages own: build-device.sh diverts the ones a device package ships too (SYSFILES)
cat "$M"/var/lib/dpkg/info/*.list | LC_ALL=C sort -u > "$L/packaged-final.txt"
$CH apt-get -q check > "$L/apt-check.txt" 2>&1 || { cat "$L/apt-check.txt" >&2; exit 1; }
$CH dpkg --audit > "$L/dpkg-audit.txt" 2>&1; [ ! -s "$L/dpkg-audit.txt" ] || { cat "$L/dpkg-audit.txt" >&2; exit 1; }
# never in an image
for p in showtime gnome-control-center $FORBID; do
	! grep -q "^$p " "$L/installed.txt" || { grep "^$p " "$L/installed.txt" | grep -q -E ' (un|rc|pn) *$' || { echo "$p is in the image" >&2; exit 1; }; }
done
# AppStream's catalog cache, from this build's apt update: links into the apt lists (removed below) and the icons
# unpacked from them. It lists every Debian app, Waydroid's icon among them; the next apt update makes it again
rm -rf "$M"/var/lib/swcatalog/icons/debian-* "$M"/var/lib/swcatalog/yaml/*
for w in $FORBID; do find "$M" -xdev -iname "*$w*"; done > "$L/forbidden.txt"
[ ! -s "$L/forbidden.txt" ] || { echo "forbidden files, see $L/forbidden.txt" >&2; exit 1; }
# a release image: none of the developer image's ways in, under any name the units or their files have
if [ "$DEV" = 0 ]; then
	find "$M" -xdev -name 'vibeos-usb-net*' > "$L/dev-only.txt"
	[ ! -s "$L/dev-only.txt" ] || { echo "a release image with developer files, see $L/dev-only.txt" >&2; exit 1; }
fi
grep -q '^video/mp4=vibeos.Video.desktop' "$M/etc/xdg/mimeapps.list" || { echo "Video Player is not the default for video/mp4" >&2; exit 1; }
# the build machine's name: in no setting a maintainer script writes it to
case ${VIBEOS_BUILD_HOST:-} in ""|vibeos|localhost|debian) ;; *)
	for f in etc/mailname etc/hosts etc/exim4/update-exim4.conf.conf var/lib/exim4/config.autogenerated var/cache/debconf/config.dat; do
		[ ! -f "$M/$f" ] || ! grep -q -w -F "$VIBEOS_BUILD_HOST" "$M/$f" || { echo "/$f names the build machine" >&2; exit 1; }
	done ;;
esac
[ ! -e "$M/etc/.resolv.conf.systemd-resolved.bak" ] && [ ! -e "$M/etc/ssl/private/ssl-cert-snakeoil.key" ] || { echo "a resolv.conf copy or a snakeoil key in the image" >&2; exit 1; }

# 5. clean up, unmount, check, label, shrink
$CH apt-get -q clean
rm -rf "$M"/var/lib/apt/lists/*_* "$M/tmp/vibeos-debs" "$M/usr/sbin/policy-rc.d"
rm -f "$M/etc/resolv.conf"; [ -z "$RESOLV" ] || mv "$M/etc/resolv.conf.vibeos-build" "$M/etc/resolv.conf"
: > "$M/etc/machine-id"; rm -f "$M"/etc/ssh/ssh_host_* "$M/var/lib/dbus/machine-id"
rm -rf "$M/var/log/journal/"* "$M/var/tmp/"*
cleanup; trap - EXIT
if losetup -a | grep -qF "($(readlink -f "$IMG"))"; then echo "a loop device still holds $IMG: not shrinking" >&2; exit 1; fi
e2fsck -fy "$IMG" > "$L/e2fsck-after.txt" 2>&1 || [ $? -le 1 ]
tune2fs -L vibeos -U random "$IMG" > /dev/null
resize2fs -M "$IMG" > /dev/null 2>&1; resize2fs -M "$IMG" > /dev/null 2>&1
BC=$(dumpe2fs -h "$IMG" 2>/dev/null | sed -n 's/^Block count: *//p')
truncate -s $((BC * 4096)) "$IMG"
e2fsck -fn "$IMG" > "$L/e2fsck-final.txt" 2>&1
log "done: $IMG, $((BC * 4096)) bytes; logs in $L"
