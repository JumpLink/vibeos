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
# r8s-touchlog: copied out of build-rootfs.sh itself, so it stays byte for byte the S20's
K=$G/kit/rootfs/build-rootfs.sh
install -d "$P/usr/local/sbin"
sed -n "/^cat > \"\$M\/usr\/local\/sbin\/r8s-touchlog\" <<'EOF'\$/,/^EOF\$/p" "$K" | sed '1d;$d' > "$P/usr/local/sbin/r8s-touchlog"
chmod 755 "$P/usr/local/sbin/r8s-touchlog"
head -n 1 "$P/usr/local/sbin/r8s-touchlog" | grep -q '^#!/usr/bin/python3' || { echo "r8s-touchlog not extracted" >&2; exit 1; }
sed -n "/^cat > \"\$M\/etc\/systemd\/system\/r8s-touchlog.service\" <<'EOF'\$/,/^EOF\$/p" "$K" | sed '1d;$d' > "$P/etc/systemd/system/r8s-touchlog.service"
grep -q '^ExecStart=/usr/local/sbin/r8s-touchlog' "$P/etc/systemd/system/r8s-touchlog.service" || { echo "r8s-touchlog.service not extracted" >&2; exit 1; }
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
# r8s-sleepd (gnome/device/r8s-sleep, on the phone since 11-20 11:30): sleep 5 s after the screen goes off; its unit
# runs only on a kernel with the WFI-s2idle state (#49 and later; #51 has it)
SL=${S20_SLEEP:?set S20_SLEEP}
install -D -m 755 "$SL/r8s-sleepd.arm64" "$P/usr/local/sbin/r8s-sleepd"
install -D -m 644 "$SL/r8s-sleepd.service" "$P/etc/systemd/system/r8s-sleepd.service"
install -D -m 644 "$SL/r8s-sleepd.conf" "$P/etc/r8s-sleepd.conf"
install -D -m 644 "$SL/r8s-mem-sleep.conf" "$P/etc/tmpfiles.d/r8s-mem-sleep.conf"
install -D -m 644 "$SL/README.md" "$P/usr/local/share/doc/r8s-sleep/README.md"
# the S5123 modem stack (G780F/kit/modem, install-modem.sh's set; s5123-cp.ko is in the modules above). It boots
# the CP from the radio partition, read only, with the phone's own NV copy in /root/modem-private, which is that
# phone's private data and never in this package: without it the unit does not start (ConditionPathExists).
M5=$G/kit/modem
for f in cp-boot.sh cp-gpio.sh cp-ipc cp-spiboot.py gpio-line.py cpd.py; do
	install -D -m 644 "$M5/tools/$f" "$P/usr/local/lib/s5123-cp/$f"
done
chmod 755 "$P/usr/local/lib/s5123-cp/cp-boot.sh" "$P/usr/local/lib/s5123-cp/cp-gpio.sh" "$P/usr/local/lib/s5123-cp/cp-ipc"
install -D -m 644 "$M5/udev/78-s5123-cp-net.rules" "$P/etc/udev/rules.d/78-s5123-cp-net.rules"
install -D -m 644 "$M5/systemd/s5123-modem.service" "$P/etc/systemd/system/s5123-modem.service"
