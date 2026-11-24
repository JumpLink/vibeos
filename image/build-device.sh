#!/bin/sh
# build-device.sh DEVICE: one device's half of VibeOS 0.1.3 (s10e, poco, s5e, s20), into OUT/DEVICE/:
#   vibeos-device-DEVICE_0.1.3_arm64.deb  the device layer as a package: the kernel's modules, ALL the device's
#                                         firmware, its units, udev/modprobe/UCM/sensor/GPU settings, its splash;
#                                         the kernel too where it lives in the root file system (S10e: /boot/s10e)
#   debs/                                 that package, plus the system packages the device runs in its own build
#                                         (ModemManager, libcamera...) or that a generic image lacks (Qualcomm
#                                         support: rmtfs, qrtr, pd-mapper, hexagonrpcd, firmware-qcom-*...): the
#                                         first boot installs them offline (vibeos-device-setup)
#   payload.tar                           the files of all of them, which the device zip unpacks straight into the
#                                         root file system so the first boot already finds its hardware
#   boot/                                 the boot images, byte for byte the device's verified ones (U-Boot, ESP,
#                                         recovery partition, BOOT)
# Sources: every input is a device kit, stage or backup of this build machine (never a research-dl folder), named
# below by its setting in image/local.conf (local.conf.example lists them).
# Nothing is read from a phone. Run as root (sudo): payload files keep their modes, owned by root.
set -eu
DEV=${1:?s10e|poco|s5e|s20}
[ "$(id -u)" = 0 ] || { echo "run as root (sudo)" >&2; exit 1; }
T=$(cd "$(dirname "$0")" && pwd)
[ ! -f "$T/local.conf" ] || . "$T/local.conf"	# this machine's settings (local.conf.example)
O=${VIBEOS_OUT:?set VIBEOS_OUT (image/local.conf): the directory builds, work files and logs go to}
FORBID="gsm2sip waydroid ${FORBID_EXTRA:-}"	# never in an image: these package and file names
OUT=${TEST_OUT:-$O/$DEV}	# TEST_OUT, TEST_W: a test build elsewhere, without the downloaded packages
W=${TEST_W:-$O/work/device-$DEV}
L=$O/logs/device-$DEV-$(date +%Y%m%d-%H%M%S); mkdir -p "$L"
V=0.1.3
# 0.1.3+1 (2026-11-23): the S10e's and the S5e's initramfs always start Mobian, the oneshots ahead of gdm get start
# timeouts, and the POCO's package lost the restart into recovery and Android and checks its root with fsck -y.
# Above the archive's 0.1.3, whose POCO package would bring that restart back (every restart into recovery)
case $DEV in s10e|s5e|poco) V=0.1.3+1 ;; esac
P=$W/pkg
log() { echo "build-device $DEV: $*" | tee -a "$L/build.txt"; }
[ ! -e "$OUT" ] || { echo "$OUT exists: move it aside first" >&2; exit 1; }
[ ! -e "$W" ] || { echo "$W exists: move it aside first" >&2; exit 1; }
mkdir -p "$P/DEBIAN" "$W/debs" "$W/boot"
case "$(readlink -f "$W")" in */research-dl/*) exit 1 ;; esac

# enable UNIT...: the links "systemctl enable" makes, made in the package's own tree (the zip has no systemctl)
enable() { systemctl --root="$P" enable "$@" > "$L/enable.txt" 2>&1 || { cat "$L/enable.txt" >&2; exit 1; }; }
mask() { for u; do mkdir -p "$P/etc/systemd/system"; ln -sfn /dev/null "$P/etc/systemd/system/$u"; done; }
# not_held UNIT TIME WHY: a start timeout for a oneshot the GUI waits for (they wait for ever by default; a hang
# would keep gdm from ever starting). Past TIME it is stopped and the boot goes on
not_held() {
	install -d "$P/etc/systemd/system/$1.d"
	printf '%s\n' "# $3" "# A oneshot waits for ever by default, so a hang here would keep the GUI from ever starting; past this" \
		"# it is stopped and the boot goes on (nothing stops the boot to the GUI)." \
		"[Service]" "TimeoutStartSec=$2" "TimeoutStopSec=15s" > "$P/etc/systemd/system/$1.d/boot-not-held.conf"
}
# fw SRC DST...: firmware files, mode 644
fw() { s=$1; shift; for f; do install -D -m 644 "$s/$f" "$P/usr/lib/firmware/$f"; echo "$f $(sha256sum < "$s/$f" | cut -c1-16) $s" >> "$L/firmware.txt"; done; }
# modules KVER SRCDIR: SRCDIR/lib/modules/KVER (a modules_install tree) into the package, depmod'ed
modules() {
	mkdir -p "$P/usr/lib/modules"
	cp -a "$2/$1" "$P/usr/lib/modules/$1"
	rm -f "$P/usr/lib/modules/$1/build" "$P/usr/lib/modules/$1/source"
	depmod -b "$P/usr" "$1" 2>> "$L/depmod.txt" || depmod -b "$P" "$1"
	echo "modules $1: $(find "$P/usr/lib/modules/$1" -name '*.ko*' | wc -l) from $2" >> "$L/firmware.txt"
}
identity() {	# NAME PRETTY VENDOR MODEL
	install -D -m 644 /dev/stdin "$P/usr/share/vibeos-device/hostname" <<EOF
$1
EOF
	printf 'PRETTY_HOSTNAME="%s"\nCHASSIS=handset\nHARDWARE_VENDOR=%s\nHARDWARE_MODEL="%s"\n' "$2" "$3" "$4" |
		install -D -m 644 /dev/stdin "$P/usr/share/vibeos-device/machine-info"
}
# repack PKG: a .deb of PKG as the mounted k22 root ($KR) has it installed: its status stanza as the control file,
# its files from its .list, each checked against its .md5sums (for a Debian version the archive no longer has)
repack() {
	R=$W/repack-$1; rm -rf "$R"; mkdir -p "$R/DEBIAN"
	I=$KR/var/lib/dpkg/info
	[ ! -e "$I/$1.postinst" ] && [ ! -e "$I/$1.preinst" ] && [ ! -e "$I/$1.conffiles" ] || { echo "repack $1: it has scripts or conffiles" >&2; exit 1; }
	dpkg-query --admindir="$KR/var/lib/dpkg" -s "$1" | grep -v -E '^(Status|Config-Version):' > "$R/DEBIAN/control"
	(cd "$KR" && while IFS= read -r f; do [ -d ".$f" ] && [ ! -L ".$f" ] || printf '.%s\n' "$f"; done < "$I/$1.list") > "$R.files"
	tar -C "$KR" --no-recursion -cf - -T "$R.files" | tar -C "$R" -xpf - --no-same-owner
	(cd "$R" && md5sum -c --quiet "$I/$1.md5sums") || { echo "repack $1: files differ from dpkg's md5sums" >&2; exit 1; }
	[ "$(grep -c . "$I/$1.md5sums")" = "$(cd "$R" && find . -path ./DEBIAN -prune -o -type f -print | wc -l)" ] || { echo "repack $1: file count" >&2; exit 1; }
	cp "$I/$1.md5sums" "$R/DEBIAN/md5sums"
	dpkg-deb --root-owner-group -Zxz -b "$R" "$W/debs/${1}_$(sed -n 's/^Version: //p' "$R/DEBIAN/control" | sed 's/^[0-9]*://')_all.deb" > /dev/null
	rm -rf "${R:?}" "$R.files"
}
# Wi-Fi's regulatory database. cfg80211 checks regulatory.db against the signers built into the kernel. These
# kernels have the upstream signers only (sforshee, wens), and Debian's wireless-regdb defaults to its own re-signed
# copy (regulatory.db-debian, see its README.Debian), so the postinst selects regulatory.db-upstream. A cfg80211
# built into the kernel asks for the database once, at late_initcall, before any root is mounted, and never again by
# itself (dmesg "failed to load regulatory.db", then the world domain, whatever cfg80211.ieee80211_regdom says):
# that kernel's initramfs must carry it. regdb_cpio makes $W/regdb.cpio, a newc archive of lib/firmware/regulatory.db
# and .p7s (the system image's upstream-signed pair, checked against wireless-regdb's md5sums); regdb_append FILE
# adds it to an initramfs after zeros to a 4-byte boundary, where the kernel looks for the next archive (it
# unpacks concatenated archives, compressed or not, one after another).
SYSIMG=${SYSIMG:-$O/system/rootfs.img}
signers() {	# FILE: the wireless-regdb signers a cfg80211.ko[.zst|.xz] or a kernel Image (raw or .gz) trusts
	case $1 in *.zst) zstd -dcq "$1" ;; *.xz) xz -dc "$1" ;; *.gz) gzip -dc "$1" ;; *) cat "$1" ;; esac > "$W/signers.bin"
	if grep -aq 'benh@debian.org' "$W/signers.bin"; then echo debian
	elif grep -aq -e sforshee -e wens "$W/signers.bin"; then echo upstream
	else echo none; fi
	rm -f "$W/signers.bin"
}
regdb_cpio() {
	RG=$W/regdb; rm -rf "$RG"; mkdir -p "$RG/lib/firmware"
	[ -s "$SYSIMG" ] || { echo "no $SYSIMG (the system image, for its wireless-regdb)" >&2; exit 1; }
	debugfs -R "cat /var/lib/dpkg/info/wireless-regdb.md5sums" "$SYSIMG" 2> /dev/null > "$RG.md5"
	RV=$(debugfs -R "cat /var/lib/dpkg/status" "$SYSIMG" 2> /dev/null | awk '/^Package: wireless-regdb$/ {f = 1} f && /^Version:/ {print $2; exit}')
	for f in regulatory.db regulatory.db.p7s; do
		debugfs -R "dump /usr/lib/firmware/$f-upstream $RG/lib/firmware/$f" "$SYSIMG" 2> /dev/null
		m=$(awk -v p="usr/lib/firmware/$f-upstream" '$2 == p {print $1}' "$RG.md5")
		[ -n "$m" ] && [ "$(md5sum < "$RG/lib/firmware/$f" | cut -d' ' -f1)" = "$m" ] || { echo "$f-upstream: not wireless-regdb's" >&2; exit 1; }
		echo "initramfs: lib/firmware/$f = /usr/lib/firmware/$f-upstream of wireless-regdb $RV $(sha256sum < "$RG/lib/firmware/$f" | cut -c1-16) $SYSIMG" >> "$L/firmware.txt"
	done
	chmod 755 "$RG/lib/firmware"; chmod 644 "$RG/lib/firmware/regulatory.db" "$RG/lib/firmware/regulatory.db.p7s"
	touch -d @0 "$RG/lib/firmware/regulatory.db" "$RG/lib/firmware/regulatory.db.p7s" "$RG/lib/firmware"
	(cd "$RG" && printf '%s\n' lib/firmware lib/firmware/regulatory.db lib/firmware/regulatory.db.p7s |
		cpio -o -H newc -R 0:0 --reproducible --quiet) > "$W/regdb.cpio"
	rm -rf "${RG:?}" "$RG.md5"
}
regdb_append() {
	[ -s "$W/regdb.cpio" ] || regdb_cpio
	n=$(stat -c %s "$1")
	head -c $(((4 - n % 4) % 4)) /dev/zero >> "$1"
	cat "$W/regdb.cpio" >> "$1"
	log "initramfs $(basename "$1"): regulatory.db and .p7s appended (at byte $n, padded to $(((n + 3) / 4 * 4)))"
	REGDB_RD=done
}
DEPS= DESC= KVER= POSTINST_EXTRA= POSTINST_SH= REGDB_KERNEL= REGDB_RD=

case $DEV in
s10e)
	# The S10e's device stage (device.sh, read from the running phone 11-12..16; kernel 7.2.15-s10e and its 461
	# modules checked identical to the phone by md5 on 11-16, + updates/exynos9820-decon-kms.ko)
	ST=${S10E_STAGE:?set S10E_STAGE}
	K=${S10E_KIT:?set S10E_KIT}
	(cd "$ST/root" && tar -cf - .) | tar -C "$P" -xpf - --no-same-owner
	(cd "$P/usr/lib/firmware" 2>/dev/null && find . -type f | sed 's|^\./||' | LC_ALL=C sort) > "$L/stage-firmware.txt"
	echo "stage firmware: $(wc -l < "$L/stage-firmware.txt") files from $ST/root: $(tr '\n' ' ' < "$L/stage-firmware.txt")" >> "$L/firmware.txt"
	KVER=7.2.15-s10e
	depmod -b "$P/usr" $KVER 2>> "$L/depmod.txt"
	# S10E_IMAGE: the same kernel built again (7.2.15-s10e, the stage's modules are its own), e.g. with a neutral
	# banner, shipped instead of the stage's
	if [ -n "${S10E_IMAGE:-}" ]; then
		install -m 755 "$S10E_IMAGE" "$P/boot/s10e/Image"
		log "/boot/s10e/Image <- $S10E_IMAGE ($(sha256sum < "$S10E_IMAGE" | cut -c1-16))"
	fi
	cp "$P/boot/s10e/Image" "$P/boot/s10e/Image.prev"		# U-Boot's second menu entry
	REGDB_KERNEL=$P/boot/s10e/Image
	# the initramfs's /init <- s10e/init (11-23, owner: nothing stops the boot to the GUI): Mobian always boots,
	# a boot-attempt marker left by a boot that died (a brown-out) is only logged, a root that does not mount gets
	# e2fsck -fy and then a read-only mount; only U-Boot's Rescue entry (s10e.stay) or a root that cannot be started
	# at all keeps the phone in it. The same initramfs as the 11-23 phone's (cfd2a954); every other member byte for byte.
	python3 -I "$T/replace-init.py" "$P/boot/s10e/initramfs.cpio.gz" \
		8587b5dece736170db39596a131f0584d71836e3188f606ceae941cefdaf384f "$T/s10e/init" "$W/initramfs-s10e.cpio.gz"
	install -m 644 "$W/initramfs-s10e.cpio.gz" "$P/boot/s10e/initramfs.cpio.gz"; rm -f "$W/initramfs-s10e.cpio.gz"
	log "initramfs: /init <- s10e/init ($(sha256sum < "$T/s10e/init" | cut -c1-16))"
	regdb_append "$P/boot/s10e/initramfs.cpio.gz"	# both entries' initramfs (U-Boot passes its file size)
	# the oneshots ordered before gdm wait for ever by default: a hang in one kept the GUI from ever starting
	for u in s10e-firstboot:5min s10e-gpu-load:2min s10e-touch-bind:1min; do
		install -d "$P/etc/systemd/system/${u%%:*}.service.d"
		printf '%s\n' "# Ordered before gdm. A oneshot waits for ever by default, so a hang here would keep the GUI from ever" \
			"# starting; past this it is stopped and the boot goes on (owner 11-23: nothing stops the boot to the GUI)." \
			"[Service]" "TimeoutStartSec=${u#*:}" "TimeoutStopSec=15s" > "$P/etc/systemd/system/${u%%:*}.service.d/boot-not-held.conf"
	done
	printf '%s\n' "# Wanted by sysinit.target, and a oneshot goes inactive when it ends: every later start job that pulls in" \
		'# sysinit.target (nearly every service at boot) started it again, until "start-limit-hit" marked it failed and' \
		"# the system degraded. Run once; a failed run is still retried that way." \
		"[Service]" "RemainAfterExit=yes" > "$P/etc/systemd/system/s10e-touch-bind.service.d/run-once.conf"
	install -d "$P/etc/systemd/system/s10e-arm-reset.service.d"	# the same trap: sysinit.target, oneshot, re-run
	cp "$P/etc/systemd/system/s10e-touch-bind.service.d/run-once.conf" "$P/etc/systemd/system/s10e-arm-reset.service.d/"
	sed -i 's| dyndbg="[^"]*"||' "$P/boot/s10e/bootargs.env"
	# Samsung's firmware from the vendor partition (audio DSP, Wi-Fi, Bluetooth, the video codec): what
	# s10e-firstboot copies on a first boot, bundled here (11-21). Source: a root copy of our S10e (s10e-firstboot's
	# copies from that phone's vendor partition, 2026-10-29).
	YF=${S10E_VENDOR_FW:?set S10E_VENDOR_FW}
	fw "$YF" $(cd "$YF" && ls *_AUDIO_SLSI.bin calliope*.bin calliope*.dt rxse.bin txse*.bin SoundBoosterParam.bin) s5p-mfc-v13.fw \
		brcm/bcm4375B1_murata.hcd brcm/bcm4375B1_semco.hcd brcm/bcm4375B1_semco_sem.hcd brcm/nvram.txt_CS01_semco_b1 \
		brcm/nvram.txt_1rh_es43_b1 brcm/brcmfmac4375b1-pcie.bin brcm/brcmfmac4375b1-pcie.clm_blob
	ln -s bcm4375B1_semco.hcd "$P/usr/lib/firmware/brcm/BCM4362A2.hcd"
	ln -s nvram.txt_CS01_semco_b1 "$P/usr/lib/firmware/brcm/brcmfmac4375b1-pcie.txt"
	[ -z "${S10E_CALLAUDIO_FW:-}" ] || cmp -s "$YF/AP_AUDIO_SLSI.bin" "$S10E_CALLAUDIO_FW/AP_AUDIO_SLSI.bin" || log "NOTE: AP_AUDIO_SLSI.bin differs from kit/callaudio/fw"
	install -D -m 644 "$K/clean/60-s10e-gpu.conf" "$P/etc/environment.d/60-s10e-gpu.conf"
	# Wi-Fi back when its chip drops off PCIe (a brown-out at low battery froze the phone, 11-23): reloads the RC and
	# brcmfmac as s10e-minimal does, under its lock (s10e-minimal comes from the stage)
	install -D -m 755 "$T/s10e/s10e-wifi-revive" "$P/usr/local/sbin/s10e-wifi-revive"
	install -D -m 644 "$T/s10e/s10e-wifi-revive.service" "$P/etc/systemd/system/s10e-wifi-revive.service"
	install -d -m 700 "$P/var/lib/exynos-cp"; install -d "$P/var/lib/s10e"
	identity s10e "Galaxy S10e" Samsung "Galaxy S10e"
	enable exynos-modem.service s10e-sleepd.service s10e-gpu-load.service s10e-touch-bind.service \
		s10e-bt-addr.service s10e-arm-reset.service s10e-boot-ok.service s10e-usb-net.service s10e-usb-watch.service \
		s10e-firstboot.service s10e-dp-audio.service s10e-cec-recheck.timer s10e-wifi-revive.service
	mask sleep.target suspend.target hibernate.target hybrid-sleep.target suspend-then-hibernate.target
	# boot: U-Boot (recovery, and boot for a whole-phone install): the first 4 MiB of v51-pinsleep (the S10e
	# kit's default since 11-1x, r7's zip); the kernel, initramfs and arguments are in /boot/s10e above
	head -c 4194304 "${S10E_UBOOT:?set S10E_UBOOT}" > "$W/boot/uboot.img"
	cp "$K/tools/s10e-logo" "$W/boot/s10e-logo"; mkdir -p "$W/boot/boot-logo"
	cp "$K/boot-logo/logo.jpg" "$K/boot-logo/svb_orange.jpg" "$K/boot-logo/booting_warning.jpg" "$W/boot/boot-logo/"
	DESC="Galaxy S10e (beyond0lte): kernel 7.2.15-s10e in /boot/s10e (U-Boot in recovery boots it), its modules, the vendor firmware, modem, sleep, GPU, audio, camera and sensor settings"
	;;
poco)
	# The POCO X3 NFC as its k22 ROM ran on the phone (11-17 hardware check: cameras, speaker/mic, keyboard,
	# modem, BT, Wi-Fi): stage poco-stage-20261117 (device.sh: the payload's firmware, hexagonrpcd, sensors,
	# UCM, sleep, tuning...), kernel 7.2.15-22surya, the ROM's ESP. Its modules and the units build.sh enabled
	# come from that ROM's own root (vibeos-0.1.1-k22, read-only).
	ST=${POCO_STAGE:?set POCO_STAGE}
	KR=$O/work/poco-k22-root			# mounted read-only by the caller
	KIMG=${POCO_K22_IMG:?set POCO_K22_IMG}
	[ -d "$KR/usr/lib/modules/7.2.15-surya" ] || { echo "mount the k22 root at $KR first" >&2; exit 1; }
	(cd "$ST/root" && tar -cf - .) | tar -C "$P" -xpf - --no-same-owner
	# The stage's /var/lib/hexagonrpc/surya/persist is our POCO's own sensor registry (copied from its persist
	# partition on 10-05, then written by its ADSP: calibration among it): never shipped. The directory goes in
	# empty. On the first boot the postinst seeds it from the registry the payload ships anyway (sensors/registry:
	# Nura's qcom-sm7150 set, byte for byte, as are the 46 sensors/config files) and gives it to fastrpc (made by
	# hexagonrpcd's sysusers.d), the user hexagonrpcd-adsp-sensorspd runs as. Its hexagonrpcd (the PR#27 build)
	# serves sensors/persist, a link to it, read-write; the ADSP crash-loops (audio too) when its registry cannot be
	# written.
	SR=usr/share/qcom/sm7150/Xiaomi/surya/sensors
	rm -rf "${P:?}/var/lib/hexagonrpc/surya/persist" && install -d -m 755 "$P/var/lib/hexagonrpc/surya/persist" || exit 1
	[ "$(readlink "$P/$SR/persist")" = /var/lib/hexagonrpc/surya/persist ] || { echo "$SR/persist: not the link to the state" >&2; exit 1; }
	[ "$(ls "$P/$SR/registry" | wc -l)" -gt 100 ] && [ "$(ls "$P/$SR/config" | wc -l)" -gt 10 ] || { echo "no sensor registry or config in $SR" >&2; exit 1; }
	[ -z "$(find "$P/var" -mindepth 1 ! -type d | head -n 1)" ] || { echo "state left in the stage's /var: $(find "$P/var" -mindepth 1 ! -type d | head -n 3 | tr '\n' ' ')" >&2; exit 1; }
	# the UCM file's header named the build machine's kernel tree; it names the kernel instead
	sed -i 's|^# /home/[^ ]*/surya-kernel:$|# the POCO kernel (7.2.15-surya):|' "$P/usr/share/alsa/ucm2/Xiaomi/surya/HiFi.conf"
	grep -q '^# the POCO kernel (7.2.15-surya):$' "$P/usr/share/alsa/ucm2/Xiaomi/surya/HiFi.conf" || { echo "HiFi.conf: header not changed" >&2; exit 1; }
	POSTINST_SH='# the sensor registry: a copy the ADSP may write, seeded once from the shipped one, never replaced
	S=/var/lib/hexagonrpc/surya/persist
	if getent passwd fastrpc > /dev/null; then
		[ -n "$(ls -A $S 2> /dev/null)" ] || { mkdir -p $S/registry && cp -a /usr/share/qcom/sm7150/Xiaomi/surya/sensors/registry/. $S/registry/; }
		chown -R fastrpc:fastrpc $S
	fi'
	KVER=7.2.15-surya
	rm -rf "$P/usr/lib/modules"; modules $KVER "$KR/usr/lib/modules"
	# the files the k22 root's packages do not own under the stage's firmware are the stage's own (PAYLOAD);
	# Debian's firmware-atheros/-qcom-soc/-qcom-dsp come as their packages (debs/), at the k22 versions
	(cd "$P/usr/lib/firmware" && find . -type f | sed 's|^\./||') > "$L/stage-firmware.txt"
	echo "stage firmware: $(wc -l < "$L/stage-firmware.txt") files from $ST/root (device.sh PAYLOAD)" >> "$L/firmware.txt"
	# what build.sh did in the chroot, as files: the units it enabled (and masked), the kernel hooks diverted
	for u in surya-grow-root.service neo-charge-guard.service neo-gps-unlock.service neo-audio-heal.service \
		hexagonrpcd-resume.service hexagonrpcd-adsp-rootpd.service hexagonrpcd-adsp-sensorspd.service surya-sleepd.service \
		surya-cameras.service surya-call-uplink.service touchboost.service; do
		[ -e "$P/etc/systemd/system/$u" ] || [ -e "$P/usr/lib/systemd/system/$u" ] || [ -e "$P/lib/systemd/system/$u" ] ||
			{ echo "  $u: not in the stage, postinst enables it" >> "$L/build.txt"; POSTINST_EXTRA="$POSTINST_EXTRA $u"; continue; }
		enable "$u"
	done
	mkdir -p "$P/etc/systemd/user/default.target.wants"
	for u in surya-call-controls.service surya-mic-boot.service; do
		[ -e "$P/etc/systemd/user/$u" ] || [ -e "$P/usr/lib/systemd/user/$u" ] || { POSTINST_EXTRA="$POSTINST_EXTRA --global:$u"; continue; }
		systemctl --root="$P" --global enable "$u" >> "$L/enable.txt" 2>&1
	done
	POSTINST_EXTRA="$POSTINST_EXTRA upower.service"
	mask hexagonrpcd.service hexagonrpcd-suspend.service pd-mapper.service systemd-validatefs@.service qbootctl.service \
		droid-juicer.service sleep.target suspend.target hibernate.target hybrid-sleep.target suspend-then-hibernate.target
	install -d "$P/var/lib/hexagonrpc"
	# No kernel or initrd in /boot: the ESP inside "linux" holds them (boot/esp.img, the k22 ROM's). With an
	# initrd.img-7.2.15-surya in /boot, the first boot's firmware packages would have update-initramfs rebuild it
	# ON the phone, where its hooks pull in qcom_q6v5_pas without modem.mbn and the modem stays off; with none,
	# update-initramfs finds nothing to update. surya-esp-update (the kernel hooks) still names the k22 root's
	# UUID: it names the root by its label, as the ESP's entry does, should it ever run (with fsck.repair=yes: below).
	sed -i 's/root=UUID=b2b58066-08bc-40ed-95e7-8da4eac5d02f rw /root=LABEL=vibeos fsck.repair=yes fsck.mode=skip ro /' "$P/usr/local/sbin/surya-esp-update"
	grep -q 'options root=LABEL=vibeos fsck.repair=yes fsck.mode=skip ro ' "$P/usr/local/sbin/surya-esp-update" && ! grep -q b2b58066 "$P/usr/local/sbin/surya-esp-update" ||
		{ echo "surya-esp-update: root= not changed" >&2; exit 1; }
	# The root is checked by systemd-fsck-root, not by the initramfs (the ESP's entry: ro fsck.mode=skip, below);
	# this unit's own credential overrides the command line's skip, and nothing it finds stops the boot
	install -D -m 644 "$T/poco/vibeos-fsck-in-root.conf" "$P/etc/systemd/system/systemd-fsck-root.service.d/vibeos-fsck-in-root.conf"
	[ ! -e "$P/boot" ] || { echo "the POCO stage has a /boot: $(ls "$P/boot")" >&2; exit 1; }
	# U-Boot is in BOOT (since 2026-11-23: VibeOS on the whole phone): every power-on starts VibeOS, and the
	# recovery partition is LineageOS's recovery again. So no restart names recovery any more: the stage's
	# tmpfiles.d line did that at every boot, for U-Boot in recovery (and no "Restart into Android", below)
	[ -f "$P/etc/tmpfiles.d/surya-reboot-linux.conf" ] || { echo "no surya-reboot-linux.conf in the POCO stage" >&2; exit 1; }
	rm -f "${P:?}/etc/tmpfiles.d/surya-reboot-linux.conf"
	! grep -rl -E 'reboot-param|surya-boot-android' "$P/etc" "$P/usr/local" "$P/usr/lib/systemd" 2>/dev/null ||
		{ echo "the POCO stage still names a reboot parameter or the Android restart" >&2; exit 1; }
	identity poco "POCO X3 NFC" Xiaomi "POCO X3 NFC"
	DEPS="gdisk, cloud-guest-utils, hexagonrpcd, rmtfs, qrtr-tools, tqftpserv, qcom-phone-utils"
	# the extra Debian/Mobian packages (k22 versions, fetched by build-system.sh) and the POCO's own builds
	[ -n "${TEST_OUT:-}" ] || cp $O/work/downloads/poco/*.deb "$W/debs/"
	# Debian's firmware packages at the k22 versions: what the archive no longer has (firmware-atheros and
	# firmware-qcom-soc 20260916-1, superseded in sid) is packed again from the k22 root's installed files, every
	# one checked against dpkg's md5sums there (they have no maintainer scripts); a downloaded one must be the
	# k22 version
	for fp in firmware-atheros firmware-qcom-soc firmware-qcom-dsp; do
		kv=$(dpkg-query --admindir="$KR/var/lib/dpkg" -W -f '${Version}' $fp)
		set -- "$W"/debs/${fp}_*.deb
		if [ -f "$1" ]; then
			[ "$(dpkg-deb -f "$1" Version)" = "$kv" ] || { echo "$1: not the k22 version $kv" >&2; exit 1; }
			echo "$fp $kv: Debian's .deb (downloaded, apt-verified)" >> "$L/firmware.txt"
			continue
		fi
		[ -n "${TEST_OUT:-}" ] && continue
		repack $fp
		echo "$fp $kv: packed from the k22 root's installed files ($(wc -l < "$KR/var/lib/dpkg/info/$fp.md5sums") files, md5sums checked)" >> "$L/firmware.txt"
	done
	for f in "$ST"/debs/*.deb; do
		case ${f##*/} in linux-image-*) continue ;; gstreamer1.0-libcamera_*) continue ;; esac	# kernel: files only; gst: system's +surya21
		cp "$f" "$W/debs/"
	done
	# boot: U-Boot for BOOT, made here by poco/mkrecovery.py from LineageOS 22.2 surya 20261102's boot.img (its
	# header and DTB byte for byte, gzip(u-boot.bin) as the kernel) and POCO_UBOOT, the U-Boot build whose recovery
	# image the k22 zip carried (POCOs ran it from recovery since 11-09): checked first, by making that
	# image again, byte for byte, from LineageOS's recovery.img. With it that build's dtbo.img and vbmeta.img (flags
	# 3), which ABL reads beside BOOT. The k22 zip's recovery image is not shipped, only its sum (oldrecovery: the
	# device zip says so when a POCO's recovery partition still holds it). And the k22 ESP, with one change: the
	# kernel finds the root by its label (root=LABEL=vibeos), not the k22 root's UUID
	LZ=${POCO_LINEAGE_ZIP:?set POCO_LINEAGE_ZIP}
	UBB=${POCO_UBOOT:?set POCO_UBOOT}
	AVB=${POCO_AVB:?set POCO_AVB}
	ZK=$(mktemp -d "$O/tmp/k22zip.XXXXXX")
	unzip -q -d "$ZK" "${POCO_K22_ZIP:?set POCO_K22_ZIP}" recovery-uboot.img
	mkdir "$ZK/lin" && unzip -q -d "$ZK/lin" "$LZ" boot.img dtbo.img vbmeta.img recovery.img
	python3 -I "$T/poco/mkrecovery.py" "$ZK/lin/recovery.img" "$UBB" "$ZK/recovery-again.img" "$AVB/avbtool.py" \
		"$AVB/testkey_rsa4096.pem" > "$L/mkrecovery.txt"
	cmp -s "$ZK/recovery-again.img" "$ZK/recovery-uboot.img" ||
		{ echo "$UBB and LineageOS's recovery.img do not make the k22 zip's recovery-uboot.img again" >&2; exit 1; }
	python3 -I "$T/poco/mkrecovery.py" "$ZK/lin/boot.img" "$UBB" "$W/boot/boot.img" "$AVB/avbtool.py" >> "$L/mkrecovery.txt"
	[ "$(od -An -tx1 -j 120 -N 4 "$ZK/lin/vbmeta.img" | tr -d ' \n')" = 00000003 ] ||
		{ echo "LineageOS's vbmeta.img does not have flags 3" >&2; exit 1; }
	cp "$ZK/lin/dtbo.img" "$ZK/lin/vbmeta.img" "$W/boot/"
	echo "oldrecovery $(stat -c %s "$ZK/recovery-uboot.img") $(sha256sum < "$ZK/recovery-uboot.img" | cut -d' ' -f1)" > "$W/boot/oldrecovery.sum"
	log "BOOT U-Boot $(sha256sum < "$W/boot/boot.img" | cut -c1-16) from $(basename "$LZ")'s boot.img and $(sha256sum < "$UBB" | cut -c1-16)"
	rm -rf "${ZK:?}"
	dd if="$KIMG" of="$W/boot/esp.img" bs=4096 skip=2048 count=122880 status=none
	ME=$(mktemp -d "$O/tmp/esp.XXXXXX")
	mcopy -i "$W/boot/esp.img" ::/loader/entries/mobian.conf "$ME/mobian.conf"
	# fsck.repair=yes (2026-11-23: nothing but an unmountable root stops the boot): the initramfs (initramfs-tools'
	# init, the k22 ROM's) checked the root with fsck -a, and where that could not fix it (a journal a brown-out
	# left) it stopped in its shell, "requires a manual fsck". And no check runs there any more (fsck.mode=skip, the
	# root mounted ro): U-Boot leaves the APSS watchdog running and nothing pets it until qcom_wdt loads from the
	# root, so a long fsck in the initramfs would reset the phone mid-run, at every boot. The kernel still replays
	# the journal at the ro mount; systemd-fsck-root then checks the root with fsck -y while PID 1 pets the watchdog
	# (vibeos-fsck-in-root.conf, above) and systemd-remount-fs makes it read-write. The initrd itself stays the k22
	# ROM's, byte for byte (rebuilt on the phone it loses the modem)
	sed 's/root=UUID=[0-9a-f-]* rw /root=LABEL=vibeos fsck.repair=yes fsck.mode=skip ro /' "$ME/mobian.conf" > "$ME/new.conf"
	grep -q 'root=LABEL=vibeos fsck.repair=yes fsck.mode=skip ro ' "$ME/new.conf" || { echo "ESP: no root= to change" >&2; exit 1; }
	mcopy -o -i "$W/boot/esp.img" "$ME/new.conf" ::/loader/entries/mobian.conf
	mcopy -i "$W/boot/esp.img" ::/loader/entries/mobian.conf - | diff "$ME/mobian.conf" - > "$L/esp-entry.diff" || true
	rm -rf "${ME:?}"
	# POCO_ESP_KERNEL: the same kernel built again (7.2.15-surya, its EFI zboot vmlinuz.efi), e.g. with a neutral
	# banner, on the ESP instead of the k22 ROM's. Written over the old file's own clusters, so the FAT, the
	# directory entry and FSInfo stay as they were: it must be the same size, in one run of clusters
	if [ -n "${POCO_ESP_KERNEL:-}" ]; then
		[ "$(mcopy -i "$W/boot/esp.img" ::/vmlinuz-mobian - | wc -c)" = "$(stat -c %s "$POCO_ESP_KERNEL")" ] ||
			{ echo "ESP: $POCO_ESP_KERNEL is not vmlinuz-mobian's size" >&2; exit 1; }
		python3 -I - "$W/boot/esp.img" "$POCO_ESP_KERNEL" "$(mshowfat -i "$W/boot/esp.img" ::/vmlinuz-mobian)" <<-'PYEOF'
		import re, struct, sys
		img, new, chain = sys.argv[1:4]
		m = re.fullmatch(r"\S+ <(\d+)-(\d+)>", chain.strip())
		assert m, "vmlinuz-mobian is not one run of clusters: " + chain
		first, last = int(m[1]), int(m[2])
		with open(img, "r+b") as f:
		    bs = f.read(512)
		    sec, spc, res, nfat = struct.unpack_from("<HBHB", bs, 11)
		    fatsz = struct.unpack_from("<I", bs, 36)[0]
		    clus = sec * spc
		    data = open(new, "rb").read()
		    assert len(data) <= (last - first + 1) * clus
		    f.seek((res + nfat * fatsz) * sec + (first - 2) * clus)
		    f.write(data)
		PYEOF
		mcopy -i "$W/boot/esp.img" ::/vmlinuz-mobian - | cmp -s - "$POCO_ESP_KERNEL" || { echo "ESP: vmlinuz-mobian not replaced" >&2; exit 1; }
		log "ESP vmlinuz-mobian <- $POCO_ESP_KERNEL ($(sha256sum < "$POCO_ESP_KERNEL" | cut -c1-16)), in its own clusters"
	fi
	DESC="POCO X3 NFC (surya): kernel 7.2.15-22surya modules (the kernel and initramfs are on the ESP), the POCO's firmware, hexagonrpcd, sensors, UCM, sleep, cameras and tuning"
	;;
