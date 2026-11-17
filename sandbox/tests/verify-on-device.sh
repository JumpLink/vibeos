#!/bin/bash
# Checks on the phone itself that vibeos-sandbox keeps each permission, both ways: for every permission the probe
# app is given it, then not, and a command inside the sandbox tries what the permission covers. Only whether it
# worked is printed, never what it found (no names, numbers, places, messages or recordings).
#
# Run as root on the phone, with the session running:   verify-on-device.sh [USER] [--slow]
# ($VIBEOS_SANDBOX names another vibeos-sandbox to check, a build not yet installed)
#   --slow  also the background check (an app with no window, not allowed in the background, is stopped by the
#           background portal within about three minutes)
#
# The probe is the app id vibeos.test.Probe; its permission store entries are taken away at the end, and so is its
# home ($XDG_DATA_HOME/vibeos-sandbox/data/vibeos.test.Probe).
set -u

USER_NAME=${1:-user}
SLOW=0
[ "${2:-}" = "--slow" ] && SLOW=1
UID_=$(id -u "$USER_NAME")
RUNTIME=/run/user/$UID_
APP=vibeos.test.Probe
VS=${VIBEOS_SANDBOX:-vibeos-sandbox}
PASS=0
FAIL=0

as_user () {
  sudo -u "$USER_NAME" env XDG_RUNTIME_DIR="$RUNTIME" DBUS_SESSION_BUS_ADDRESS="unix:path=$RUNTIME/bus" \
    WAYLAND_DISPLAY=wayland-0 HOME="$(getent passwd "$USER_NAME" | cut -d: -f6)" LANG=C.UTF-8 "$@"
}
# runs a shell command in the probe's sandbox; its output is what it prints
inside () {
  as_user timeout 60 "$VS" run "$APP" -- sh -c "$1" 2>/dev/null
}
set_perm () {
  as_user "$VS" set "$APP" "$1" "$2" >/dev/null
}
check () {   # name, what was expected, what came out
  if [ "$2" = "$3" ]; then
    PASS=$((PASS + 1)); printf 'PASS  %-46s %s\n' "$1" "$3"
  else
    FAIL=$((FAIL + 1)); printf 'FAIL  %-46s expected %s, got %s\n' "$1" "$2" "$3"
  fi
}

# a portal request from inside: prints the response code (0 granted, 1 cancelled, 2 refused) and, for the location,
# only whether a location came
PORTAL_PY='
import sys
from gi.repository import Gio, GLib
bus = Gio.bus_get_sync(Gio.BusType.SESSION)
loop = GLib.MainLoop()
out = {"response": None, "location": False}
def on_signal(conn, sender, path, iface, name, params, data):
    if name == "Response":
        out["response"] = params[0]
        if sys.argv[1] == "camera" or params[0] != 0:
            loop.quit()
    elif name == "LocationUpdated":
        out["location"] = True
        loop.quit()
bus.signal_subscribe("org.freedesktop.portal.Desktop", None, None, None, None, Gio.DBusSignalFlags.NONE, on_signal, None)
GLib.timeout_add_seconds(25, loop.quit)
token = "probe%d" % GLib.random_int_range(1, 1000000)
P = "/org/freedesktop/portal/desktop"
try:
    if sys.argv[1] == "location":
        s = bus.call_sync("org.freedesktop.portal.Desktop", P, "org.freedesktop.portal.Location", "CreateSession",
                          GLib.Variant("(a{sv})", ({"session_handle_token": GLib.Variant("s", token)},)),
                          None, 0, 10000, None)[0]
        bus.call_sync("org.freedesktop.portal.Desktop", P, "org.freedesktop.portal.Location", "Start",
                      GLib.Variant("(osa{sv})", (s, "", {"handle_token": GLib.Variant("s", token)})),
                      None, 0, 10000, None)
    else:
        bus.call_sync("org.freedesktop.portal.Desktop", P, "org.freedesktop.portal.Camera", "AccessCamera",
                      GLib.Variant("(a{sv})", ({"handle_token": GLib.Variant("s", token)},)), None, 0, 10000, None)
    loop.run()
