#!/bin/sh
# keyboard-slide.sh SOURCE_DIR [SHELL]: GNOME Console under the on-screen keyboard, slid up over the keys instead
# of resized (keyboard-slide.js), in a headless GNOME Shell of the test's own: SHELL, the VibeOS shell's gnome-shell
# (default gnome-shell on the PATH; its keyboard must be the mobile one: /opt/gnome-mobile/bin/gnome-shell with
# GSETTINGS_SCHEMA_DIR=/opt/gnome-mobile/share/glib-2.0/schemas on a VibeOS phone, where it takes a few minutes).
#
# Not run by meson: it starts a whole shell, and needs GNOME Console (kgx) and dbus-daemon. Everything it runs is its
# own: a session bus and a stand-in system bus, the runtime directory, HOME and XDG directories in a temporary
# directory, settings in memory; Console's shell starts in that HOME. It never touches a session that is running, and it
# can run beside one, as the phone's user (a Console open there is another instance, on another bus).
# GSETTINGS_SCHEMA_DIR, GI_TYPELIB_PATH and LD_LIBRARY_PATH are passed on (the shell's own schemas where they are not
# installed in the usual place). GTK asks no portal (none answers on the test's bus, and GTK waits for one).
set -eu
src=$(cd "$1" && pwd)
shell=${2:-gnome-shell}
here=$(cd "$(dirname "$0")" && pwd)
t=$(mktemp -d)
bus=
# (set +e: a kill or rm failing here must not turn a pass into a failure)
trap 'set +e; [ -z "$bus" ] || kill "$bus" 2>/dev/null; rm -rf "${t:?}"' EXIT
mkdir "$t/schemas" "$t/home" "$t/data" "$t/config" "$t/cache"
mkdir -m 700 "$t/run"
cp -a "$src" "$t/ext"
cp "$here/keyboard-slide.js" "$here/keyboard-standin.js" "$t/"
cp "$src/../vibeos-launcher/data/vibeos.Shell.gschema.xml" "$t/schemas/"
glib-compile-schemas "$t/schemas"
dbus-daemon --session --address="unix:path=$t/run/system_bus" --nofork --nopidfile >/dev/null 2>&1 &
bus=$!
sleep 0.5
env -i PATH=/usr/bin:/bin HOME="$t/home" XDG_RUNTIME_DIR="$t/run" XDG_DATA_HOME="$t/data" \
	XDG_CONFIG_HOME="$t/config" XDG_CACHE_HOME="$t/cache" XDG_DATA_DIRS=/usr/local/share:/usr/share \
	GSETTINGS_BACKEND=memory GSETTINGS_SCHEMA_DIR="$t/schemas${GSETTINGS_SCHEMA_DIR:+:$GSETTINGS_SCHEMA_DIR}" \
	${GI_TYPELIB_PATH:+GI_TYPELIB_PATH="$GI_TYPELIB_PATH"} ${LD_LIBRARY_PATH:+LD_LIBRARY_PATH="$LD_LIBRARY_PATH"} \
	LANG=C.UTF-8 NO_AT_BRIDGE=1 GIO_USE_VFS=local GDK_DEBUG=no-portals \
	DBUS_SYSTEM_BUS_ADDRESS="unix:path=$t/run/system_bus" \
	dbus-run-session -- timeout -k 10 1800 "$shell" --headless --wayland --no-x11 --virtual-monitor 1080x2280 \
	--automation-script "$t/keyboard-slide.js" > "$t/shell.log" 2>&1 || true
grep '^TEST' "$t/shell.log" || true
if grep -q '^TEST ALL PASSED' "$t/shell.log"; then
	exit 0
fi
grep -A8 'JS ERROR\|Script failed' "$t/shell.log" || tail -20 "$t/shell.log"
exit 1