s5e)
	# The Tab S5e: device stage device-20261111-dual (device.sh, read from the tablet 11-11; its firmware from
	# the tablet's vendor/modem partitions), with the tablet's kernel since 11-19 (7.2.15-gts4lv-uc: slot image
	# linux-k11-uc.img e57e0831, modules from that build's modules_install) and the two device files the tablet
	# changed since, from work-20261119/tabk: touchboost.py 4d633e7c, and gts4lv-powerkey as the tablet runs it
	# on 11-24 (5ef44a11: radios left as they are for a sleep since 11-20, the power key passed on to the shell,
	# and only the upside-down portrait swaps the volume keys, as the volume slider is vertical)
	ST=${S5E_STAGE:?set S5E_STAGE}
	TK=${S5E_TABK:?set S5E_TABK}
	[ "$(sha256sum < "$TK/gts4lv-powerkey.20261124" | cut -c1-16)" = 5ef44a117b8b5dda ] ||
		{ echo "$TK/gts4lv-powerkey.20261124: not the tablet's of 11-24 (5ef44a11)" >&2; exit 1; }
	(cd "$ST/root" && tar -cf - .) | tar -C "$P" -xpf - --no-same-owner
	KVER=7.2.15-gts4lv-uc
	rm -rf "$P/usr/lib/modules"; modules $KVER "${S5E_MODULES:?set S5E_MODULES}"
	for f in $(cd "$P" && find . -name touchboost.py | sed 's|^\./||'); do install -m 755 "$TK/touchboost.py" "$P/$f"; log "/$f <- tabk/touchboost.py"; done
	for f in $(cd "$P" && find . -name gts4lv-powerkey -type f | sed 's|^\./||'); do install -m 755 "$TK/gts4lv-powerkey.20261124" "$P/$f"; log "/$f <- tabk/gts4lv-powerkey.20261124"; done
	(cd "$P/usr/lib/firmware" && find . -type f | sed 's|^\./||') > "$L/stage-firmware.txt"
	echo "stage firmware: $(wc -l < "$L/stage-firmware.txt") files from $ST/root (the tablet's vendor/modem partitions, 11-11)" >> "$L/firmware.txt"
	for u in $(sed -e 's/#.*//' "$ST/enable"); do
		[ -e "$P/etc/systemd/system/$u" ] || [ -e "$P/usr/lib/systemd/system/$u" ] || [ -e "$P/lib/systemd/system/$u" ] ||
			{ POSTINST_EXTRA="$POSTINST_EXTRA $u"; continue; }
		enable "$u"
	done
	identity gts4lv "Galaxy Tab S5e" Samsung "Galaxy Tab S5e"
	DEPS="hexagonrpcd, rmtfs, qrtr-tools, tqftpserv"
	[ -n "${TEST_OUT:-}" ] || cp $O/work/downloads/s5e/*.deb "$W/debs/"
	cp "$ST"/debs/*.deb "$W/debs/"
	# boot: the recovery partition as the tablet has it since 11-19 (U-Boot dual + LineageOS recovery in its
	# first 34 MiB, from the 11-19 backup), with linux-k11-uc.img (kernel + initramfs + DT) at 34 MiB; the
	# installer writes the extent list at its end. The kit's extents and keepmisc tools.
	B=$TK/recovery-partition-before-k11-20261119.img
	[ "$(sha256sum < "$B" | cut -c1-16)" = 0ae284819ec38554 ] || { echo "$B: not the 11-19 recovery backup" >&2; exit 1; }
	[ "$(sha256sum < "$TK/linux-k11-uc.img" | cut -c1-16)" = e57e083136ad5cb8 ] || { echo "linux-k11-uc.img: not e57e0831" >&2; exit 1; }
	# the slot image with regulatory.db in its ramdisk (cfg80211 is built in): its kernel must be tabk/Image-uc.gz,
	# and the kit's repack-v2.py must give the image back byte for byte before it changes the ramdisk
	SL=$W/slot; mkdir -p "$SL"
	python3 -I - "$TK/linux-k11-uc.img" "$SL" <<'EOF'
import struct, sys
b = open(sys.argv[1], 'rb').read()
assert b[:8] == b'ANDROID!'
ks, ka, rs = struct.unpack_from('<III', b, 8)
ps, = struct.unpack_from('<I', b, 36)
o = ps + -(-ks // ps) * ps
open(sys.argv[2] + '/kernel', 'wb').write(b[ps:ps + ks])
open(sys.argv[2] + '/ramdisk', 'wb').write(b[o:o + rs])
EOF
	cmp -s "$SL/kernel" "$TK/Image-uc.gz" || { echo "linux-k11-uc.img: its kernel is not tabk/Image-uc.gz" >&2; exit 1; }
	python3 -I "$TK/repack-v2.py" "$TK/linux-k11-uc.img" "$SL/same.img" && cmp -s "$SL/same.img" "$TK/linux-k11-uc.img" ||
		{ echo "repack-v2.py does not give linux-k11-uc.img back" >&2; exit 1; }
	# S5E_KERNEL: the same kernel built again (7.2.15-gts4lv-uc, gzip -9n of its Image), e.g. with a neutral
	# banner, in the slot image instead of tabk/Image-uc.gz
	KG=${S5E_KERNEL:-$TK/Image-uc.gz}
	REGDB_KERNEL=$KG
	# the ramdisk's /init <- s5e/init (11-23): the root shells over USB (telnet, SSH, the ACM serial console) only
	# with gts4lv.debug on the command line, which only U-Boot's rescue entry passes (S5E_UBOOT below). Every other
	# member of the cpio stays byte for byte; rewritten unchanged, the ramdisk must come back byte for byte first.
	python3 -I - "$SL/ramdisk" "$T/s5e/init" <<'EOF'
import subprocess, sys, hashlib, zlib
rd, new = sys.argv[1:3]
raw = open(rd, 'rb').read()
z = zlib.decompressobj(31)
c = z.decompress(raw)
assert z.eof and not z.unused_data, "the ramdisk is not one gzip member"
def members(c):
    i, out = 0, []
    while True:
        h = c[i:i + 110]
        assert h[:6] == b'070701', "not a newc cpio at %d" % i
        f = [int(h[6 + 8 * k:14 + 8 * k], 16) for k in range(13)]
        ns, fs = f[11], f[6]
        name = c[i + 110:i + 110 + ns - 1]
        d0 = (i + 110 + ns + 3) & ~3
        out.append([f, name, c[d0:d0 + fs]])
        i = (d0 + fs + 3) & ~3
        if name == b'TRAILER!!!':
            return out, c[i:]
def cpio(ms):
    b = bytearray()
    for f, name, data in ms:
        f = f[:6] + [len(data)] + f[7:]
        b += b'070701' + b''.join(b'%08X' % x for x in f) + name + b'\0'
        b += b'\0' * (-len(b) % 4) + data
        b += b'\0' * (-len(b) % 4)
    return bytes(b + b'\0' * (-len(b) % 512))
def gz(b):
    return subprocess.run(['gzip', '-9n'], input=b, stdout=subprocess.PIPE, check=True).stdout
ms, rest = members(c)
assert not rest.strip(b'\0') and cpio(ms) == c, "the cpio does not come back byte for byte"
assert gz(c) == raw, "gzip -9n does not give the ramdisk back byte for byte"
init = [m for m in ms if m[1] == b'init']
assert len(init) == 1 and hashlib.sha256(init[0][2]).hexdigest() == \
    '37febcd00efbb64ec2f66df7cdf83b24eb3b0762475b6c412bfd62ec16d05e48', "the ramdisk's /init is not the 11-11 one"
init[0][2] = open(new, 'rb').read()
assert b'gts4lv.debug' in init[0][2]
open(rd, 'wb').write(gz(cpio(ms)))
EOF
	log "initramfs: /init <- s5e/init ($(sha256sum < "$T/s5e/init" | cut -c1-16)): the root shells over USB only with gts4lv.debug"
	regdb_append "$SL/ramdisk"
	python3 -I "$TK/repack-v2.py" "$TK/linux-k11-uc.img" "$SL/linux-k11-uc-regdb.img" --ramdisk "$SL/ramdisk" --kernel "$KG"
	[ -z "${S5E_KERNEL:-}" ] || log "slot image kernel <- $S5E_KERNEL ($(sha256sum < "$S5E_KERNEL" | cut -c1-16))"
	head -c $((34 * 1048576)) "$B" > "$W/boot/recovery.img"
	# S5E_UBOOT: the dual-boot U-Boot built by s5e/build-uboot.sh, whose default environment must be s5e/uboot.env
	# (its rescue entry passes gts4lv.debug), in place of the backup's in the first 4 MiB: the DTB appended to it,
	# the recovery_dtbo and the header stay (as the kit's swap-uboot.py does it); put back in unchanged, the old
	# U-Boot must give the backup's 4 MiB back byte for byte first
	python3 -I - "$W/boot/recovery.img" "${S5E_UBOOT:?set S5E_UBOOT: U-Boot from s5e/build-uboot.sh}" "$T/s5e/uboot.env" <<'EOF'
import gzip, struct, sys, zlib
rec, new, envf = sys.argv[1:4]
PAGE, LIN = 4096, 4 << 20
d = open(rec, 'rb').read()
assert d[:8] == b'ANDROID!'
ks, kaddr, rs, raddr, ss, saddr, taddr, ps, hv = struct.unpack_from('<9I', d, 8)
assert ps == PAGE and hv == 1 and rs == 0 and ss == 0
dtbo_size, dtbo_off = struct.unpack_from('<IQ', d, 1632)
pad = lambda n: n + (-n) % PAGE
z = zlib.decompressobj(31)
old = z.decompress(d[PAGE:PAGE + ks])
dtb = z.unused_data
assert dtb[:4] == b'\xd0\x0d\xfe\xed' and struct.unpack('>I', dtb[4:8])[0] == len(dtb), "no single DTB after U-Boot"
assert dtbo_off == PAGE + pad(ks)
dtbo = d[dtbo_off:dtbo_off + dtbo_size]
def head(u):
    k = gzip.compress(u, 9, mtime=0) + dtb
    h = bytearray(d[:PAGE])
    struct.pack_into('<I', h, 8, len(k))
    struct.pack_into('<Q', h, 1636, PAGE + pad(len(k)))
    img = bytes(h) + k + b'\0' * ((-len(k)) % PAGE) + dtbo + b'\0' * ((-len(dtbo)) % PAGE)
    assert len(img) <= LIN
    return img + b'\0' * (LIN - len(img))
assert head(old) == d[:LIN], "the old U-Boot does not give the first 4 MiB back"
u = open(new, 'rb').read()
env = [l for l in open(envf).read().split('\n') if l and not l.startswith('#')]
assert u.count(b'\0'.join(e.encode() for e in env) + b'\0\0') == 1, "U-Boot's default environment is not s5e/uboot.env"
assert any(e.startswith('linux_rescue=') and 'gts4lv.debug' in e for e in env)
open(rec, 'r+b').write(head(u))
EOF
	log "recovery.img: U-Boot <- $S5E_UBOOT ($(sha256sum < "$S5E_UBOOT" | cut -c1-16)), environment s5e/uboot.env ($(sha256sum < "$T/s5e/uboot.env" | cut -c1-16))"
	cat "$SL/linux-k11-uc-regdb.img" >> "$W/boot/recovery.img"
	log "recovery.img: linux-k11-uc.img with regulatory.db in its ramdisk ($(sha256sum < "$SL/linux-k11-uc-regdb.img" | cut -c1-16))"
	rm -rf "${SL:?}"
	[ $(stat -c %s "$W/boot/recovery.img") -le $((72744960 - 524288)) ] || { echo "recovery.img does not leave 512 KiB for the extents" >&2; exit 1; }
	cp "${S5E_KIT_TOOLS:?set S5E_KIT_TOOLS}/extents" "$S5E_KIT_TOOLS/keepmisc" "$S5E_KIT_TOOLS/dmlinear" "$W/boot/"
	# gdm starts after systemd-user-sessions, which waits for network.target, which waits for NetworkManager: these
	# two oneshots, ordered ahead of them, hold the GUI too
	for u in gts4lv-usbnet gts4lv-wifi-mac; do [ -f "$P/etc/systemd/system/$u.service" ] || { echo "no $u.service in the S5e stage" >&2; exit 1; }; done
	not_held gts4lv-usbnet.service 1min "Ordered before network.target, which gdm waits for (systemd-user-sessions)."
	not_held gts4lv-wifi-mac.service 1min "Ordered before NetworkManager, which gdm waits for (network.target, systemd-user-sessions)."
	DESC="Galaxy Tab S5e (gts4lv): kernel 7.2.15-gts4lv-uc modules (the kernel is in the recovery partition behind U-Boot), the tablet's firmware, modem, audio, touch, power key, USB/OTG, sensors and boot settings"
	;;
s20)
	# The Galaxy S20 FE: BOOT = out/boot-r8s-ub13.img (4a82c5e3; in BOOT since 11-20 19:52 after the #51 check);
	# its modules = the #51 build tree's own (build/, Image = Image-47 beedd019 byte for byte), as modules_install
	# would install them; the r8s settings of kit/rootfs/build-rootfs.sh step 3; the Wi-Fi/BT firmware of
	# kit/wifi-fw (install-radio-fw.sh's set)
	G=${S20_DIR:?set S20_DIR}
	KVER=7.2.15-r8s+
	[ "$(sha256sum < "$G/build/arch/arm64/boot/Image" | cut -c1-16)" = beedd01959941df2 ] || { echo "build/ Image is not #51" >&2; exit 1; }
	REGDB_KERNEL=$G/build/arch/arm64/boot/Image
	MS=$W/modstage/lib/modules/$KVER
	mkdir -p "$MS"
	while read -r o; do
		k=${o%.o}.ko
		install -D -m 644 "$G/build/$k" "$MS/kernel/$k"
		aarch64-linux-gnu-strip --strip-debug "$MS/kernel/$k"
	done < "$G/build/modules.order"
	cp "$G/build/modules.order" "$G/build/modules.builtin" "$G/build/modules.builtin.modinfo" "$MS/"
	sed -i 's/\.o$/.ko/; s|^|kernel/|' "$MS/modules.order"
	modules "$KVER" "$W/modstage/lib/modules"
	fw "$G/kit/wifi-fw/linux-firmware" amss.bin m3.bin board-2.bin Notice.txt
	for f in amss.bin m3.bin board-2.bin Notice.txt; do mkdir -p "$P/usr/lib/firmware/ath11k/QCA6390/hw2.0"; mv "$P/usr/lib/firmware/$f" "$P/usr/lib/firmware/ath11k/QCA6390/hw2.0/$f"; done
	fw "$G/kit/wifi-fw/vendor" htbtfw20.tlv htnv20.bin
	mkdir -p "$P/usr/lib/firmware/qca"; mv "$P/usr/lib/firmware/htbtfw20.tlv" "$P/usr/lib/firmware/htnv20.bin" "$P/usr/lib/firmware/qca/"
	sh "$T/s20-files.sh" "$P"
	enable r8s-usb-net.service r8s-boot-ok.service r8s-touchlog.service r8s-sleepd.service s5123-modem.service
	# OpenSSH on port 22 (the S20's way in: the initramfs dropbear stops at switch_root); root by key only, and no
	# key is in this package (the S20 install script puts the dev PC's in)
	POSTINST_EXTRA="$POSTINST_EXTRA ssh.service"
	# no swap line: since 11-20 the S20's old super is Lineage's again, and its fstab has the root only
	mask console-setup.service keyboard-setup.service exim4.service lm-sensors.service systemd-binfmt.service \
		proc-sys-fs-binfmt_misc.automount proc-sys-fs-binfmt_misc.mount
	identity r8s "Galaxy S20 FE" Samsung "Galaxy S20 FE"
	cp "$G/out/boot-r8s-ub13.img" "$W/boot/boot-r8s-ub13.img"
	[ "$(sha256sum < "$W/boot/boot-r8s-ub13.img" | cut -c1-16)" = 4a82c5e3c5db7ab4 ] || { echo "ub13 is not 4a82c5e3" >&2; exit 1; }
	# the power-on logo, which s20-install-from-pc.sh puts into up_param as the S10e's device zip does: src/s10e-logo
	# (the same tar rewrite; up_param is the S10e's format) and the S20's pictures, logo.jpg being the S10e's swirl
	# at 1080x2400; on the S20 since 11-17 (up_param abf985d0)
	[ "$(od -An -tx1 -j 16 -N 4 "$T/src/s10e-logo" 2>/dev/null | tr -d ' \n')" = 0200b700 ] ||
		{ echo "no static aarch64 $T/src/s10e-logo: make -C $T/src" >&2; exit 1; }
	cp "$T/src/s10e-logo" "$W/boot/s10e-logo"; mkdir -p "$W/boot/boot-logo"
	cp "$G/boot-logo/logo.jpg" "$G/boot-logo/svb_orange.jpg" "$G/boot-logo/booting_warning.jpg" "$W/boot/boot-logo/"
	[ "$(cd "$W/boot/boot-logo" && cat logo.jpg svb_orange.jpg booting_warning.jpg | sha256sum | cut -c1-16)" = e05fc7e1b43ad160 ] ||
		{ echo "$G/boot-logo: not the S20's pictures of 11-17" >&2; exit 1; }
	DESC="Galaxy S20 FE (r8s, G780F): the modules of kernel #51 (7.2.15-r8s+, in BOOT with U-Boot ub13), Wi-Fi and Bluetooth firmware, the modem stack, sleep, the quiet boot, the r8s settings (USB network, watchdog, sensors, UCM, panel cutout, GPU)"
	;;
*) echo "unknown device $DEV" >&2; exit 1 ;;
esac

# the regulatory database (see regdb_cpio): the signers this kernel's cfg80211 trusts, and a built-in cfg80211 must
# have its initramfs carry the database. The S20's initramfs is built into its kernel (BOOT 4a82c5e3, not rebuilt
# here): it boots in the world domain until a kernel with CONFIG_EXTRA_FIRMWARE or a ramdisk in its FIT
CF=$(find "$P/usr/lib/modules/$KVER" -name 'cfg80211.ko*' | head -n 1)
if grep -Eq '(^|/)net/wireless/cfg80211\.ko$' "$P/usr/lib/modules/$KVER/modules.builtin"; then
	RB=built-in; RK=$REGDB_KERNEL
	[ -n "$RK" ] || { echo "cfg80211 is built in: no REGDB_KERNEL for $DEV" >&2; exit 1; }
else
	RB=module; RK=$CF
	[ -n "$RK" ] || { echo "no cfg80211.ko for $KVER" >&2; exit 1; }
fi
RS=$(signers "$RK")
[ "$RS" = upstream ] || { echo "cfg80211 ($RB) trusts the $RS signers: the postinst assumes the upstream ones only" >&2; exit 1; }
log "cfg80211: $RB, trusts the upstream wireless-regdb signers: the postinst selects regulatory.db-upstream"
if [ $RB = built-in ] && [ "$REGDB_RD" != done ]; then
	[ $DEV = s20 ] || { echo "cfg80211 is built in, and no initramfs got regulatory.db" >&2; exit 1; }
	log "NOTE: cfg80211 is built in, and the S20's initramfs is inside its kernel: no regulatory.db at boot (the world domain)"
fi
REGDB_SH='# Wi-Fi: this kernel trusts the upstream wireless-regdb signers only, not Debian'"'"'s re-signed copy
	if [ -e /lib/firmware/regulatory.db-upstream ]; then
		update-alternatives --set regulatory.db /lib/firmware/regulatory.db-upstream || true
	fi'
# a file both this package and a firmware package of debs/ ship would stop dpkg on the first boot ("trying to
# overwrite"): the firmware package keeps it, when the two are the same bytes (a stage copy of a packaged file)
for f in "$W"/debs/firmware-*.deb; do
	[ -f "$f" ] || continue
	FX=$(mktemp -d "$W/fx.XXXXXX"); dpkg-deb -x "$f" "$FX"
	(cd "$FX" && find . ! -type d | sed 's|^\./||') | while IFS= read -r x; do
		[ -e "$P/$x" ] || [ -L "$P/$x" ] || continue
		if cmp -s "$FX/$x" "$P/$x" || { [ -L "$FX/$x" ] && [ "$(readlink "$FX/$x")" = "$(readlink "$P/$x")" ]; }; then
			rm -f "$P/$x"; echo "/$x: left to $(basename "$f")" >> "$L/build.txt"
		else
			echo "/$x: the device's differs from $(basename "$f")" >&2; exit 1
		fi
	done || exit 1
	rm -rf "${FX:?}"
done
# the package
cat > "$P/DEBIAN/control" <<EOF
Package: vibeos-device-$DEV
Version: $V
Architecture: arm64
Maintainer: pulpoff <pashkovsky@gmail.com>
Section: kernel
Priority: optional
${DEPS:+Depends: $DEPS
}Description: VibeOS device layer: ${DESC%%:*}
 $DESC.
 The device zip unpacks these files into the system zip's root file system;
 the first boot registers them (vibeos-device-setup).
EOF
cat > "$P/DEBIAN/postinst" <<EOF
#!/bin/sh
set -e
if [ "\$1" = configure ]; then
	# the device's name, while the system image's generic one is set
	if [ "\$(cat /etc/hostname 2>/dev/null)" = vibeos ]; then
		cp /usr/share/vibeos-device/hostname /etc/hostname
		sed -i "s/^127\.0\.1\.1[[:space:]].*/127.0.1.1\t\$(cat /etc/hostname)/" /etc/hosts
		cp /usr/share/vibeos-device/machine-info /etc/machine-info
	fi
	for u in $POSTINST_EXTRA; do
		case \$u in --global:*) systemctl --global enable "\${u#--global:}" || true ;; *) systemctl enable "\$u" 2>/dev/null || true ;; esac
	done
	depmod -a $KVER || true
	$POSTINST_SH
	$REGDB_SH
	glib-compile-schemas /usr/share/glib-2.0/schemas 2>/dev/null || true
