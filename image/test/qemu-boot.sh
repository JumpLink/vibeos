#!/bin/sh
# qemu-boot.sh SYSTEM.img OUTDIR: boot a COPY of a system image in qemu-system-aarch64 (machine virt, TCG, no GPU
# acceleration) with Debian's arm64 kernel, and keep its journal. A smoke test of the generic image without any
# device: systemd comes up, the first-boot units run (vibeos-growroot, vibeos-device-setup with no device package,
# the SSH and snakeoil keys), GDM starts and logs "user" into the vibeos session (GNOME Shell mobile from
# /opt/gnome-mobile, on virtio-gpu with Mesa's software renderer).
#   - the copy gets linux-image-arm64 from Debian (apt in an arm64 chroot): its kernel and initramfs boot the copy
#   - qemu runs BOOT_WAIT seconds (default 1200), then gets a power-down request (GNOME may hold the power key:
#     after 120 s it is stopped); journald syncs every 15 s in the copy, so the journal read off it is complete
# OUTDIR (new) keeps the copy (qemu.img), console.txt, journal.txt and a summary in result.txt. Needs root (sudo -n),
# qemu-system-aarch64, qemu-user binfmt, the network.
set -eu
SYS=${1:?SYSTEM.img} OUT=${2:?OUTDIR}
WAIT=${BOOT_WAIT:-1200}
[ ! -e "$OUT" ] || { echo "$OUT exists" >&2; exit 1; }
command -v qemu-system-aarch64 > /dev/null || { echo "needs qemu-system-aarch64" >&2; exit 1; }
# its mounts are its own (a private mount namespace, entered as root and left again for this user): a service that
# starts meanwhile copies the host's mounts into a namespace of its own, where an image mounted here would stay
# mounted, and its loop device busy, until that service exits
[ -n "${VIBEOS_TEST_NS:-}" ] || exec sudo -n env VIBEOS_TEST_NS=1 PATH="$PATH" HOME="$HOME" TMPDIR="${TMPDIR:-/tmp}" BOOT_WAIT="$WAIT" \
	unshare --mount --propagation private setpriv --reuid="$(id -u)" --regid="$(id -g)" --init-groups sh "$0" "$@"
mkdir -p "$OUT/mnt"; OUT=$(cd "$OUT" && pwd); M=$OUT/mnt; IMG=$OUT/qemu.img
cleanup() {
	for d in dev/pts dev proc sys run; do sudo -n umount "$M/$d" 2>/dev/null || true; done
	sudo -n umount "$M" 2>/dev/null || true
}
trap cleanup EXIT
cp --sparse=always "$SYS" "$IMG"
sudo -n e2fsck -fy "$IMG" > "$OUT/e2fsck.txt" 2>&1 || [ $? -le 1 ]
truncate -s 16G "$IMG"; sudo -n resize2fs "$IMG" > /dev/null 2>&1

# 1. Debian's kernel into the copy (the image has none: its device package brings one)
sudo -n mount -o loop "$IMG" "$M"
for d in proc sys dev dev/pts run; do sudo -n mkdir -p "$M/$d"; done
sudo -n mount -t proc proc "$M/proc"; sudo -n mount -t sysfs sys "$M/sys"
sudo -n mount --bind /dev "$M/dev"; sudo -n mount --bind /dev/pts "$M/dev/pts"; sudo -n mount -t tmpfs tmpfs "$M/run"
sudo -n mv "$M/etc/resolv.conf" "$M/etc/resolv.conf.qemu-boot" 2>/dev/null || true
sudo -n cp -L /etc/resolv.conf "$M/etc/resolv.conf"
printf '#!/bin/sh\nexit 101\n' | sudo -n tee "$M/usr/sbin/policy-rc.d" > /dev/null; sudo -n chmod 755 "$M/usr/sbin/policy-rc.d"
CH="sudo -n chroot $M /usr/bin/env -i PATH=/usr/sbin:/usr/bin:/sbin:/bin HOME=/root LC_ALL=C.UTF-8 DEBIAN_FRONTEND=noninteractive"
$CH apt-get -q update > "$OUT/apt.txt" 2>&1
$CH apt-get -y -q --no-install-recommends install linux-image-arm64 >> "$OUT/apt.txt" 2>&1 || { tail -20 "$OUT/apt.txt" >&2; exit 1; }
K=$(ls "$M"/boot/vmlinuz-* | tail -n 1); I=$(ls "$M"/boot/initrd.img-* | tail -n 1)
cp "$K" "$OUT/vmlinuz"; cp "$I" "$OUT/initrd.img"; sudo -n chown "$(id -u)" "$OUT/vmlinuz" "$OUT/initrd.img"
sudo -n rm -rf "$M"/var/lib/apt/lists/*_* "$M/usr/sbin/policy-rc.d" "$M/etc/resolv.conf"
sudo -n mv "$M/etc/resolv.conf.qemu-boot" "$M/etc/resolv.conf" 2>/dev/null || true
sudo -n mkdir -p "$M/var/log/journal" "$M/etc/systemd/journald.conf.d"
printf '[Journal]\nStorage=persistent\nSyncIntervalSec=15s\n' | sudo -n tee "$M/etc/systemd/journald.conf.d/90-qemu-boot.conf" > /dev/null
echo "kernel: ${K##*/}" > "$OUT/result.txt"
cleanup

