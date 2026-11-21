#!/bin/bash
# [QEMU_DISK=usb] qemu-boot.sh BOOTIMG KIND [EXTRA-CMDLINE]: the boot image nura-to-vibeos made (Nura's kernel + VibeOS's
# initramfs) booted in QEMU's arm64 "virt" machine against a virtio disk with 4096-byte sectors (as UFS), to see
# the initramfs find the root and start its init:
#   part    a GPT partition named linux: the root (ext4, label vibeos)
#   nested  "linux" carries a GPT of its own: ESP + root (the POCO's layout)
#   image   a partition named userdata (ext4) with /lost+found/vibeos/rootfs.img
#   label   a partition named something else, ext4 labelled vibeos
#   none    no root: the initramfs must say so and wait (the test stops it)
# The root is a stand-in whose /sbin/init prints what it got and powers off. The kernel's device drivers for the
# phone do nothing here; what runs is the kernel's generic part, the initramfs, the root search and switch_root.
set -u
export PATH=$PATH:/usr/sbin:/sbin
N=${NURA_WORK:?set NURA_WORK: a work directory for the tests}
LIB=$(cd "$(dirname "$0")/../lib" && pwd)
BOOT=$(realpath $1) KIND=$2 EXTRA=${3:-}
W=$N/work/qemu-$KIND; rm -rf $W; mkdir -p $W
python3 -I - $BOOT $W $LIB <<'EOF'
import sys, os
sys.path.insert(0, sys.argv[3])
import bootimg, zlib
h = bootimg.parse(open(sys.argv[1], 'rb').read())
k = h['kernel']
if k[:2] == b'\x1f\x8b':
    d = zlib.decompressobj(16 + zlib.MAX_WBITS); d.decompress(k); k = k[:len(k) - len(d.unused_data)]
open(sys.argv[2] + '/kernel', 'wb').write(k)
open(sys.argv[2] + '/ramdisk', 'wb').write(h['ramdisk'])
open(sys.argv[2] + '/cmdline', 'w').write(h['cmdline'].decode())
EOF
# the stand-in root: busybox-static, an init that reports and powers off
BB=$(ls $N/work/debian/debs/busybox-static_*_arm64.deb | tail -n1)
S=$W/stage; mkdir -p $S/usr/bin $S/usr/lib $S/usr/sbin $S/etc/vibeos $S/proc $S/sys $S/dev $S/run $S/tmp
ln -s usr/bin $S/bin; ln -s usr/lib $S/lib; ln -s usr/sbin $S/sbin
dpkg-deb --fsys-tarfile $BB | tar -x -C $W ./usr/bin/busybox && cp $W/usr/bin/busybox $S/usr/bin/busybox
echo 'VERSION=1:0.1.3' > $S/etc/vibeos/system-image
cat > $S/usr/sbin/init <<'EOF'
#!/bin/busybox sh
/bin/busybox --install -s /bin
echo "=== ROOT REACHED: pid $$, / is $(grep ' / ' /proc/mounts | head -n1)"
echo "=== /run: $(ls /run/vibeos 2>/dev/null | tr '\n' ' ')"
echo "=== initramfs log:"; cat /run/vibeos/initramfs.log
echo "=== mounts:"; cat /proc/mounts
echo "=== loop: $(for l in /sys/block/loop*; do [ -e $l/loop/backing_file ] && echo "${l##*/} $(cat $l/loop/backing_file) offset $(cat $l/loop/offset) sizelimit $(cat $l/loop/sizelimit)"; done)"
echo "=== cmdline: $(cat /proc/cmdline)"
echo "=== END"
sync
poweroff -f
EOF
chmod 755 $S/usr/sbin/init
truncate -s 600M $W/root.img
fakeroot mke2fs -q -t ext4 -b 4096 -I 256 -O ^orphan_file,^metadata_csum_seed -L vibeos -E root_owner=0:0 -d $S $W/root.img || exit 1
D=$W/disk.img; truncate -s 3G $D
L=$(sudo -n losetup -f --show --sector-size 4096 $D) || exit 1
trap 'sudo -n umount $W/u 2>/dev/null; sudo -n losetup -d $L 2>/dev/null' EXIT
NAME2=linux; [ $KIND = label ] && NAME2=other
sudo -n sgdisk -Z $L > /dev/null 2>&1
sudo -n sgdisk -n1:0:+16M -c1:misc -n2:0:+1200M -c2:userdata -n3:0:+1200M -c3:$NAME2 $L > /dev/null || exit 1
sudo -n losetup -d $L; L=$(sudo -n losetup -f --show -P --sector-size 4096 $D)
sudo -n udevadm settle; sleep 1
case $KIND in
part|label) sudo -n dd if=$W/root.img of=${L}p3 bs=4M conv=fsync status=none ;;
nested)
	L2=$(sudo -n losetup -f --show --sector-size 4096 ${L}p3)
	printf 'label: gpt\nstart=256, size=30720, type=C12A7328-F81F-11D2-BA4B-00A0C93EC93B, name=esp\nstart=30976, size=153600, type=B921B045-1DF0-41C3-AF44-4C6F280D3FAE, name=root\n' | sudo -n sfdisk -q $L2 > /dev/null
	sudo -n losetup -d $L2
	sudo -n dd if=$W/root.img of=${L}p3 bs=4096 seek=30976 conv=notrunc,fsync status=none ;;
image)
	sudo -n mkfs.ext4 -q -F ${L}p2; mkdir -p $W/u; sudo -n mount ${L}p2 $W/u
	sudo -n mkdir -p $W/u/lost+found/vibeos; sudo -n cp $W/root.img $W/u/lost+found/vibeos/rootfs.img; sudo -n umount $W/u ;;
none) ;;
esac
sudo -n losetup -d $L; trap - EXIT
CMD="$(cat $W/cmdline) console=ttyAMA0 earlycon=pl011,mmio32,0x9000000 $EXTRA"
# QEMU_DISK=usb: the disk on USB (xHCI + usb-storage) for kernels without a virtio transport (the sm7150 one)
case ${QEMU_DISK:-virtio} in
usb) DISKDEV="-device qemu-xhci,id=x -device usb-storage,bus=x.0,drive=d0,logical_block_size=4096,physical_block_size=4096" ;;
*) DISKDEV="-device virtio-blk-device,drive=d0,logical_block_size=4096,physical_block_size=4096" ;;
esac
echo "==== qemu: $(basename $BOOT) kind=$KIND"
echo "  cmdline: $CMD"
timeout ${QEMU_TIMEOUT:-420} qemu-system-aarch64 -M virt,gic-version=3 -cpu max -smp 2 -m 1536 -nographic -no-reboot \
	-kernel $W/kernel -initrd $W/ramdisk -append "$CMD" \
	-drive if=none,file=$D,format=raw,id=d0 $DISKDEV \
	< /dev/null > $W/console.log 2>&1
echo "  qemu exit $? ($(wc -l < $W/console.log) console lines)"
grep -a -E 'vibeos-initramfs|VibeOS:|=== ' $W/console.log | head -n 60 | sed 's/^/  | /'
case $KIND in
none) grep -a -q 'no VibeOS root' $W/console.log && echo "  ok: no root, said so" || { echo "  FAIL: no failure message"; exit 1; } ;;
*) grep -a -q '=== ROOT REACHED' $W/console.log && echo "  ok: the root's init ran" || { echo "  FAIL: the root's init did not run"; tail -n 30 $W/console.log; exit 1; } ;;
esac
