// The extension's words in the session's language, and its dates and numbers in that language's own forms.
//
// Words: gettext's domain vibeos-shell, metadata.json's gettext-domain, which GNOME Shell binds as it makes the
// extension (ExtensionBase.initTranslations: the system's locale directory, or a locale/ next to the extension). The
// modules translate through this one rather than the shell's gettext wrapper, which finds the extension from the
// caller's stack: the tests load the modules from outside any extension directory. A string a module sets as it loads
// is marked N_() there and translated where it is shown, by then in the bound domain.
//
// Dates and numbers: ICU's forms for the language (Intl), the ones Android's DateFormat and NumberFormat give: English
// "Mon, Nov 24" is "Mo., 24. Nov." in German, 1.5 is 1,5 and 75% is "75 %".
import GLib from 'gi://GLib';

const DOMAIN = 'vibeos-shell';

export const _ = text => GLib.dgettext(DOMAIN, text);
export const N_ = text => text;
export const ngettext = (one, other, n) => GLib.dngettext(DOMAIN, one, other, n);
export const pgettext = (context, text) => GLib.dpgettext2(DOMAIN, context, text);

// Android's EEEMMMd ("Mon, Nov 24") and EEEEMMMMd ("Monday, November 24")
export const DATE_SHORT = {weekday: 'short', month: 'short', day: 'numeric'};
export const DATE_LONG = {weekday: 'long', month: 'long', day: 'numeric'};

// the language of a locale category (LC_TIME, LC_NUMERIC) as a BCP 47 tag, de_DE.UTF-8 as de-DE; English for C and
// POSIX, and for a language ICU does not have
function tag(category) {
    const name = GLib.get_language_names_with_category(category)[0] ?? 'C';
    const m = /^([a-z]{2,3})(?:_([A-Z]{2}))?(?:[.@]|$)/.exec(name);
    const found = m ? m[2] ? `${m[1]}-${m[2]}` : m[1] : null;
    return found && Intl.NumberFormat.supportedLocalesOf([found]).length ? found : 'en-US';
}

// the formatters, made once each (the session's language does not change under a running shell)
const formatters = new Map();
function formatter(kind, options) {
    const key = kind + JSON.stringify(options);
    let made = formatters.get(key);
    if (!made) {
        made = kind === 'date'
            ? new Intl.DateTimeFormat(tag('LC_TIME'), {...options, timeZone: 'UTC'})
            : new Intl.NumberFormat(tag('LC_NUMERIC'), options);
        formatters.set(key, made);
    }
    return made;
}

// the day of a GLib.DateTime with the fields `options` names (DATE_SHORT, DATE_LONG). Intl is given the same day at
// noon in UTC, so its own idea of the time zone, which can lag GLib's after a change, plays no part
export function formatDate(datetime, options) {
    const day = new Date(Date.UTC(datetime.get_year(), datetime.get_month() - 1, datetime.get_day_of_month(), 12));
    return formatter('date', options).format(day);
}

// `n` with `digits` places after the language's decimal sign
export function formatNumber(n, digits = 0) {
    return formatter('number', {minimumFractionDigits: digits, maximumFractionDigits: digits}).format(n);
}

// a level of 0 to 100 as the language writes a percentage
export function formatPercent(level) {
    return formatter('number', {style: 'percent', maximumFractionDigits: 0}).format(level / 100);
}