fi
exit 0
EOF
chmod 755 "$P/DEBIAN/postinst"
# vibeos-growroot (the system's, the first boot) is a oneshot, and vibeos-device-setup, ordered before gdm, waits for it
case $DEV in s10e|s5e|poco)
	not_held vibeos-growroot.service 10min "The system's first-boot grow; vibeos-device-setup, ordered before gdm, comes after it." ;;
esac
# /etc/vibeos/android: how the shell's power menu restarts into Android (one line; no file, no menu item):
# reboot=ARG (systemctl reboot --reboot-argument=ARG) or helper=PATH (run with pkexec). The POCO has none since
# 2026-11-23 (VibeOS on the whole phone); its helper until then (surya-boot-android) is in git history
case $DEV in
s10e) A=reboot=android ;;		# S-Boot's normal boot (DT reboot-mode mode-android), LineageOS in boot
s20)  A=reboot=recovery ;;		# its RECOVERY partition holds LineageOS's boot image
s5e)  A=helper=/usr/local/sbin/gts4lv-boot-android	# the stage's, polkit de.gts4lv.boot-android
      [ -x "$P/usr/local/sbin/gts4lv-boot-android" ] && [ -f "$P/usr/share/polkit-1/actions/de.gts4lv.boot-android.policy" ] ||
	{ echo "no gts4lv-boot-android in the S5e stage" >&2; exit 1; } ;;
