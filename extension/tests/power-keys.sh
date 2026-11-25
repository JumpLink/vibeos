#!/bin/sh
# power-keys.sh SOURCE_DIR: the power key's handlers (power.js, chord.js, keylaunch.js) driven directly, as the shell
# would call them for presses and releases, with no key ever sent: no KEY_POWER reaches any input device, so neither
# logind nor a phone can take one for a real press (a held one can power a phone off). GNOME Shell's own modules are
# stand-ins (mock/), GLib and Gio are real, on a bus of the test's own (dbus-run-session), where the test is Agent,
# and UPower for a tablet's cover.
set -eu
src=$1
here=$(cd "$(dirname "$0")" && pwd)
t=$(mktemp -d)
trap 'rm -rf "${t:?}"' EXIT
mkdir "$t/schemas"
cp "$here/mock/shell.js" "$here/power-keys.js" "$t/"
sh "$here/mock/modules.sh" "$t"
for f in power.js chord.js keylaunch.js; do
	sed -E -e "s#'gi://(Clutter|Meta|Shell|St)'#'./gi-\1.js'#" \
	    -e "s#'resource:///org/gnome/shell/(ui|misc)/([a-z]+\.js)'#'./\2'#" "$src/$f" > "$t/$f"
done
cp "$src/../vibeos-launcher/data/vibeos.Shell.gschema.xml" "$t/schemas/"
glib-compile-schemas "$t/schemas"
# nothing of the session's: no GVFS daemon started on the test's bus, the runtime directory the test's own
mkdir -m 700 "$t/run"
# the system bus is the test's own as well: UPower there is the test's (the cover), and nothing reaches the machine's
GSETTINGS_SCHEMA_DIR="$t/schemas" GSETTINGS_BACKEND=memory GIO_USE_VFS=local XDG_RUNTIME_DIR="$t/run" \
	dbus-run-session -- sh -c 'DBUS_SYSTEM_BUS_ADDRESS=$DBUS_SESSION_BUS_ADDRESS exec gjs -m "$1"' sh "$t/power-keys.js"
