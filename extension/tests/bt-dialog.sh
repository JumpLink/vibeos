#!/bin/sh
# bt-dialog.sh SOURCE_DIR [SHELL] [SHOTS]: Bluetooth's pairing dialogs (pairdialog.js) in a headless GNOME Shell of the
# test's own (bt-dialog.js, its automation script): each kind of question, typed into with the extension's keyboard
# and tapped with a virtual touchscreen of that shell's; a question asked while locked waiting behind a notification;
# the agent (bluetooth.js) made and taken down on the stand-in system bus, where no BlueZ is. SHELL as in
# keyboard-stale.sh (/opt/gnome-mobile/bin/gnome-shell with GSETTINGS_SCHEMA_DIR=/opt/gnome-mobile/share/glib-2.0/
# schemas on a VibeOS phone); pictures of the dialogs go to SHOTS when given.
#
# Not run by meson: it starts a whole shell. Everything it runs is its own (a session bus and a stand-in system bus,
# the runtime directory, HOME and XDG directories in a temporary directory, settings in memory): it never touches a
# session that is running, nor the system's BlueZ, and it can run beside one, as the phone's user.
set -eu
src=$(cd "$1" && pwd)
shell=${2:-gnome-shell}
shots=${3:-}
here=$(cd "$(dirname "$0")" && pwd)
t=$(mktemp -d)
bus=
trap 'set +e; [ -z "$bus" ] || kill "$bus" 2>/dev/null; rm -rf "${t:?}"' EXIT
mkdir "$t/schemas" "$t/home" "$t/data" "$t/config" "$t/cache" "$t/shots"
mkdir -m 700 "$t/run"
cp -a "$src" "$t/ext"
cp "$here/bt-dialog.js" "$t/"
cp "$src/../vibeos-launcher/data/vibeos.Shell.gschema.xml" "$t/schemas/"
glib-compile-schemas "$t/schemas"
dbus-daemon --session --address="unix:path=$t/run/system_bus" --nofork --nopidfile >/dev/null 2>&1 &
bus=$!
sleep 0.5
env -i PATH=/usr/bin:/bin HOME="$t/home" XDG_RUNTIME_DIR="$t/run" XDG_DATA_HOME="$t/data" \
	XDG_CONFIG_HOME="$t/config" XDG_CACHE_HOME="$t/cache" XDG_DATA_DIRS=/usr/local/share:/usr/share \
	GSETTINGS_BACKEND=memory GSETTINGS_SCHEMA_DIR="$t/schemas${GSETTINGS_SCHEMA_DIR:+:$GSETTINGS_SCHEMA_DIR}" \
	${GI_TYPELIB_PATH:+GI_TYPELIB_PATH="$GI_TYPELIB_PATH"} ${LD_LIBRARY_PATH:+LD_LIBRARY_PATH="$LD_LIBRARY_PATH"} \
	LANG=C.UTF-8 NO_AT_BRIDGE=1 GIO_USE_VFS=local GDK_DEBUG=no-portals VIBEOS_DENSITY=3 \
	DBUS_SYSTEM_BUS_ADDRESS="unix:path=$t/run/system_bus" \
	dbus-run-session -- timeout -k 10 1800 "$shell" --headless --wayland --no-x11 --virtual-monitor 1080x2280 \
	--automation-script "$t/bt-dialog.js" > "$t/shell.log" 2>&1 || true
[ -z "$shots" ] || { mkdir -p "$shots" && cp "$t"/shots/*.png "$shots/" 2>/dev/null || true; }
grep '^TEST' "$t/shell.log" || true
if grep -q '^TEST ALL PASSED' "$t/shell.log"; then
	exit 0
fi
grep -A8 'JS ERROR\|Script failed' "$t/shell.log" || tail -30 "$t/shell.log"
exit 1
