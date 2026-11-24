#!/bin/bash
# Push messages end to end, through a real push server (https://push.vibeos.top, or $E2E_SERVER), on this machine:
# the distributor (vibeos-unifiedpush), apps built on libvibeos's VibeosUnifiedPush (vibeos-push-test), an app that
# speaks the UnifiedPush D-Bus spec itself (spec-client.py), and a sender that encrypts and signs as an app server
# does (vibeos-push-send).
#
#   tools/unifiedpush/e2e.sh TOP_BUILD_DIR LIBVIBEOS_BUILD_DIR
#
# The build directories are the top-level project's (unifiedpush/vibeos-unifiedpush) and libvibeos's
# (unifiedpush/vibeos-push-test, unifiedpush/vibeos-push-send). Needs dbus-run-session, pasta (passt), iproute2,
# gdbus and python3-gi.
#
# Everything runs on a private session bus (dbus-run-session) with a home, XDG directories and a runtime directory of
# its own, in a network namespace of its own (pasta), so that the network can be taken from the distributor alone;
# the user's session is never touched. It tries: registering (and the endpoint's form), a repeated Register, two
# apps and two registrations of one app at once, messages in order, the size limits, Register's checks, Collect, a
# restart of the distributor (the messages sent meanwhile come after it), an app started by a message (and exiting
# by itself), the network going and coming back, a stream that goes silent, unregistering (from the app and from
# Settings), an uninstalled app, and that the distributor's log gives away no topic or token.
#
# The work directory is kept and printed at the end: it holds the logs, and the test's endpoints and keys.
set -u
SELF=$(readlink -f "$0")
SRC=$(cd "$(dirname "$SELF")/../.." && pwd)

# --- inside the network namespace: the private bus
if [ "${1:-}" = --inside ]; then
  exec dbus-run-session --config-file="$E2E_T/session.conf" -- "$SELF" --steps
fi

