#!/bin/sh
# s20-files.sh PKG: the Galaxy S20 FE's settings into the package tree PKG: what G780F/kit/rootfs/build-rootfs.sh
# step 3 writes into the S20's root (the r8s parts), and the S10e-layer files that root kept from its S10e base
# (zram, hang panic, libcamera soft ISP, the splash, the flashlight rule). Same content, as files of a package;
# the fstab swap line and the hostname come from the package's postinst.
set -eu
P=${1:?PKG}
S10E=${S10E_STAGE:?set S10E_STAGE}/root
G=${S20_DIR:?set S20_DIR}
for f in etc/plymouth/plymouthd.conf etc/environment.d/60-libcamera-softisp.conf etc/sysctl.d/60-zram-ui.conf \
	etc/sysctl.d/60-hang-panic.conf etc/systemd/zram-generator.conf etc/udev/rules.d/99-flashlight.rules \
	usr/share/plymouth/themes/debian-swirl/debian-swirl.script usr/share/plymouth/themes/debian-swirl/debian-swirl.plymouth \
	usr/share/plymouth/themes/debian-swirl/boot.png; do
	install -D -m 644 "$S10E/$f" "$P/$f"
done
install -D -m 644 "$S10E/usr/share/glib-2.0/schemas/90-s10e.gschema.override" "$P/usr/share/glib-2.0/schemas/90-r8s.gschema.override"
install -D -m 644 /dev/stdin "$P/etc/sysctl.d/61-r8s-hang.conf" <<'EOF'
# /etc/sysctl.d/61-r8s-hang.conf (r8s, after 60-hang-panic.conf)
# No pseudo-NMI on this kernel, so a CPU stuck with interrupts off never answers the all-CPU backtrace, and the
# 10 s wait for it only delays the panic dump.
kernel.hardlockup_all_cpu_backtrace = 0
EOF
install -D -m 644 /dev/stdin "$P/etc/NetworkManager/conf.d/r8s-usb.conf" <<'EOF'
# r8s: usb0/usb1 belong to r8s-usb-net.service (the initramfs gadget).
[keyfile]
unmanaged-devices=interface-name:usb0;interface-name:usb1
EOF
install -D -m 644 /dev/stdin "$P/etc/systemd/system/r8s-usb-net.service" <<'EOF'
[Unit]
Description=r8s: USB network on the gadget the initramfs left up (172.16.43.1)
BindsTo=sys-subsystem-net-devices-usb0.device
After=sys-subsystem-net-devices-usb0.device

[Service]
Type=oneshot
RemainAfterExit=yes
ExecStart=/usr/sbin/ip link set usb0 up
ExecStart=-/usr/sbin/ip addr add 172.16.43.1/24 dev usb0

[Install]
WantedBy=sys-subsystem-net-devices-usb0.device
EOF
install -D -m 644 /dev/stdin "$P/etc/systemd/system/r8s-boot-ok.service" <<'EOF'
[Unit]
Description=r8s: this boot came up, clear the boot counter (PMU INFORM 0x1586085c; at 3 the shim goes to recovery)
After=multi-user.target

[Service]
Type=oneshot
ExecStart=/usr/bin/busybox devmem 0x1586085c 32 0x52380000

[Install]
WantedBy=multi-user.target
EOF
# copied out of build-rootfs.sh itself, so they stay byte for byte the S20's. Not its r8s-touchlog (every touch,
# with its place on the screen, to a log: never in a public image, 2026-11-24)
K=$G/kit/rootfs/build-rootfs.sh
for x in 61-r8s-partlabel.rules 61-r8s-sensors.rules; do
	sed -n "/^cat > \"\$M\/etc\/udev\/rules.d\/$x\" <<'EOF'\$/,/^EOF\$/p" "$K" | sed '1d;$d' | install -D -m 644 /dev/stdin "$P/etc/udev/rules.d/$x"
	[ -s "$P/etc/udev/rules.d/$x" ] || { echo "$x not extracted" >&2; exit 1; }
