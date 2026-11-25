#!/usr/bin/python3 -I
# update-po.py SRCDIR...: brings each component's translations (SRCDIR/po/LANG.po for each language in LINGUAS) up to
# date with its sources: the template is made from POTFILES.in, a missing language is started with msginit, and every
# file is merged with the template, without fuzzy matching (a string that changed is translated again, not guessed),
# without the strings no longer in the sources. Use it instead of meson's <domain>-update-po, which matches fuzzily.
import os
import re
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from component import Component  # noqa: E402
from pofile import PLURAL_FORMS, PoFile  # noqa: E402

LANGUAGE_NAMES = {'de': 'German', 'es': 'Spanish', 'fr': 'French', 'it': 'Italian', 'nl': 'Dutch', 'ru': 'Russian',
                  'pl': 'Polish', 'he': 'Hebrew', 'uk': 'Ukrainian'}


def without_date(text):
    return re.sub(r'^"POT-Creation-Date: [^"]*"$', '', text, flags=re.M)


def update(srcdir):
    c = Component(srcdir)
    with tempfile.TemporaryDirectory() as tmp:
        pot = os.path.join(tmp, c.domain + '.pot')
        c.make_pot(pot)
        for lang in c.languages():
            po = c.po(lang)
            if not os.path.exists(po):
                subprocess.run(['msginit', '--no-translator', '-l', lang, '-i', pot, '-o', po], check=True,
                               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
                f = PoFile(po)
                f.set_header_field('Project-Id-Version', c.domain)
                f.set_header_field('Last-Translator', 'VibeOS translators')
                f.set_header_field('Language-Team', LANGUAGE_NAMES.get(lang, lang))
                f.set_header_field('Plural-Forms', PLURAL_FORMS[lang])
                # (msginit takes the template's: ASCII where the sources have no other character)
                f.set_header_field('Content-Type', 'text/plain; charset=UTF-8')
                f.header.comments = [
                    f'{LANGUAGE_NAMES.get(lang, lang)} translations of {c.domain}.',
                    'Translations marked "android:" come from the Android Open Source Project and LineageOS,',
                    'under the Apache License 2.0 (see tools/i18n/NOTICE).',
                ]
                f.write()
            before = open(po, encoding='utf-8').read()
            subprocess.run(['msgmerge', '--quiet', '--no-fuzzy-matching', '--add-location=file', '-o', po, po, pot],
                           check=True)
            subprocess.run(['msgattrib', '--no-obsolete', '-o', po, po], check=True)
            # a file whose strings did not change keeps its date too
            if without_date(open(po, encoding='utf-8').read()) == without_date(before):
                open(po, 'w', encoding='utf-8').write(before)
        print(f'{c.domain}: {", ".join(c.languages())}')


for d in sys.argv[1:]:
    update(d)
