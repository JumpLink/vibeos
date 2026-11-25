#!/bin/sh
# sounds-pipewire.sh SOURCE_DIR: the keyboard's click and the volume sound really played, through a PipeWire of the
# test's own (sounds-pipewire.js): a private runtime directory and session bus, pipewire, wireplumber (with no
# hardware: no sound card, Bluetooth or camera is opened) and pipewire-pulse, and two null sinks, each recorded at
# its monitor. The host's own PipeWire and WirePlumber are neither used nor touched: every process here is started by
# this script, its pid kept, and ended by that pid.
#
# Needs libcanberra-pulse (the driver the shell's sounds go through), gir1.2-gsound-1.0 and GNOME Shell's Gvc
# (GVC_DIR names the directory with Gvc-1.0.typelib and libgvc.so; found under /usr/lib by default).
set -eu
# skipped (77, meson's "skip") where what it needs is not installed
for p in pipewire pipewire-pulse wireplumber pw-cli pw-record pw-dump pw-metadata gjs dbus-run-session; do
	command -v "$p" >/dev/null 2>&1 || { echo "skipped: no $p"; exit 77; }
done
ls /usr/lib/*/libcanberra-0.30/libcanberra-pulse.so >/dev/null 2>&1 || { echo "skipped: no libcanberra-pulse"; exit 77; }
if [ -z "${SOUNDS_PW_INNER:-}" ]; then
	SOUNDS_PW_INNER=1 exec dbus-run-session -- "$0" "$@"
fi
src=$1
here=$(cd "$(dirname "$0")" && pwd)
t=$(mktemp -d)
pids=""
cleanup() {
	for p in $pids; do
		kill "$p" 2>/dev/null || true
	done
	for p in $pids; do
		wait "$p" 2>/dev/null || true
	done
	rm -rf "${t:?}"
}
trap cleanup EXIT
mkdir -p "$t/ext/sounds" "$t/config/wireplumber/wireplumber.conf.d" "$t/state" "$t/cache" "$t/home"
mkdir -m 700 "$t/run"
cp "$src/sounds.js" "$src/flagfile.js" "$t/ext/"
cp "$src/sounds/key-tap.oga" "$t/ext/sounds/"
cp "$here/sounds-pipewire.js" "$t/ext/"
cat > "$t/config/wireplumber/wireplumber.conf.d/10-test.conf" <<'CONF'
wireplumber.profiles = {
  main = {
    hardware.audio = disabled
    hardware.bluetooth = disabled
    hardware.video-capture = disabled
  }
}
CONF
export XDG_RUNTIME_DIR="$t/run" XDG_CONFIG_HOME="$t/config" XDG_STATE_HOME="$t/state" XDG_CACHE_HOME="$t/cache"
export HOME="$t/home" PIPEWIRE_RUNTIME_DIR="$t/run" PULSE_RUNTIME_PATH="$t/run/pulse" GIO_USE_VFS=local
unset PULSE_SERVER PIPEWIRE_REMOTE PULSE_SINK PULSE_SOURCE PIPEWIRE_CONFIG_DIR WIREPLUMBER_CONFIG_DIR
gvc=${GVC_DIR:-}
if [ -z "$gvc" ]; then
	for d in /usr/lib/gnome-shell /usr/lib/*/gnome-shell; do
		[ -f "$d/Gvc-1.0.typelib" ] && gvc=$d && break
	done
fi
if [ -n "$gvc" ]; then
	export GI_TYPELIB_PATH="$gvc${GI_TYPELIB_PATH:+:$GI_TYPELIB_PATH}" LD_LIBRARY_PATH="$gvc${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
fi

pipewire >"$t/pipewire.log" 2>&1 &
pids="$pids $!"
i=0
while [ ! -S "$t/run/pipewire-0" ]; do
	i=$((i + 1))
	[ $i -gt 100 ] && { cat "$t/pipewire.log"; echo "no pipewire socket"; exit 1; }
	sleep 0.1
done
wireplumber >"$t/wireplumber.log" 2>&1 &
pids="$pids $!"
pipewire-pulse >"$t/pipewire-pulse.log" 2>&1 &
pids="$pids $!"
i=0
while [ ! -S "$t/run/pulse/native" ]; do
	i=$((i + 1))
	[ $i -gt 100 ] && { cat "$t/pipewire-pulse.log"; echo "no pulse socket"; exit 1; }
	sleep 0.1
done
# two sinks that play to nowhere; sink-a is the default one
for n in a b; do
	pw-cli create-node adapter "{ factory.name=support.null-audio-sink node.name=sink-$n media.class=Audio/Sink \
object.linger=true audio.position=[FL FR] monitor.channel-volumes=true }" >/dev/null
done
sleep 1
pw-metadata -n default 0 default.configured.audio.sink '{ "name": "sink-a" }' >/dev/null
sleep 1
cd "$t"
gjs -m "$t/ext/sounds-pipewire.js"
