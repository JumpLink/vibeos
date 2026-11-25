#!/bin/sh
# sounds.sh SOURCE_DIR: sounds.js (the keyboard's click and the volume sound: when there is a motor, when they are on,
# when they are silent, the throttle, the calls) against made-up /sys, /run/udev trees, a memory GSettings backend with
# the real vibeos.Shell schema, and a ModemManager and a Phone service stand-in on a bus of the test's own
# (dbus-run-session). No sound is played and no real device or server is reached.
set -eu
src=$1
here=$(cd "$(dirname "$0")" && pwd)
t=$(mktemp -d)
trap 'rm -rf "${t:?}"' EXIT
cp "$src/sounds.js" "$src/flagfile.js" "$t/"
cp "$here/sounds-check.js" "$t/"
mkdir -p "$t/schemas" "$t/sounds" "$t/run"
cp "$src/../vibeos-launcher/data/vibeos.Shell.gschema.xml" "$t/schemas/"
# feedbackd's profile, as its own schema has it
cat > "$t/schemas/org.sigxcpu.feedbackd.gschema.xml" <<'XML'
<schemalist><schema id="org.sigxcpu.feedbackd" path="/org/sigxcpu/feedbackd/">
  <key name="profile" type="s"><default>'full'</default></key>
</schema></schemalist>
XML
glib-compile-schemas "$t/schemas"
chmod 700 "$t/run"
cd "$t"
GSETTINGS_SCHEMA_DIR="$t/schemas" GSETTINGS_BACKEND=memory GIO_USE_VFS=local XDG_RUNTIME_DIR="$t/run" \
	dbus-run-session -- gjs -m "$t/sounds-check.js"
