#!/usr/bin/python3 -I
# android-import.py [--review FILE] CACHEDIR SRCDIR...: fills the untranslated strings of each component's PO files
# (SRCDIR/po/LANG.po) with the official translations of the same English strings in the Android apps VibeOS copies,
# fetched into CACHEDIR by android-fetch.py.
#
# Android's English must be the same as ours, up to quotes, ellipses, non-breaking spaces and the placeholders'
# numbering ("exact"), or differ only in its case, its last full stop or colon, or "&" for "and" ("adapted": the
# translation is adapted the same way). When several Android strings have that English, the translation most of them
# share is taken, those of the component's own Android apps (PREFER) counting more. Plural strings are matched with
# Android's plurals, form by form, from the component's apps first. Android's numbered placeholders become the
# component's own (%1$s → %s, or %2$s and %1$s kept numbered when the translation reorders them; never reordered in
# JavaScript). A translation whose placeholders do not match is not taken. Every string taken carries a translator
# comment naming its source ("android: Settings wifi_settings"), and translations already there are never changed.
#
# --review FILE appends, as tab-separated lines, what a translator should look at: adapted strings, short ones (whose
# sense the English alone does not fix), strings Android translates in more than one way, strings taken from another
# app than the component's own, and translations with quotes the English does not have.
#
# CACHEDIR is only parsed (xml.etree, no entities are expanded), never run.
import collections
import os
import re
import sys
import xml.etree.ElementTree as ET

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from component import Component  # noqa: E402
from pofile import PoFile  # noqa: E402

LANGS = ['de', 'es', 'fr', 'it', 'nl', 'ru']
# Android's plural quantities, in the order of each language's gettext plural forms (pofile.PLURAL_FORMS)
PLURAL_QUANTITIES = {'de': ['one', 'other'], 'es': ['one', 'other'], 'fr': ['one', 'other'], 'it': ['one', 'other'],
                     'nl': ['one', 'other'], 'ru': ['one', 'few', 'many']}
# plural forms that stand for n = 1 alone: their translation may leave the number out
SINGLE_ONE = {'de', 'es', 'it', 'nl'}
DEFAULT = ['Framework', 'Settings', 'SettingsLib', 'SystemUI', 'Keyguard', 'Launcher3', 'DocumentsUI', 'Contacts',
           'Dialer', 'Messaging', 'DeskClock', 'Etar', 'Aperture', 'Jelly', 'ExactCalculator', 'PackageInstaller',
           'LatinIME']
PREFER = {
    'vibeos-settings': ['Settings', 'SettingsLib', 'Framework', 'SystemUI', 'Keyguard', 'Launcher3', 'LatinIME'],
    'vibeos-shell': ['SystemUI', 'Keyguard', 'Framework', 'Launcher3', 'LatinIME', 'Settings', 'SettingsLib'],
    'vibeos-launcher': ['Launcher3', 'SystemUI', 'Framework'],
    'vibeos-dialer': ['Dialer', 'Contacts', 'Framework'],
    'vibeos-messaging': ['Messaging', 'Contacts', 'Dialer', 'Framework'],
    'vibeos-contacts': ['Contacts', 'Dialer', 'Framework'],
    'vibeos-clock': ['DeskClock', 'Framework'],
    'vibeos-calendar': ['Etar', 'Framework'],
    'vibeos-camera': ['Aperture', 'Framework'],
    'vibeos-browser': ['Jelly', 'Framework'],
    'vibeos-files': ['DocumentsUI', 'Framework'],
    'vibeos-calculator': ['ExactCalculator', 'Framework'],
    'vibeos-installer': ['PackageInstaller', 'Framework', 'Settings'],
    'vibeos-software': ['PackageInstaller', 'Settings', 'Framework'],
    'libvibeos-1': ['Framework', 'Settings', 'SettingsLib', 'SystemUI'],
}
QUOTES = {'de': ('„', '“'), 'es': ('«', '»'), 'fr': ('« ', ' »'), 'it': ('«', '»'), 'nl': ('“', '”'),
          'ru': ('«', '»')}