except GLib.Error as e:
    print("error", e.domain)
    sys.exit(0)
print("response", out["response"], "location", "yes" if out["location"] else "no")
'
portal () {   # location|camera
  as_user timeout 60 "$VS" run "$APP" -- python3 -c "$PORTAL_PY" "$1" 2>/dev/null | tail -n1
}
# whether a bus name answers from inside (Introspect on the service's object: nothing is read)
reach () {   # bus name, object path
  inside "gdbus introspect --session --dest $1 --object-path $2 >/dev/null 2>&1 && echo reachable || echo unreachable"
}
# how many PipeWire nodes of a class the app sees
pw_count () {   # media.class prefix
  inside "pw-dump 2>/dev/null | grep -c '\"media.class\": \"$1' || true"
}
# how many links PipeWire has to or from the probe's own nodes, seen from outside
probe_links () {
  as_user pw-dump 2>/dev/null | python3 -c '
import json, sys
objs = json.load(sys.stdin)
def props(o): return (o.get("info") or {}).get("props") or {}
clients = {str(o["id"]) for o in objs if o["type"].endswith(":Client")
           and props(o).get("pipewire.access.portal.app_id") == sys.argv[1]}
nodes = {o["id"] for o in objs if o["type"].endswith(":Node") and str(props(o).get("client.id")) in clients}
print(sum(1 for o in objs if o["type"].endswith(":Link")
          and ((o.get("info") or {}).get("output-node-id") in nodes or (o.get("info") or {}).get("input-node-id") in nodes)))
' "$APP"
}
# a few seconds of sound recorded from inside, whatever the command: "recorded" or "nothing" (only the size counts)
RECORDED='s=$(cat /tmp/r.* 2>/dev/null | wc -c); rm -f /tmp/r.*; [ "$s" -gt 4096 ] && echo recorded || echo nothing'
# the probe's own capture stream, left unlinked, and a link the probe makes to it itself from the port with the id
# PORT: "linked", "refused", or why it could not try. Ports by id: pw-link by name needs the port's node in view.
OWN_LINK='pw-record --target 0 --rate 8000 --channels 1 /tmp/own.wav >/dev/null 2>&1 & p=$!; sleep 2
own=$(pw-cli ls Port 2>/dev/null | awk '"'"'/^\tid / { id = $2 } /port.alias = "pw-record:input/ { sub(",", "", id); print id; exit }'"'"')
r=refused
if [ -z "$own" ]; then r="no stream"; elif [ -z "$PORT" ]; then r="no port"
elif pw-link "$PORT" "$own" >/dev/null 2>&1; then r=linked; fi
kill $p 2>/dev/null; wait $p 2>/dev/null; rm -f /tmp/own.wav; echo "$r"'
own_link () {   # a port's id ("" to find a sink's monitor from inside)
  if [ -n "$1" ]; then
    inside "PORT=$(printf %q "$1"); $OWN_LINK"
  else
    inside "PORT=\$(pw-link -o -I 2>/dev/null | awk '/:monitor_/ { print \$1; exit }'); $OWN_LINK"
  fi
}

echo "== vibeos-sandbox on $(hostname), user $USER_NAME, $(date -Is)"
command -v "$VS" >/dev/null || { echo "$VS is not installed"; exit 2; }

# ---- what every sandboxed app has, whatever its permissions ----
echo "-- always"
check "its own home (no .ssh, .local/share/evolution)" "private" \
  "$(inside '[ -e ~/.ssh ] || [ -e ~/.local/share/evolution ] || [ -e ~/.config/dconf ] && echo shared || echo private')"
check "the system is read-only" "read-only" "$(inside 'touch /usr/vs-probe 2>/dev/null && echo writable || echo read-only')"
check "no system bus socket" "absent" "$(inside '[ -e /run/dbus/system_bus_socket ] && echo present || echo absent')"
check "the system bus cannot be reached" "unreachable" \
  "$(inside 'gdbus call --system --dest org.freedesktop.DBus --object-path /org/freedesktop/DBus --method org.freedesktop.DBus.GetId >/dev/null 2>&1 && echo reachable || echo unreachable')"
check "ModemManager (system bus)" "unreachable" \
  "$(inside 'gdbus introspect --system --dest org.freedesktop.ModemManager1 --object-path /org/freedesktop/ModemManager1 >/dev/null 2>&1 && echo reachable || echo unreachable')"
check "GeoClue (system bus)" "unreachable" \
  "$(inside 'gdbus introspect --system --dest org.freedesktop.GeoClue2 --object-path /org/freedesktop/GeoClue2/Manager >/dev/null 2>&1 && echo reachable || echo unreachable')"
check "dconf (every app's settings)" "unreachable" "$(reach ca.desrt.dconf /ca/desrt/dconf/Writer/user)"
check "GNOME Shell (screenshots, windows)" "unreachable" "$(reach org.gnome.Shell /org/gnome/Shell)"
check "GNOME Shell screenshots" "unreachable" "$(reach org.gnome.Shell.Screenshot /org/gnome/Shell/Screenshot)"
check "VibeOS's shell service" "unreachable" "$(reach vibeos.Shell /vibeos/Shell)"
check "the keyring (secrets)" "unreachable" "$(reach org.freedesktop.secrets /org/freedesktop/secrets)"
check "the permission store itself" "unreachable" \
  "$(reach org.freedesktop.impl.portal.PermissionStore /org/freedesktop/impl/portal/PermissionStore)"
check "the portals" "reachable" "$(reach org.freedesktop.portal.Desktop /org/freedesktop/portal/desktop)"
check "notifications" "reachable" "$(reach org.freedesktop.Notifications /org/freedesktop/Notifications)"
check "other processes are not visible" "only its own" \
  "$(inside 'n=$(ls /proc | grep -c "^[0-9]"); [ "$n" -lt 10 ] && echo "only its own" || echo "$n processes"')"
check "no sound cards, no input devices" "none" "$(inside 'ls /dev/snd /dev/input >/dev/null 2>&1 && echo present || echo none')"
check "the GPU" "present" "$(inside '[ -e /dev/dri ] && echo present || echo absent')"
check "no X11 display" "none" "$(inside '[ -n "${DISPLAY:-}" ] && echo "$DISPLAY" || echo none')"
check "its machine id is not the phone's" "different" \
  "$(inside 'cat /etc/machine-id' | cmp -s - /etc/machine-id && echo same || echo different)"

# ---- each permission, not allowed and allowed ----
echo "-- network"
set_perm network deny
# a connection to a public DNS server's port 53 (nothing is sent)
CONNECT='python3 -c "import socket; socket.create_connection((\"9.9.9.9\", 53), 5)" 2>/dev/null && echo works || echo fails'
check "network denied: routes" "none" "$(inside '[ "$(tail -n +2 /proc/net/route | wc -l)" = 0 ] && echo none || echo some')"
check "network denied: a connection" "fails" "$(inside "$CONNECT")"
check "network denied: a name lookup" "fails" "$(inside 'getent hosts debian.org >/dev/null && echo works || echo fails')"
set_perm network allow
check "network allowed: a connection" "works" "$(inside "$CONNECT")"
check "network allowed: a name lookup" "works" "$(inside 'getent hosts debian.org >/dev/null && echo works || echo fails')"

echo "-- location"
set_perm location deny
# "granted" or "refused", then whether a location came
loc () {
  portal location | sed -e 's/^response 0 location/granted,/' -e 's/^response [0-9]* location/refused,/' \
                        -e 's/^error.*/refused, no/'
}
check "location denied: the portal" "refused, no" "$(loc)"
# with Location off for the whole phone the portal refuses every app
if [ "$(as_user gsettings get org.gnome.system.location enabled)" = true ]; then
  set_perm location allow
  check "location allowed: the portal" "granted, yes" "$(loc)"
else
  echo "SKIP  location allowed: Location is off for the whole phone"
fi
set_perm location deny

echo "-- camera"
set_perm camera deny
check "camera denied: devices" "none" "$(inside 'ls /dev/video* >/dev/null 2>&1 && echo present || echo none')"
check "camera denied: the portal" "refused" "$(portal camera | grep -q '^response 0' && echo granted || echo refused)"
set_perm camera allow
check "camera allowed: devices" "present" "$(inside 'ls /dev/video* >/dev/null 2>&1 && echo present || echo none')"
check "camera allowed: the portal" "granted" "$(portal camera | grep -q '^response 0' && echo granted || echo refused)"
# WirePlumber loads the script as it starts: live only when it started after the script was installed
WP_LIVE=0
WP_PID=$(pgrep -u "$USER_NAME" -x wireplumber | head -n1)
if [ -n "$WP_PID" ] && [ -e /usr/share/wireplumber/scripts/vibeos/sandbox-access.lua ] && \
   [ "$(stat -c %Y /usr/share/wireplumber/scripts/vibeos/sandbox-access.lua)" -lt "$(stat -c %Y /proc/$WP_PID)" ]; then
  WP_LIVE=1
fi
set_perm camera deny
if [ "$WP_LIVE" = 1 ]; then
  check "camera denied: PipeWire camera nodes" "0" "$(pw_count Video/Source)"
  set_perm camera allow
  check "camera allowed: PipeWire camera nodes" "nonzero" "$( [ "$(pw_count Video/Source)" -gt 0 ] && echo nonzero || echo 0)"
  set_perm camera deny
else
  echo "SKIP  camera through PipeWire: WirePlumber started before the script was installed"
fi

echo "-- microphone"
set_perm microphone deny
if [ "$WP_LIVE" = 1 ]; then
  check "microphone denied: PipeWire sources" "0" "$(pw_count Audio/Source)"
  check "microphone denied: other apps' streams" "0" "$(pw_count Stream/)"
  check "microphone denied: recording" "nothing" \
    "$(inside "timeout 3 pw-record --rate 8000 --channels 1 /tmp/r.wav >/dev/null 2>&1; $RECORDED")"
  check "microphone denied: recording what plays" "nothing" \
    "$(inside "timeout 3 pw-record -P '{ stream.capture.sink = true }' --rate 8000 --channels 1 /tmp/r.wav >/dev/null 2>&1; $RECORDED")"
  check "microphone denied: PulseAudio sources" "0" \
    "$(inside 'pactl list short sources 2>/dev/null | grep -vc "\.monitor" || true')"
  check "microphone denied: PulseAudio recording" "nothing" \
    "$(inside "timeout 3 parecord --raw --rate 8000 --channels 1 /tmp/r.raw >/dev/null 2>&1; $RECORDED")"
  check "microphone denied: PulseAudio, what plays" "nothing" \
    "$(inside "d=\$(pactl get-default-sink 2>/dev/null); timeout 3 parecord -d \"\$d.monitor\" --raw --rate 8000 --channels 1 /tmp/r.raw >/dev/null 2>&1; $RECORDED")"
  # the microphone's port, from outside (the probe does not see it): the probe tries to link it all the same
  MIC_PORT=$(as_user pw-dump 2>/dev/null | python3 -c '
import json, sys
objs = json.load(sys.stdin)
mics = {o["id"] for o in objs if o["type"].endswith(":Node")
        and str((o.get("info") or {}).get("props", {}).get("media.class", "")).startswith("Audio/Source")}
for o in objs:
    p = (o.get("info") or {}).get("props") or {}
    if o["type"].endswith(":Port") and p.get("node.id") in mics and p.get("port.direction") == "out":
        print(o["id"]); break
')
  check "capture: linking a sink's monitor itself" "refused" "$(own_link "")"
  if [ -n "$MIC_PORT" ]; then
    check "capture: linking the microphone itself" "refused" "$(own_link "$MIC_PORT")"
  else
    echo "SKIP  capture: linking the microphone itself: no microphone node on this phone"
  fi
  check "capture: a node made inside PipeWire" "refused" \
    "$(inside "pw-cli create-node adapter '{ factory.name = support.null-audio-sink node.name = vs-probe-node media.class = Audio/Sink }' 2>&1 | grep -q 'error\\|unknown' && echo refused || echo made")"
  check "capture: a sink of its own" "gone" \
    "$(inside "pw-record -P '{ media.class = Audio/Sink node.name = vs-probe-sink priority.session = 5000 }' /tmp/s.wav >/dev/null 2>&1 & p=\$!; sleep 3; n=\$(pw-dump 2>/dev/null | grep -c '\"node.name\": \"vs-probe-sink\"'); kill \$p 2>/dev/null; rm -f /tmp/s.wav; [ \"\$n\" = 0 ] && echo gone || echo kept")"
  check "capture: a PulseAudio module" "refused" \
    "$(inside 'pactl load-module module-null-sink sink_name=vs_probe_null >/dev/null 2>&1 && echo loaded || echo refused')"
  as_user pactl list short modules 2>/dev/null | awk '/vs_probe_null/ { print $1 }' | while read -r m; do as_user pactl unload-module "$m"; done
  # silence, played: sound still works without the microphone
  as_user setsid -f timeout 6 "$VS" run "$APP" -- pw-cat -p --raw --format=s16 --rate=48000 --channels=2 /dev/zero \
    </dev/null >/dev/null 2>&1
  sleep 4
  check "microphone denied: playing sound" "linked" "$( [ "$(probe_links)" -gt 0 ] && echo linked || echo unlinked)"
  sleep 3
  set_perm microphone allow
  check "microphone allowed: PipeWire sources" "nonzero" "$( [ "$(pw_count Audio/Source)" -gt 0 ] && echo nonzero || echo 0)"
  check "microphone allowed: recording" "recorded" \
    "$(inside "timeout 3 pw-record --rate 8000 --channels 1 /tmp/r.wav >/dev/null 2>&1; $RECORDED")"
  check "microphone allowed: PulseAudio recording" "recorded" \
    "$(inside "timeout 3 parecord --raw --rate 8000 --channels 1 /tmp/r.raw >/dev/null 2>&1; $RECORDED")"
  check "microphone allowed: still no links of its own" "refused" "$(own_link "")"
  set_perm microphone deny
else
  echo "SKIP  microphone: WirePlumber started before the script was installed (needs a session restart)"
fi

echo "-- contacts, calendar"
set_perm contacts deny
set_perm calendar deny
check "contacts denied: address books" "unreachable" "$(reach org.gnome.evolution.dataserver.AddressBook10 /org/gnome/evolution/dataserver/AddressBookFactory)"
check "contacts denied: the source list" "unreachable" "$(reach org.gnome.evolution.dataserver.Sources5 /org/gnome/evolution/dataserver/SourceManager)"
check "calendar denied: calendars" "unreachable" "$(reach org.gnome.evolution.dataserver.Calendar8 /org/gnome/evolution/dataserver/CalendarFactory)"
check "calendar denied: the shell's calendar server" "unreachable" "$(reach org.gnome.Shell.CalendarServer /org/gnome/Shell/CalendarServer)"
check "contacts denied: the files" "absent" "$(inside '[ -e ~/.local/share/evolution/addressbook ] && echo present || echo absent')"
set_perm contacts allow
check "contacts allowed: address books" "reachable" "$(reach org.gnome.evolution.dataserver.AddressBook10 /org/gnome/evolution/dataserver/AddressBookFactory)"
check "contacts allowed: calendars still" "unreachable" "$(reach org.gnome.evolution.dataserver.Calendar8 /org/gnome/evolution/dataserver/CalendarFactory)"
set_perm contacts deny
set_perm calendar allow
check "calendar allowed: calendars" "reachable" "$(reach org.gnome.evolution.dataserver.Calendar8 /org/gnome/evolution/dataserver/CalendarFactory)"
check "calendar allowed: address books still" "unreachable" "$(reach org.gnome.evolution.dataserver.AddressBook10 /org/gnome/evolution/dataserver/AddressBookFactory)"
set_perm calendar deny

echo "-- phone, SMS"
set_perm phone deny
set_perm sms deny
check "phone denied: the call service" "unreachable" "$(reach vibeos.Dialer.Calls /vibeos/Dialer/Calls)"
check "SMS denied: the SMS service" "unreachable" "$(reach vibeos.Messaging.Sms /vibeos/Messaging/Sms)"
set_perm phone allow
check "phone allowed: the call service" "reachable" "$(reach vibeos.Dialer.Calls /vibeos/Dialer/Calls)"
check "phone allowed: SMS still" "unreachable" "$(reach vibeos.Messaging.Sms /vibeos/Messaging/Sms)"
set_perm phone deny
set_perm sms allow
check "SMS allowed: the SMS service" "reachable" "$(reach vibeos.Messaging.Sms /vibeos/Messaging/Sms)"
set_perm sms deny

echo "-- files"
PICS=$(as_user sh -c 'd=$(xdg-user-dir PICTURES 2>/dev/null); [ -n "$d" ] && [ "$d" != "$HOME" ] && ls -A "$d" | wc -l || echo 0')
set_perm files deny
check "files denied: Pictures" "empty" "$(inside 'd=~/Pictures; [ -d "$d" ] && [ -n "$(ls -A "$d")" ] && echo "not empty" || echo empty')"
set_perm files allow
if [ "$PICS" -gt 0 ]; then
  check "files allowed: Pictures" "not empty" "$(inside 'd=~/Pictures; [ -n "$(ls -A "$d" 2>/dev/null)" ] && echo "not empty" || echo empty')"
else
  echo "SKIP  files allowed: Pictures is empty on this phone"
fi
check "files allowed: still no dotfiles" "private" "$(inside '[ -e ~/.ssh ] || [ -e ~/.config/dconf ] && echo shared || echo private')"
set_perm files deny

echo "-- taking a permission away stops the app"
set_perm microphone allow
as_user setsid -f "$VS" run "$APP" -- sleep 120 </dev/null >/dev/null 2>&1
sleep 3
BEFORE=$(as_user "$VS" ps | grep -c "	$APP	")
set_perm microphone deny
sleep 2
AFTER=$(as_user "$VS" ps | grep -c "	$APP	")
check "running before, stopped after the revoke" "1 0" "$BEFORE $AFTER"

if [ "$SLOW" = 1 ]; then
  echo "-- background (about three minutes)"
  set_perm background deny
  as_user setsid -f "$VS" run "$APP" -- sleep 400 </dev/null >/dev/null 2>&1
  sleep 200
  check "background denied: a windowless app is stopped" "0" "$(as_user "$VS" ps | grep -c "	$APP	")"
  as_user "$VS" kill "$APP" >/dev/null
fi

# ---- cleaning up ----
for e in "vibeos-sandbox network" "vibeos-sandbox microphone" "vibeos-sandbox contacts" "vibeos-sandbox calendar" \
         "vibeos-sandbox phone" "vibeos-sandbox sms" "vibeos-sandbox files" "location location" "devices camera" \
         "background background"; do
  set -- $e
  as_user gdbus call --session --dest org.freedesktop.impl.portal.PermissionStore \
    --object-path /org/freedesktop/impl/portal/PermissionStore \
    --method org.freedesktop.impl.portal.PermissionStore.DeletePermission "$1" "$2" "$APP" >/dev/null 2>&1
done
rm -rf "$(getent passwd "$USER_NAME" | cut -d: -f6)/.local/share/vibeos-sandbox/data/$APP"
echo "== $PASS passed, $FAIL failed"
[ "$FAIL" = 0 ]
