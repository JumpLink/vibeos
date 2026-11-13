#!/bin/sh
# try-files.sh: neo-files on a phone in a session of its own (spec/files.md §15), on a neo-headless display, so the
# phone can stay locked and nothing the rig does reaches the owner's session.
#
# Everything of the rig's is under /tmp: its runtime dir /tmp/files-rt (mode 700: gvfsd's FUSE mount, the keyring's
# and the document portal's sockets, the display's Wayland socket), and /tmp/files-rig: the app's HOME (the "Main
# storage" it shows, with a test tree), XDG_DATA_HOME (its Recycle Bin, recent files, keyring), XDG_CONFIG_HOME,
# XDG_STATE_HOME, the compiled schema (GSETTINGS_BACKEND=keyfile) and the SFTP server's files.
#
# Run it as root on the phone; it does the rest as the session user.
#
#   try-files.sh setup           the rig's directories, the test tree, the SSH keys and the schema
#   try-files.sh start           the display (neo-headless, NEO_HEADLESS=files) and the rig's touchscreen
#   try-files.sh sshd            the SFTP server: the phone's sshd as the session user on 127.0.0.1:2222, its own
#                                host key, the rig's client key only
#   try-files.sh app [URI]       the app (BUILD=..., default the work tree's) under dbus-run-session, with a
#                                gnome-keyring of its own unlocked with a throwaway password
#   try-files.sh shot OUT.png
#   try-files.sh tap X Y | press X Y [MS] | swipe X0 Y0 X1 Y1 [MS]
#                                touches (dp) on the rig's one touchscreen; press is a long press (800 ms)
#   try-files.sh type TEXT       TEXT typed into the focused field (a server's address, a password)
#   try-files.sh key NAME...     keys: Return, BackSpace, Escape, Tab, Back
#   try-files.sh secrets         the rig keyring's items: their labels and attributes (no secrets)
#   try-files.sh quit            the app and its session (bus, gvfsd, keyring)
#   try-files.sh stop            the app, the SFTP server and the display
#
# The WebDAV, FTP and SMB servers run on the development machine and reach the phone's 127.0.0.1 through `ssh -R`
# (tests/files-servers.sh). The LAN is only ever listed (network:///), never mounted.
set -e

USER_NAME=${NEO_USER:-user}
RT=/tmp/files-rt
RIG=/tmp/files-rig
HERE=$(dirname "$(readlink -f "$0")")
BUILD=${BUILD:-$HERE/../../build/neo-files}
# the display tool of this tree (tools/neo-headless): it must know NEO_HEADLESS_RUNTIME, or the display and the app
# run in the session's own /run/user/UID, where the rig's xdg-document-portal takes /run/user/UID/doc over from the
# session's (2026-11-10, with an older copy in /root/neo-tools: the session's portal died, exit 21)
HL=${HL:-$HERE/../../../../tools/neo-headless}
export NEO_HEADLESS=files NEO_HEADLESS_RUNTIME=$RT
STATE=$RT/files.d

if ! grep -q NEO_HEADLESS_RUNTIME "$HL" 2>/dev/null; then
    echo "try-files: $HL does not know NEO_HEADLESS_RUNTIME; HL=path/to/tools/neo-headless" >&2
    exit 1
fi

as_user() {
    sudo -u "$USER_NAME" env XDG_RUNTIME_DIR="$RT" "$@"
}