XLIFF = '{urn:oasis:names:tc:xliff:document:1.2}g'
# Android strings whose English is ours but not their sense: paper sizes ("Legal", "Letter"), Contacts' "About" (the
# contact), the accessibility services' "About %s", a network's speed ("Okay"), the keyboard's spoken emoji names
# ("Package", "Radio"), the battery tips' "Check", an archived app's "%s is uninstalled", a recent task's "Close %s"
EXCLUDE_NAMES = re.compile(r'^(mediasize_|about_card_title$|accessibility_introduction_title$|speed_label_|'
                           r'spoken_emoji_|battery_tips_|unarchive_|close_task_accessibility)')
# a contact's phone, address and event types ("Home", "Work", "Other"), for the apps with contacts only
CONTACT_TYPES = re.compile(r'^(phoneTypes|emailAddressTypes|postalAddressTypes|imAddressTypes|imProtocols|'
                           r'organizationTypes|phoneType|emailType|postalType|imType|orgType|relationType|eventType|'
                           r'sipAddressType)')
CONTACT_DOMAINS = {'vibeos-contacts', 'vibeos-dialer', 'vibeos-messaging'}
# a message's state ("Downloading…" is a message's), for the apps with messages only
MESSAGE_STATES = re.compile(r'^message_status_')
MESSAGE_DOMAINS = {'vibeos-messaging', 'vibeos-dialer'}
PLACEHOLDER = re.compile(r"%(?:(\d+)\$)?([-+ 0#']*\d*(?:\.\d+)?(?:hh|h|ll|l|z|j|t|L)?)([sdiuxXfFeEgGc%])")


# ---- Android's resources ----

def flat(el):
    """an element's text with its xliff:g wrappers taken off; None when it holds other markup"""
    parts = [el.text or '']
    for c in el:
        if c.tag != XLIFF:
            return None
        inner = flat(c)
        if inner is None:
            return None
        parts += [inner, c.tail or '']
    return ''.join(parts)


def android_unescape(s):
    """Android's resource text as shown: quoted text keeps its spaces, other text has them collapsed"""
    s = s.strip()
    if len(s) >= 2 and s[0] == '"' and s[-1] == '"':
        s = s[1:-1]
    else:
        s = re.sub(r'[ \t\n]+', ' ', s)
    s = re.sub(r'\\u([0-9a-fA-F]{4})', lambda m: chr(int(m.group(1), 16)), s)
    return re.sub(r'\\(.)', lambda m: {'n': '\n', 't': '\t'}.get(m.group(1), m.group(1)), s)


def parse(path):
    strings, plurals = {}, {}
    try:
        root = ET.parse(path).getroot()
    except (ET.ParseError, OSError):
        return strings, plurals
    for el in root:
        name = el.get('name')
        if not name or el.get('translatable') == 'false':
            continue
        if el.tag == 'string':
            t = flat(el)
            if t is not None:
                strings[name] = android_unescape(t)
        elif el.tag in ('string-array', 'array'):
            for i, it in enumerate(el):
                t = flat(it)
                if t is not None:
                    strings[f'{name}[{i}]'] = android_unescape(t)
        elif el.tag == 'plurals':
            forms = {}
            for it in el:
                t = flat(it)
                if t is not None:
                    forms[it.get('quantity')] = android_unescape(t)
            if forms:
                plurals[name] = forms
    return strings, plurals


ICU_FORM = re.compile(r'\s*(=\d+|zero|one|two|few|many|other)\s*\{')


def icu_plural(text):
    """the forms of a string in ICU's plural syntax, which newer Android strings use ("{count, plural, =1 {# day}
    other {# days}}"): {quantity: text}, the number '#' made %d; None for any other string, and for one whose forms
    hold arguments of their own"""
    m = re.match(r'^\{\s*\w+\s*,\s*plural\s*,', text)
    if not m or not text.endswith('}'):
        return None
    i, n, forms = m.end(), len(text) - 1, {}
    while i < n:
        q = ICU_FORM.match(text, i)
        if not q:
            if text[i:n].strip():
                return None
            break
        j = start = q.end()
        depth = 1
        while j < n and depth:
            depth += {'{': 1, '}': -1}.get(text[j], 0)
            j += 1
        body = text[start:j - 1]
        if depth or '{' in body or '%' in body.replace('%%', ''):
            return None
        forms[q.group(1)] = body.replace("''", "'").replace('#', '%d')
        i = j
    return forms if 'other' in forms else None


