#!/bin/sh
# try-calendar.sh: the Calendar against an evolution-data-server of its own, on a test display of its own
# (tools/neo-headless). The phone's calendars are never touched: the rig's EDS runs on a private session bus, with
# its own config, data and cache directories, so it has only the built-in calendars and what cal-seed puts there.
#
#   try-calendar.sh start BUILD_DIR     the private bus (EDS starts on it when first asked), the display
#   try-calendar.sh seed [ARGS]         test events (BUILD_DIR/cal-seed: a "Work" calendar, events around today;
#                                       "seed alarm M": one starting in M minutes with a reminder at its start)
#   try-calendar.sh app [ARGS...]       the app on the display, on the rig's bus and settings
#   try-calendar.sh reminders           the reminder service on the rig's bus, with a stand-in notification server
#                                       there (fake-notify.py: nothing is shown or heard); its wake-ups go to the
#                                       phone's neo-wakeup under a key of their own, cleared on stop
#   try-calendar.sh notify-log          the notifications the reminder service sent
#   try-calendar.sh invoke ID ACTION    a tap on notification ID ("default") or its button ("snooze")
#   try-calendar.sh env                 the rig's environment, for running something else in it
#   try-calendar.sh shot OUT.png | tap X Y | swipe X0 Y0 X1 Y1 [MS]   as neo-headless
#   try-calendar.sh stop                the display, the app, the service, EDS and the bus
#
# Run it as root on the phone (or as the session user on a desktop, HEADLESS pointing at a neo-headless that works
# there); it does the rest as the session user.
#
# Nothing in the rig uses the session's runtime directory (/run/user/UID): the rig, its display and everything on
# its bus get one of their own, CAL_RIG_RUNTIME (/tmp/try-calendar-rt), made 700 for the session user. A GTK app on a
# private bus activates xdg-desktop-portal there, which brings up a second xdg-document-portal; in the session's
# directory that one takes over $XDG_RUNTIME_DIR/doc and the session's own portal dies (11-10), and an a11y bus
# started there would take .../at-spi the same way. The display needs a neo-headless that knows
# NEO_HEADLESS_RUNTIME (tools/neo-headless).
set -e

USER_NAME=${NEO_USER:-user}
RUNTIME=${CAL_RIG_RUNTIME:-/tmp/try-calendar-rt}
RIG=$RUNTIME/try-calendar.d
HERE=$(dirname "$(readlink -f "$0")")
HEADLESS=${HEADLESS:-/root/neo-tools/neo-headless}
export NEO_HEADLESS=${NEO_HEADLESS:-cal}
export NEO_HEADLESS_RUNTIME=$RUNTIME
if [ "$RUNTIME" = "/run/user/$(id -u "$USER_NAME")" ]; then
    echo "try-calendar: CAL_RIG_RUNTIME must not be the session's runtime directory" >&2
    exit 1
fi
if ! grep -q NEO_HEADLESS_RUNTIME "$HEADLESS" 2>/dev/null; then
    echo "try-calendar: $HEADLESS does not know NEO_HEADLESS_RUNTIME; HEADLESS=a newer tools/neo-headless" >&2
    exit 1
fi
# the session user's, 700, as a runtime directory must be (made when missing, kept when there)
if [ "$(id -un)" = "$USER_NAME" ]; then
    install -d -m 700 "$RUNTIME"
else
    install -d -m 700 -o "$USER_NAME" -g "$(id -gn "$USER_NAME")" "$RUNTIME"
fi

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

# what everything in the rig runs with: its bus, its runtime and other directories (EDS keeps its sources and events
# there), its settings in a keyfile of its own ($run/config/glib-2.0/settings/keyfile), with the app's schema
rig_env() {
    run=$(cat "$RIG/run")
    echo CAL_RIG_DIR="$run" DBUS_SESSION_BUS_ADDRESS="$(cat "$run/bus")" XDG_RUNTIME_DIR="$RUNTIME" \
        XDG_CONFIG_HOME="$run/config" XDG_DATA_HOME="$run/data" XDG_CACHE_HOME="$run/cache" \
        XDG_STATE_HOME="$run/state" GSETTINGS_BACKEND=keyfile GSETTINGS_SCHEMA_DIR="$run/schemas"
}