case $1 in
setup)
    install -d -m 700 -o "$USER_NAME" -g "$USER_NAME" "$RT"
    as_user mkdir -p "$RIG/home/.ssh" "$RIG/data" "$RIG/config" "$RIG/state" "$RIG/cache" "$RIG/schemas" "$RIG/sshd"
    as_user chmod 700 "$RIG" "$RIG/home/.ssh" "$RIG/sshd"
    # the test tree: the XDG folders Home shows, files of each kind, a hidden one, a deep path
    as_user sh -c '
        cd "$1/home"
        mkdir -p Documents Downloads Pictures Music Videos "Projects/2026/neo files/deep" .hidden-folder
        printf "%s\n" "a text file" >Documents/notes.txt
        printf "%s\n" "<html></html>" >Documents/page.html
        head -c 120000 /dev/urandom >Downloads/blob.bin
        head -c 3000000 /dev/urandom >Downloads/big.bin
        printf "%s\n" "x" >.hidden-file
        for n in 1 2 3; do printf "%s\n" "$n" >"Projects/2026/file $n.txt"; done
        n=0
        for f in /usr/share/backgrounds/*/*.jpg /usr/share/backgrounds/*/*.png /usr/share/pixmaps/*.png; do
            [ -f "$f" ] && [ $n -lt 6 ] && cp "$f" Pictures/ && n=$((n + 1))
        done
        cat >"$1/config/user-dirs.dirs" <<EOF
XDG_DOCUMENTS_DIR="\$HOME/Documents"
XDG_DOWNLOAD_DIR="\$HOME/Downloads"
XDG_PICTURES_DIR="\$HOME/Pictures"
XDG_MUSIC_DIR="\$HOME/Music"
XDG_VIDEOS_DIR="\$HOME/Videos"
XDG_DESKTOP_DIR="\$HOME"
EOF
    ' sh "$RIG"
    # the SFTP server's host key and the rig's client key (no passphrase), authorized for the server only
    [ -f "$RIG/sshd/host_ed25519" ] || as_user ssh-keygen -q -t ed25519 -N "" -f "$RIG/sshd/host_ed25519"
    [ -f "$RIG/home/.ssh/id_ed25519" ] || as_user ssh-keygen -q -t ed25519 -N "" -f "$RIG/home/.ssh/id_ed25519"
    as_user cp "$RIG/home/.ssh/id_ed25519.pub" "$RIG/sshd/authorized_keys"
    as_user mkdir -p "$RIG/sftp-root/shared/inside"
    as_user sh -c 'printf "%s\n" "over sftp" >"$1/sftp-root/shared/hello.txt"' sh "$RIG"
    as_user sh -c "cat >'$RIG/sshd/sshd_config'" <<EOF
Port 2222
ListenAddress 127.0.0.1
HostKey $RIG/sshd/host_ed25519
PidFile $RIG/sshd/sshd.pid
AuthorizedKeysFile $RIG/sshd/authorized_keys
PubkeyAuthentication yes
PasswordAuthentication no
KbdInteractiveAuthentication no
UsePAM no
StrictModes no
Subsystem sftp /usr/lib/openssh/sftp-server
EOF
    as_user cp "$HERE/../data/neo.Files.gschema.xml" "$RIG/schemas/"
    as_user glib-compile-schemas "$RIG/schemas"
    ;;
start)
    # mutter's RemoteDesktop (the rig's touches) streams the screen through the session's PipeWire; the app plays
    # no sound
    [ -e "$RT/pipewire-0" ] || as_user ln -s "/run/user/$(id -u "$USER_NAME")/pipewire-0" "$RT/pipewire-0"
    sh "$HL" start
    sh "$0" touchscreen
    ;;
touchscreen)
    # One RemoteDesktop session for all the rig's touches, made before the app runs. neo-headless-input makes a
    # touchscreen per touch and drops it after, and that misbehaved here twice over: the seat lost its touch
    # capability between touches, so a touch that came while the app was busy was lost on the way (the app binds
    # wl_touch a round trip after the capability comes back), and GTK 4.24 crashed handling a touch queued from a
    # device it had already dropped (gdk_surface_handle_event -> gdk_seat_get_topmost_grab_surface on a NULL
    # seat). A phone's touchscreen never goes away; this one does not either. touch.in takes a command a line,
    # touch.ack answers when it is done.
    rm -f "$STATE/touch.in" "$STATE/touch.ack" "$STATE/touch.pid"
    as_user mkfifo "$STATE/touch.in" "$STATE/touch.ack"
    as_user setsid env NEO_HEADLESS_BUS="$(cat "$STATE/bus")" python3 -c '
import os, sys, time
from gi.repository import Gio, GLib
state = sys.argv[1]
with open(os.path.join(state, "touch.pid"), "w") as pid:
    pid.write("%d\n" % os.getpid())
bus = Gio.DBusConnection.new_for_address_sync(
    os.environ["NEO_HEADLESS_BUS"],
    Gio.DBusConnectionFlags.AUTHENTICATION_CLIENT | Gio.DBusConnectionFlags.MESSAGE_BUS_CONNECTION, None, None)
RD = "org.gnome.Mutter.RemoteDesktop"
SC = "org.gnome.Mutter.ScreenCast"
def call(name, path, interface, method, args=None, reply=None):
    return bus.call_sync(name, path, interface, method, args, GLib.VariantType(reply) if reply else None,
                         Gio.DBusCallFlags.NONE, -1, None)
session = call(RD, "/org/gnome/Mutter/RemoteDesktop", RD, "CreateSession", reply="(o)").unpack()[0]
session_id = call(RD, session, "org.freedesktop.DBus.Properties", "Get",
                  GLib.Variant("(ss)", (RD + ".Session", "SessionId")), "(v)").unpack()[0]
cast = call(SC, "/org/gnome/Mutter/ScreenCast", SC, "CreateSession",
            GLib.Variant("(a{sv})", ({"remote-desktop-session-id": GLib.Variant("s", session_id)},)),
            "(o)").unpack()[0]
# absolute touches need a stream of the monitor to give them a frame
stream = call(SC, cast, SC + ".Session", "RecordMonitor", GLib.Variant("(sa{sv})", ("Meta-0", {})),
              "(o)").unpack()[0]
def rd(method, signature=None, *args):
    call(RD, session, RD + ".Session", method, GLib.Variant(signature, args) if signature else None)
def down(x, y):
    rd("NotifyTouchDown", "(sudd)", stream, 0, x, y)
def move(x, y):
    rd("NotifyTouchMotion", "(sudd)", stream, 0, x, y)
def up():
    rd("NotifyTouchUp", "(u)", 0)
rd("Start")
# the touchscreen is made on its first touch, and that touch is lost: it lands before any app runs
down(1.0, 1.0)
up()
while True:
    with open(os.path.join(state, "touch.in")) as commands:
        line = commands.readline().split()
    try:
        # after a pause the first touch went missing, over and over; a Shift first (which no app acts on alone)
        # wakes the seat, and the touch after it arrives
        rd("NotifyKeyboardKeysym", "(ub)", 0xffe1, True)
        rd("NotifyKeyboardKeysym", "(ub)", 0xffe1, False)
        time.sleep(0.15)
        numbers = [float(v) for v in line[1:]]
        if line[0] == "tap":
            down(*numbers[:2]); time.sleep(0.06); up()
        elif line[0] == "press":
            # a finger held still goes on reporting, as a panel does at 120 Hz (and the app gets its events
            # while it waits for the long press)
            x, y = numbers[:2]
            down(x, y)
            for i in range(int((numbers[2] if len(numbers) > 2 else 800) / 8)):
                time.sleep(0.008)
                move(x, y)
            up()
        elif line[0] == "swipe":
            x0, y0, x1, y1 = numbers[:4]
            steps = max(1, int((numbers[4] if len(numbers) > 4 else 300) / 8))
            down(x0, y0)
            for i in range(1, steps + 1):
                time.sleep(0.008)
                move(x0 + (x1 - x0) * i / steps, y0 + (y1 - y0) * i / steps)
            up()
        answer = "ok"
    except Exception as error:
        answer = "error: %s" % error
    with open(os.path.join(state, "touch.ack"), "w") as ack:
        ack.write(answer + "\n")
 ' "$STATE" </dev/null >"$STATE/touch.log" 2>&1 &
    ;;
sshd)
    as_user /usr/sbin/sshd -f "$RIG/sshd/sshd_config" -E "$RIG/sshd/sshd.log"
    ;;
app)
    shift
    # never on a display in the session's runtime dir
    if [ ! -S "$RT/files" ]; then
        echo "try-files: no display in $RT (try-files.sh start)" >&2
        exit 1
    fi
    # the app's own pid goes into the display's app.pid (for shot), its bus address beside it (for secrets)
    sh "$HL" run env HOME="$RIG/home" XDG_DATA_HOME="$RIG/data" XDG_CONFIG_HOME="$RIG/config" \
        XDG_STATE_HOME="$RIG/state" XDG_CACHE_HOME="$RIG/cache" GSETTINGS_BACKEND=keyfile \
        GSETTINGS_SCHEMA_DIR="$RIG/schemas" LANG="${RIG_LANG:-en_US.UTF-8}" G_MESSAGES_DEBUG="${G_MESSAGES_DEBUG:-}" \
        dbus-run-session -- sh -c '
            state=$1; shift
            echo "$DBUS_SESSION_BUS_ADDRESS" >"$state/rig-bus"
            eval "$(printf %s rig-keyring | gnome-keyring-daemon --unlock --components=secrets --daemonize)"
            export GNOME_KEYRING_CONTROL
            "$@" &
            echo $! >"$state/app.pid"
            wait $! || true
            # the rig keyring: the one gnome-keyring-daemon of this runtime dir
            for p in $(pgrep -u "$(id -u)" gnome-keyring-d); do
                grep -qxz "XDG_RUNTIME_DIR=$XDG_RUNTIME_DIR" "/proc/$p/environ" 2>/dev/null && kill "$p"
            done
        ' sh "$STATE" "$BUILD" "$@"
    ;;
shot)
    sh "$HL" "$@"
    ;;
tap | press | swipe)
    if [ ! -p "$STATE/touch.in" ]; then
        echo "try-files: no touchscreen (try-files.sh touchscreen)" >&2
        exit 1
    fi
    as_user sh -c 'printf "%s\n" "$*" >"$0/touch.in"; read -r answer <"$0/touch.ack"; [ "$answer" = ok ] || { echo "$answer" >&2; exit 1; }' "$STATE" "$@"
    ;;
type | key)
    # keys through mutter's RemoteDesktop on the display's bus, by keysym: a printable character is its own keysym
    # (mutter presses the level's Shift itself); the display's keyboard keeps the seat's wl_keyboard, so none is lost
    as_user env NEO_HEADLESS_BUS="$(cat "$STATE/bus")" python3 -c '
import os, sys, time
from gi.repository import Gio, GLib
NAMES = {"Return": 0xff0d, "BackSpace": 0xff08, "Escape": 0xff1b, "Tab": 0xff09, "Back": 0x1008ff26}
bus = Gio.DBusConnection.new_for_address_sync(
    os.environ["NEO_HEADLESS_BUS"],
    Gio.DBusConnectionFlags.AUTHENTICATION_CLIENT | Gio.DBusConnectionFlags.MESSAGE_BUS_CONNECTION, None, None)
service = "org.gnome.Mutter.RemoteDesktop"
session = bus.call_sync(service, "/org/gnome/Mutter/RemoteDesktop", service, "CreateSession", None,
                        GLib.VariantType("(o)"), Gio.DBusCallFlags.NONE, -1, None).unpack()[0]
def rd(method, args=None):
    bus.call_sync(service, session, service + ".Session", method, args, None, Gio.DBusCallFlags.NONE, -1, None)
if sys.argv[1] == "type":
    keysyms = [ord(c) if ord(c) < 0x100 else 0x1000000 + ord(c) for c in " ".join(sys.argv[2:])]
else:
    keysyms = [NAMES[name] for name in sys.argv[2:]]
rd("Start")
for keysym in keysyms:
    rd("NotifyKeyboardKeysym", GLib.Variant("(ub)", (keysym, True)))
    rd("NotifyKeyboardKeysym", GLib.Variant("(ub)", (keysym, False)))
    time.sleep(0.03)
time.sleep(0.1)
rd("Stop")
' "$@"
    ;;
secrets)
    as_user env DBUS_SESSION_BUS_ADDRESS="$(cat "$STATE/rig-bus")" python3 -c '
import gi
gi.require_version("Secret", "1")
from gi.repository import Secret
service = Secret.Service.get_sync(Secret.ServiceFlags.LOAD_COLLECTIONS, None)
for collection in service.get_collections():
    print("collection", collection.get_label(), "locked" if collection.get_locked() else "unlocked")
    for item in collection.get_items():
        print("  item", repr(item.get_label()), item.get_schema_name(), dict(item.get_attributes()))
'
    ;;
quit)
    [ -f "$STATE/app.pid" ] && kill "$(cat "$STATE/app.pid")" 2>/dev/null || true
    ;;
stop)
    [ -f "$STATE/app.pid" ] && kill "$(cat "$STATE/app.pid")" 2>/dev/null || true
    [ -f "$STATE/touch.pid" ] && kill "$(cat "$STATE/touch.pid")" 2>/dev/null || true
    [ -f "$RIG/sshd/sshd.pid" ] && kill "$(cat "$RIG/sshd/sshd.pid")" 2>/dev/null || true
    sh "$HL" stop
    ;;
*)
    awk 'NR > 1 && !/^#/ { exit } NR > 1 { sub(/^# ?/, ""); print }' "$0"
    exit 2
    ;;
esac
