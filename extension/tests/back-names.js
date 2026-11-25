// What a Back button is called in an app's language (back-names.sh compiled the catalogs under ARGV[0]): a11y.js's
// catalogLanguages, translation and BackNames
import GLib from 'gi://GLib';
import System from 'system';

import {BackNames, catalogLanguages, translation} from './a11y.js';

const dir = ARGV[0];
let failures = 0;
function check(name, ok, detail = '') {
    print(ok ? 'PASS' : 'FAIL', name, detail);
    if (!ok)
        failures++;
}
const same = (a, b) => JSON.stringify(a) === JSON.stringify(b);
const sorted = set => [...set].sort();

// ---- the directories gettext reads ----
for (const [locale, language, want] of [
    ['ru_UA.UTF-8', '', ['ru_UA', 'ru']],
    ['de_DE.UTF-8', 'ru:fr_CA', ['ru', 'fr_CA', 'fr', 'de_DE', 'de']],
    ['sr_RS.UTF-8@latin', '', ['sr_RS@latin', 'sr_RS', 'sr@latin', 'sr']],
    ['nl', '', ['nl']],
    ['C.UTF-8', 'ru', []],
    ['C', 'ru', []],
    ['POSIX', '', []],
    [null, 'ru', []],
]) {
    const got = catalogLanguages(locale, language);
    check(`catalogs for ${locale} (LANGUAGE ${language || 'unset'})`, same(got, want), JSON.stringify(got));
}

// ---- a word out of a catalog ----
const bytes = path => GLib.file_get_contents(path)[1];
check('a word', translation(bytes(`${dir}/locale/ru/LC_MESSAGES/vibeos-test.mo`), 'Back') === 'Назад');
check('another word', translation(bytes(`${dir}/locale/ru/LC_MESSAGES/vibeos-test.mo`), 'Next') === 'Далее');
check('big-endian', translation(bytes(`${dir}/big.mo`), 'Back') === 'Назад');
check("a plural's first form", translation(bytes(`${dir}/locale/ru_UA/LC_MESSAGES/libadwaita.mo`), 'Back') === 'Взад');
check('untranslated', translation(bytes(`${dir}/locale/de/LC_MESSAGES/gtk40.mo`), 'Back') === null);
check('not there', translation(bytes(`${dir}/locale/de/LC_MESSAGES/gtk40.mo`), 'Forward') === null);
check('not a catalog', translation(new TextEncoder().encode('Back to the start of a file that is no catalog'),
    'Back') === null);
check('cut short', translation(bytes(`${dir}/big.mo`).subarray(0, 40), 'Back') === null);

// ---- a language's words ----
const loop = new GLib.MainLoop(null, false);
const names = new BackNames([`${dir}/nowhere`, `${dir}/locale`]);
(async () => {
    const ru = await names.names('ru_UA.UTF-8');
    check("Russian (Ukraine): its own catalogs' and Russian's, and English", same(sorted(ru), ['Back', 'Взад', 'Назад']),
        JSON.stringify(sorted(ru)));
    const de = await names.names('de_DE.UTF-8', 'ru');
    check('German with LANGUAGE ru: both', same(sorted(de), ['Back', 'Zurück', 'Назад']), JSON.stringify(sorted(de)));
    const c = await names.names('C.UTF-8', 'ru');
    check('C: English only', same(sorted(c), ['Back']), JSON.stringify(sorted(c)));
    const none = await names.names('fi_FI.UTF-8');
    check('a language with no catalogs: English', same(sorted(none), ['Back']), JSON.stringify(sorted(none)));
    // (a language is read once)
    check('read once', names._languages.get('ru') === names._languages.get('ru') && names._languages.size === 6,
        [...names._languages.keys()].join(' '));
})().catch(e => check('no error', false, `${e}\n${e.stack}`)).finally(() => loop.quit());
loop.run();

print(failures ? `${failures} FAILED` : 'ALL PASSED');
System.exit(failures ? 1 : 0);