case $1 in
start)
    if [ -f "$RIG/run" ] && [ -f "$(cat "$RIG/run")/bus.pid" ] && kill -0 "$(cat "$(cat "$RIG/run")/bus.pid")" 2>/dev/null; then
        echo "try-calendar: running already; stop it first" >&2
        exit 1
    fi
    build=$(readlink -f "${2:?BUILD_DIR}")
    # each run its own calendars (the rig is on tmpfs)
    run=$RIG/run-$(date +%s)
    as_user mkdir -p "$run/config" "$run/data" "$run/cache" "$run/state" "$run/schemas" "$run/bin"
    echo "$run" | as_user tee "$RIG/run" >/dev/null
    # the programs and the schema where the session user can run and read them (not root's home)
    for f in neo-calendar neo-calendar-reminders cal-seed; do
        as_user tee "$run/bin/$f" <"$build/$f" >/dev/null
        chmod 755 "$run/bin/$f"
    done
    as_user tee "$run/bin/fake-notify.py" <"$HERE/fake-notify.py" >/dev/null
    as_user tee "$run/schemas/de.yesman.Calendar.gschema.xml" <"$HERE/../data/de.yesman.Calendar.gschema.xml" >/dev/null
    as_user glib-compile-schemas "$run/schemas"
    # The bus. Services it activates (evolution-source-registry, evolution-calendar-factory) inherit its
    # environment, so they keep everything under $run.
    as_user env CAL_RIG_DIR="$run" XDG_CONFIG_HOME="$run/config" XDG_DATA_HOME="$run/data" \
        XDG_CACHE_HOME="$run/cache" XDG_STATE_HOME="$run/state" GSETTINGS_BACKEND=keyfile \
        GSETTINGS_SCHEMA_DIR="$run/schemas" \
        dbus-daemon --session --fork --print-address=1 --print-pid=1 </dev/null >"$run/bus.tmp" 2>"$run/bus.log"
    head -1 "$run/bus.tmp" | as_user tee "$run/bus" >/dev/null
    tail -1 "$run/bus.tmp" | as_user tee "$run/bus.pid" >/dev/null
    "$HEADLESS" start
    ;;
seed)
    shift
    # shellcheck disable=SC2046
    as_user env $(rig_env) "$(cat "$RIG/run")/bin/cal-seed" "$@"
    ;;
app)
    shift
    # shellcheck disable=SC2046
    "$HEADLESS" run env $(rig_env) "$(cat "$RIG/run")/bin/neo-calendar" "$@"
    ;;
reminders)
    run=$(cat "$RIG/run")
    # shellcheck disable=SC2046
    as_user_daemon notify env $(rig_env) python3 -u "$run/bin/fake-notify.py"
    sleep 1
    # shellcheck disable=SC2046
    as_user_daemon reminders env $(rig_env) "$run/bin/neo-calendar-reminders"
    ;;
notify-log)
    cat "$RIG/notify.log"
    ;;
invoke)
    # shellcheck disable=SC2046
    as_user env $(rig_env) gdbus call --session --dest org.freedesktop.Notifications \
        --object-path /org/freedesktop/Notifications --method org.freedesktop.Notifications.TestInvoke "$2" "$3"
    ;;
env)
    rig_env
    ;;
shot | tap | swipe)
    "$HEADLESS" "$@"
    ;;
stop)
    "$HEADLESS" stop || true
    # each by its pid file, if that pid is still the program it was (a stale file's pid may be another's now)
    for p in reminders:neo-calendar-reminders notify:fake-notify.py; do
        f=$RIG/${p%%:*}.pid
        pid=$(cat "$f" 2>/dev/null || true)
        if [ -n "$pid" ] && tr '\0' ' ' <"/proc/$pid/cmdline" 2>/dev/null | grep -q "${p#*:}"; then
            kill "$pid" || true
        fi
        [ -f "$f" ] && : >"$f"
    done
    # the rig service's wake-up, if it left one
    as_user busctl --system call de.yesman.Wakeup /de/yesman/Wakeup de.yesman.Wakeup Set 'sxa{sv}' \
        calendar-reminder-rig 0 0 2>/dev/null || true
    if [ -f "$RIG/run" ] && [ -f "$(cat "$RIG/run")/bus" ]; then
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
    # a document portal the rig's bus started leaves its FUSE mount behind when it is killed
    if grep -q " $RUNTIME/doc " /proc/mounts 2>/dev/null; then
        as_user fusermount3 -u -z "$RUNTIME/doc" 2>/dev/null || umount -l "$RUNTIME/doc" 2>/dev/null || true
    fi
    ;;
*)
    awk 'NR > 1 && !/^#/ { exit } NR > 1 { sub(/^# ?/, ""); print }' "$0"
    exit 2
    ;;
esac
