#!/bin/sh
# keyboard-overlay.sh SOURCE_DIR [SHELL [MONITOR [SCALE]]]: a window under the on-screen keyboard, which goes over it
# and slides it up only as far as the text cursor needs (keyboard-overlay.js), in a headless GNOME Shell of the test's
# own: SHELL, the VibeOS shell's gnome-shell (default gnome-shell on the PATH; its keyboard must be the mobile one:
# /opt/gnome-mobile/bin/gnome-shell with GSETTINGS_SCHEMA_DIR=/opt/gnome-mobile/share/glib-2.0/schemas on a VibeOS
# phone), on a virtual monitor of MONITOR (default 1080x2280, a phone) at SCALE (default 1; 1600x2560 at 2 is a tablet).
#
# Not run by meson: it starts a whole shell, and needs gjs with GTK 4 and dbus-daemon. Everything it runs is its own:
# a session bus and a stand-in system bus, the runtime directory, HOME and XDG directories in a temporary directory,
# settings in memory, and the desktop files of the test's three apps. It never touches a session that is running.
# GSETTINGS_SCHEMA_DIR, GI_TYPELIB_PATH and LD_LIBRARY_PATH are passed on (the shell's own schemas where they are not
# installed in the usual place). GTK asks no portal (none answers on the test's bus, and GTK waits for one). On a
# stock shell the test stands in for what the VibeOS shell adds to the keyboard (keyboard-overlay.js).
set -eu
src=$(cd "$1" && pwd)
shell=${2:-gnome-shell}
monitor=${3:-1080x2280}
scale=${4:-1}
here=$(cd "$(dirname "$0")" && pwd)
t=$(mktemp -d)
bus=
# (set +e: a kill or rm failing here must not turn a pass into a failure)
trap 'set +e; [ -z "$bus" ] || kill "$bus" 2>/dev/null; rm -rf "${t:?}"' EXIT
mkdir "$t/schemas" "$t/home" "$t/data" "$t/data/applications" "$t/config" "$t/cache" "$t/shots"
mkdir -m 700 "$t/run"
cp -a "$src" "$t/ext"
cp "$here/keyboard-overlay.js" "$here/keyboard-overlay-client.js" "$here/keyboard-standin.js" "$t/"
cp "$src/../vibeos-launcher/data/vibeos.Shell.gschema.xml" "$t/schemas/"
glib-compile-schemas "$t/schemas"
# the shell knows a window's app, and its X-Vibeos-Keyboard, from a desktop file; the plain app has none to say
for app in Overlay:overlay Fit:fit; do
	cat > "$t/data/applications/test.vibeos.Keyboard${app%%:*}.desktop" <<DESKTOP
[Desktop Entry]
Type=Application
Name=Keyboard ${app%%:*}
Exec=gjs -m $t/keyboard-overlay-client.js test.vibeos.Keyboard${app%%:*}
X-Vibeos-Keyboard=${app##*:}
DESKTOP
done
dbus-daemon --session --address="unix:path=$t/run/system_bus" --nofork --nopidfile >/dev/null 2>&1 &
bus=$!
sleep 0.5
env -i PATH=/usr/bin:/bin HOME="$t/home" XDG_RUNTIME_DIR="$t/run" XDG_DATA_HOME="$t/data" \
	XDG_CONFIG_HOME="$t/config" XDG_CACHE_HOME="$t/cache" XDG_DATA_DIRS=/usr/local/share:/usr/share \
	GSETTINGS_BACKEND=memory GSETTINGS_SCHEMA_DIR="$t/schemas${GSETTINGS_SCHEMA_DIR:+:$GSETTINGS_SCHEMA_DIR}" \
	${GI_TYPELIB_PATH:+GI_TYPELIB_PATH="$GI_TYPELIB_PATH"} ${LD_LIBRARY_PATH:+LD_LIBRARY_PATH="$LD_LIBRARY_PATH"} \
	LANG=C.UTF-8 NO_AT_BRIDGE=1 GIO_USE_VFS=local GDK_DEBUG=no-portals ${OSK_ONLY:+OSK_ONLY="$OSK_ONLY"} OSK_MONITOR="$monitor" OSK_SCALE="$scale" \
	DBUS_SYSTEM_BUS_ADDRESS="unix:path=$t/run/system_bus" \
	dbus-run-session -- timeout -k 10 1800 "$shell" --headless --wayland --no-x11 --virtual-monitor "$monitor" \
	--automation-script "$t/keyboard-overlay.js" > "$t/shell.log" 2>&1 || true
grep '^TEST' "$t/shell.log" || true
if [ -n "${OVERLAY_SHOTS:-}" ]; then
	mkdir -p "$OVERLAY_SHOTS"
	cp "$t"/shots/*.png "$t/shell.log" "$OVERLAY_SHOTS/" 2>/dev/null || true
fi
if grep -q '^TEST ALL PASSED' "$t/shell.log"; then
	exit 0
fi
grep -A8 'JS ERROR\|Script failed' "$t/shell.log" || tail -20 "$t/shell.log"
exit 1
