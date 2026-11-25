#!/bin/sh
# mic-prompt.sh SOURCE_DIR [SHELL] [SHOTS]: the microphone's question (micprompt.js) in a headless GNOME Shell of the
# test's own (mic-prompt.js, its automation script): the dialog for an app that sandbox-access.lua holds a stream of
# (a bus name, here owned by the script), its buttons tapped with a virtual touchscreen of that shell's, and what it
# writes to a permission store of the script's own. SHELL as in keyboard-stale.sh (/opt/gnome-mobile/bin/gnome-shell
# with GSETTINGS_SCHEMA_DIR=/opt/gnome-mobile/share/glib-2.0/schemas on a VibeOS phone); pictures of the dialog go to
# SHOTS when given.
#
# Not run by meson: it starts a whole shell. Everything it runs is its own (a session bus and a stand-in system bus,
# the runtime directory, HOME and XDG directories in a temporary directory, settings in memory): it never touches a
# session that is running, and can run beside one, as the phone's user.
set -eu
src=$(cd "$1" && pwd)
shell=${2:-gnome-shell}
shots=${3:-}
here=$(cd "$(dirname "$0")" && pwd)
t=$(mktemp -d)
bus=
trap 'set +e; [ -z "$bus" ] || kill "$bus" 2>/dev/null; rm -rf "${t:?}"' EXIT
mkdir "$t/schemas" "$t/home" "$t/data" "$t/config" "$t/cache" "$t/shots" "$t/data/applications" "$t/data/dbus-1" \
	"$t/data/dbus-1/services"
mkdir -m 700 "$t/run"
cp -a "$src" "$t/ext"
cp "$here/mic-prompt.js" "$t/"
cp "$src/../vibeos-launcher/data/vibeos.Shell.gschema.xml" "$t/schemas/"
# the app's desktop file, as vibeos-sandbox writes it (<app id>.desktop): its Name is what the question calls it
printf '[Desktop Entry]\nType=Application\nName=Voice Recorder\nExec=true\n' > "$t/data/applications/org.example.Voice.desktop"
# the real permission store is not started for the shell's own lookups: the script's stand-in takes its name
printf '[D-BUS Service]\nName=org.freedesktop.impl.portal.PermissionStore\nExec=/bin/false\n' \
	> "$t/data/dbus-1/services/org.freedesktop.impl.portal.PermissionStore.service"
glib-compile-schemas "$t/schemas"
dbus-daemon --session --address="unix:path=$t/run/system_bus" --nofork --nopidfile >/dev/null 2>&1 &
bus=$!
sleep 0.5
env -i PATH=/usr/bin:/bin HOME="$t/home" XDG_RUNTIME_DIR="$t/run" XDG_DATA_HOME="$t/data" \
	XDG_CONFIG_HOME="$t/config" XDG_CACHE_HOME="$t/cache" XDG_DATA_DIRS="$t/data:/usr/local/share:/usr/share" \
	GSETTINGS_BACKEND=memory GSETTINGS_SCHEMA_DIR="$t/schemas${GSETTINGS_SCHEMA_DIR:+:$GSETTINGS_SCHEMA_DIR}" \
	${GI_TYPELIB_PATH:+GI_TYPELIB_PATH="$GI_TYPELIB_PATH"} ${LD_LIBRARY_PATH:+LD_LIBRARY_PATH="$LD_LIBRARY_PATH"} \
	LANG=C.UTF-8 NO_AT_BRIDGE=1 GIO_USE_VFS=local GDK_DEBUG=no-portals VIBEOS_DENSITY=3 \
	DBUS_SYSTEM_BUS_ADDRESS="unix:path=$t/run/system_bus" \
	dbus-run-session -- timeout -k 10 600 "$shell" --headless --wayland --no-x11 --virtual-monitor 1080x2280 \
	--automation-script "$t/mic-prompt.js" > "$t/shell.log" 2>&1 || true
[ -z "$shots" ] || { mkdir -p "$shots" && cp "$t"/shots/*.png "$shots/" 2>/dev/null || true; }
grep '^TEST' "$t/shell.log" || true
if grep -q '^TEST ALL PASSED' "$t/shell.log"; then
	exit 0
fi
grep -A8 'JS ERROR\|Script failed' "$t/shell.log" || tail -30 "$t/shell.log"
exit 1
