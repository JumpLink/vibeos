# component.py: a translated VibeOS component, as the tools in this directory see it: its source directory holds
# po/LINGUAS (the languages), po/POTFILES.in (the files with translatable strings, relative to the source directory)
# and po/meson.build, whose i18n.gettext() names the gettext domain.
import os
import re
import subprocess
import tempfile

# xgettext's arguments: meson's 'glib' preset (i18n.gettext(preset: 'glib')) and the ones po/meson.build adds
XGETTEXT_ARGS = [
    '--from-code=UTF-8', '--add-comments',
    '--keyword=_', '--keyword=N_', '--keyword=C_:1c,2', '--keyword=NC_:1c,2', '--keyword=g_dcgettext:2',
    '--keyword=g_dngettext:2,3', '--keyword=g_dpgettext2:2c,3',
    '--flag=N_:1:pass-c-format', '--flag=C_:2:pass-c-format', '--flag=NC_:2:pass-c-format',
    '--flag=g_dngettext:2:pass-c-format', '--flag=g_strdup_printf:1:c-format', '--flag=g_string_printf:2:c-format',
    '--flag=g_string_append_printf:2:c-format', '--flag=g_error_new:3:c-format', '--flag=g_set_error:4:c-format',
    '--flag=g_markup_printf_escaped:1:c-format', '--flag=g_log:3:c-format', '--flag=g_print:1:c-format',
    '--flag=g_printerr:1:c-format', '--flag=g_printf:1:c-format', '--flag=g_fprintf:2:c-format',
    '--flag=g_sprintf:2:c-format', '--flag=g_snprintf:3:c-format',
    # added by every po/meson.build: the file without line numbers, which would change every translation at each edit
    '--add-location=file',
]

# the files that call gettext: a call to one of these, a word of its own
GETTEXT_CALL = re.compile(r'(?<![\w.])(_|N_|C_|NC_|ngettext|dgettext|g_dgettext|g_dngettext|g_dpgettext2|pgettext)\s*\(')
SOURCE_SUFFIXES = ('.c', '.h', '.js')
SKIP_DIRS = {'tests', 'test', 'demo', 'tools', 'po', 'subprojects', 'cross', 'maplibre', '.git'}


class Component:
    def __init__(self, srcdir):
        self.srcdir = os.path.abspath(srcdir)
        self.podir = os.path.join(self.srcdir, 'po')
        build = open(os.path.join(self.podir, 'meson.build'), encoding='utf-8').read()
        m = re.search(r"\.gettext\(\s*'([^']+)'", build)
        if not m:
            raise SystemExit(f'{self.podir}/meson.build: no i18n.gettext()')
        self.domain = m.group(1)

    def languages(self):
        return [w for line in open(os.path.join(self.podir, 'LINGUAS'), encoding='utf-8')
                if not line.startswith('#') for w in line.split()]

    def potfiles(self):
        out = []
        for line in open(os.path.join(self.podir, 'POTFILES.in'), encoding='utf-8'):
            line = line.strip()
            if line and not line.startswith('#'):
                out.append(line)
        return out

    def po(self, lang):
        return os.path.join(self.podir, f'{lang}.po')

    def sources_with_gettext(self):
        """the source files (relative paths) that call gettext"""
        found = []
        for dp, dns, fns in os.walk(self.srcdir):
            dns[:] = sorted(d for d in dns if d not in SKIP_DIRS and not d.startswith('obj'))
            for fn in sorted(fns):
                if not fn.endswith(SOURCE_SUFFIXES):
                    continue
                p = os.path.join(dp, fn)
                text = open(p, encoding='utf-8', errors='replace').read()
                if GETTEXT_CALL.search(strip_comments(text)):
                    found.append(os.path.relpath(p, self.srcdir))
        return found

    def make_pot(self, out):
        """the template from the sources, into the file @out"""
        with tempfile.NamedTemporaryFile('w', suffix='.list', delete=False) as f:
            f.write('\n'.join(self.potfiles()) + '\n')
            listing = f.name
        try:
            subprocess.run(['xgettext', '--package-name=' + self.domain,
                            '--msgid-bugs-address=https://github.com/pulpoff/vibeos/issues', '-D', self.srcdir,
                            '-f', listing, '-o', out] + XGETTEXT_ARGS, check=True)
        finally:
            os.unlink(listing)


def strip_comments(text):
    """C and JS source without its comments (string literals kept), for finding the gettext calls"""
    out, i, n = [], 0, len(text)
    while i < n:
        if text.startswith('/*', i):
            j = text.find('*/', i + 2)
            i = n if j < 0 else j + 2
        elif text.startswith('//', i):
            j = text.find('\n', i)
            i = n if j < 0 else j
        elif text[i] in '"\'`':
            q, j = text[i], i + 1
            while j < n and text[j] != q:
                j += 2 if text[j] == '\\' else 1
            out.append(text[i:j + 1])
            i = j + 1
        else:
            out.append(text[i])
            i += 1
    return ''.join(out)
