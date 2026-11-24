#!/usr/bin/python3 -I
# check-po.py SRCDIR: the checks on a component's translations, run by its tests (po/meson.build):
#  - every file POTFILES.in names is there, and every source file that calls gettext is named in it;
#  - every language in LINGUAS has its PO file, and msgfmt accepts it: placeholders matching the English ones, as many
#    plural forms as the language has, a complete header;
#  - every PO file has every string of the sources (else tools/i18n/update-po.py was not run after a change);
#  - with VIBEOS_I18N_COMPLETE=1 (a release build), no string is left untranslated or fuzzy.
# Each language's counts are printed.
import os
import re
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from component import Component  # noqa: E402


def main():
    c = Component(sys.argv[1])
    errors = []
    listed = c.potfiles()
    for p in listed:
        if not os.path.exists(os.path.join(c.srcdir, p)):
            errors.append(f'POTFILES.in names {p}, which is not there')
    for p in c.sources_with_gettext():
        if p not in listed:
            errors.append(f'{p} has translatable strings but is not in POTFILES.in')
    complete = os.environ.get('VIBEOS_I18N_COMPLETE') == '1'
    with tempfile.TemporaryDirectory() as tmp:
        pot = os.path.join(tmp, c.domain + '.pot')
        c.make_pot(pot)
        for lang in c.languages():
            po = c.po(lang)
            if not os.path.exists(po):
                errors.append(f'po/{lang}.po is missing')
                continue
            r = subprocess.run(['msgfmt', '--check', '--check-format', '--check-header', '--statistics', '-o',
                                os.devnull, po], capture_output=True, text=True)
            if r.returncode:
                errors.append(f'po/{lang}.po:\n{r.stderr.strip()}')
                continue
            stats = r.stderr.strip()
            print(f'{c.domain} {lang}: {stats}')
            r = subprocess.run(['msgcmp', '--use-untranslated', '--use-fuzzy', po, pot], capture_output=True,
                               text=True)
            if r.returncode:
                missing = len(re.findall(r'this message is used but not defined', r.stderr))
                errors.append(f'po/{lang}.po lacks {missing or "some"} of the sources\' strings: run '
                              f'tools/i18n/update-po.py {c.srcdir}')
            if complete and re.search(r'(fuzzy|untranslated) message', stats):
                errors.append(f'po/{lang}.po is not complete: {stats}')
    for e in errors:
        print(e, file=sys.stderr)
    sys.exit(1 if errors else 0)


main()
