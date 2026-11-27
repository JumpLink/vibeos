#!/bin/sh
# test-close.sh NAVI [POI_DIR]: Navi's window closed the way Recents closes it (vibeos-navi --close-at), in a drive, on
# the arrived screen and in the routes. Each time Navi must end cleanly, with its exit status 0 (a crash is 139) and no
# critical after the close, and must save its settings as it goes: a group the test adds to navi.ini after Navi's own
# last save is gone again, the voice the test set off is still off, and the destination is among the recent places.
#
# Everything is the test's own: a headless display of a phone's size, 360x761 (weston, else mutter: NAVI_TEST_MUTTER
# names one with its arguments), a D-Bus (the system bus too: no logind, no GeoClue), HOME and the XDG directories,
# and a stand-in route server on 127.0.0.1 that answers every route with tests/data/route-zoo-potsdamer-platz.json.
# The map's styles have no sources, so nothing goes to the network. Without a display, dbus-daemon or python3 the
# test is skipped (77). NAVI_TEST_WRAPPER runs the app (qemu-aarch64 -L ROOT for an arm64 build on a PC). Its files
# are kept where it says when a case fails, or always with NAVI_TEST_KEEP=1.
set -u
NAVI=$1
HERE=$(cd "$(dirname "$0")" && pwd)
POI=${2:-$HERE/../data/poi}
FIXTURE=$HERE/data/route-zoo-potsdamer-platz.json

skip () {
	echo "test-close: skipped: $*"
	exit 77
}
command -v dbus-daemon > /dev/null || skip "no dbus-daemon"
command -v python3 > /dev/null || skip "no python3"
MUTTER=${NAVI_TEST_MUTTER:-$(command -v mutter || true)}
if command -v weston > /dev/null; then
	DISPLAY_KIND=weston GSK=cairo
elif [ -n "$MUTTER" ]; then
	DISPLAY_KIND=mutter GSK=gl
else
	skip "no weston or mutter for a headless display"
fi

T=$(mktemp -d "${TMPDIR:-/tmp}/navi-close.XXXXXX")
RUN=$T/run
mkdir -p "$RUN" "$T/home" "$T/data"
chmod 700 "$RUN"
PIDS=
FAILED=
trap 'for p in $PIDS; do kill "$p" 2> /dev/null || true; done' EXIT
fail () {
	echo "test-close: FAIL: $*"
	FAILED=1
}

# the bus: no services to start, so nothing of the system's runs for the test
cat > "$T/bus.conf" << EOF
<!DOCTYPE busconfig PUBLIC "-//freedesktop//DTD D-Bus Bus Configuration 1.0//EN"
 "http://www.freedesktop.org/standards/dbus/1.0/busconfig.dtd">
<busconfig>
  <type>session</type>
  <listen>unix:path=$RUN/bus</listen>
  <policy context="default">
    <allow send_destination="*" eavesdrop="true"/>
    <allow eavesdrop="true"/>
    <allow own="*"/>
  </policy>
</busconfig>
EOF
dbus-daemon --config-file="$T/bus.conf" --nofork --nopidfile > "$T/bus.log" 2>&1 &
PIDS="$PIDS $!"

if [ $DISPLAY_KIND = weston ]; then
	env XDG_RUNTIME_DIR="$RUN" HOME="$T/home" weston --no-config --backend=headless --renderer=pixman --shell=kiosk \
		--width=360 --height=761 --scale=1 --socket=navi-close --idle-time=0 > "$T/display.log" 2>&1 &
else
	# shellcheck disable=SC2086 # the command and its arguments
	env XDG_RUNTIME_DIR="$RUN" HOME="$T/home" DBUS_SESSION_BUS_ADDRESS="unix:path=$RUN/bus" GSETTINGS_BACKEND=memory \
		$MUTTER --headless --wayland --no-x11 --wayland-display=navi-close --virtual-monitor 360x761 \
		> "$T/display.log" 2>&1 &
fi
PIDS="$PIDS $!"

# the route server: POST /route answers with the saved route, anything else is not there
python3 -I - "$FIXTURE" "$T/port" > "$T/server.log" 2>&1 << 'EOF' &
import http.server
import sys

ROUTE = open(sys.argv[1], 'rb').read()


class Server(http.server.BaseHTTPRequestHandler):
    def answer(self, status, body=b''):
        self.send_response(status)
        self.send_header('Content-Type', 'application/json')
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_POST(self):
        self.rfile.read(int(self.headers.get('Content-Length') or 0))
        self.answer(200, ROUTE) if self.path.split('?')[0] == '/route' else self.answer(404)

    def do_GET(self):
        self.answer(404)

    def log_message(self, fmt, *args):
        sys.stderr.write('%s %s\n' % (self.command, self.path.split('?')[0]))


server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Server)
with open(sys.argv[2] + '.new', 'w') as f:
    f.write(str(server.server_address[1]))
__import__('os').rename(sys.argv[2] + '.new', sys.argv[2])
server.serve_forever()
EOF
PIDS="$PIDS $!"

