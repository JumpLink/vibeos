#!/bin/sh
# try-messaging.sh: the Messaging app on a test display of its own (tools/vibeos-headless), against the mock modem
# (mock-modem.py) and the SMS service on two buses of their own. No modem, no message leaves the phone, nothing on
# the phone's own screen; and no contacts (the rig's session bus has no address books), so none of the owner's
# show in its pictures.
#
#   try-messaging.sh start BUILD_DIR       the buses, the mock modem, the service (from BUILD_DIR), the display
#   try-messaging.sh app [ARGS...]         the app on the display
#   try-messaging.sh modem METHOD [ARGS]   one of the mock's controls: Incoming '"+49170…"' '["text"]' '""',
#                                          SetSendError '"…NoNetwork"', Report… (gdbus syntax for the arguments)
#   try-messaging.sh sms METHOD [ARGS]     one of the service's methods: GetConversation, Send…
#   try-messaging.sh open ID               what a tap on a message's notification does
#   try-messaging.sh shot OUT.png | tap X Y | swipe X0 Y0 X1 Y1 [MS]   as vibeos-headless
#   try-messaging.sh stop
#
# Run it as root on the phone, from the source tree's tests directory; it does the rest as the session user.
set -e

USER_NAME=${VIBEOS_USER:-user}
RUNTIME=/run/user/$(id -u "$USER_NAME")
RIG=$RUNTIME/try-messaging.d
HERE=$(dirname "$(readlink -f "$0")")
HEADLESS=${HEADLESS:-/root/vibeos-tools/vibeos-headless}
export VIBEOS_HEADLESS=try-messaging

as_user() {
    sudo -u "$USER_NAME" env XDG_RUNTIME_DIR="$RUNTIME" "$@"
}

# a daemon as the session user, detached, as vibeos-headless starts its own (see there why not `as_user ... &`)
as_user_daemon() {
    name=$1
    shift
    as_user setsid -f sh -c 'echo $$ >"$0"; exec "$@"' "$RIG/$name.pid" "$@" </dev/null >"$RIG/$name.log" 2>&1
}

# what the service, the mock and the app run with: the rig's buses, this run's message database and blocked list,
# the app's settings in memory, and no sound server (the sent sound stays quiet)
rig_env() {
    echo DBUS_SESSION_BUS_ADDRESS="$(cat "$RIG/session")" VIBEOS_MESSAGING_SYSTEM_BUS="$(cat "$RIG/system")" \
        GSETTINGS_BACKEND=memory GSETTINGS_SCHEMA_DIR="$RIG" PULSE_SERVER="unix:$RIG/no-sound" \
        XDG_DATA_HOME="$(cat "$RIG/run")/data"
}

session_call() {
    dest=$1 path=$2 method=$3
    shift 3
    as_user gdbus call --address "$(cat "$RIG/session")" --dest "$dest" --object-path "$path" --method "$method" "$@"
}

case $1 in
start)
    # a second start would take over the first one's pid files and leave its daemons running
    if [ -f "$RIG/sms.pid" ] && kill -0 "$(cat "$RIG/sms.pid")" 2>/dev/null; then
        echo "try-messaging: running already; stop it first" >&2
        exit 1
    fi
    build=$(readlink -f "$2")
    as_user mkdir -p "$RIG"
    # each run a database of its own (the rig is on tmpfs)
    run=$RIG/run-$(date +%s)
    as_user mkdir -p "$run/data"
    echo "$run" >"$RIG/run"
    # the session user cannot read root's home, where the build is
    install -o "$USER_NAME" -m 755 "$build/vibeos-messaging" "$build/vibeos-messaging-sms" "$HERE/mock-modem.py" "$RIG/"
    glib-compile-schemas --targetdir "$RIG" "$HERE/../data"
    cat >"$RIG/bus.conf" <<EOF
<busconfig><type>session</type><listen>unix:dir=$RIG</listen><auth>EXTERNAL</auth>
<policy context="default"><allow send_destination="*" eavesdrop="true"/><allow eavesdrop="true"/>
<allow own="*"/></policy></busconfig>
EOF
    for bus in session system; do
        as_user dbus-daemon --config-file="$RIG/bus.conf" --fork --print-address=1 --print-pid=1 \
            </dev/null >"$RIG/$bus.tmp"
        head -1 "$RIG/$bus.tmp" >"$RIG/$bus"
        tail -1 "$RIG/$bus.tmp" >"$RIG/$bus.bus-pid"
        rm -f "$RIG/$bus.tmp"
    done
    # --no-app: the real app is vibeos.Messaging on this bus
    # shellcheck disable=SC2046
    as_user_daemon mock env $(rig_env) python3 "$RIG/mock-modem.py" "$(cat "$RIG/system")" --no-app
    sleep 1
    # shellcheck disable=SC2046
    as_user_daemon sms env $(rig_env) "$RIG/vibeos-messaging-sms"
    sh "$HEADLESS" start
    ;;
app)
    shift
    # shellcheck disable=SC2046
    sh "$HEADLESS" run env $(rig_env) "$RIG/vibeos-messaging" "$@"
    ;;
modem)
    shift
    method=$1
    shift
    session_call vibeos.Test.Modem /vibeos/Test/Modem "vibeos.Test.Modem.$method" "$@"
    ;;
sms)
    shift
    method=$1
    shift
    session_call vibeos.Messaging.Sms /vibeos/Messaging/Sms "vibeos.Messaging.Sms.$method" "$@"
    ;;
open)
    session_call vibeos.Messaging /vibeos/Messaging org.freedesktop.Application.ActivateAction \
        open-conversation "[<int64 $2>]" "{}"
    ;;
shot | tap | swipe)
    sh "$HEADLESS" "$@"
    ;;
stop)
    sh "$HEADLESS" stop || true
    for p in sms mock session.bus system.bus; do
        [ -f "$RIG/$p-pid" ] && kill "$(cat "$RIG/$p-pid")" 2>/dev/null || true
        [ -f "$RIG/$p.pid" ] && kill "$(cat "$RIG/$p.pid")" 2>/dev/null || true
    done
    ;;
*)
    awk 'NR > 1 && !/^#/ { exit } NR > 1 { sub(/^# ?/, ""); print }' "$0"
    exit 2
    ;;
esac
