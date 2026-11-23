"""Reading a Nura (postmarketOS) image without mounting or running anything from it: xz and sparse unpacking, the
partitions, files out of ext4 (debugfs, read only) and FAT (mtools). Every symlink that comes out stays a symlink;
nothing here ever opens a path through one."""
import os, re, shutil, stat, subprocess

import gpt, simg

DEBUGFS = shutil.which('debugfs') or '/sbin/debugfs'


class Cat:
    """a stream that first gives back bytes already read"""
    def __init__(self, head, f):
        self.h, self.f = head, f

    def read(self, n):
        if self.h:
            b, self.h = self.h[:n], self.h[n:]
            if len(b) < n:
                b += self.f.read(n - len(b))
            return b
        return self.f.read(n)


def unxz(src, dst):
    """src .xz (or plain) -> dst, an Android sparse image expanded, holes kept; returns 'sparse' or 'raw'"""
    p = None
    if src.endswith('.xz'):
        if shutil.which('xz'):
            p = subprocess.Popen(['xz', '-dc', '-T0', src], stdout=subprocess.PIPE)
            f = p.stdout
        else:
            import lzma
            f = lzma.open(src, 'rb')
    else:
        f = open(src, 'rb')
    head = f.read(28)
    with open(dst + '.part', 'wb') as o:
        if simg.is_sparse(head):
            simg.unsparse(Cat(head, f), o)
            kind = 'sparse'
        else:
            o.write(head)
            while True:
                b = f.read(1 << 20)
                if not b:
                    break
                if not b.strip(b'\0'):
                    o.seek(len(b), 1)
                else:
                    o.write(b)
            o.truncate()
            kind = 'raw'
    if p and p.wait():
        raise SystemExit('xz failed on %s' % src)
    os.rename(dst + '.part', dst)
    return kind


def ext4_spec(img, off):
    return '%s?offset=%d' % (img, off) if off else img


def debugfs(spec, cmd):
    r = subprocess.run([DEBUGFS, '-R', cmd, spec], capture_output=True)
    return r.stdout


def ext4_cat(spec, path):
    """a regular file's bytes, or None (a symlink or directory is not followed)"""
    st = debugfs(spec, 'stat "%s"' % path).decode(errors='replace')
    if 'Type: regular' not in st:
        return None
    return debugfs(spec, 'cat "%s"' % path)


def ext4_type(spec, path):
    m = re.search(r'Type: (\w+)', debugfs(spec, 'stat "%s"' % path).decode(errors='replace'))
    return m and m.group(1)


def ext4_readlink(spec, path):
    m = re.search(r'Fast link dest: "(.*)"', debugfs(spec, 'stat "%s"' % path).decode(errors='replace'))
    if m:
        return m.group(1)
    t = ext4_type(spec, path)
    if t == 'symlink':  # a slow symlink: its target is the block content
        return debugfs(spec, 'cat "%s"' % path).decode(errors='replace')
    return None


def resolve(spec, path, depth=0):
    """path with every symlink component resolved inside the image (never outside it)"""
    parts, out = [p for p in path.split('/') if p], ''
    for i, p in enumerate(parts):
        cur = out + '/' + p
        t = ext4_readlink(spec, cur) if depth < 16 else None
        if t is not None:
            base = t if t.startswith('/') else os.path.normpath(out + '/' + t)
            out = resolve(spec, base, depth + 1).rstrip('/')
        else:
            out = cur
    return out or '/'


def ext4_rdump(spec, path, dest):
    """the directory path (symlinks resolved inside the image) into the NEW directory dest/<its name>"""
    real = resolve(spec, path)
    if ext4_type(spec, real) != 'directory':
        return None
    os.makedirs(dest, exist_ok=False)
    subprocess.run([DEBUGFS, '-R', 'rdump "%s" "%s"' % (real, dest), spec], capture_output=True)
    return os.path.join(dest, os.path.basename(real.rstrip('/')))


def fat_copy(img, off, dest):
    """the whole FAT file system at byte off of img into dest (mtools, no mount)"""
    os.makedirs(dest, exist_ok=False)
    env = dict(os.environ, MTOOLS_SKIP_CHECK='1')
    r = subprocess.run(['mcopy', '-s', '-n', '-m', '-i', '%s@@%d' % (img, off), '::/*', dest], capture_output=True, env=env)
    if r.returncode:
        raise SystemExit('mcopy: %s' % r.stderr.decode(errors='replace'))


def walk(top):
    """(relative path, lstat) of every file, symlink and directory below top, sorted; nothing followed"""
    out = []
    for root, dirs, files in os.walk(top):
        dirs.sort()
        for n in sorted(dirs) + sorted(files):
            p = os.path.join(root, n)
            out.append((os.path.relpath(p, top), os.lstat(p)))
    return out


def copy_entry(src, dst):
    """one file or symlink, its mode kept (setuid/setgid dropped), never through a symlink"""
    st = os.lstat(src)
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    if os.path.lexists(dst):
        os.unlink(dst)
    if stat.S_ISLNK(st.st_mode):
        os.symlink(os.readlink(src), dst)
    elif stat.S_ISREG(st.st_mode):
        fd = os.open(src, os.O_RDONLY | os.O_NOFOLLOW)
        with os.fdopen(fd, 'rb') as i, open(dst, 'wb') as o:
            shutil.copyfileobj(i, o, 1 << 20)
        os.chmod(dst, stat.S_IMODE(st.st_mode) & 0o755 | 0o644)
    elif stat.S_ISDIR(st.st_mode):
        os.makedirs(dst, exist_ok=True)


def read_nofollow(path):
    fd = os.open(path, os.O_RDONLY | os.O_NOFOLLOW)
    with os.fdopen(fd, 'rb') as f:
        return f.read()


def partitions(img):
    out = []
    for idx, name, typ, off, size, ss in gpt.partitions(img):
        fs, label = gpt.fstype(img, off)
        out.append({'index': idx, 'name': name, 'type': typ, 'offset': off, 'size': size, 'sector': ss,
                    'fs': fs, 'label': label})
    return out


def deviceinfo(text):
    """the deviceinfo_* variables of a deviceinfo file, read as data (never sourced)"""
    d = {}
    for line in text.splitlines():
        m = re.match(r'\s*(deviceinfo_\w+)="(.*)"\s*$', line)
        if m:
            d[m.group(1)[len('deviceinfo_'):]] = m.group(2)
    return d