poco) A= ;;			# U-Boot in BOOT, Android gone or never started (since 2026-11-23): no Android entry
*)    A= ;;
esac
[ -z "$A" ] || { install -d "$P/etc/vibeos"; echo "$A" > "$P/etc/vibeos/android"; chmod 644 "$P/etc/vibeos/android"; log "/etc/vibeos/android: $A"; }
# Files the system image's packages own that this package ships too (the device's plymouthd.conf, the POCO's
# UCM, the S5e's libssc): diverted in preinst, so that dpkg lets this package have them and an upgrade of the
# other package writes its own to PATH.vibeos-distrib. Not renamed: the device zip has already put this
# package's file there. (Removing this package leaves the other's file missing until it is reinstalled.)
SYSFILES=${SYSFILES:-$O/work/system-packaged-files.txt}	# from build-system.sh (packaged-final.txt)
[ -s "$SYSFILES" ] || { echo "no $SYSFILES (the files the system image's packages own)" >&2; exit 1; }
(cd "$P" && find . -path ./DEBIAN -prune -o ! -type d -print | sed 's|^\.||') | LC_ALL=C sort > "$L/package-files.txt"
LC_ALL=C comm -12 "$L/package-files.txt" "$SYSFILES" > "$L/diverted.txt"
! grep -q "[^A-Za-z0-9_./+@:= -]" "$L/diverted.txt" || { echo "a path to divert has odd characters: $L/diverted.txt" >&2; exit 1; }
[ ! -s "$L/diverted.txt" ] || log "diverted from the system's packages: $(sed 's/.*/"&"/' "$L/diverted.txt" | tr '\n' ' ')"
# each path single-quoted in the scripts (the check above leaves no quote in them; the POCO's UCM has a space)
dv() { while IFS= read -r f; do printf "\tdpkg-divert --package vibeos-device-$DEV --no-rename --$1 --divert '%s.vibeos-distrib' '%s' > /dev/null\n" "$f" "$f"; done < "$L/diverted.txt"; }
if [ -s "$L/diverted.txt" ] || [ $DEV = poco ]; then
	{
		printf '#!/bin/sh\nset -e\nif [ "$1" = install ] || [ "$1" = upgrade ]; then\n'
		dv add
		# qcom-phone-utils' kernel hook would repack every kernel into the "boot" partition (LineageOS's):
		# diverted, as the POCO kit's build.sh did, before anything can run it
		[ $DEV != poco ] || printf '\tdpkg-divert --package vibeos-device-poco --local --rename --divert /etc/kernel/zz-qcom-bootimg.off --add /etc/kernel/postinst.d/zz-qcom-bootimg > /dev/null\n'
		printf 'fi\nexit 0\n'
	} > "$P/DEBIAN/preinst"
	chmod 755 "$P/DEBIAN/preinst"
	sh -n "$P/DEBIAN/preinst"
