#!/bin/sh
# try-calendar.sh: the Calendar against an evolution-data-server of its own, on a test display of its own
# (tools/neo-headless). The phone's calendars are never touched: the rig's EDS runs on a private session bus, with
# its own config, data and cache directories, so it has only the built-in calendars and what cal-seed puts there.
#
#   try-calendar.sh start BUILD_DIR     the private bus (EDS starts on it when first asked), the display
#   try-calendar.sh seed [ARGS]         test events (BUILD_DIR/cal-seed: a "Work" calendar, events around today;
#                                       "seed alarm M": one starting in M minutes with a reminder at its start)
#   try-calendar.sh app [ARGS...]       the app on the display, on the rig's bus and settings
#   try-calendar.sh reminders           the reminder service on the rig's bus (notifications go to the rig's bus,
#                                       where nothing shows them: see notify-log)
#   try-calendar.sh notify-log          the notifications the reminder service sent (dbus-monitor on the rig's bus)
#   try-calendar.sh env                 the rig's environment, for running something else in it
#   try-calendar.sh shot OUT.png | tap X Y | swipe X0 Y0 X1 Y1 [MS]   as neo-headless
#   try-calendar.sh stop                the display, the app, the service, EDS and the bus
#
# Run it as root on the phone (or as the session user on a desktop, HEADLESS pointing at a neo-headless that works
# there); it does the rest as the session user.
set -e

USER_NAME=${NEO_USER:-user}
RUNTIME=/run/user/$(id -u "$USER_NAME")
RIG=$RUNTIME/try-calendar.d
HERE=$(dirname "$(readlink -f "$0")")
HEADLESS=${HEADLESS:-/root/neo-tools/neo-headless}
export NEO_HEADLESS=${NEO_HEADLESS:-cal}

as_user() {
    if [ "$(id -un)" = "$USER_NAME" ]; then
        env XDG_RUNTIME_DIR="$RUNTIME" "$@"
    else
        sudo -u "$USER_NAME" env XDG_RUNTIME_DIR="$RUNTIME" "$@"
    fi
}

# a daemon as the session user, detached, as neo-headless starts its own
as_user_daemon() {
    name=$1
    shift
    as_user setsid -f sh -c 'echo $$ >"$0"; exec "$@"' "$RIG/$name.pid" "$@" </dev/null >"$RIG/$name.log" 2>&1
}

# what everything in the rig runs with: its bus, its directories (EDS keeps its sources and events there), its
# settings in memory, with the app's schema
rig_env() {
    run=$(cat "$RIG/run")
    echo CAL_RIG_DIR="$run" DBUS_SESSION_BUS_ADDRESS="$(cat "$run/bus")" XDG_CONFIG_HOME="$run/config" \
        XDG_DATA_HOME="$run/data" XDG_CACHE_HOME="$run/cache" XDG_STATE_HOME="$run/state" \
        GSETTINGS_BACKEND=memory GSETTINGS_SCHEMA_DIR="$run/schemas"
}

case $1 in
start)
    if [ -f "$RIG/run" ] && [ -f "$(cat "$RIG/run")/bus.pid" ] && kill -0 "$(cat "$(cat "$RIG/run")/bus.pid")" 2>/dev/null; then
        echo "try-calendar: running already; stop it first" >&2
        exit 1
    fi
    build=$(readlink -f "$2")
    # each run its own calendars (the rig is on tmpfs)
    run=$RIG/run-$(date +%s)
    as_user mkdir -p "$run/config" "$run/data" "$run/cache" "$run/state" "$run/schemas"
    echo "$run" | as_user tee "$RIG/run" >/dev/null
    echo "$build" | as_user tee "$RIG/build" >/dev/null
    as_user cp "$HERE/../data/de.yesman.Calendar.gschema.xml" "$run/schemas/"
    as_user glib-compile-schemas "$run/schemas"
    # The bus. Services it activates (evolution-source-registry, evolution-calendar-factory) inherit its
    # environment, so they keep everything under $run.
    as_user env CAL_RIG_DIR="$run" XDG_CONFIG_HOME="$run/config" XDG_DATA_HOME="$run/data" \
        XDG_CACHE_HOME="$run/cache" XDG_STATE_HOME="$run/state" GSETTINGS_BACKEND=memory \
        GSETTINGS_SCHEMA_DIR="$run/schemas" \
        dbus-daemon --session --fork --print-address=1 --print-pid=1 </dev/null >"$run/bus.tmp"
    head -1 "$run/bus.tmp" | as_user tee "$run/bus" >/dev/null
    tail -1 "$run/bus.tmp" | as_user tee "$run/bus.pid" >/dev/null
    rm -f "$run/bus.tmp"
    "$HEADLESS" start
    ;;
seed)
    shift
    # shellcheck disable=SC2046
    as_user env $(rig_env) "$(cat "$RIG/build")/cal-seed" "$@"
    ;;
app)
    shift
    # shellcheck disable=SC2046
    "$HEADLESS" run env $(rig_env) "$(cat "$RIG/build")/neo-calendar" "$@"
    ;;
reminders)
    run=$(cat "$RIG/run")
    # shellcheck disable=SC2046
    as_user_daemon reminders env $(rig_env) "$(cat "$RIG/build")/neo-calendar-reminders"
    # shellcheck disable=SC2046
    as_user_daemon monitor env $(rig_env) dbus-monitor --session \
        "interface='org.freedesktop.Notifications',member='Notify'"
    ;;
notify-log)
    cat "$RIG/monitor.log"
    ;;
env)
    rig_env
    ;;
shot | tap | swipe)
    "$HEADLESS" "$@"
    ;;
stop)
    "$HEADLESS" stop || true
    for p in reminders monitor; do
        [ -f "$RIG/$p.pid" ] && kill "$(cat "$RIG/$p.pid")" 2>/dev/null || true
        rm -f "$RIG/$p.pid"
    done
    if [ -f "$RIG/run" ]; then
        run=$(cat "$RIG/run")
        bus=$(cat "$run/bus")
        # the services the bus started (EDS), by their exact pids, then the bus
        for name in $(as_user gdbus call --address "$bus" --dest org.freedesktop.DBus --object-path /org/freedesktop/DBus \
                --method org.freedesktop.DBus.ListNames 2>/dev/null | grep -o "':[0-9.]*'" | tr -d "'"); do
            pid=$(as_user gdbus call --address "$bus" --dest org.freedesktop.DBus --object-path /org/freedesktop/DBus \
                --method org.freedesktop.DBus.GetConnectionUnixProcessID "$name" 2>/dev/null | grep -o '[0-9]\+' | tail -1)
            [ -n "$pid" ] && [ "$pid" != "$$" ] && kill "$pid" 2>/dev/null || true
        done
        kill "$(cat "$run/bus.pid")" 2>/dev/null || true
    fi
    ;;
*)
    awk 'NR > 1 && !/^#/ { exit } NR > 1 { sub(/^# ?/, ""); print }' "$0"
    exit 2
    ;;
esac
