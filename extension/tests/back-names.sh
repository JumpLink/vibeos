#!/bin/sh
# back-names.sh SOURCE_DIR: what a Back button is called in an app's language (a11y.js: catalogLanguages, translation,
# BackNames), read by back-names.js from catalogs this script compiles: gettext's directories for a locale, a word
# out of a .mo (either byte order, a plural's first form, an entry with a context left out) and the words of a
# language from the catalogs a Back button's name comes from, none from another catalog. Needs gjs and msgfmt; no
# bus, no display.
set -eu
src=$(cd "$1" && pwd)
here=$(cd "$(dirname "$0")" && pwd)
command -v msgfmt >/dev/null || { echo "no msgfmt: skipped"; exit 77; }
t=$(mktemp -d)
trap 'rm -rf "${t:?}"' EXIT
po() {   # po FILE.mo [msgfmt options] < PO
	cat > "$t/in.po"
	mkdir -p "$(dirname "$1")"
	out=$1
	shift
	msgfmt "$@" -o "$out" "$t/in.po"
}
head='msgid ""
msgstr "Content-Type: text/plain; charset=UTF-8\n"
'
# a VibeOS app's catalog: the word, an entry with a context, and another message
printf '%s\nmsgid "Back"\nmsgstr "Назад"\n\nmsgctxt "card"\nmsgid "Back"\nmsgstr "Оборот"\n\nmsgid "Next"\nmsgstr "Далее"\n' \
	"$head" | po "$t/locale/ru/LC_MESSAGES/vibeos-test.mo"
# the same, big-endian
printf '%s\nmsgid "Back"\nmsgstr "Назад"\n' "$head" | po "$t/big.mo" --endianness=big
# a plural
printf '%s\nmsgid "Back"\nmsgid_plural "Backs"\nmsgstr[0] "Взад"\nmsgstr[1] "Взадов"\n' "$head" | \
	po "$t/locale/ru_UA/LC_MESSAGES/libadwaita.mo"
# untranslated
printf '%s\nmsgid "Back"\nmsgstr ""\n\nmsgid "Next"\nmsgstr "Weiter"\n' "$head" | po "$t/locale/de/LC_MESSAGES/gtk40.mo"
printf '%s\nmsgid "Back"\nmsgstr "Zurück"\n' "$head" | po "$t/locale/de/LC_MESSAGES/libvibeos-1.mo"
# a catalog no Back button's name comes from
printf '%s\nmsgid "Back"\nmsgstr "Спина"\n' "$head" | po "$t/locale/ru/LC_MESSAGES/anatomy.mo"
cp "$here/back-names.js" "$src/a11y.js" "$t/"
env -u LANGUAGE -u LC_ALL -u LC_MESSAGES LANG=C.UTF-8 gjs -m "$t/back-names.js" "$t"