# --- on the private bus: the test
if [ "${1:-}" = --steps ]; then
  T=$E2E_T
  SERVER=${E2E_SERVER:-https://push.vibeos.top}
  NAME=org.unifiedpush.Distributor.vibeos
  DIST_PID= ONE_PID= TWO_PID= GONE_PID=

  log () { echo "-- $*"; }
  result () {   # NAME PASS|FAIL [detail]
    echo "$2 $1${3:+ ($3)}" | tee -a "$T/results"
  }
  check () {    # NAME COMMAND...: PASS when the command succeeds
    local name=$1; shift
    if "$@"; then result "$name" PASS; else result "$name" FAIL; fi
  }
  # waits until FILE has a line matching the extended regular expression, for at most SECONDS
  wait_for () {
    local file=$1 pattern=$2 seconds=${3:-30} i=0
    while [ $i -lt $((seconds * 10)) ]; do
      grep -qE -- "$pattern" "$file" 2>/dev/null && return 0
      sleep 0.1; i=$((i + 1))
    done
    return 1
  }
  has_owner () {
    gdbus call --session --dest org.freedesktop.DBus --object-path /org/freedesktop/DBus \
      --method org.freedesktop.DBus.NameHasOwner "$1" 2>/dev/null | grep -q true
  }
  wait_owner () {   # NAME yes|no SECONDS
    local i=0
    while [ $i -lt $(($3 * 10)) ]; do
      if has_owner "$1"; then [ "$2" = yes ] && return 0; else [ "$2" = no ] && return 0; fi
      sleep 0.1; i=$((i + 1))
    done
    return 1
  }
  prop () {
    gdbus call --session --dest $NAME --object-path /vibeos/UnifiedPush \
      --method org.freedesktop.DBus.Properties.Get vibeos.UnifiedPush1 "$1" | sed -E "s/^\(<(uint16 )?'?([^']*)'?>,\)$/\2/"
  }
  manager () {
    local method=$1; shift
    gdbus call --session --dest $NAME --object-path /vibeos/UnifiedPush --method "vibeos.UnifiedPush1.$method" "$@"
  }
  register_raw () {   # the options of a Register call, as GVariant text
    gdbus call --session --dest $NAME --object-path /org/unifiedpush/Distributor \
      --method org.unifiedpush.Distributor2.Register "$1" 2>&1
  }
  # the registrations, one a line: service id active activatable pending messages
  registrations () {
    python3 -I -c '
from gi.repository import Gio
bus = Gio.bus_get_sync(Gio.BusType.SESSION)
reply = bus.call_sync("org.unifiedpush.Distributor.vibeos", "/vibeos/UnifiedPush", "vibeos.UnifiedPush1",
                      "ListRegistrations", None, None, 0, -1, None)
for r in reply.unpack()[0]:
    print(r["service"], r["id"], r["active"], r["activatable"], r["pending"], r["messages"])'
  }
  wait_state () {   # STATE SECONDS
    local i=0
    while [ $i -lt $(($2 * 10)) ]; do
      [ "$(prop State 2>/dev/null)" = "$1" ] && return 0
      sleep 0.1; i=$((i + 1))
    done
    return 1
  }
  # the last endpoint, keys included, that LOG has for INSTANCE: "endpoint p256dh auth"
  endpoint_of () {
    grep -E "^ENDPOINT $2 " "$1" | tail -n 1 | cut -d' ' -f3-5
  }
  sender_args () {   # LOG INSTANCE
    local e
    read -r -a e <<<"$(endpoint_of "$1" "$2")"
    printf '%q ' --endpoint "${e[0]}" --p256dh "${e[1]}" --auth "${e[2]}" --vapid-key "$T/vapid.key"
  }
  send () {   # LOG INSTANCE [vibeos-push-send's options and TEXT]
    local args
    args=$(sender_args "$1" "$2"); shift 2
    eval "\"\$E2E_PUSH_SEND\" $args $(printf '%q ' "$@")" >>"$T/send.log" 2>&1
  }
  # runs a shell command line in the script outside the namespace (which has the network, and $PASTA)
  outside () {
    printf '%s\n' "$1" >"$T/ctl/request.new" && mv "$T/ctl/request.new" "$T/ctl/request"
    local i=0
    while [ ! -e "$T/ctl/done" ]; do
      sleep 0.1; i=$((i + 1)); [ $i -gt 1200 ] && return 1
    done
    rm -f "$T/ctl/done"
    cat "$T/ctl/out"
    return "$(cat "$T/ctl/status")"
  }
  send_outside () {   # as send, from outside the namespace
    local args
    args=$(sender_args "$1" "$2"); shift 2
    outside "\"\$E2E_PUSH_SEND\" $args $(printf '%q ' "$@") >>\"\$E2E_T/send.log\" 2>&1"
  }
  start_distributor () {
    "$T/bin/distributor" &
    DIST_PID=$!
    wait_owner $NAME yes 10
  }
  messages () {   # LOG INSTANCE: the texts of the messages, in order
    grep -E "^MESSAGE $2 " "$1" | cut -d' ' -f4-
  }
  finish () {
    for pid in $GONE_PID $TWO_PID $ONE_PID $DIST_PID; do kill "$pid" 2>/dev/null; done
    wait 2>/dev/null
  }
  trap finish EXIT

  gsettings set vibeos.UnifiedPush server-url "$SERVER"
  # a stream with nothing in it for 2 x 25 + 15 s is taken for dead, so the silent stream is found sooner
  gsettings set vibeos.UnifiedPush keepalive-interval 25
  VAPID=$("$E2E_PUSH_SEND" --vapid-key "$T/vapid.key" --print-public-key)

  log "the distributor"
  check "the distributor starts and owns $NAME" start_distributor
  check "with nothing registered it is idle" test "$(prop State)" = idle

  log "registering"
  "$E2E_PUSH_TEST" --app-id test.vibeos.PushOne --description "Test one" --vapid "$VAPID" --log "$T/one.log" \
    >>"$T/one.out" 2>&1 &
  ONE_PID=$!
  check "an app registers and gets its endpoint" wait_for "$T/one.log" '^ENDPOINT default ' 30
  endpoint=$(endpoint_of "$T/one.log" default | cut -d' ' -f1)
  check "the endpoint is <server>/up<12 characters>?up=1" \
    grep -qE "^${SERVER//./\\.}/up[A-Za-z0-9_-]{12}\\?up=1\$" <<<"$endpoint"
  check "the stream is connected" wait_state connected 10
  # the tokens, for the look at the log at the end (unregistering deletes them)
  grep -h '^token=' "$XDG_DATA_HOME"/test.vibeos.*/unifiedpush/* | cut -d= -f2 >>"$T/tokens"
  result "the stream's server address, for a wake pattern" INFO \
    "$(prop StreamRemoteAddress):$(prop StreamRemotePort) from $(prop StreamLocalAddress):$(prop StreamLocalPort)"

  "$E2E_PUSH_TEST" --app-id test.vibeos.PushTwo --instance default --instance account-2 --log "$T/two.log" \
    >>"$T/two.out" 2>&1 &
  TWO_PID=$!
  wait_for "$T/two.log" '^ENDPOINT default ' 30 && wait_for "$T/two.log" '^ENDPOINT account-2 ' 30
  grep -h '^token=' "$XDG_DATA_HOME"/test.vibeos.*/unifiedpush/* | cut -d= -f2 >>"$T/tokens"
  check "two registrations of one app get two endpoints" test -n "$(endpoint_of "$T/two.log" account-2)" \
    -a "$(endpoint_of "$T/two.log" default | cut -d' ' -f1)" != "$(endpoint_of "$T/two.log" account-2 | cut -d' ' -f1)"

  spec_token=$(head -c 16 /dev/urandom | base64 | tr '+/' '-_' | tr -d '=')
  check "every Register, a repeated one too, is followed by NewEndpoint (an app on the spec alone)" \
    python3 -I "$SRC/tools/unifiedpush/spec-client.py" test.vibeos.SpecClient "$spec_token" 3 >"$T/spec.log" 2>&1
  check "  ... three Registers, three NewEndpoints" test "$(grep -c '^NEWENDPOINT' "$T/spec.log")" = 3
  spec_token2=$(head -c 16 /dev/urandom | base64 | tr '+/' '-_' | tr -d '=')
  check "  ... three Registers at once, three NewEndpoints" \
    python3 -I "$SRC/tools/unifiedpush/spec-client.py" test.vibeos.SpecClient "$spec_token2" 3 burst >"$T/spec2.log" 2>&1

  log "Register's checks"
  long=$(printf 'x%.0s' $(seq 101))
  check "a token over 100 bytes is refused (InvalidArgs)" \
    grep -q InvalidArgs <<<"$(register_raw "{'service': <'test.vibeos.Nobody'>, 'token': <'$long'>}")"
  check "a description over 100 bytes is refused (InvalidArgs)" \
    grep -q InvalidArgs <<<"$(register_raw "{'service': <'test.vibeos.Nobody'>, 'token': <'t1'>, 'description': <'$long'>}")"
  check "a VAPID key that is not one is refused (InvalidArgs)" \
    grep -q InvalidArgs <<<"$(register_raw "{'service': <'test.vibeos.Nobody'>, 'token': <'t1'>, 'vapid': <'abc'>}")"
  check "registering for another app's name is refused (UNAUTHORIZED)" \
    grep -q UNAUTHORIZED <<<"$(register_raw "{'service': <'test.vibeos.PushOne'>, 'token': <'t2'>}")"

  log "messages"
  send "$T/one.log" default "Hello, one"
  check "a message arrives, decrypted" wait_for "$T/one.log" '^MESSAGE default 10 Hello, one$' 30
  for i in 1 2 3 4 5; do
    send "$T/two.log" default "two default $i"
    send "$T/two.log" account-2 "two account-2 $i"
  done
  wait_for "$T/two.log" '^MESSAGE default .* two default 5$' 30
  wait_for "$T/two.log" '^MESSAGE account-2 .* two account-2 5$' 30
  check "two registrations' messages each go to their own, in order" \
    test "$(messages "$T/two.log" default | tr '\n' /)$(messages "$T/two.log" account-2 | tr '\n' /)" = \
         "two default 1/two default 2/two default 3/two default 4/two default 5/two account-2 1/two account-2 2/two account-2 3/two account-2 4/two account-2 5/"

  log "sizes"
  send "$T/one.log" default --size 3993
  check "3993 bytes, the most a Web Push message carries, arrive whole" \
    wait_for "$T/one.log" '^MESSAGE default 3993 0123456789abcdefghij' 30
  send "$T/one.log" default --size 3994 --too-big
  check "one byte more (a 4097-byte body) cannot be decrypted (the server cuts it to 4096)" \
    wait_for "$T/one.out" 'cannot be decrypted' 30
  send "$T/one.log" default "After the big one"
  check "  ... and the next message still comes" wait_for "$T/one.log" '^MESSAGE default 17 After the big one$' 30

  log "Collect"
  send "$T/one.log" default "Collected"
  collected=$(manager Collect 2>&1)
  check "Collect returns once what was waiting is delivered" grep -q '^MESSAGE default 9 Collected$' "$T/one.log"
  result "  ... Collect's answer" INFO "$collected"

  log "restarting the distributor"
  kill -TERM "$DIST_PID"; wait "$DIST_PID"
  check "it stops on SIGTERM" test $? = 0
  send "$T/one.log" default "While it was down, 1"
  send "$T/one.log" default "While it was down, 2"
  start_distributor
  check "the messages sent while it was down come once it is back, in order" \
    wait_for "$T/one.log" '^MESSAGE default .* While it was down, 2$' 30
  check "  ... in order" test "$(messages "$T/one.log" default | grep -c 'While it was down')" = 2 \
    -a "$(messages "$T/one.log" default | grep 'While it was down' | head -n 1)" = "While it was down, 1"
  check "  ... and nothing before them comes again" test "$(grep -c 'Hello, one$' "$T/one.log")" = 1

  log "an app started by a message"
  kill -TERM "$ONE_PID"; wait "$ONE_PID"; ONE_PID=
  wait_owner test.vibeos.PushOne no 10
  send "$T/one.log" default "Wakes the app"
  check "a message starts an app that is not running (D-Bus activation) and reaches it" \
    wait_for "$T/one.log" '^MESSAGE default .* Wakes the app$' 40
  check "  ... and the app exits by itself afterwards" wait_owner test.vibeos.PushOne no 30

  log "the network goes"
  dev=$(ip route show default | awk '{ print $5; exit }')
  routes=$(ip -4 route show | tac)    # the subnet's route first, then the default one
  ip link set dev "$dev" down
  check "the distributor sees the network go" wait_state offline 20
  send_outside "$T/two.log" default "While offline, 1"
  send_outside "$T/two.log" default "While offline, 2"
  sleep 5
  ip link set dev "$dev" up
  # pasta's address is "noprefixroute": the routes do not come back by themselves
  while read -r route; do ip route replace $route; done <<<"$routes"
  check "the messages sent while offline come once the network is back" \
    wait_for "$T/two.log" '^MESSAGE default .* While offline, 2$' 60
  check "  ... both, in order" test "$(messages "$T/two.log" default | grep 'While offline' | tr '\n' /)" = \
    "While offline, 1/While offline, 2/"

  wait_state connected 30
  sleep 5
  reconnects=$(grep -c 'connecting to' "$T/distributor.log")
  ip route add 192.0.2.0/24 dev "$dev"      # a change that has nothing to do with the stream
  sleep 4
  ip route del 192.0.2.0/24 dev "$dev"
  sleep 3
  check "a network change that leaves the stream's address alone leaves the stream alone" \
    test "$(grep -c 'connecting to' "$T/distributor.log")" = "$reconnects"

  log "the stream goes silent"
  wait_state connected 30
  outside 'kill -STOP "$PASTA"'
  stopped=$(date +%s)
  send_outside "$T/two.log" account-2 "While the stream was silent"
  check "a stream with nothing in it, not even a keepalive, is taken for dead" \
    wait_for "$T/distributor.log" 'not even a keepalive' 120
  result "  ... after" INFO "$(($(date +%s) - stopped)) s"
  outside 'kill -CONT "$PASTA"'
  check "  ... and the message sent meanwhile comes on the new one" \
    wait_for "$T/two.log" '^MESSAGE account-2 .* While the stream was silent$' 90

  log "unregistering"
  kill -TERM "$TWO_PID"; wait "$TWO_PID"; TWO_PID=
  old_account=$(sender_args "$T/two.log" account-2)
  "$E2E_PUSH_TEST" --app-id test.vibeos.PushTwo --instance account-2 --unregister --log "$T/two.log" \
    >>"$T/two.out" 2>&1
  check "an app unregisters" grep -q '^UNREGISTERED account-2$' "$T/two.log"
  check "  ... and the distributor has its other registration only" \
    test "$(registrations | grep -c '^test.vibeos.PushTwo ')" = 1
  "$E2E_PUSH_TEST" --app-id test.vibeos.PushTwo --instance default --log "$T/two.log" >>"$T/two.out" 2>&1 &
  TWO_PID=$!
  wait_owner test.vibeos.PushTwo yes 10
  status=$(eval "\"\$E2E_PUSH_SEND\" $old_account 'After unregistering'" 2>&1 | head -n 1)
  result "  ... the push server's answer to a message for the old endpoint" INFO "$status"
  sleep 10
  check "  ... which does not reach the app" test "$(grep -c 'After unregistering' "$T/two.log")" = 0

  one_id=$(registrations | awk '$1 == "test.vibeos.PushOne" { print $2 }')
  manager RemoveRegistration "$one_id" >/dev/null
  check "removed in Settings: the app is told, started for it" wait_for "$T/one.log" '^UNREGISTERED default$' 30
  check "  ... and the registration is gone" test -z "$(registrations | grep '^test.vibeos.PushOne ')"

  log "an app that is uninstalled"
  "$E2E_PUSH_TEST" --app-id test.vibeos.PushGone --log "$T/gone.log" >>"$T/gone.out" 2>&1 &
  GONE_PID=$!
  wait_for "$T/gone.log" '^ENDPOINT default ' 30
  grep -h '^token=' "$XDG_DATA_HOME"/test.vibeos.*/unifiedpush/* | cut -d= -f2 >>"$T/tokens"
  kill -TERM "$GONE_PID"; wait "$GONE_PID"; GONE_PID=
  mv "$T/services/test.vibeos.PushGone.service" "$T/aside/"
  send "$T/gone.log" default "Nobody is there"
  send "$T/two.log" default "Two is there"
  kill -TERM "$TWO_PID"; wait "$TWO_PID"; TWO_PID=
  send "$T/two.log" default "Two is gone for now"
  check "an uninstalled app's registration goes after a minute" \
    wait_for "$T/distributor.log" 'test\.vibeos\.PushGone is not installed any more' 90
  check "  ... from the list too" test -z "$(registrations | grep '^test.vibeos.PushGone ')"
  check "an app that was never installed (only gets messages while it runs) keeps its registration" \
    test -n "$(registrations | grep '^test.vibeos.PushTwo ')"

  log "secrets"
  secrets=$( (grep -ohE '/up[A-Za-z0-9_-]{12}' "$T"/one.log "$T"/two.log "$T"/gone.log "$T"/spec.log "$T"/spec2.log | cut -c2-
             cat "$T/tokens"; echo "$spec_token"; echo "$spec_token2") | sort -u)
  leaked=0
  for s in $secrets; do
    grep -qF -- "$s" "$T/distributor.log" && leaked=$((leaked + 1))
  done
  check "the distributor's log shows no topic and no token ($(wc -w <<<"$secrets") looked for)" test $leaked = 0
  check "the distributor's state: 0700 directory, 0600 file" \
    test "$(stat -c %a "$XDG_STATE_HOME/vibeos-unifiedpush")$(stat -c %a "$XDG_STATE_HOME/vibeos-unifiedpush/state.json")" = 700600
  check "an app's push state: 0700 directory, 0600 file" \
    test "$(stat -c %a "$XDG_DATA_HOME/test.vibeos.PushTwo/unifiedpush")$(stat -c %a "$XDG_DATA_HOME/test.vibeos.PushTwo/unifiedpush/default")" = 700600
  exit 0
fi

# --- outside: the work directory, then the namespace
if [ $# -ne 2 ]; then
  echo "usage: $0 TOP_BUILD_DIR LIBVIBEOS_BUILD_DIR" >&2
  exit 2
fi
E2E_DISTRIBUTOR=$(readlink -f "$1/unifiedpush/vibeos-unifiedpush")
E2E_PUSH_TEST=$(readlink -f "$2/unifiedpush/vibeos-push-test")
E2E_PUSH_SEND=$(readlink -f "$2/unifiedpush/vibeos-push-send")
for f in "$E2E_DISTRIBUTOR" "$E2E_PUSH_TEST" "$E2E_PUSH_SEND"; do
  [ -x "$f" ] || { echo "$f: not built" >&2; exit 2; }
done
for tool in dbus-run-session pasta ip gdbus python3 glib-compile-schemas; do
  command -v $tool >/dev/null || { echo "$tool is needed" >&2; exit 2; }
done

T=$(mktemp -d "${TMPDIR:-/tmp}/vup-e2e.XXXXXX")
mkdir -p "$T"/{rt,home/.config,home/.local/share,home/.local/state,home/.cache,services,schemas,ctl,aside,bin}
chmod 700 "$T/rt"
cp "$SRC/unifiedpush/data/vibeos.UnifiedPush.gschema.xml" "$T/schemas/"
glib-compile-schemas "$T/schemas"
cat >"$T/session.conf" <<EOF
<!DOCTYPE busconfig PUBLIC "-//freedesktop//DTD D-Bus Bus Configuration 1.0//EN"
 "http://www.freedesktop.org/standards/dbus/1.0/busconfig.dtd">
<busconfig>
  <type>session</type>
  <keep_umask/>
  <listen>unix:dir=$T/rt</listen>
  <auth>EXTERNAL</auth>
  <servicedir>$T/services</servicedir>
  <policy context="default">
    <allow send_destination="*" eavesdrop="true"/>
    <allow eavesdrop="true"/>
    <allow own="*"/>
  </policy>
</busconfig>
EOF
# what D-Bus starts, with their output in the work directory
service () {   # NAME SCRIPT
  printf '#!/bin/sh\n%s\n' "$2" >"$T/bin/$1"
  chmod +x "$T/bin/$1"
  printf '[D-BUS Service]\nName=%s\nExec=%s\n' "$1" "$T/bin/$1" >"$T/services/$1.service"
}
service org.unifiedpush.Distributor.vibeos "exec '$E2E_DISTRIBUTOR' >>'$T/distributor.log' 2>&1"
cp "$T/bin/org.unifiedpush.Distributor.vibeos" "$T/bin/distributor"
for app in One Gone; do
  service test.vibeos.Push$app "exec '$E2E_PUSH_TEST' --app-id test.vibeos.Push$app --log '$T/$(echo $app | tr A-Z a-z).log' --gapplication-service >>'$T/$(echo $app | tr A-Z a-z).out' 2>&1"
done

export E2E_T=$T E2E_DISTRIBUTOR E2E_PUSH_TEST E2E_PUSH_SEND
export XDG_RUNTIME_DIR=$T/rt HOME=$T/home XDG_CONFIG_HOME=$T/home/.config XDG_DATA_HOME=$T/home/.local/share \
  XDG_STATE_HOME=$T/home/.local/state XDG_CACHE_HOME=$T/home/.cache \
  GSETTINGS_BACKEND=keyfile GSETTINGS_SCHEMA_DIR=$T/schemas NO_AT_BRIDGE=1 GTK_A11Y=none GIO_USE_VFS=local \
  G_MESSAGES_DEBUG=vibeos-unifiedpush,VibeosUnifiedPush
# no system bus either: nothing reaches this machine's logind or NetworkManager (whose view of the network is not
# the namespace's), so GIO watches the namespace's own network (netlink)
export DBUS_SYSTEM_BUS_ADDRESS=unix:path=$T/rt/no-system-bus
unset DBUS_SESSION_BUS_ADDRESS DISPLAY WAYLAND_DISPLAY

echo "work directory: $T"
# through bash: pasta's AppArmor profile lets it start a shell, not any program
pasta --quiet --config-net -t none -u none -- /bin/bash "$SELF" --inside &
PASTA=$!
# requests from inside (outside ()): command lines to run out here
while kill -0 "$PASTA" 2>/dev/null; do
  if [ -e "$T/ctl/request" ]; then
    PASTA=$PASTA bash "$T/ctl/request" >"$T/ctl/out" 2>&1
    echo $? >"$T/ctl/status"
    rm -f "$T/ctl/request"
    touch "$T/ctl/done"
  fi
  sleep 0.1
done
wait "$PASTA"
echo
passed=$(grep -c '^PASS' "$T/results" 2>/dev/null)
failed=$(grep -c '^FAIL' "$T/results" 2>/dev/null)
echo "${passed:-0} passed, ${failed:-0} failed; the logs: $T"
! grep -q '^FAIL' "$T/results" && [ -s "$T/results" ]
