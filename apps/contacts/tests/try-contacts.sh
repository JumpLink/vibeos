#!/bin/sh
# try-contacts.sh: the Contacts app on a test display, against an address book of its own.
#
# Everything here is private: a D-Bus session bus of the rig's own, with evolution-data-server on it (started by
# D-Bus activation from that bus, so it keeps its books in the rig's XDG dirs), GSettings in memory, and made-up
# contacts (ct-seed). The phone's real session bus, its EDS and its address book are never touched. Links the app
# hands out (tel:, sms:, mailto:, geo:, http:) go to a stand-in that writes them to $RIG/opened.log, so a tap on a
# number calls nobody.
#
#   try-contacts.sh start BUILD     the bus, EDS, the schema, the contacts (BUILD: the meson build directory)
#   try-contacts.sh app BUILD [ARGS] the app on the neo-headless display "contacts" (started if needed)
#   try-contacts.sh action NAME [PARAMETER]   activate one of the app's actions (gapplication action)
#   try-contacts.sh stop            the app, the display, EDS and the bus: only the PIDs this script recorded
#
# Run it as root on the phone, as neo-headless is; the app and EDS run as the session user.
set -eu

USER_NAME=${NEO_USER:-user}
UID_=$(id -u "$USER_NAME")
RUNTIME=/run/user/$UID_
RIG=${CT_RIG_DIR:-$RUNTIME/contacts-rig}
HEADLESS=${NEO_HEADLESS_TOOL:-/root/neo-tools/neo-headless}
HERE=$(dirname "$(readlink -f "$0")")
export NEO_HEADLESS=contacts

as_user() {
    sudo -u "$USER_NAME" env XDG_RUNTIME_DIR="$RUNTIME" "$@"
}

# a command as the session user in the rig's environment, on its bus
rig() {
    as_user env DBUS_SESSION_BUS_ADDRESS="$(cat "$RIG/bus")" CT_RIG_DIR="$RIG" \
        XDG_DATA_HOME="$RIG/data" XDG_CONFIG_HOME="$RIG/config" XDG_CACHE_HOME="$RIG/cache" \
        GSETTINGS_BACKEND=memory GSETTINGS_SCHEMA_DIR="$RIG/schemas" "$@"
}

# the PID of the process that owns a name on the rig's bus
owner_pid() {
    rig gdbus call --session --dest org.freedesktop.DBus --object-path /org/freedesktop/DBus \
        --method org.freedesktop.DBus.GetConnectionUnixProcessID "$1" 2>/dev/null | sed -n 's/.*uint32 \([0-9]*\).*/\1/p'
}

case ${1:-} in
start)
    build=$(readlink -f "$2")
    if [ -f "$RIG/bus.pid" ] && kill -0 "$(cat "$RIG/bus.pid")" 2>/dev/null; then
        echo "try-contacts: the rig is running already; stop it first" >&2
        exit 1
    fi
    as_user mkdir -p "$RIG/data/applications" "$RIG/config" "$RIG/cache" "$RIG/schemas"
    # the app's schema, compiled where GSETTINGS_SCHEMA_DIR points (the system's stay on the path too)
    as_user cp "$HERE/../data/de.yesman.Contacts.gschema.xml" "$RIG/schemas/"
    as_user glib-compile-schemas "$RIG/schemas"
    # the stand-in for the apps that take links: it only writes down what it was given
    as_user sh -c "cat >'$RIG/links.sh'" <<EOF
#!/bin/sh
echo "\$(date +%T) \$1" >>"$RIG/opened.log"
EOF
    as_user sh -c "cat >'$RIG/data/applications/ct-rig-links.desktop'" <<EOF
[Desktop Entry]
Type=Application
Name=Rig links
Exec=/bin/sh $RIG/links.sh %u
MimeType=x-scheme-handler/tel;x-scheme-handler/sms;x-scheme-handler/smsto;x-scheme-handler/mailto;x-scheme-handler/geo;x-scheme-handler/http;x-scheme-handler/https;x-scheme-handler/sip;
NoDisplay=true
EOF
    as_user sh -c "cat >'$RIG/config/mimeapps.list'" <<EOF
