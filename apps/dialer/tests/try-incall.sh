#!/bin/sh
# try-incall.sh: the in-call screen on a test display of its own (tools/neo-headless), against the mock phone
# (mock-phone.py) and the call service on two buses of their own. No modem, no real call, no sound, nothing on the
# phone's own screen: the service rings into a sound server that is not there and notifies the mock.
#
#   try-incall.sh start BUILD_DIR       the buses, the mock phone, the service (from BUILD_DIR), the display;
#                                       HOLD=1 in the environment for a modem that can hold calls, DTMF=1
#                                       for one that sends touch tones
#   try-incall.sh ui [ARGS...]          the in-call app on the display (neo-dialer --incall ARGS)
#   try-incall.sh app [ARGS...]         the Phone app on the display instead
#   try-incall.sh phone METHOD [ARGS]   one of the mock's controls: Incoming '"+4930…"', RemoteAnswer, RemoteHangup,
#                                       SetHold, Report… (gdbus syntax for the arguments)
#   try-incall.sh calls METHOD [ARGS]   one of the service's methods: Dial, Answer, Hangup…
#   try-incall.sh shot OUT.png | tap X Y | swipe X0 Y0 X1 Y1 [MS]   as neo-headless
#   try-incall.sh stop
#
# Run it as root on the phone, from the source tree's tests directory; it does the rest as the session user.
set -e

USER_NAME=${NEO_USER:-user}
RUNTIME=/run/user/$(id -u "$USER_NAME")
RIG=$RUNTIME/try-incall.d
HERE=$(dirname "$(readlink -f "$0")")
HEADLESS=${HEADLESS:-/root/neo-tools/neo-headless}
export NEO_HEADLESS=try-incall

as_user() {
    sudo -u "$USER_NAME" env XDG_RUNTIME_DIR="$RUNTIME" "$@"
}

# a daemon as the session user, detached, as neo-headless starts its own (see there why not `as_user ... &`)
as_user_daemon() {
    name=$1
    shift
    as_user setsid -f sh -c 'echo $$ >"$0"; exec "$@"' "$RIG/$name.pid" "$@" </dev/null >"$RIG/$name.log" 2>&1
}

# what the service and the mock run with: the rig's buses, its own settings and call log, and no sound server
rig_env() {
    echo DBUS_SESSION_BUS_ADDRESS="$(cat "$RIG/session")" NEO_DIALER_SYSTEM_BUS="$(cat "$RIG/system")" \
        GSETTINGS_BACKEND=memory GSETTINGS_SCHEMA_DIR="$RIG" PULSE_SERVER="unix:$RIG/no-sound"
}

case $1 in
start)
    # a second start would take over the first one's pid files and leave its daemons running
    if [ -f "$RIG/calls.pid" ] && kill -0 "$(cat "$RIG/calls.pid")" 2>/dev/null; then
        echo "try-incall: running already; stop it first" >&2
        exit 1
    fi
    build=$(readlink -f "$2")
    # each run its own call log, settings and what the service learned of the modem (the rig is on tmpfs)
    run=$RIG/run-$(date +%s)
    as_user mkdir -p "$run/data" "$run/state" "$run/config"
    # the session user cannot read root's home, where the build is
    install -o "$USER_NAME" -m 755 "$build/neo-dialer" "$build/neo-dialer-calls" "$HERE/mock-phone.py" "$RIG/"
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
    # shellcheck disable=SC2046
    as_user_daemon mock env $(rig_env) python3 "$RIG/mock-phone.py" "$(cat "$RIG/system")"
    sleep 1
    # HOLD=1: a modem that holds calls (the service finds out once, as it starts); DTMF=1: one that sends tones
    for feature in Hold:$HOLD Dtmf:$DTMF; do
        if [ -n "${feature#*:}" ]; then
            as_user gdbus call --address "$(cat "$RIG/session")" --dest de.yesman.Test.Phone \
                --object-path /de/yesman/Test/Phone --method "de.yesman.Test.Phone.Set${feature%%:*}" true >/dev/null
        fi
    done
    # shellcheck disable=SC2046
    as_user_daemon calls env $(rig_env) XDG_DATA_HOME="$run/data" XDG_STATE_HOME="$run/state" \
        XDG_CONFIG_HOME="$run/config" "$RIG/neo-dialer-calls"
    sh "$HEADLESS" start
    ;;
ui | app)
    what=$1
    shift
    [ "$what" = ui ] && set -- --incall "$@"
    # shellcheck disable=SC2046
    sh "$HEADLESS" run env $(rig_env) "$RIG/neo-dialer" "$@"
    ;;
phone)
    shift
    method=$1
    shift
    as_user gdbus call --address "$(cat "$RIG/session")" --dest de.yesman.Test.Phone \
        --object-path /de/yesman/Test/Phone --method "de.yesman.Test.Phone.$method" "$@"
    ;;
calls)
    shift
    method=$1
    shift
    as_user gdbus call --address "$(cat "$RIG/session")" --dest de.yesman.Dialer.Calls \
        --object-path /de/yesman/Dialer/Calls --method "de.yesman.Dialer.Calls.$method" "$@"
    ;;
shot | tap | swipe)
    sh "$HEADLESS" "$@"
    ;;
stop)
    sh "$HEADLESS" stop || true
    for p in calls mock session.bus system.bus; do
        [ -f "$RIG/$p-pid" ] && kill "$(cat "$RIG/$p-pid")" 2>/dev/null || true
        [ -f "$RIG/$p.pid" ] && kill "$(cat "$RIG/$p.pid")" 2>/dev/null || true
    done
    ;;
*)
    awk 'NR > 1 && !/^#/ { exit } NR > 1 { sub(/^# ?/, ""); print }' "$0"
    exit 2
    ;;
esac
