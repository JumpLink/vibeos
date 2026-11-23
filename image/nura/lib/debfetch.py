"""Debian arm64 packages straight from the archive, checked the way apt checks them: InRelease signed by a key of
the local debian-archive-keyring (gpgv), Packages.xz against InRelease's SHA256, each .deb against Packages."""
import hashlib, lzma, os, subprocess, urllib.request

MIRROR = 'https://deb.debian.org/debian'
KEYRING = '/usr/share/keyrings/debian-archive-keyring.gpg'
UA = {'User-Agent': 'nura-to-vibeos/0.1'}


def _get(url):
    with urllib.request.urlopen(urllib.request.Request(url, headers=UA), timeout=120) as r:
        return r.read()


def _sha(b):
    return hashlib.sha256(b).hexdigest()


def stanzas(text):
    cur, key = {}, None
    for line in text.splitlines():
        if not line.strip():
            if cur:
                yield cur
            cur, key = {}, None
        elif line[0] in ' \t' and key:
            cur[key] += '\n' + line.strip()
        else:
            key, _, v = line.partition(':')
            cur[key] = v.strip()
    if cur:
        yield cur


class Archive:
    def __init__(self, cache, suite='sid', mirror=MIRROR, keyring=KEYRING, log=print):
        self.cache, self.suite, self.mirror, self.keyring, self.log = cache, suite, mirror, keyring, log
        os.makedirs(cache, exist_ok=True)
        self.pkgs = None

    def load(self):
        if self.pkgs is not None:
            return
        if not os.path.exists(self.keyring):
            raise SystemExit('no %s: install debian-archive-keyring (or use --debs DIR)' % self.keyring)
        ir = os.path.join(self.cache, 'InRelease')
        open(ir, 'wb').write(_get('%s/dists/%s/InRelease' % (self.mirror, self.suite)))
        out = os.path.join(self.cache, 'Release.verified')
        r = subprocess.run(['gpgv', '--keyring', self.keyring, '--output', out, ir], capture_output=True, text=True)
        if r.returncode:
            raise SystemExit('Debian InRelease: signature check failed:\n' + r.stderr)
        rel = open(out, encoding='utf-8').read()
        want = None
        insha = False
        for line in rel.splitlines():
            if line.startswith('SHA256:'):
                insha = True
                continue
            if insha and line.startswith(' '):
                h, size, name = line.split()
                if name == 'main/binary-arm64/Packages.xz':
                    want = h
            elif insha:
                insha = False
        if not want:
            raise SystemExit('Debian InRelease lists no main/binary-arm64/Packages.xz')
        px = _get('%s/dists/%s/main/binary-arm64/Packages.xz' % (self.mirror, self.suite))
        if _sha(px) != want:
            raise SystemExit('Debian Packages.xz does not match InRelease')
        self.pkgs = {}
        for s in stanzas(lzma.decompress(px).decode('utf-8')):
            self.pkgs[s['Package']] = s
        self.log('  Debian %s: %d arm64 packages (InRelease signature good)' % (self.suite, len(self.pkgs)))

    def info(self, name):
        self.load()
        return self.pkgs.get(name)

    def fetch(self, name, dest):
        s = self.info(name)
        if not s:
            raise SystemExit('Debian %s has no arm64 package %s' % (self.suite, name))
        fn = os.path.join(dest, os.path.basename(s['Filename']))
        c = os.path.join(self.cache, os.path.basename(s['Filename']))
        if not (os.path.exists(c) and _sha(open(c, 'rb').read()) == s['SHA256']):
            b = _get('%s/%s' % (self.mirror, s['Filename']))
            if _sha(b) != s['SHA256']:
                raise SystemExit('%s: sha256 differs from Packages' % s['Filename'])
            open(c, 'wb').write(b)
        os.makedirs(dest, exist_ok=True)
        if os.path.abspath(c) != os.path.abspath(fn):
            open(fn, 'wb').write(open(c, 'rb').read())
        return fn, s