[Default Applications]
x-scheme-handler/tel=ct-rig-links.desktop
x-scheme-handler/sms=ct-rig-links.desktop
x-scheme-handler/smsto=ct-rig-links.desktop
x-scheme-handler/mailto=ct-rig-links.desktop
x-scheme-handler/geo=ct-rig-links.desktop
x-scheme-handler/http=ct-rig-links.desktop
x-scheme-handler/https=ct-rig-links.desktop
x-scheme-handler/sip=ct-rig-links.desktop
EOF
    # the bus; whatever D-Bus starts on it (EDS) gets this environment
    as_user env XDG_DATA_HOME="$RIG/data" XDG_CONFIG_HOME="$RIG/config" XDG_CACHE_HOME="$RIG/cache" \
        GSETTINGS_BACKEND=memory GSETTINGS_SCHEMA_DIR="$RIG/schemas" \
        dbus-daemon --session --fork --print-address=1 --print-pid=1 </dev/null >"$RIG/bus.tmp"
    head -1 "$RIG/bus.tmp" >"$RIG/bus"
    tail -1 "$RIG/bus.tmp" >"$RIG/bus.pid"
    rm -f "$RIG/bus.tmp"
    # the made-up contacts; this starts EDS's registry and address book factory on the rig's bus
    rig "$build/ct-seed"
    for name in org.gnome.evolution.dataserver.Sources5 org.gnome.evolution.dataserver.AddressBook10; do
        pid=$(owner_pid "$name")
        [ -n "$pid" ] && echo "$pid" >>"$RIG/eds.pids"
    done
    echo "try-contacts: bus $(cat "$RIG/bus.pid"), EDS $(tr '\n' ' ' <"$RIG/eds.pids" 2>/dev/null)"
    ;;
app)
    build=$(readlink -f "$2")
    shift 2
    if ! [ -f "$RUNTIME/contacts.d/mutter.pid" ] || ! kill -0 "$(cat "$RUNTIME/contacts.d/mutter.pid")" 2>/dev/null
    then
        sh "$HEADLESS" start
    fi
    # `env` execs the app, so the display's recorded app PID is the app's own (its shots come on SIGUSR1)
    sh "$HEADLESS" run env DBUS_SESSION_BUS_ADDRESS="$(cat "$RIG/bus")" CT_RIG_DIR="$RIG" \
        XDG_DATA_HOME="$RIG/data" XDG_CONFIG_HOME="$RIG/config" XDG_CACHE_HOME="$RIG/cache" \
        GSETTINGS_BACKEND=memory GSETTINGS_SCHEMA_DIR="$RIG/schemas" "$build/neo-contacts" "$@"
    ;;
action)
    shift
    rig gapplication action de.yesman.Contacts "$@"
    ;;
stop)
    sh "$HEADLESS" stop || true
    # EDS's services as they are now (the address book factory comes and goes with its clients); anything else
    # they started has the bus as its only connection and leaves with it
    if [ -f "$RIG/bus.pid" ] && kill -0 "$(cat "$RIG/bus.pid")" 2>/dev/null; then
        for name in org.gnome.evolution.dataserver.AddressBook10 org.gnome.evolution.dataserver.Sources5; do
            owner_pid "$name" >>"$RIG/eds.pids" || true
        done
    fi
    # an EDS PID is killed only while it is still the rig's (its environment has the rig's data dir), in case the
    # number has been reused
    if [ -f "$RIG/eds.pids" ]; then
        sort -u "$RIG/eds.pids" | while read -r pid; do
            [ -n "$pid" ] && tr '\0' '\n' <"/proc/$pid/environ" 2>/dev/null | grep -qx "XDG_DATA_HOME=$RIG/data" &&
                kill "$pid" 2>/dev/null || true
        done
        rm -f "$RIG/eds.pids"
    fi
    if [ -f "$RIG/bus.pid" ]; then
        kill "$(cat "$RIG/bus.pid")" 2>/dev/null || true
        rm -f "$RIG/bus.pid"
    fi
    ;;
*)
    awk 'NR > 1 && !/^#/ { exit } NR > 1 { sub(/^# ?/, ""); print }' "$0"
    exit 2
    ;;
esac
