#!/usr/bin/python3 -I
"""recovery-chroot.py RECOVERY.img DIR: a LineageOS recovery image's ramdisk unpacked into DIR (new), for running
a zip's update-binary under qemu-user as recovery would. Paths are kept inside DIR; nothing is executed."""
import gzip, os, stat, subprocess, sys
sys.path.insert(0, os.path.join(os.path.dirname(os.path.realpath(__file__)), '..', 'lib'))
import bootimg, cpio

img, R = sys.argv[1], os.path.abspath(sys.argv[2])
h = bootimg.parse(open(img, 'rb').read())
rd = h['ramdisk']
if rd[:2] == b'\x1f\x8b':
    rd = gzip.decompress(rd)
elif rd[:4] == b'\x02\x21\x4c\x18':
    rd = subprocess.run(['lz4', '-dc'], input=rd, capture_output=True, check=True).stdout
os.makedirs(R)
n = 0
for name, mode, body in cpio.read(rd):
    p = os.path.normpath(os.path.join(R, name.lstrip('/')))
    if not (p == R or p.startswith(R + '/')):
        continue
    t = stat.S_IFMT(mode)
    if t == stat.S_IFDIR:
        os.makedirs(p, exist_ok=True); os.chmod(p, 0o755)
    elif t == stat.S_IFLNK:
        os.makedirs(os.path.dirname(p), exist_ok=True)
        if os.path.lexists(p):
            os.unlink(p)
        os.symlink(body.decode(), p)
    elif t == stat.S_IFREG:
        os.makedirs(os.path.dirname(p), exist_ok=True)
        open(p, 'wb').write(body); os.chmod(p, (mode & 0o755) | 0o600); n += 1
print('%s: %d files from %s' % (R, n, os.path.basename(img)))
