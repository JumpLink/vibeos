#!/usr/bin/env python3
"""replace-init.py IN.cpio.gz OLD_INIT_SHA256 NEW_INIT OUT.cpio.gz

OUT = IN with only its /init member's data replaced by NEW_INIT (the size field follows; every other member, header
field and the padding stay byte for byte). IN must be one gzip member holding one newc archive whose /init has
OLD_INIT_SHA256, and must come back byte for byte when rewritten unchanged (cpio and gzip -9n) before anything is
changed. NEW_INIT must be a busybox sh script."""
import hashlib
import subprocess
import sys
import zlib

src, old_sha, new_init, dst = sys.argv[1:5]
raw = open(src, 'rb').read()
z = zlib.decompressobj(31)
c = z.decompress(raw)
assert z.eof and not z.unused_data, '%s: not one gzip member' % src


def members(c):
    i, out = 0, []
    while True:
        h = c[i:i + 110]
        assert h[:6] == b'070701', 'not a newc cpio at %d' % i
        f = [int(h[6 + 8 * k:14 + 8 * k], 16) for k in range(13)]
        ns, fs = f[11], f[6]
        name = c[i + 110:i + 110 + ns - 1]
        d0 = (i + 110 + ns + 3) & ~3
        out.append([f, name, c[d0:d0 + fs]])
        i = (d0 + fs + 3) & ~3
        if name == b'TRAILER!!!':
            return out, c[i:]


def cpio(ms, tail):
    b = bytearray()
    for f, name, data in ms:
        f = f[:6] + [len(data)] + f[7:]
        b += b'070701' + b''.join(b'%08X' % x for x in f) + name + b'\0'
        b += b'\0' * (-len(b) % 4) + data
        b += b'\0' * (-len(b) % 4)
    return bytes(b) + tail


def gz(b):
    return subprocess.run(['gzip', '-9n'], input=b, stdout=subprocess.PIPE, check=True).stdout


ms, tail = members(c)
assert not tail.strip(b'\0'), 'data after the archive'
assert cpio(ms, tail) == c, 'the cpio does not come back byte for byte'
assert gz(c) == raw, 'gzip -9n does not give %s back byte for byte' % src
init = [m for m in ms if m[1] == b'init']
assert len(init) == 1, 'no single /init'
got = hashlib.sha256(init[0][2]).hexdigest()
assert got == old_sha, '/init is %s, not %s' % (got, old_sha)
new = open(new_init, 'rb').read()
assert new.startswith(b'#!/bin/busybox sh\n'), 'NEW_INIT is not a busybox sh script'
init[0][2] = new
open(dst, 'wb').write(gz(cpio(ms, tail)))