def plural_form(forms, q, lang):
    """the text of Android's plural @forms for the gettext form that @q names; None if there is none"""
    if q != 'one':
        return forms.get(q) or forms.get('other')
    # ICU's "=1" is n = 1 alone: right for a form that is n = 1 alone, or that shows its number
    exact = forms.get('=1')
    if exact is not None and lang in SINGLE_ONE:
        return exact
    return forms.get('one') or (exact if exact is not None and placeholders(exact) else None) or forms.get('other')


def city(text):
    """DeskClock's cities.xml value "A=Abidjan|Africa/Abidjan": the name"""
    m = re.match(r'^[^=]*=([^|]+)\|', text)
    return m.group(1).strip() if m else text


class Android:
    def __init__(self, cache):
        # source -> norm(english) -> [(name, {lang: translation})]
        self.singles = collections.defaultdict(lambda: collections.defaultdict(list))
        self.loose = collections.defaultdict(lambda: collections.defaultdict(list))
        # source -> norm(english "other") -> [(name, {quantity: english}, {lang: {quantity: translation}})]
        self.plurals = collections.defaultdict(lambda: collections.defaultdict(list))
        for source in sorted(os.listdir(cache)):
            base = os.path.join(cache, source)
            if not os.path.isdir(base):
                continue
            for dp, _dns, fns in os.walk(base):
                if os.path.basename(dp) != 'values':
                    continue
                for fn in fns:
                    if fn.endswith('.xml'):
                        self._add(source, os.path.dirname(dp), fn)

    def _add(self, source, resdir, fn):
        en_s, en_p = parse(os.path.join(resdir, 'values', fn))
        tr = {lang: parse(os.path.join(resdir, f'values-{lang}', fn)) for lang in LANGS}
        cities = fn == 'cities.xml'
        for name, text in en_s.items():
            ts = {lang: tr[lang][0][name] for lang in LANGS if tr[lang][0].get(name)}
            en_forms = icu_plural(text)
            if en_forms is not None:
                ts = {lang: f for lang, f in ((lang, icu_plural(t)) for lang, t in ts.items()) if f}
                one = en_forms.get('=1', en_forms.get('one'))
                if ts:
                    self.plurals[source][norm(en_forms['other'])].append(
                        (name, {'other': en_forms['other'], **({'one': one} if one else {})}, ts))
                continue
            if cities:
                text, ts = city(text), {lang: city(t) for lang, t in ts.items()}
            if not text.strip() or not ts:
                continue
            self.singles[source][norm(text)].append((name, text, ts))
            self.loose[source][loose(text)].append((name, text, ts))
        for name, forms in en_p.items():
            other = forms.get('other')
            if not other:
                continue
            ts = {lang: tr[lang][1][name] for lang in LANGS if tr[lang][1].get(name)}
            if ts:
                self.plurals[source][norm(other)].append((name, forms, ts))


# ---- matching ----

def norm(s):
    s = s.replace('’', "'").replace('‘', "'").replace('“', '"').replace('”', '"')
    s = s.replace('‑', '-').replace('‐', '-').replace('...', '…')
    s = s.replace(' ', ' ').replace(' ', ' ')
    s = PLACEHOLDER.sub(lambda m: '%%' if m.group(3) == '%' else
                        ('%d' if m.group(3) in 'diuxXc' else '%f' if m.group(3) in 'fFeEgG' else '%s'), s)
    return re.sub(r'\s+', ' ', s).strip()


def loose(s):
    s = norm(s).lower().replace('&', 'and')
    return re.sub(r'[.:!]+$', '', s).strip()


def placeholders(s):
    """[(argument number or None, conversion)] of a format string, %% left out"""
    return [(int(m.group(1)) if m.group(1) else None, m.group(3)) for m in PLACEHOLDER.finditer(s)
            if m.group(3) != '%']


def kind(conv):
    return 'd' if conv in 'diuxXc' else 'f' if conv in 'fFeEgG' else 's'


