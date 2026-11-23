#!/bin/sh
# What a sandboxed app's PipeWire client sees and can link under the real WirePlumber with sandbox-access.lua: a
# private PipeWire and WirePlumber (no devices) with a fake microphone, speaker and camera, and clients labelled as
# sandboxed apps' (pipewire.client.access = flatpak and an app id, as PipeWire labels a client from a sandbox).
# Nothing in it touches the session's own PipeWire, WirePlumber or devices.
#
#   dbus-run-session -- sh wireplumber-rig.sh DATA-DIR [WORK-DIR]
#
# DATA-DIR holds sandbox-access.lua, 60-vibeos-sandbox.conf and 60-vibeos-sandbox-pulse.conf (sandbox/data);
# WORK-DIR (default: a new one under /tmp) gets the rig's runtime directory and logs. Needs pipewire, pipewire-pulse,
# wireplumber, pw-cli, pw-dump, pw-link, pw-record, pw-cat, wpctl, python3 with libpulse and xdg-permission-store;
# the private bus's permission store keeps its tables in the rig's own home.
DATA=${1:?usage: dbus-run-session -- sh wireplumber-rig.sh DATA-DIR [WORK-DIR]}
R=${2:-$(mktemp -d /tmp/wireplumber-rig.XXXXXX)}
rm -rf $R/run $R/config $R/data $R/home $R/as-*; mkdir -p $R; mkdir -p $R/run $R/config/wireplumber/wireplumber.conf.d $R/data/wireplumber/scripts/vibeos $R/home
chmod 700 $R/run
cp "$DATA/60-vibeos-sandbox.conf" $R/config/wireplumber/wireplumber.conf.d/
cp "$DATA/sandbox-access.lua" $R/data/wireplumber/scripts/vibeos/
mkdir -p $R/config/pipewire/pipewire-pulse.conf.d
cp "$DATA/60-vibeos-sandbox-pulse.conf" $R/config/pipewire/pipewire-pulse.conf.d/
cat > $R/config/wireplumber/wireplumber.conf.d/10-no-devices.conf <<'EOC'
wireplumber.profiles = {
  main = {
    hardware.audio = disabled
    hardware.bluetooth = disabled
    hardware.video-capture = disabled
    support.reserve-device = disabled
    support.logind = disabled
  }
}
EOC
export XDG_RUNTIME_DIR=$R/run XDG_CONFIG_HOME=$R/config XDG_DATA_HOME=$R/data PIPEWIRE_RUNTIME_DIR=$R/run HOME=$R/home
ps_set () {
  gdbus call --session --dest org.freedesktop.impl.portal.PermissionStore --object-path /org/freedesktop/impl/portal/PermissionStore \
    --method org.freedesktop.impl.portal.PermissionStore.SetPermission "$1" true "$2" "$3" "['$4']" >/dev/null
}
ps_set vibeos-sandbox microphone org.example.Recorder yes
ps_set vibeos-sandbox microphone org.example.Game no
ps_set devices camera org.example.Scanner yes
pipewire > $R/pw.log 2>&1 &
PW=$!
sleep 8
WIREPLUMBER_DEBUG="2,s-client:4" wireplumber > $R/wp.log 2>&1 &
WP=$!
sleep 30
pw-cli create-node adapter '{ factory.name=support.null-audio-sink node.name=fakemic media.class=Audio/Source/Virtual object.linger=true audio.position=[MONO] }' >/dev/null
pw-cli create-node adapter '{ factory.name=support.null-audio-sink node.name=fakespeaker media.class=Audio/Sink object.linger=true audio.position=[FL,FR] monitor.passthrough=true }' >/dev/null
pw-cli create-node spa-node-factory '{ factory.name=videotestsrc library.name=videotestsrc/libspa-videotestsrc node.name=fakecam media.class=Video/Source media.role=Camera object.linger=true }' >/dev/null
sleep 5
as_app () {  # app id, then the command: a client that says it is that app's, as a sandboxed one is labelled
  app=$1; shift
  mkdir -p $R/as-$app/pipewire/client.conf.d
  printf 'context.properties = {\n  pipewire.client.access = flatpak\n  pipewire.access.portal.app_id = %s\n}\n' "$app" \
    > $R/as-$app/pipewire/client.conf.d/99-app.conf
  ln -sfn client.conf.d $R/as-$app/pipewire/client-rt.conf.d
  XDG_CONFIG_HOME=$R/as-$app "$@"
}
sees () { as_app $1 pw-dump 2>/dev/null | grep -oE '"(node|factory).name": "(fake[a-z]+|link-factory|spa-node-factory|client-node)"' | sed 's/.*: //' | sort | tr '\n' ' '; }
echo "host sees: $(pw-dump 2>/dev/null | grep -oE '"node.name": "fake[a-z]+"' | sed 's/.*: //' | sort | tr '\n' ' ')"
for app in org.example.Game org.example.Recorder org.example.Scanner; do echo "$app sees: $(sees $app)"; done
links_from () { pw-link -l 2>/dev/null | grep -A4 "^$1:" | grep -c "|->"; }
echo "--- what WirePlumber links (recording 4 s)"
for who in org.example.Game org.example.Recorder host; do
  if [ $who = host ]; then timeout 6 pw-record --target fakemic $R/h.wav >/dev/null 2>&1 & P=$!
  else as_app $who timeout 6 pw-record --target fakemic $R/$who.wav >/dev/null 2>&1 & P=$!; fi
  sleep 4; echo "$who recording the microphone: $(links_from fakemic) links"; wait $P