done
install -D -m 644 /dev/stdin "$P/etc/systemd/system.conf.d/90-r8s-watchdog.conf" <<'EOF'
# r8s: systemd feeds cluster0's watchdog (10050000), the one S-Boot's shim and U-Boot arm
[Manager]
RuntimeWatchdogSec=60s
RebootWatchdogSec=off
WatchdogDevice=/dev/watchdog-r8s
EOF
echo 'SUBSYSTEM=="watchdog", KERNEL=="watchdog[0-9]*", DEVPATH=="*/10050000.watchdog/*", SYMLINK+="watchdog-r8s"' |
	install -D -m 644 /dev/stdin "$P/etc/udev/rules.d/90-r8s-watchdog.rules"
install -D -m 644 /dev/stdin "$P/etc/environment.d/60-r8s-gpu.conf" <<'EOF'
# GTK 4 draws with OpenGL ES on the Mali-G77 (Panfrost). Mesa's Vulkan driver does not cover this Valhall
# generation, so GTK's preferred Vulkan renderer would run on the CPU (lavapipe).
GSK_RENDERER=ngl
EOF
install -D -m 644 /dev/stdin "$P/etc/gmobile/display-panels/samsung,r8s.json" <<'EOF'
{
  "name": "Samsung Galaxy S20 FE",
  "x-res": 1080,
  "y-res": 2400,
  "cutouts": [
    {
      "name": "camera",
      "path": "M 480,0 H 600 V 120 H 480 Z"
    }
  ]
}
EOF
for x in r8s.conf HiFi.conf; do
	sed -n "/^cat > \"\$M\/usr\/share\/alsa\/ucm2\/Samsung\/r8s\/$x\" <<'EOF'\$/,/^EOF\$/p" "$K" | sed '1d;$d' | install -D -m 644 /dev/stdin "$P/usr/share/alsa/ucm2/Samsung/r8s/$x"
	[ -s "$P/usr/share/alsa/ucm2/Samsung/r8s/$x" ] || { echo "UCM $x not extracted" >&2; exit 1; }
done
mkdir -p "$P/usr/share/alsa/ucm2/conf.d/S20FE"
ln -sfn ../../Samsung/r8s/r8s.conf "$P/usr/share/alsa/ucm2/conf.d/S20FE/S20FE.conf"
# the quiet boot (G780F/quietboot/rootfs, on the phone since 11-19): the splash at the S20's 1080x2400, and
# r8s-reached.service, which stands the initramfs's 10-minute dead man's switch down once GDM is up (without it
# every VibeOS boot reboots itself after 10 minutes)
Q=$G/quietboot/rootfs
(cd "$Q" && tar -cf - .) | tar -C "$P" -xpf - --no-same-owner
[ -L "$P/etc/systemd/system/graphical.target.wants/r8s-reached.service" ] || { echo "r8s-reached not enabled" >&2; exit 1; }
# r8s-sleepd (on the phone since 11-20 11:30): sleep 5 s after the screen goes off; its unit
# runs only on a kernel with the WFI-s2idle state (#49 and later; #51 has it)
SL=${S20_SLEEP:?set S20_SLEEP}
install -D -m 755 "$SL/r8s-sleepd.arm64" "$P/usr/local/sbin/r8s-sleepd"
install -D -m 644 "$SL/r8s-sleepd.service" "$P/etc/systemd/system/r8s-sleepd.service"
install -D -m 644 "$SL/r8s-sleepd.conf" "$P/etc/r8s-sleepd.conf"
install -D -m 644 "$SL/r8s-mem-sleep.conf" "$P/etc/tmpfiles.d/r8s-mem-sleep.conf"
# (not its README.md: it names the machine it was built on)
# the S5123 modem stack (G780F/kit/modem, install-modem.sh's set; s5123-cp.ko is in the modules above). It boots
# the CP from the radio partition, read only, with the phone's own NV copy in /root/modem-private, which is that
# phone's private data and never in this package: r8s-efs-import makes it from the phone's own EFS (below), and
# without it the unit does not start (ConditionPathExists).
M5=$G/kit/modem
for f in cp-boot.sh cp-gpio.sh cp-ipc cp-spiboot.py gpio-line.py cpd.py; do
	install -D -m 644 "$M5/tools/$f" "$P/usr/local/lib/s5123-cp/$f"