def convert_placeholders(ours, en, tr, js):
    """the translation @tr of Android's @en with our string @ours's placeholders, or None"""
    our_specs = [m.group(0) for m in PLACEHOLDER.finditer(ours) if m.group(3) != '%']
    en_ph = placeholders(en)
    if len(our_specs) != len(en_ph):
        return None
    if not our_specs:
        return None if '%' in tr.replace('%%', '') and '%' not in ours else tr
    # English placeholder k is argument arg_of[k]
    arg_of = [a if a is not None else k + 1 for k, (a, _c) in enumerate(en_ph)]
    if sorted(arg_of) != list(range(1, len(arg_of) + 1)):
        return None
    spec_of_arg = {}
    for k, a in enumerate(arg_of):
        if kind(en_ph[k][1]) != kind(PLACEHOLDER.match(our_specs[k]).group(3)):
            return None
        spec_of_arg[a] = k                          # argument a is our k-th placeholder
    tr_ph = list(PLACEHOLDER.finditer(tr))
    tr_ph = [m for m in tr_ph if m.group(3) != '%']
    used = [int(m.group(1)) if m.group(1) else i + 1 for i, m in enumerate(tr_ph)]
    if sorted(used) != list(range(1, len(arg_of) + 1)):
        return None
    order = [spec_of_arg[a] for a in used]          # our placeholder index of each, in the translation's order
    in_order = order == list(range(len(order)))
    if not in_order and js:
        return None
    out, last = [], 0
    for m, k in zip(tr_ph, order):
        out.append(tr[last:m.start()])
        spec = our_specs[k]
        out.append(spec if in_order else '%' + f'{k + 1}$' + spec[1:])
        last = m.end()
    out.append(tr[last:])
    return ''.join(out)


def adapt(ours, en, tr, lang):
    """@tr adapted to the differences between our English @ours and Android's @en; None if it cannot be"""
    if '“' in ours and '"' in en and '"' in tr:
        o, c = QUOTES[lang]
        tr = re.sub(r'"([^"]*)"', lambda m: o + m.group(1) + c, tr)
    if '…' in ours:
        tr = tr.replace('...', '…')
    o, e = ours.rstrip(), en.rstrip()
    if o.endswith('.') and not e.endswith('.') and not re.search(r'[.!?…]$', tr):
        tr += '.'
    elif e.endswith('.') and not o.endswith('.') and tr.endswith('.') and not tr.endswith('...'):
        tr = tr[:-1]
    if o.endswith(':') and not e.endswith(':') and not tr.endswith(':'):
        tr += ' :' if lang == 'fr' else ':'
    elif e.endswith(':') and not o.endswith(':'):
        tr = re.sub(r'[  ]?:$', '', tr)
    oc, ec = first_letter(ours), first_letter(en)
    if oc and ec and oc.isupper() != ec.isupper():
        tc = first_letter(tr)
        if not tc:
            return None
        if oc.isupper():
            tr = recase(tr, str.upper)
        elif lang == 'de':
            return None                              # a German noun keeps its capital
        else:
            tr = recase(tr, str.lower)
    return tr


def first_letter(s):
    """the letter a string starts with (after quotes and brackets), or None when it starts otherwise"""
    m = re.match(r'^[\s"“„«( ]*([^\W\d_])', s)
    return m.group(1) if m else None


def recase(s, f):
    m = re.search(r'[^\W\d_]', s)
    return s[:m.start()] + f(s[m.start()]) + s[m.start() + 1:] if m else s


def has_markup_mismatch(ours, tr):
    return set(re.findall(r'</?\w+', ours)) != set(re.findall(r'</?\w+', tr))


