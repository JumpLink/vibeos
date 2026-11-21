#!/usr/bin/python3 -I
"""compare-boot.py NEW.img [NURA-boot.img | --vmlinuz VMLINUZ --dtb DTB]: the boot image nura-to-vibeos made, unpacked
and set against Nura's: every header field, the kernel and its DTB(s) byte for byte, the command line (what was
dropped); then the new initramfs listed (busybox, init, modules, firmware) and every module in it checked for the
kernel's vermagic. Reads files only."""
import gzip, os, re, stat, struct, subprocess, sys, zlib
sys.path.insert(0, os.path.join(os.path.dirname(os.path.realpath(__file__)), '..', 'lib'))
import bootimg, cpio

a = sys.argv[1:]
new = bootimg.parse(open(a[0], 'rb').read())
fails = []


def ok(c, msg):
    print('  %s: %s' % ('ok' if c else 'FAIL', msg))
    if not c:
        fails.append(msg)


FIELDS = ('header_version', 'page_size', 'kernel_addr', 'ramdisk_addr', 'second_addr', 'tags_addr', 'os_version', 'name',
          'dtb_addr')
print('new: %s' % os.path.basename(a[0]))
print('  header v%d page %d kernel 0x%x ramdisk 0x%x second 0x%x tags 0x%x os_version 0x%x%s' % (
    new['header_version'], new['page_size'], new['kernel_addr'], new['ramdisk_addr'], new['second_addr'],
    new['tags_addr'], new['os_version'], (' dtb 0x%x' % new['dtb_addr']) if 'dtb_addr' in new else ''))
fmt, img, clen, dtbs = bootimg.split_kernel(new['kernel'])
kver = bootimg.linux_version(img)
print('  kernel %s %d bytes (Image %d bytes), Linux %s; appended DTBs %d, dtb section %d bytes, recovery_dtbo %d, second %d' % (
    fmt, len(new['kernel']), len(img or b''), kver, len(dtbs), len(new.get('dtb') or b''), len(new.get('recovery_dtbo') or b''),
    len(new.get('second') or b'')))
for d in dtbs + ([new['dtb']] if new.get('dtb') else []):
    print('    DTB: %s %s' % bootimg.dtb_model(d))
print('  cmdline: %s' % new['cmdline'].decode())
if len(a) >= 2 and a[1] != '--vmlinuz':
    old = bootimg.parse(open(a[1], 'rb').read())
    print('Nura: %s' % os.path.basename(a[1]))
    for f in FIELDS:
        if f in old or f in new:
            ok(old.get(f) == new.get(f), '%s %s' % (f, old.get(f) if f == 'name' else hex(old.get(f, 0))))
    for f in ('kernel', 'second', 'recovery_dtbo', 'dtb'):
        ok((old.get(f) or b'') == (new.get(f) or b''), '%s section byte for byte (%d bytes)' % (f, len(old.get(f) or b'')))
    oc, nc = old['cmdline'].decode().split(), new['cmdline'].decode().split()
    ok(all(t in oc for t in nc), 'every new cmdline word is Nura\'s')
    dropped = [t for t in oc if t not in nc]
    ok(all(t.startswith('pmos_') for t in dropped), 'dropped only pmOS\'s: %s' % ' '.join(dropped))
    ok(old['ramdisk'] != new['ramdisk'], 'ramdisk replaced (Nura\'s %d bytes, ours %d)' % (len(old['ramdisk']), len(new['ramdisk'])))
elif len(a) >= 5:
    v = open(a[2], 'rb').read(); dtb = open(a[4], 'rb').read()
    if v[:2] == b'MZ' and v[4:8] == b'zimg':
        off, size = struct.unpack_from('<II', v, 8)
        comp = v[24:56].split(b'\0')[0].decode()
        vimg = subprocess.run(['zstd', '-dc'] if comp == 'zstd' else ['gzip', '-dc'], input=v[off:off + size],
                              capture_output=True, check=True).stdout
    elif v[:2] == b'\x1f\x8b':
        vimg = gzip.decompress(v)
    else:
        vimg = v
    print('Nura: %s (%d bytes) + %s' % (os.path.basename(a[2]), len(v), os.path.basename(a[4])))
    ok(img == vimg, 'the kernel Image is vmlinuz\'s byte for byte (%d bytes)' % len(vimg))
    ok(dtb in dtbs or new.get('dtb') == dtb, 'the DTB is %s byte for byte' % os.path.basename(a[4]))
# the initramfs
rd = new['ramdisk']
if rd[:4] == b'\x28\xb5\x2f\xfd':
    comp, raw = 'zstd', subprocess.run(['zstd', '-dcq'], input=rd, capture_output=True, check=True).stdout
elif rd[:2] == b'\x1f\x8b':
    comp, raw = 'gzip', gzip.decompress(rd)
else:
    import lzma
    comp, raw = 'xz', lzma.decompress(rd)
ents = cpio.read(raw)
names = {n.lstrip('./'): (m, b) for n, m, b in ents}
mods = sorted(n for n in names if n.endswith('.ko'))
fw = sorted(n for n in names if n.startswith('lib/firmware/') and stat.S_ISREG(names[n][0]))
print('initramfs: %s, %d bytes (%d unpacked), %d entries' % (comp, len(rd), len(raw), len(ents)))
for n in ('init', 'bin/busybox', 'bin/vibeos-loop', 'etc/mdev.conf', 'etc/vibeos-initramfs.conf', 'dev/console',
          'lib/modules/initramfs.load'):
    m = names.get(n)
    ok(m is not None, '%-28s %s' % (n, ('%o %d bytes' % (m[0], len(m[1]))) if m else 'missing'))
conf = names.get('etc/vibeos-initramfs.conf', (0, b''))[1].decode()
print('  conf: %s' % ' '.join(conf.split()))
ok(('KVER=%s' % kver) in conf, 'the initramfs is for kernel %s' % kver)
print('  early load: %s' % ' '.join(names.get('lib/modules/initramfs.load', (0, b''))[1].decode().split()))
print('  modules (%d): %s' % (len(mods), ' '.join(os.path.basename(m)[:-3] for m in mods)))
print('  firmware (%d): %s' % (len(fw), ' '.join(f[len('lib/firmware/'):] for f in fw)))
bad = []
for m in mods:
    b = names[m][1]
    i = b.find(b'vermagic=')
    vm = b[i + 9:b.index(b'\0', i)].decode() if i >= 0 else ''
    if vm.split()[:1] != [kver]:
        bad.append((m, vm))
ok(not bad, 'vermagic of all %d initramfs modules is %s%s' % (len(mods), kver, (': ' + str(bad[:3])) if bad else ''))
deps = names.get('lib/modules/%s/modules.dep' % kver, (0, b''))[1].decode()
missing = [x for l in deps.splitlines() for x in l.replace(':', ' ').split() if 'lib/modules/%s/%s' % (kver, x) not in names]
ok(deps and not missing, 'modules.dep: every module and dependency is in the initramfs%s' % ((' (missing %s)' % missing[:4]) if missing else ''))
bb = names.get('bin/busybox', (0, b''))[1]
ok(bb[:4] == b'\x7fELF' and bb[18:20] == b'\xb7\x00' and b'/lib/ld-linux' not in bb[:4096],
   'busybox: static arm64 ELF')
print('== %d failures' % len(fails))
sys.exit(1 if fails else 0)
