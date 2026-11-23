"""Debian version comparison (dpkg's algorithm) and a Depends check against a package set."""
import re


def _order(c):
    if c == '~':
        return -1
    if c.isdigit():
        return 0
    if c.isalpha():
        return ord(c)
    return ord(c) + 256


def _cmp_part(a, b):
    i = j = 0
    while i < len(a) or j < len(b):
        diff = 0
        while (i < len(a) and not a[i].isdigit()) or (j < len(b) and not b[j].isdigit()):
            ac = _order(a[i]) if i < len(a) and not a[i].isdigit() else 0
            bc = _order(b[j]) if j < len(b) and not b[j].isdigit() else 0
            if ac != bc:
                return ac - bc
            i += 1; j += 1
        while i < len(a) and a[i] == '0':
            i += 1
        while j < len(b) and b[j] == '0':
            j += 1
        while i < len(a) and a[i].isdigit() and j < len(b) and b[j].isdigit():
            if not diff:
                diff = ord(a[i]) - ord(b[j])
            i += 1; j += 1
        if i < len(a) and a[i].isdigit():
            return 1
        if j < len(b) and b[j].isdigit():
            return -1
        if diff:
            return diff
    return 0


def split(v):
    e, _, r = v.partition(':') if ':' in v else ('0', '', v)
    u, _, rev = r.rpartition('-') if '-' in r else (r, '', '0')
    return int(e or 0), u, rev


def cmp(a, b):
    ea, ua, ra = split(a)
    eb, ub, rb = split(b)
    if ea != eb:
        return ea - eb
    return _cmp_part(ua, ub) or _cmp_part(ra, rb)


def satisfies(have, op, want):
    c = cmp(have, want)
    return {'>=': c >= 0, '<=': c <= 0, '>>': c > 0, '<<': c < 0, '=': c == 0, '>': c >= 0, '<': c <= 0}[op]


class PackageSet:
    """name -> version, plus provides (name -> [(version or None, by)])"""
    def __init__(self):
        self.ver, self.prov = {}, {}

    def add(self, name, version, provides=''):
        self.ver[name] = version
        for p in filter(None, (x.strip() for x in provides.split(','))):
            m = re.match(r'(\S+)\s*(?:\(\s*=\s*([^)]+)\))?', p)
            self.prov.setdefault(m.group(1), []).append(((m.group(2) or '').strip() or None, name))

    @classmethod
    def from_list(cls, path):
        s = cls()
        for line in open(path, encoding='utf-8'):
            f = line.split()
            if f and f[0] == 'pkg':
                s.ver[f[1]] = f[2]
            elif f and f[0] == 'provides':
                s.prov.setdefault(f[1], []).append((None if f[2] == '-' else f[2], f[3]))
        return s

    def has(self, rel):
        m = re.match(r'([^\s(:]+)(?::\S+)?\s*(?:\(\s*([<>=]+)\s*([^)\s]+)\s*\))?', rel.strip())
        name, op, want = m.group(1), m.group(2), m.group(3)
        if name in self.ver and (not op or satisfies(self.ver[name], op, want)):
            return True
        for v, _by in self.prov.get(name, []):
            if not op or (v and satisfies(v, op, want)):
                return True
        return False

    def missing(self, depends):
        """the alternatives groups of a Depends/Pre-Depends field that nothing here satisfies"""
        out = []
        for group in filter(None, (g.strip() for g in depends.split(','))):
            if not any(self.has(alt) for alt in group.split('|')):
                out.append(group)
        return out
