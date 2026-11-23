"""cpio newc: read (members, never written to disk unless asked) and write (owned by root)."""
import os, stat


def read(data):
    """[(name, mode, target_or_data)] of a newc archive (several concatenated archives are read one after another)."""
    out, o = [], 0
    while o + 110 <= len(data):
        if data[o:o + 6] not in (b'070701', b'070702'):
            # padding between concatenated archives
            if data[o:o + 1] == b'\0':
                o += 1
                continue
            break
        h = data[o:o + 110]
        f = [int(h[6 + 8 * i:14 + 8 * i], 16) for i in range(13)]
        mode, fsize, nsize = f[1], f[6], f[11]
        n0 = o + 110
        name = data[n0:n0 + nsize - 1].decode('utf-8', 'replace')
        d0 = (n0 + nsize + 3) & ~3
        body = data[d0:d0 + fsize]
        o = (d0 + fsize + 3) & ~3
        if name == 'TRAILER!!!':
            continue
        out.append((name, mode, body))
    return out


class Writer:
    def __init__(self):
        self.parts, self.ino = [], 1

    def _hdr(self, name, mode, size, nlink=1, rdev=(0, 0)):
        nb = name.encode() + b'\0'
        h = b'070701' + b''.join(b'%08X' % v for v in (
            self.ino, mode, 0, 0, nlink, 0, size, 0, 0, rdev[0], rdev[1], len(nb), 0))
        self.ino += 1
        pad = (-(110 + len(nb))) % 4
        self.parts.append(h + nb + b'\0' * pad)

    def _body(self, b):
        self.parts.append(b + b'\0' * ((-len(b)) % 4))

    def dir(self, name, perm=0o755):
        self._hdr(name, stat.S_IFDIR | perm, 0, 2)

    def file(self, name, data, perm=0o644):
        self._hdr(name, stat.S_IFREG | perm, len(data))
        self._body(data)

    def symlink(self, name, target):
        t = target.encode()
        self._hdr(name, stat.S_IFLNK | 0o777, len(t))
        self._body(t)

    def chardev(self, name, perm, major, minor):
        self._hdr(name, stat.S_IFCHR | perm, 0, 1, (major, minor))

    def tree(self, src, prefix):
        """src's files under prefix, modes kept (owner root), sorted"""
        for root, dirs, files in os.walk(src):
            dirs.sort()
            rel = os.path.relpath(root, src)
            base = prefix if rel == '.' else prefix + '/' + rel
            for d in dirs:
                p = os.path.join(root, d)
                if os.path.islink(p):
                    self.symlink(base + '/' + d, os.readlink(p))
                else:
                    self.dir(base + '/' + d, os.stat(p).st_mode & 0o7777)
            for f in sorted(files):
                p = os.path.join(root, f)
                if os.path.islink(p):
                    self.symlink(base + '/' + f, os.readlink(p))
                else:
                    self.file(base + '/' + f, open(p, 'rb').read(), os.stat(p).st_mode & 0o7777)

    def bytes(self):
        self._hdr('TRAILER!!!', 0, 0, 1)
        return b''.join(self.parts)
