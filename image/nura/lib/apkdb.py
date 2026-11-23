"""Alpine's /lib/apk/db/installed: packages, their versions, dependencies and files."""


def parse(text):
    pkgs, cur, d = {}, None, ''
    for line in text.splitlines():
        if not line:
            cur = None
            continue
        k, _, v = line.partition(':')
        if k == 'P':
            cur = pkgs[v] = {'name': v, 'files': [], 'version': '', 'depends': '', 'origin': '', 'provides': ''}
        elif cur is None:
            continue
        elif k == 'V':
            cur['version'] = v
        elif k == 'D':
            cur['depends'] = v
        elif k == 'o':
            cur['origin'] = v
        elif k == 'p':
            cur['provides'] = v
        elif k == 'F':
            d = v
        elif k == 'R':
            cur['files'].append((d + '/' + v) if d else v)
    return pkgs