fi
{
	printf '#!/bin/sh\nset -e\nif [ "$1" = remove ] || [ "$1" = abort-install ]; then\n'
	dv remove
	printf '\t:\nfi\n'
	# Debian's choice of wireless-regdb again
	printf 'if [ "$1" = remove ] || [ "$1" = purge ]; then\n\tupdate-alternatives --auto regulatory.db 2> /dev/null || true\nfi\nexit 0\n'
} > "$P/DEBIAN/postrm"
chmod 755 "$P/DEBIAN/postrm"
sh -n "$P/DEBIAN/postrm" && sh -n "$P/DEBIAN/postinst"
# no build machine in a text file: its host name, or the home directory of one of its users (a vendor's own path,
# such as the /home/qca_main/ in Qualcomm's firmware notices, is not this machine's)
H=$(hostname -s)
if grep -rIlwi -e "$H" "$P" | grep .; then echo "the build host's name is in these files" >&2; exit 1; fi
for u in $(ls /home); do
	if grep -rIlF "/home/$u/" "$P" | grep .; then echo "this machine's /home/$u is in these files" >&2; exit 1; fi
done
# no forbidden content (FORBID), and no identifiers baked in
if for w in $FORBID; do find "$P" -iname "*$w*"; done | grep .; then echo "forbidden files in the payload" >&2; exit 1; fi
D=$W/debs/vibeos-device-${DEV}_${V}_arm64.deb
dpkg-deb --root-owner-group -Zxz -b "$P" "$D" > /dev/null
# no path in two of the debs (dpkg -i of them all at once would stop)
for f in "$W"/debs/*.deb; do dpkg-deb -c "$f" | awk '$1 !~ /^d/ { n = $6; sub(/^\./, "", n); print n }'; done | LC_ALL=C sort | uniq -d > "$L/shared-paths.txt"
[ ! -s "$L/shared-paths.txt" ] || { echo "paths in more than one deb ($L/shared-paths.txt): $(head -n 3 "$L/shared-paths.txt" | tr '\n' ' ')" >&2; exit 1; }
# payload.tar: the device package's files, and those of every deb whose files must be there on the first boot
# (firmware): extracted the way dpkg would, owned by root
X=$W/x; rm -rf "$X"; mkdir -p "$X"
dpkg-deb -x "$D" "$X"
for f in "$W"/debs/firmware-*.deb; do [ -f "$f" ] && dpkg-deb -x "$f" "$X" && echo "payload: $(basename "$f")" >> "$L/build.txt"; done
# nothing at the top but real directories of a merged-/usr root: a "lib", "bin" or "sbin" entry would replace the
# root's symlink with a directory when the zip unpacks it
for t in $(cd "$X" && ls -A); do
	case $t in boot|etc|usr|var|opt|root) [ -d "$X/$t" ] && [ ! -L "$X/$t" ] || { echo "payload: /$t is not a directory" >&2; exit 1; } ;;
	*) echo "payload: /$t at the top (only boot etc usr var opt root)" >&2; exit 1 ;; esac
done
(cd "$X" && tar --owner=0 --group=0 --numeric-owner --sort=name -cf "$W/payload.tar" .)
rm -rf "$X"
mkdir -p "$OUT"
mv "$W/debs" "$W/payload.tar" "$W/boot" "$OUT/"
cp "$L/firmware.txt" "$OUT/firmware-sources.txt"
(cd "$OUT" && find . -type f ! -name SHA256SUMS | sort | xargs sha256sum) > "$OUT/SHA256SUMS"
log "done: $OUT ($(du -sh "$OUT" | cut -f1)); deb $(du -h "$OUT/debs/vibeos-device-${DEV}_${V}_arm64.deb" | cut -f1)"
