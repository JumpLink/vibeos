#!/bin/sh
# back-probe.sh SOURCE_DIR: the back gesture's question to an app (a11y.js's BackProbe: is a Back button showing?)
# asked of a real libadwaita app over a real accessibility bus, the shell in one language and the app in another, as
# after Settings changed the session's language: the shell (back-probe.js) in German, the app (back-probe-client.js)
# in Russian. libadwaita names its back button in the app's language ("Назад"), which the shell's own word for it
# ("Zurück") is not.
#
#   pushed       a page pushed, its back button showing: the app's (button)
#   first page   none: back leaves the app (Android's rule for an app at its first screen)
#   LANGUAGE     the app's LANG German, its LANGUAGE Russian (gettext reads LANGUAGE first): the button
#   own catalog  a button of the app's own named from its own catalog (as VibeOS's apps do), the catalog where the
#                shell's XDG_DATA_DIRS has it: the button
#   started over the probe knowing the app by a name it no longer has (Settings started over in place after a language
#                change): it asks again, and finds the button
#   same language  shell and app in German: the button (as before)
#
# Not run by meson: it starts a Wayland compositor (mutter, else weston, headless), the accessibility bus and GTK
# apps; needs gjs with GTK 4 and libadwaita, dbus-daemon, localedef with the de_DE and ru_RU sources, msgfmt, and
# libadwaita's German and Russian catalogs. Everything it runs is its own: a session bus, the runtime directory, HOME
# and XDG directories in a temporary directory, settings in memory; it can run beside a session.
set -eu
src=$(cd "$1" && pwd)
here=$(cd "$(dirname "$0")" && pwd)
skip() { echo "skipped: $1"; exit 77; }
if command -v mutter >/dev/null; then
	compositor='mutter --headless --wayland --no-x11 --wayland-display=wl-back --virtual-monitor 1080x2280'
elif command -v weston >/dev/null; then
	compositor='weston --backend=headless --shell=kiosk --socket=wl-back --no-config'
else
	skip "no mutter or weston"
fi
for p in gjs dbus-run-session localedef msgfmt; do command -v $p >/dev/null || skip "no $p"; done
for l in de ru; do
	[ -f /usr/share/locale/$l/LC_MESSAGES/libadwaita.mo ] || skip "no libadwaita catalog for $l"
done
# (a short directory: the compositor's and the buses' sockets are in it)
t=$(mktemp -d /tmp/back-probe.XXXXXX)
trap 'rm -rf "${t:?}"' EXIT
mkdir "$t/home" "$t/locale" "$t/share"
mkdir -m 700 "$t/run"
for l in de_DE ru_RU; do
	localedef -i $l -f UTF-8 "$t/locale/$l.UTF-8" 2>/dev/null || [ -d "$t/locale/$l.UTF-8" ] || skip "no locale $l"
done
mkdir -p "$t/share/locale/ru/LC_MESSAGES"
printf 'msgid ""\nmsgstr "Content-Type: text/plain; charset=UTF-8\\n"\n\nmsgid "Back"\nmsgstr "К списку"\n' > "$t/test.po"
msgfmt -o "$t/share/locale/ru/LC_MESSAGES/vibeos-backtest.mo" "$t/test.po"
cp "$src/a11y.js" "$here/back-probe.js" "$here/back-probe-client.js" "$t/"
cat > "$t/run.sh" <<'RUN'
# inside the test's bus: the compositor, then each case's app and probe
t=$1
$COMPOSITOR > "$t/compositor.log" 2>&1 &
c=$!
for i in $(seq 100); do [ -S "$t/run/wl-back" ] && break; sleep 0.1; done
[ -S "$t/run/wl-back" ] || { echo "FAIL the compositor did not start"; cat "$t/compositor.log"; kill $c; exit 1; }
fails=0
# case NAME APP-ENV... -- PROBE-LANG WANT [stale]
case_() {
	name=$1
	shift
	env_=
	while [ "$1" != -- ]; do env_="$env_ $1"; shift; done
	shift
	rm -f "$t/app.out"
	env WAYLAND_DISPLAY=wl-back GDK_BACKEND=wayland BACKPROBE_LOCALEDIR="$t/share/locale" $env_ \
		gjs -m "$t/back-probe-client.js" > "$t/app.out" 2>&1 &
	app=$!
	for i in $(seq 150); do grep -q READY "$t/app.out" 2>/dev/null && break; sleep 0.1; done
	grep -q READY "$t/app.out" || { echo "FAIL $name: the app did not start"; cat "$t/app.out"; }
	LANG=$1 XDG_DATA_DIRS="$t/share:/usr/local/share:/usr/share" gjs -m "$t/back-probe.js" $app "$2" "${3:--}" \
		"$name" || fails=$((fails + 1))
	kill $app 2>/dev/null
	wait $app 2>/dev/null
}
case_ pushed LANG=ru_RU.UTF-8 BACKPROBE_PAGE=pushed -- de_DE.UTF-8 button
case_ 'first page' LANG=ru_RU.UTF-8 -- de_DE.UTF-8 none
case_ LANGUAGE LANG=de_DE.UTF-8 LANGUAGE=ru BACKPROBE_PAGE=pushed -- de_DE.UTF-8 button
case_ 'own catalog' LANG=ru_RU.UTF-8 BACKPROBE_OWN=1 -- de_DE.UTF-8 button
case_ 'started over' LANG=ru_RU.UTF-8 BACKPROBE_PAGE=pushed -- de_DE.UTF-8 button stale
case_ 'same language' LANG=de_DE.UTF-8 BACKPROBE_PAGE=pushed -- de_DE.UTF-8 button
kill $c
exit $fails
RUN
env -i PATH=/usr/bin:/bin HOME="$t/home" XDG_RUNTIME_DIR="$t/run" XDG_CONFIG_HOME="$t/home/.config" \
	XDG_DATA_HOME="$t/home/.local/share" XDG_CACHE_HOME="$t/home/.cache" LOCPATH="$t/locale" LANG=C.UTF-8 \
	GSETTINGS_BACKEND=memory GIO_USE_VFS=local GDK_DEBUG=no-portals COMPOSITOR="$compositor" \
	${GI_TYPELIB_PATH:+GI_TYPELIB_PATH="$GI_TYPELIB_PATH"} ${LD_LIBRARY_PATH:+LD_LIBRARY_PATH="$LD_LIBRARY_PATH"} \
	dbus-run-session -- sh "$t/run.sh" "$t" 2> "$t/stderr.log" && status=0 || status=$?
[ $status = 0 ] && echo "ALL PASSED" || { echo "$status FAILED"; tail -20 "$t/stderr.log"; }
exit $status