# 2. boot, wait, power down
QMP=$OUT/qmp.sock
qemu-system-aarch64 -M virt -cpu max -smp 4 -m 4096 -display none -serial "file:$OUT/console.txt" \
	-kernel "$OUT/vmlinuz" -initrd "$OUT/initrd.img" -append "root=LABEL=vibeos rw console=ttyAMA0" \
	-drive if=none,file="$IMG",format=raw,id=hd -device virtio-blk-pci,drive=hd \
	-netdev user,id=n -device virtio-net-pci,netdev=n -device virtio-gpu-pci \
	-device qemu-xhci -device usb-kbd -device usb-tablet -qmp "unix:$QMP,server=on,wait=off" &
Q=$!
s=$(date +%s)
while kill -0 $Q 2>/dev/null && [ $(( $(date +%s) - s )) -lt "$WAIT" ]; do sleep 5; done
if kill -0 $Q 2>/dev/null; then
	python3 -I - "$QMP" <<'EOF'
import json, socket, sys
s = socket.socket(socket.AF_UNIX); s.connect(sys.argv[1]); f = s.makefile("rw")
f.readline()
for c in ({"execute": "qmp_capabilities"}, {"execute": "system_powerdown"}):
    f.write(json.dumps(c) + "\n"); f.flush(); f.readline()
EOF
	t=$(date +%s)
	while kill -0 $Q 2>/dev/null && [ $(( $(date +%s) - t )) -lt 120 ]; do sleep 5; done
	kill -0 $Q 2>/dev/null && { echo "no power-down in 120 s (GNOME holds the power key): qemu stopped" >> "$OUT/result.txt"; kill $Q; }
fi
wait $Q 2>/dev/null || true
echo "ran $(( $(date +%s) - s )) s" >> "$OUT/result.txt"

# 3. the journal
sudo -n mount -o loop,ro,noload "$IMG" "$M"
sudo -n journalctl --directory="$M/var/log/journal" -o short-monotonic --no-pager > "$OUT/journal.txt" 2>&1 || true
cleanup
J=$OUT/journal.txt
# the lines a good boot has (a count each): the first-boot units, GDM, the vibeos session's shell from /opt, the home
# screen drawing (vibeos-device-setup is skipped: no device package)
for p in 'Reached target graphical.target' 'Finished vibeos-growroot.service' 'vibeos-device-setup.service.*skipped' \
	'Finished vibeos-ssh-keygen.service' 'Finished vibeos-ssl-cert.service' 'Started gdm.service' \
	'Started org.gnome.Shell@vibeos.service' 'comm="/opt/gnome-mobile/bin/gnome-shell --mode=vibeos"' \
	'vibeos-launcher.*gfx: drawing with' 'core dumped|SIGSEGV|segfault'; do
	printf '%-75s %s\n' "$p" "$(grep -c -E "$p" "$J" || true)" >> "$OUT/result.txt"
done
cat "$OUT/result.txt"
