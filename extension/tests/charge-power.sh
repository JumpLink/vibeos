#!/bin/sh
# charge-power.sh SOURCE_DIR: the charging label's rule (chargepower.js: what flows into the battery, never what the
# charger may give), driven by charge-power.js on fake /sys/class/power_supply trees built from real readings. Only
# GLib is used; no shell, no device, no bus.
set -eu
src=$1
here=$(cd "$(dirname "$0")" && pwd)
t=$(mktemp -d)
trap 'rm -rf "${t:?}"' EXIT
cp "$here/charge-power.js" "$src/chargepower.js" "$t/"
gjs -m "$t/charge-power.js"