class Importer:
    def __init__(self, android, review):
        self.a = android
        self.review = review

    def sources(self, domain):
        prefer = PREFER.get(domain, [])
        return prefer + [s for s in DEFAULT if s not in prefer] + \
            [s for s in sorted(self.a.singles) if s not in prefer and s not in DEFAULT]

    def single(self, domain, e, lang, js):
        """the translation of @e from Android: of the strings with its English, the translation most of them have
        (the component's own apps counting twice, and a string named for its English, Framework's "ok" for "OK",
        five times more), else the first of the component's apps' order"""
        prefer = PREFER.get(domain, [])
        order = self.sources(domain)
        own_name = re.sub(r'\W+', '_', e.msgid.lower()).strip('_')
        for how, table, key in (('exact', self.a.singles, norm(e.msgid)), ('adapted', self.a.loose, loose(e.msgid))):
            votes, first = collections.Counter(), {}
            for rank, source in enumerate(order):
                for name, en, ts in table[source].get(key, []):
                    if lang not in ts or EXCLUDE_NAMES.match(name) or \
                            domain not in CONTACT_DOMAINS and CONTACT_TYPES.match(name) or \
                            domain not in MESSAGE_DOMAINS and MESSAGE_STATES.match(name):
                        continue
                    tr = convert_placeholders(e.msgid, en, ts[lang], js)
                    if tr is None:
                        continue
                    tr = adapt(e.msgid, en, tr, lang)
                    if tr is None or has_markup_mismatch(e.msgid, tr) or '\n' in e.msgid and '\n' not in tr and \
                            len(e.msgid) > 60:
                        continue
                    votes[tr] += (2 if source in prefer else 1) * (5 if name.lower() == own_name else 1)
                    first.setdefault(tr, (rank, source, name, en))
            if not votes:
                continue
            tr = max(votes, key=lambda t: (votes[t], -first[t][0]))
            _rank, source, name, en = first[tr]
            notes = []
            if how == 'adapted':
                notes.append('adapted')
            if source not in prefer and prefer:
                notes.append('other app')
            if len(votes) > 1:
                notes.append('several translations')
            if len(e.msgid.split()) <= 2 and not placeholders(e.msgid) and (source not in prefer[:1] or len(votes) > 1):
                notes.append('short')
            if re.search(r'["“”„«»]', tr) and not re.search(r'["“”„«»]', e.msgid + en):
                notes.append('quotes added')
            return tr, f'{source} {name}', notes
        return None

    def plural(self, domain, e, lang, js):
        prefer = PREFER.get(domain, [])
        for source in self.sources(domain):
            for name, en, ts in self.a.plurals[source].get(norm(e.msgid_plural), []):
                if lang not in ts or ('one' in en and norm(en['one']) != norm(e.msgid) and
                                      loose(en['one']) != loose(e.msgid)):
                    continue
                forms = []
                for i, q in enumerate(PLURAL_QUANTITIES[lang]):
                    t = plural_form(ts[lang], q, lang)
                    if not t:
                        break
                    ours = e.msgid if i == 0 else e.msgid_plural
                    eng = en.get('one', en['other']) if i == 0 else en['other']
                    if i == 0 and lang in SINGLE_ONE and not placeholders(t):
                        c = t                               # "one call": the form for n = 1 alone
                    else:
                        c = convert_placeholders(e.msgid_plural, en['other'], t, js)
                    if c is None:
                        break
                    forms.append(adapt(ours, eng, c, lang) or c)
                else:
                    notes = ['other app'] if source not in prefer and prefer else []
                    return forms, f'{source} {name}', notes
        return None

    def run(self, srcdir):
        c = Component(srcdir)
        js = any(p.endswith('.js') for p in c.potfiles())
        totals = collections.Counter()
        for lang in c.languages():
            path = c.po(lang)
            if not os.path.exists(path):
                continue
            f = PoFile(path)
            for e in f.messages():
                totals['strings', lang] += 1
                if e.translated() or e.fuzzy():
                    totals['had', lang] += 1
                    continue
                got = self.plural(c.domain, e, lang, js) if e.plural else self.single(c.domain, e, lang, js)
                if not got:
                    continue
                tr, where, notes = got
                e.msgstr = tr
                e.comments = [x for x in e.comments if not x.startswith('android:')] + \
                    [f'android: {where}' + (f' ({", ".join(notes)})' if notes else '')]
                e.changed = True
                totals['taken', lang] += 1
                if notes and self.review:
                    shown = ' | '.join(tr) if e.plural else tr
                    self.review.write('\t'.join([c.domain, lang, ', '.join(notes), e.msgctxt or '',
                                                 e.msgid.replace('\n', '\\n'), shown.replace('\n', '\\n'),
                                                 where]) + '\n')
            f.write()
        langs = c.languages()
        print(f'{c.domain}: ' + ', '.join(f'{lang} {totals["taken", lang]}+{totals["had", lang]}/'
                                         f'{totals["strings", lang]}' for lang in langs))


def main():
    args = sys.argv[1:]
    review = None
    if args[:1] == ['--review']:
        review = open(args[1], 'a', encoding='utf-8')
        args = args[2:]
    importer = Importer(Android(args[0]), review)
    for srcdir in args[1:]:
        importer.run(srcdir)


main()
