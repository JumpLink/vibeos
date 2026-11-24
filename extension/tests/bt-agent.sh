#!/bin/sh
# bt-agent.sh SOURCE_DIR: bluetooth.js (the pairing agent, trust after pairing, the audio devices connected again)
# against a BlueZ stand-in on a bus of the test's own (dbus-run-session), its questions answered by the test in place
# of the dialogs: no real BlueZ, adapter or device is ever reached.
set -eu
src=$1
here=$(cd "$(dirname "$0")" && pwd)
t=$(mktemp -d)
trap 'rm -rf "${t:?}"' EXIT
cp "$src/bluetooth.js" "$here/bt-agent.js" "$t/"
mkdir -m 700 "$t/run"
GIO_USE_VFS=local XDG_RUNTIME_DIR="$t/run" dbus-run-session -- gjs -m "$t/bt-agent.js"