for _ in $(seq 100); do
	[ -S "$RUN/bus" ] && [ -S "$RUN/navi-close" ] && [ -f "$T/port" ] && break
	sleep 0.1
done
[ -S "$RUN/bus" ] && [ -S "$RUN/navi-close" ] && [ -f "$T/port" ] || {
	echo "test-close: no display, bus or route server (see $T)"
	exit 1
}
PORT=$(cat "$T/port")

# the map's styles: a background only, day and night
printf '{"version": 8, "sources": {}, "layers": [%s]}\n' \
	'{"id": "background", "type": "background", "paint": {"background-color": "#e8e4dc"}}' > "$T/data/style-day.json"
printf '{"version": 8, "sources": {}, "layers": [%s]}\n' \
	'{"id": "background", "type": "background", "paint": {"background-color": "#1d2228"}}' > "$T/data/style-night.json"
ln -s "$POI" "$T/data/poi"

# the drive's two ends: the saved route's
ENDS=$(python3 -I -c 'import json, sys
t = json.load(open(sys.argv[1]))["trip"]["locations"]
print("%.6f,%.6f %.6f,%.6f" % (t[0]["lat"], t[0]["lon"], t[-1]["lat"], t[-1]["lon"]))' "$FIXTURE")
FROM=${ENDS% *}
TO=${ENDS#* }

# close_in MODE ARGS: Navi from the start to MODE, closed 9 s after it shows it; the test changes navi.ini meanwhile,
# 5 s in, when Navi's own saves (2 s after a change) are done
close_in () {
	mode=$1
	shift
	home=$T/$mode
	ini=$home/config/vibeos-navi/navi.ini
	log=$T/$mode.log
	mkdir -p "$home/config/vibeos-navi" "$home/data" "$home/cache"
	printf '[voice]\nmode=2\n\n[alerts]\ncamera-notice-seen=true\n' > "$ini"
	# shellcheck disable=SC2086 # NAVI_TEST_WRAPPER: a command and its arguments
	env -i PATH="$PATH" HOME="$home" XDG_RUNTIME_DIR="$RUN" XDG_CONFIG_HOME="$home/config" \
		XDG_DATA_HOME="$home/data" XDG_CACHE_HOME="$home/cache" WAYLAND_DISPLAY=navi-close GDK_BACKEND=wayland \
		GDK_DEBUG=no-portals GSK_RENDERER=$GSK DBUS_SESSION_BUS_ADDRESS="unix:path=$RUN/bus" \
		DBUS_SYSTEM_BUS_ADDRESS="unix:path=$RUN/bus" GSETTINGS_BACKEND=memory NO_AT_BRIDGE=1 GTK_A11Y=none \
		SPEECHD_CMD=/bin/false \
		LANG=C.UTF-8 LANGUAGE= NAVI_SERVER="http://127.0.0.1:$PORT" NAVI_PHOTON_URL="http://127.0.0.1:$PORT" \
		NAVI_DATA_DIR="$T/data" NAVI_NO_COUNTDOWN=1 \
		${NAVI_TEST_WRAPPER:-} "$NAVI" --simulate --from "$FROM" --to "$TO,Test destination" --seconds 600 \
		--close-at "$mode:9" "$@" > "$log" 2>&1 &
	pid=$!
	PIDS="$PIDS $pid"
	# Navi shows MODE (at most 5 minutes: an emulated arm64 build is slow to start)
	i=0
	until grep -qF "navi: --close-at: $mode, the window closes" "$log"; do
		i=$((i + 1))
		if [ $i -gt 600 ] || ! kill -0 $pid 2> /dev/null; then
			kill $pid 2> /dev/null || true
			wait $pid 2> /dev/null
			fail "$mode: Navi never showed $mode (see $log)"
			return
		fi
		sleep 0.5
	done
	sleep 5
	printf '\n[test-close]\nmode=%s\n' "$mode" >> "$ini"
	status=0
	wait $pid || status=$?
	if [ $status != 0 ]; then
		fail "$mode: Navi ended with status $status (see $log)"
	elif ! grep -qF "navi: --close-at: the window closes, in $mode" "$log"; then
		fail "$mode: the window was not closed in $mode (see $log)"
	elif sed -n '/navi: --close-at: the window closes, in/,$p' "$log" | grep -q 'CRITICAL'; then
		fail "$mode: criticals after the close (see $log)"
	elif grep -qF '[test-close]' "$ini"; then
		fail "$mode: the settings were not saved as Navi closed (see $ini)"
	elif ! grep -qx 'mode=2' "$ini" || ! grep -qx 'name=Test destination' "$ini"; then
		fail "$mode: the saved settings lack the voice or the destination (see $ini)"
	else
		echo "test-close: ok: closed in $mode, the settings saved"
	fi
}

close_in routes
close_in drive --go
close_in arrived --go --sim-speed 20

if [ -n "$FAILED" ] || [ -n "${NAVI_TEST_KEEP:-}" ]; then
	echo "test-close: the test's files are in $T"
else
	rm -rf "$T"
fi
[ -z "$FAILED" ]