done
as_app org.example.Game timeout 6 pw-record -P '{ stream.capture.sink=true }' $R/m.wav >/dev/null 2>&1 & P=$!
sleep 4; echo "game recording what plays (sink monitor): $(links_from fakespeaker) links"; wait $P
into_speaker () { pw-link -l 2>/dev/null | grep -A2 "^fakespeaker:playback" | grep -c "|<-"; }
for who in org.example.Game host; do
  if [ $who = host ]; then timeout 6 pw-cat -p --raw --format=s16 --rate=48000 --channels=2 /dev/zero >/dev/null 2>&1 & P=$!
  else as_app $who timeout 6 pw-cat -p --raw --format=s16 --rate=48000 --channels=2 /dev/zero >/dev/null 2>&1 & P=$!; fi
  sleep 4; echo "$who playing: $(into_speaker) links into the speaker"; wait $P
done
echo "--- what it can do itself"
as_app org.example.Game timeout 8 pw-record --target 0 $R/own.wav >/dev/null 2>&1 & P=$!
sleep 3
PORT=$(pw-link -i 2>/dev/null | grep -m1 "pw-record:input")
echo "its own stream port: ${PORT:-none}"
as_app org.example.Game pw-link fakespeaker:monitor_FL "$PORT" 2>&1 | head -2 | sed 's/^/game links the speaker monitor: /'
echo "speaker monitor links now: $(links_from fakespeaker)"
as_app org.example.Game pw-link fakemic:capture_MONO "$PORT" 2>&1 | head -1 | sed 's/^/game links the microphone: /'
pw-link fakespeaker:monitor_FL "$PORT" >/dev/null 2>&1 && echo "a host app links the speaker monitor: ok"; pw-link -d fakespeaker:monitor_FL "$PORT" >/dev/null 2>&1
as_app org.example.Game pw-cli create-node spa-node-factory '{ factory.name=audiotestsrc library.name=audiotestsrc/libspa-audiotestsrc node.name=gamenode }' 2>&1 | head -1 | sed 's/^/game makes a node in the daemon: /'
echo "gamenode exists: $(pw-dump 2>/dev/null | grep -c '"node.name": "gamenode"')"
wait $P
as_app org.example.Game timeout 8 pw-record -P '{ media.class=Audio/Sink node.name=gamesink node.description=gamesink priority.session=5000 priority.driver=5000 }' $R/gs.wav >/dev/null 2>&1 & P=$!
sleep 3
echo "game's own sink (a stream that says it is one): $(pw-dump 2>/dev/null | grep -c '"node.name": "gamesink"') nodes"
echo "the default sink: $(wpctl inspect @DEFAULT_AUDIO_SINK@ 2>/dev/null | grep -o 'node.name = "[a-z]*"' | head -1)"
wait $P
echo "--- PulseAudio clients load no modules"
XDG_CONFIG_HOME=$R/config pipewire-pulse > $R/pulse.log 2>&1 &
PP=$!
sleep 4
python3 - <<'EOP'
import ctypes, sys
pa = ctypes.CDLL("libpulse.so.0")
for f in ("pa_mainloop_new", "pa_mainloop_get_api", "pa_context_new", "pa_context_load_module"):
    getattr(pa, f).restype = ctypes.c_void_p
pa.pa_mainloop_get_api.argtypes = [ctypes.c_void_p]
pa.pa_context_new.argtypes = [ctypes.c_void_p, ctypes.c_char_p]
pa.pa_context_connect.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_int, ctypes.c_void_p]
pa.pa_context_get_state.argtypes = [ctypes.c_void_p]
pa.pa_mainloop_iterate.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_void_p]
CB = ctypes.CFUNCTYPE(None, ctypes.c_void_p, ctypes.c_uint32, ctypes.c_void_p)
pa.pa_context_load_module.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_char_p, CB, ctypes.c_void_p]
ml = pa.pa_mainloop_new()
ctx = pa.pa_context_new(pa.pa_mainloop_get_api(ml), b"rig")
pa.pa_context_connect(ctx, None, 0, None)
for _ in range(200):
    pa.pa_mainloop_iterate(ml, 1, None)
    st = pa.pa_context_get_state(ctx)
    if st == 4 or st >= 5:
        break
