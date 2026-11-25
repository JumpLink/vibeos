"""s20/check-boot.py BOOT [IMAGE] [INIT]: what the Galaxy S20 FE's boot image BOOT starts, against what a public
image must start. BOOT is an Android boot image (header v2) whose kernel is the shim with U-Boot and whose ramdisk is a
FIT of the kernel (Image.gz) and its devicetree, which U-Boot boots (bootm ${prevbl_initrd_start_addr}).
  U-Boot  its default environment: r8s_args (the normal entry) name the partition vibeos and carry no r8s.debug;
          r8s_args_rescue ("Rescue shell (USB)") carry r8s.debug
  kernel  the FIT's kernel, decompressed, is IMAGE byte for byte (the build whose modules the zip carries)
  initramfs  the one built into that kernel (a gzip'd newc cpio): its /init is INIT byte for byte (image/s20/init),
          and it holds no .ssh directory, no authorized_keys and no key file, and the regulatory database (the built-in
          cfg80211 reads it before the root is there); every file in it is root's (0:0: a tree owned by the user who
          built it needs CONFIG_INITRAMFS_ROOT_UID/GID set to that user's ids)
Prints one line per check; exit 1 when one fails. Reads only."""
import gzip, struct, sys, zlib

boot = open(sys.argv[1], 'rb').read()
image = open(sys.argv[2], 'rb').read() if len(sys.argv) > 2 and sys.argv[2] else None
init = open(sys.argv[3], 'rb').read() if len(sys.argv) > 3 and sys.argv[3] else None
fails = 0


def say(ok, what):
    global fails
    print(('ok: ' if ok else 'FAIL: ') + what)
    fails += not ok


if boot[:8] != b'ANDROID!':
    sys.exit('FAIL: %s is not an Android boot image' % sys.argv[1])
ks, ka, rs, ra, ss, sa, ta, ps, hv = struct.unpack_from('<9I', boot, 8)
pg = lambda n: (n + ps - 1) // ps * ps
kernel = boot[ps:ps + ks]
ramdisk = boot[ps + pg(ks):ps + pg(ks) + rs]

# U-Boot's default environment: NUL-separated name=value, ending in two NULs
i = kernel.find(b'\0bootcmd=')
env = {}
if i >= 0:
    s = kernel.rfind(b'\0\0', 0, i) + 2
    for v in kernel[s:kernel.find(b'\0\0', i + 1)].split(b'\0'):
        if b'=' in v:
            k, _, val = v.partition(b'=')
            env.setdefault(k.decode('latin-1'), val.decode('latin-1'))
say('r8s_args' in env and 'r8s_args_rescue' in env, "U-Boot's environment has r8s_args and r8s_args_rescue")
a, r = env.get('r8s_args', '').split(), env.get('r8s_args_rescue', '').split()
say('r8s.root=vibeos' in a and 'r8s.debug' not in a, 'the normal entry: r8s.root=vibeos, no r8s.debug')
say('r8s.debug' in r, 'the rescue entry: r8s.debug')


# the FIT: the /images/kernel node's data
def fdt_nodes(b):
    magic, total, off_s, off_str = struct.unpack_from('>IIII', b, 0)
    assert magic == 0xd00dfeed, 'no FIT'
    path, props, out, j = [], {}, {}, off_s
    while True:
        tok, = struct.unpack_from('>I', b, j); j += 4
        if tok == 1:
            e = b.index(b'\0', j); path.append(b[j:e].decode()); j = (e + 4) & ~3
        elif tok == 3:
            ln, no = struct.unpack_from('>II', b, j)
            name = b[off_str + no:b.index(b'\0', off_str + no)].decode()
            out.setdefault('/'.join(path), {})[name] = b[j + 8:j + 8 + ln]
            j = (j + 8 + ln + 3) & ~3
        elif tok == 2:
            path.pop()
        elif tok == 4:
            continue
        else:
            return out


try:
    nodes = fdt_nodes(ramdisk)
    kn = nodes.get('/images/kernel', {})
    kz = kn.get('data', b'')
    comp = kn.get('compression', b'').rstrip(b'\0')
    kimg = gzip.decompress(kz) if comp == b'gzip' else kz
    say(True, 'the ramdisk is a FIT; its kernel %d bytes (%s), %d bytes' % (len(kz), comp.decode() or 'none', len(kimg)))
except Exception as e:
    kimg = None
    say(False, 'the ramdisk is no FIT with a kernel: %s' % e)
if image is not None:
    say(kimg == image, "the FIT's kernel is %s byte for byte" % sys.argv[2])

# the built-in initramfs: a gzip stream in the kernel that holds a newc cpio with an init
rd = None
if kimg:
    at = kimg.find(b'\x1f\x8b\x08')
    while at >= 0 and rd is None:
        try:
            d = zlib.decompressobj(31)
            x = d.decompress(kimg[at:at + (64 << 20)])
            if x[:6] == b'070701':
                rd = x
        except zlib.error:
            pass
        at = kimg.find(b'\x1f\x8b\x08', at + 1)
if rd is None:
    say(False, 'no gzip-compressed initramfs found in the kernel')
else:
    files, o = {}, 0
    while rd[o:o + 6] == b'070701':
        f = [int(rd[o + 6 + 8 * k:o + 14 + 8 * k], 16) for k in range(13)]
        mode, uid, gid, size, nsz = f[1], f[2], f[3], f[6], f[11]
        name = rd[o + 110:o + 110 + nsz - 1].decode('latin-1')
        o = (o + 110 + nsz + 3) & ~3
        if name == 'TRAILER!!!':
            break
        files[name] = (mode, uid, gid, rd[o:o + size])
        o = (o + size + 3) & ~3
    say('init' in files, 'the initramfs: %d entries, an init' % len(files))
    if init is not None and 'init' in files:
        say(files['init'][3] == init, 'its /init is %s byte for byte' % sys.argv[3])
    bad = [n for n in files if '.ssh' in n.split('/') or n.endswith('authorized_keys') or n.split('/')[-1].startswith('id_')
           or b'PRIVATE KEY-----' in files[n][3]]
    say(not bad, 'no SSH key or key file in it' + (': ' + ' '.join(bad) if bad else ''))
    say('lib/firmware/regulatory.db' in files and 'lib/firmware/regulatory.db.p7s' in files,
        'the regulatory database in it (lib/firmware/regulatory.db, .p7s)')
    owners = sorted({(u, g) for _, u, g, _ in files.values()})
    say(owners == [(0, 0)], 'every file in it is root\'s (owners: %s)' % ' '.join('%d:%d' % x for x in owners))
sys.exit(1 if fails else 0)