done
chmod 755 "$P/usr/local/lib/s5123-cp/cp-boot.sh" "$P/usr/local/lib/s5123-cp/cp-gpio.sh" "$P/usr/local/lib/s5123-cp/cp-ipc"
install -D -m 644 "$M5/udev/78-s5123-cp-net.rules" "$P/etc/udev/rules.d/78-s5123-cp-net.rules"
install -D -m 644 "$M5/systemd/s5123-modem.service" "$P/etc/systemd/system/s5123-modem.service"
# r8s-efs-import: the phone's own modem NV data and Wi-Fi MAC, read from its EFS partition, never written
install -D -m 755 /dev/stdin "$P/usr/local/sbin/r8s-efs-import" <<'EOF'
#!/bin/sh
# r8s-efs-import: this phone's own modem NV data and Wi-Fi MAC, from its EFS partition (Samsung's, where Android
# keeps them). EFS is never mounted and never written: its node is set read-only first, and debugfs only reads.
#   NV   /nv_data.bin, once: to /root/modem-private/nv_data.bin (0600), the copy s5123-modem boots the modem with
#        and cpd keeps its NV writes in; used only when EFS's nv_data.bin.md5 (MD5 of the data and
#        "Samsung_Android_RIL") vouches for it. Without it the modem unit does not start.
#   MAC  /wifi/.mac.info, every boot: NetworkManager's cloned MAC for the Wi-Fi (in /run: nothing of it is kept)
# Prints neither: one line when one of them is missing, at most.
set -u
EFS=/dev/disk/by-partlabel/efs
[ -b $EFS ] || { echo "no EFS partition" >&2; exit 0; }
blockdev --setro "$(readlink -f $EFS)" 2> /dev/null || true
P=/root/modem-private
if [ ! -s $P/nv_data.bin ]; then
	umask 077
	mkdir -p $P
	N=$P/.nv_data.bin.new
	rm -f $N
	debugfs -R "dump /nv_data.bin $N" $EFS > /dev/null 2>&1
	W=$(debugfs -R "cat /nv_data.bin.md5" $EFS 2> /dev/null | tr -d '\n\r ' | tr A-F a-f)
	H=$( { cat $N 2> /dev/null; printf Samsung_Android_RIL; } | md5sum | cut -c1-32)
	if [ -s $N ] && [ ${#W} = 32 ] && [ "$H" = "$W" ]; then
		mv $N $P/nv_data.bin
	else
		rm -f $N; echo "no modem NV data in EFS that its checksum vouches for: the modem stays off" >&2
	fi
fi
MAC=$(debugfs -R "cat /wifi/.mac.info" $EFS 2> /dev/null | tr -d '\n\r ' | tr A-F a-f)
if echo "$MAC" | grep -Eq '^([0-9a-f]{2}:){5}[0-9a-f]{2}$'; then
	mkdir -p /run/NetworkManager/conf.d
	printf '[connection-r8s-wifi-mac]\nmatch-device=driver:ath11k_pci\nwifi.cloned-mac-address=%s\n' "$MAC" \
		> /run/NetworkManager/conf.d/90-r8s-wifi-mac.conf
else
	echo "no Wi-Fi MAC in EFS: the chip's own" >&2
fi
exit 0
EOF
install -D -m 644 /dev/stdin "$P/etc/systemd/system/r8s-efs-import.service" <<'EOF'
[Unit]
Description=r8s: this phone's own modem NV data and Wi-Fi MAC, read from EFS (never written)
Before=s5123-modem.service NetworkManager.service
After=local-fs.target
Wants=dev-disk-by\x2dpartlabel-efs.device
After=dev-disk-by\x2dpartlabel-efs.device

[Service]
Type=oneshot
ExecStart=/usr/local/sbin/r8s-efs-import

[Install]
WantedBy=multi-user.target NetworkManager.service
EOF
sh -n "$P/usr/local/sbin/r8s-efs-import"