if st != 4:
    print("a PulseAudio client loading a module: no connection"); sys.exit(0)
res = []
cb = CB(lambda c, idx, ud: res.append(idx))
pa.pa_context_load_module(ctx, b"module-null-sink", b"sink_name=pulsenull", cb, None)
while not res:
    pa.pa_mainloop_iterate(ml, 1, None)
print("a PulseAudio client loading a module:", "loaded" if res[0] != 0xffffffff else "refused")
EOP
echo "pulsenull exists: $(pw-dump 2>/dev/null | grep -c '"node.name": "pulsenull"')"
kill $PP 2>/dev/null; wait $PP 2>/dev/null
echo "--- the game is granted the microphone"
ps_set vibeos-sandbox microphone org.example.Game yes
sleep 3
echo "org.example.Game sees: $(sees org.example.Game)"
as_app org.example.Game timeout 6 pw-record --target fakemic $R/g2.wav >/dev/null 2>&1 & P=$!
sleep 4; echo "game recording the microphone: $(links_from fakemic) links"; wait $P
echo "--- Settings' Microphone and Camera switches: every client"
# an unrestricted client sees every node from the moment it connects until WirePlumber has set its permissions (a
# few ms; a sandboxed one sees none until then): this one watches for two seconds and says what it sees after them
host_sees () {
  timeout 2 pw-dump -m 2>/dev/null | python3 -c '
import json, sys
text, decoder, at, seen = sys.stdin.read(), json.JSONDecoder(), 0, {}
while True:
    while at < len(text) and text[at].isspace():
        at += 1
    if at >= len(text):
        break
    update, at = decoder.raw_decode(text, at)
    for o in update:
        if o.get("info") is None:
            seen.pop(o["id"], None)
        elif o.get("type", "").endswith("Node"):
            seen[o["id"]] = o["info"].get("props", {}).get("node.name", "")
print(" ".join(chr(34) + n + chr(34) for n in sorted(seen.values()) if n.startswith("fake")), end=" ")
'
}
timeout 14 pw-record --target fakemic $R/hb.wav >/dev/null 2>&1 & P=$!
sleep 3; echo "host recording the microphone before: $(links_from fakemic) links"
ps_set vibeos-privacy microphone all blocked
sleep 3
echo "microphone blocked: that recording now has $(links_from fakemic) links"
wait $P
echo "host sees: $(host_sees)"
timeout 8 pw-record -P '{ node.autoconnect=false }' $R/hl.wav >/dev/null 2>&1 & P=$!
sleep 2; PORT=$(pw-link -i 2>/dev/null | grep -m1 "pw-record:input")
pw-link fakemic:capture_MONO "$PORT" >/dev/null 2>&1; sleep 1
echo "a host app linking the microphone itself: $(links_from fakemic) links"; kill $P 2>/dev/null; wait $P
timeout 6 pw-record --target fakemic $R/hb2.wav >/dev/null 2>&1 & P=$!
sleep 4; echo "host recording the microphone: $(links_from fakemic) links"; wait $P
as_app org.example.Game timeout 6 pw-record --target fakemic $R/gb.wav >/dev/null 2>&1 & P=$!
sleep 4; echo "game (granted) recording the microphone: $(links_from fakemic) links"; wait $P
timeout 6 pw-record -P '{ stream.capture.sink=true }' $R/hm.wav >/dev/null 2>&1 & P=$!
sleep 4; echo "host recording what plays (sink monitor): $(links_from fakespeaker) links"; wait $P
ps_set vibeos-privacy camera all blocked
sleep 3
echo "camera blocked too, host sees: $(host_sees)"
echo "org.example.Scanner (granted the camera) sees: $(sees org.example.Scanner)"
ps_set vibeos-privacy microphone all allowed
ps_set vibeos-privacy camera all allowed
sleep 3
echo "both allowed again, host sees: $(host_sees)"
timeout 6 pw-record --target fakemic $R/ha.wav >/dev/null 2>&1 & P=$!
sleep 4; echo "host recording the microphone: $(links_from fakemic) links"; wait $P
kill $WP $PW 2>/dev/null
wait $WP $PW 2>/dev/null
grep -iE "vibeos|sandbox|error" $R/wp.log | grep -v "permissions for" | head -24
