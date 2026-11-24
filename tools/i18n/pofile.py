# pofile.py: reading and writing gettext PO files, for the tools in this directory.
#
# An Entry keeps the lines it was read from; only an entry whose fields were changed is written anew, so a file
# read and written back is unchanged. The writer does not wrap long lines (msgcat or msgmerge do, later).
import re

PLURAL_FORMS = {
    'de': 'nplurals=2; plural=(n != 1);',
    'es': 'nplurals=2; plural=(n != 1);',
    'fr': 'nplurals=2; plural=(n > 1);',
    'it': 'nplurals=2; plural=(n != 1);',
    'nl': 'nplurals=2; plural=(n != 1);',
    'ru': 'nplurals=3; plural=(n%10==1 && n%100!=11 ? 0 : n%10>=2 && n%10<=4 && (n%100<10 || n%100>=20) ? 1 : 2);',
}


def unquote(s):
    s = s.strip()
    assert s.startswith('"') and s.endswith('"'), s
    out, i, body = [], 0, s[1:-1]
    while i < len(body):
        c = body[i]
        if c == '\\' and i + 1 < len(body):
            n = body[i + 1]
            out.append({'n': '\n', 't': '\t', '"': '"', '\\': '\\', 'r': '\r', 'a': '\a', 'b': '\b',
                        'f': '\f', 'v': '\v'}.get(n, '\\' + n))
            i += 2
        else:
            out.append(c)
            i += 1
    return ''.join(out)


def quote(s):
    s = s.replace('\\', '\\\\').replace('"', '\\"').replace('\t', '\\t').replace('\r', '\\r')
    parts = s.split('\n')
    if len(parts) == 1:
        return [f'"{s}"']
    lines = ['""'] if len(s) > 0 else []
    for i, p in enumerate(parts):
        last = i == len(parts) - 1
        if last and not p:
            break
        lines.append(f'"{p}\\n"' if not last else f'"{p}"')
    return lines or ['""']


class Entry:
    def __init__(self):
        self.lines = []          # as read
        self.comments = []       # '# ' translator comments
        self.extracted = []      # '#.'
        self.refs = []           # '#:'
        self.flags = []          # '#,'
        self.previous = []       # '#|'
        self.msgctxt = None
        self.msgid = None
        self.msgid_plural = None
        self.msgstr = None       # str, or a list for a plural entry
        self.obsolete = False
        self.changed = False

    @property
    def plural(self):
        return self.msgid_plural is not None

    def translated(self):
        if self.plural:
            return all(self.msgstr) and bool(self.msgstr)
        return bool(self.msgstr)

    def fuzzy(self):
        return 'fuzzy' in self.flags

    def key(self):
        return (self.msgctxt, self.msgid)

    def render(self):
        if not self.changed:
            return self.lines
        out = [f'# {c}' if c else '#' for c in self.comments]
        out += [f'#. {c}' for c in self.extracted]
        out += [f'#: {r}' for r in self.refs]
        if self.flags:
            out.append('#, ' + ', '.join(self.flags))
        out += [f'#| {p}' for p in self.previous]

        def field(name, value):
            q = quote(value)
            return [f'{name} {q[0]}'] + q[1:]
        if self.msgctxt is not None:
            out += field('msgctxt', self.msgctxt)
        out += field('msgid', self.msgid)
        if self.plural:
            out += field('msgid_plural', self.msgid_plural)
            for i, s in enumerate(self.msgstr):
                out += field(f'msgstr[{i}]', s)
        else:
            out += field('msgstr', self.msgstr or '')
        return out


class PoFile:
    def __init__(self, path):
        self.path = path
        self.entries = []
        self.header = None
        self._read()

    def _read(self):
        text = open(self.path, encoding='utf-8').read()
        blocks = re.split(r'\n\s*\n', text.strip('\n'))
        for block in blocks:
            e = Entry()
            e.lines = block.split('\n')
            current, idx = None, None
            for line in e.lines:
                if line.startswith('#~'):
                    e.obsolete = True
                    continue
                if line.startswith('#.'):
                    e.extracted.append(line[2:].strip())
                elif line.startswith('#:'):
                    e.refs.append(line[2:].strip())
                elif line.startswith('#,'):
                    e.flags += [f.strip() for f in line[2:].split(',') if f.strip()]
                elif line.startswith('#|'):
                    e.previous.append(line[2:].strip())
                elif line.startswith('#'):
                    e.comments.append(line[2:] if line.startswith('# ') else line[1:])
                elif line.startswith('msgctxt '):
                    current, e.msgctxt = 'msgctxt', unquote(line[8:])
                elif line.startswith('msgid_plural '):
                    current, e.msgid_plural = 'msgid_plural', unquote(line[13:])
                elif line.startswith('msgid '):
                    current, e.msgid = 'msgid', unquote(line[6:])
                elif line.startswith('msgstr['):
                    m = re.match(r'msgstr\[(\d+)\] (.*)$', line)
                    idx = int(m.group(1))
                    if not isinstance(e.msgstr, list):
                        e.msgstr = []
                    while len(e.msgstr) <= idx:
                        e.msgstr.append('')
                    e.msgstr[idx] = unquote(m.group(2))
                    current = 'msgstr[]'
                elif line.startswith('msgstr '):
                    current, e.msgstr = 'msgstr', unquote(line[7:])
                elif line.startswith('"'):
                    v = unquote(line)
                    if current == 'msgctxt':
                        e.msgctxt += v
                    elif current == 'msgid':
                        e.msgid += v
                    elif current == 'msgid_plural':
                        e.msgid_plural += v
                    elif current == 'msgstr':
                        e.msgstr += v
                    elif current == 'msgstr[]':
                        e.msgstr[idx] += v
            if e.msgid == '' and e.msgctxt is None and self.header is None and not e.obsolete:
                self.header = e
            self.entries.append(e)

    def messages(self):
        return [e for e in self.entries if e is not self.header and not e.obsolete and e.msgid is not None]

    def header_field(self, name):
        for line in (self.header.msgstr if self.header else '').split('\n'):
            if line.startswith(name + ':'):
                return line[len(name) + 1:].strip()
        return None

    def set_header_field(self, name, value):
        lines = self.header.msgstr.split('\n')
        for i, line in enumerate(lines):
            if line.startswith(name + ':'):
                lines[i] = f'{name}: {value}'
                break
        else:
            lines.insert(len(lines) - 1 if lines and lines[-1] == '' else len(lines), f'{name}: {value}')
        self.header.msgstr = '\n'.join(lines)
        self.header.changed = True

    def write(self, path=None):
        with open(path or self.path, 'w', encoding='utf-8') as f:
            f.write('\n\n'.join('\n'.join(e.render()) for e in self.entries) + '\n')
