// sounds-check.js (run by sounds.sh): sounds.js without a sound card.
//
//   motor      hasMotor on made-up /sys/class/input, /sys/class/leds and udev trees: force feedback, none, a game
//              controller's rumble, a vibrator LED, feedbackd's tag, nothing at all.
//   default    enabled(): a key the user never set follows the device (on without a motor), a set one is the user's, in
//              either direction, and follows the real vibeos.Shell schema (memory backend) as it changes.
//   silent     feedbackd's Vibrate and Mute profiles, a call, a muted stream, a stream at zero, the event stream's own.
//   throttle   one volume sound in 100 ms, the first at once.
//   sounds     Sounds with a recording Player: what is played, for which sink, and what is not.
//   calls      CallWatch against a Phone service and a ModemManager on the test's bus: a call ringing, up and over.
import Gio from 'gi://Gio';
import GLib from 'gi://GLib';
import System from 'system';

import {
    CallWatch, callUp, enabled, hasMotor, KEYBOARD_KEY, silence, Sounds, THROTTLE_MS, Throttle, VOLUME_KEY,
} from './sounds.js';

let failures = 0;
function check(name, ok, detail = '') {
    print(ok ? 'PASS' : 'FAIL', name, detail);
    if (!ok)
        failures++;
}

function sleep(ms) {
    return new Promise(resolve => GLib.timeout_add(GLib.PRIORITY_DEFAULT, ms, () => {
        resolve();
        return GLib.SOURCE_REMOVE;
    }));
}

async function waitFor(cond, ms = 3000) {
    const end = GLib.get_monotonic_time() + ms * 1000;
    while (!cond() && GLib.get_monotonic_time() < end)
        await sleep(10); // eslint-disable-line no-await-in-loop
    return cond();
}

// ---- motor -------------------------------------------------------------------------------------------------------------

const root = GLib.dir_make_tmp('sounds-XXXXXX');
function put(path, text) {
    const full = `${root}/${path}`;
    GLib.mkdir_with_parents(GLib.path_get_dirname(full), 0o755);
    GLib.file_set_contents(full, text);
}
function tree(name) {
    return {input: `${root}/${name}/input`, leds: `${root}/${name}/leds`, udev: `${root}/${name}/udev`};
}
function empty(name) {
    const t = tree(name);
    for (const d of Object.values(t))
        GLib.mkdir_with_parents(d, 0o755);
    return t;
}
function event(name, node, ff, abs) {
    put(`${name}/input/${node}/device/capabilities/ff`, ff);
    put(`${name}/input/${node}/device/capabilities/abs`, abs);
}

{
    const none = empty('none');
    check('motor: empty trees have none', !hasMotor(none));
    event('none', 'event0', '0\n', '0\n');
    event('none', 'event1', '0\n', '3\n');
    check('motor: a keyboard and a touchscreen are not one', !hasMotor(none));
    check('motor: trees that are not there are none',
        !hasMotor({input: `${root}/x/a`, leds: `${root}/x/b`, udev: `${root}/x/c`}));

    const ff = empty('ff');
    event('ff', 'event0', '0\n', '0\n');
    event('ff', 'event4', '0 0 0 10000\n', '0\n');
    check('motor: an event node that plays force feedback is one', hasMotor(ff));

    const pad = empty('pad');
    event('pad', 'event7', '10000\n', '3003f\n');
    check('motor: a game controller that rumbles is not', !hasMotor(pad));

    const led = empty('led');
    put('led/leds/input3::capslock/brightness', '0\n');
    check('motor: a caps lock LED is not one', !hasMotor(led));
    put('led/leds/vibrator/brightness', '0\n');
    check('motor: an LED named vibrator is one', hasMotor(led));

    const fbd = empty('fbd');
    put('fbd/udev/c13:64', 'I:1\nE:ID_INPUT=1\nG:uaccess\n');
    check('motor: an input node feedbackd did not tag is not one', !hasMotor(fbd));
    put('fbd/udev/c13:65', 'I:1\nE:ID_INPUT=1\nE:FEEDBACKD_TYPE=vibra\nG:uaccess\n');
    check('motor: an input node feedbackd tagged vibra is one', hasMotor(fbd));
    put('fbd/udev/c13:65', 'E:FEEDBACKD_TYPE=led\n');
    check('motor: one feedbackd tagged as an LED is not', !hasMotor(fbd));
    put('fbd/udev/c13:65', 'E:FEEDBACKD_TYPE=vibra\n');
    check('motor: the tag on the first line counts', hasMotor(fbd));
}

// ---- default -----------------------------------------------------------------------------------------------------------

const shell = new Gio.Settings({schema_id: 'vibeos.Shell'});
const feedbackd = new Gio.Settings({schema_id: 'org.sigxcpu.feedbackd'});

check('default: the keys are in the schema', shell.settings_schema.has_key(KEYBOARD_KEY) &&
    shell.settings_schema.has_key(VOLUME_KEY));
check('default: never set, no motor: on', enabled(shell, KEYBOARD_KEY, false) && enabled(shell, VOLUME_KEY, false));
check('default: never set, a motor: off', !enabled(shell, KEYBOARD_KEY, true) && !enabled(shell, VOLUME_KEY, true));
check('default: the schema default alone says off, which is why the user value is what is read',
    shell.get_boolean(KEYBOARD_KEY) === false);
shell.set_boolean(KEYBOARD_KEY, false);
check('default: set off on a device with no motor: off', !enabled(shell, KEYBOARD_KEY, false));
check('default: set off does not touch the other key', enabled(shell, VOLUME_KEY, false));
shell.set_boolean(KEYBOARD_KEY, true);
check('default: set on with a motor: on', enabled(shell, KEYBOARD_KEY, true));
shell.reset(KEYBOARD_KEY);
check('default: reset follows the device again', enabled(shell, KEYBOARD_KEY, false) &&
    !enabled(shell, KEYBOARD_KEY, true));
check('default: no settings at all follow the device', enabled(null, KEYBOARD_KEY, false) &&
    !enabled(null, KEYBOARD_KEY, true));

// ---- silent ------------------------------------------------------------------------------------------------------------

const live = {is_muted: false, volume: 30000, name: 'sink-a'};
check('silent: nothing in the way plays', silence({profile: 'full', inCall: false, streams: [live]}) === null);
check('silent: Vibrate mode (quiet)', silence({profile: 'quiet', inCall: false, streams: [live]}) !== null);
check('silent: Mute mode (silent)', silence({profile: 'silent', inCall: false, streams: [live]}) !== null);
check('silent: a call', silence({profile: 'full', inCall: true, streams: [live]}) !== null);
check('silent: a muted sink', silence({profile: 'full', inCall: false, streams: [{...live, is_muted: true}]}) !== null);
check('silent: a sink at zero', silence({profile: 'full', inCall: false, streams: [{...live, volume: 0}]}) !== null);
check('silent: an event stream at zero', silence({profile: 'full', inCall: false, streams: [live, {...live, volume: 0}]})
    !== null);
check('silent: an event stream muted', silence({profile: 'full', inCall: false,
    streams: [live, {...live, is_muted: true}]}) !== null);
check('silent: no sink at all', silence({profile: 'full', inCall: false, streams: [null]}) !== null);
check('calls: ModemManager states', !callUp.modemManager([0, 7]) &&
    [1, 2, 3, 4, 5, 6].every(s => callUp.modemManager([7, s])) && !callUp.modemManager([]));
check('calls: the Phone service states', callUp.dialer([0]) && callUp.dialer([2]) && callUp.dialer([5]) &&
    !callUp.dialer([6, 7]) && !callUp.dialer([]));

// ---- throttle ----------------------------------------------------------------------------------------------------------

{
    let now = 1000;
    const throttle = new Throttle(THROTTLE_MS, () => now);
    const seen = [];
    for (const at of [0, 20, 40, 60, 99, 100, 130, 199, 200, 201, 450, 460])
        seen.push([at, (now = 1000 + at, throttle.allow())]);
    check('throttle: the first is at once, then one per 100 ms',
        seen.filter(([, ok]) => ok).map(([at]) => at).join() === '0,100,200,450', seen.filter(([, ok]) => ok)
            .map(([at]) => at).join());
}

// ---- sounds ------------------------------------------------------------------------------------------------------------

class FakePlayer {
    constructor() {
        this.played = [];
        this.cached = 0;
    }

    async load() {
        return true;
    }

    preload() {
        this.cached++;
        return true;
    }

    play(attrs, done) {
        this.played.push({attrs});
        done?.(null);
        return true;
    }

    destroy() {}
}

function rig({motor, profile = 'full', inCall = false, sinkVolume = 30000, muted = false}) {
    shell.reset(KEYBOARD_KEY);
    shell.reset(VOLUME_KEY);
    feedbackd.set_string('profile', profile);
    const sink = {name: 'sink-a', volume: sinkVolume, is_muted: muted};
    const other = {name: 'sink-b', volume: 20000, is_muted: false};
    const mixer = {event: null, get_default_sink: () => sink, get_event_sink_input() {
        return this.event;
    }};
    const player = new FakePlayer();
    let now = 5000;
    const logs = [];
    const calls = {busy: inCall};
    const sounds = new Sounds({settings: shell, feedbackd, motor, calls, player, mixer, clock: () => now,
        log: line => logs.push(line)});
    return {sounds, player, sink, other, mixer, calls, logs, advance: ms => (now += ms)};
}

{
    const r = rig({motor: false});
    r.sounds.keyTap();
    check('sounds: a tablet taps its keys by default', r.player.played.length === 1 &&
        r.player.played[0].attrs['canberra.cache-control'] === 'permanent' &&
        r.player.played[0].attrs['media.filename'].endsWith('/sounds/key-tap.oga'));
    r.sounds.volumeChanged(r.sink);
    check('sounds: and plays the volume sound', r.player.played.length === 2 &&
        r.player.played[1].attrs['event.id'] === 'audio-volume-change');
    check('sounds: the click is not throttled (two thumbs)', (r.sounds.keyTap(), r.sounds.keyTap(),
        r.player.played.length === 4));
    r.advance(1000);
    shell.set_boolean(KEYBOARD_KEY, false);
    r.sounds.keyTap();
    check('sounds: Keyboard sounds switched off stops the click at once', r.player.played.length === 4);
    shell.set_boolean(VOLUME_KEY, false);
    r.sounds.volumeChanged(r.sink);
    check('sounds: Volume change sound switched off stops it', r.player.played.length === 4);
    r.sounds.destroy();
}
{
    const r = rig({motor: true});
    r.sounds.keyTap();
    r.sounds.volumeChanged(r.sink);
    check('sounds: a phone plays none by default', r.player.played.length === 0);
    shell.set_boolean(KEYBOARD_KEY, true);
    shell.set_boolean(VOLUME_KEY, true);
    r.sounds.keyTap();
    r.sounds.volumeChanged(r.sink);
    check('sounds: and both when the user switches them on (the haptics stay: not touched here)',
        r.player.played.length === 2);
    r.sounds.destroy();
}
for (const [what, setup] of [
    ['Vibrate mode', {profile: 'quiet'}], ['Mute mode', {profile: 'silent'}], ['a call', {inCall: true}],
    ['a muted sink', {muted: true}], ['a sink at zero', {sinkVolume: 0}],
]) {
    const r = rig({motor: false, ...setup});
    r.sounds.keyTap();
    r.sounds.volumeChanged(r.sink);
    check(`sounds: silent in ${what}`, r.player.played.length === 0, r.logs.join(' | '));
    r.sounds.destroy();
}
{
    const r = rig({motor: false});
    r.mixer.event = {name: 'event', volume: 0, is_muted: false};
    r.sounds.keyTap();
    check('sounds: silent while the event stream is at zero', r.player.played.length === 0);
    r.mixer.event = {name: 'event', volume: 65536, is_muted: false};
    r.sounds.keyTap();
    check('sounds: and heard again when it is not', r.player.played.length === 1);
    r.calls.busy = true;
    r.sounds.keyTap();
    check('sounds: a call that starts silences the next tap', r.player.played.length === 1);
    r.calls.busy = false;
    feedbackd.set_string('profile', 'quiet');
    r.sounds.keyTap();
    check('sounds: and a profile changed to Vibrate does', r.player.played.length === 1);
    feedbackd.set_string('profile', 'full');
    r.sounds.keyTap();
    check('sounds: all back in Sound mode', r.player.played.length === 2);
    r.sounds.destroy();
}
{
    // a drag over twenty steps in 400 ms (one per 20 ms) and a key's single step
    const r = rig({motor: false});
    for (let i = 0; i < 20; i++) {
        r.sounds.volumeChanged(r.sink);
        r.advance(20);
    }
    check('sounds: a drag of 20 steps in 400 ms plays 4', r.player.played.length === 4, `${r.player.played.length}`);
    r.advance(500);
    r.sounds.volumeChanged(r.sink);
    check('sounds: a step after a pause is heard at once', r.player.played.length === 5);
    r.advance(500);
    const count = r.player.played.length;
    r.sounds.volumeChanged(r.sink);
    check('sounds: the volume sound is the sound theme\'s, for the sink whose volume changed',
        r.player.played.length === count + 1 && r.player.played.at(-1).attrs['event.id'] === 'audio-volume-change' &&
        r.player.played.at(-1).attrs['target.object'] === 'sink-a');
    r.advance(500);
    r.sounds.volumeChanged(r.other);
    check('sounds: a sink that is not the default one gets its own, named as the target',
        r.player.played.length === count + 2 && r.player.played.at(-1).attrs['target.object'] === 'sink-b');
    check('sounds: a throttled step says so in the log', r.logs.some(l => l.includes('throttled')));
    r.sounds.destroy();
}
{
    // the sample is cached before the first tap
    const r = rig({motor: false});
    await waitFor(() => r.player.cached > 0, 2000);
    check('sounds: the click is put in the server\'s cache at start', r.player.cached === 1);
    r.sounds.destroy();
}

// ---- calls -------------------------------------------------------------------------------------------------------------

const bus = Gio.DBusConnection.new_for_address_sync(GLib.getenv('DBUS_SESSION_BUS_ADDRESS'),
    Gio.DBusConnectionFlags.AUTHENTICATION_CLIENT | Gio.DBusConnectionFlags.MESSAGE_BUS_CONNECTION, null, null);

const DIALER_XML = `<node><interface name="vibeos.Dialer.Calls">
  <property name="Calls" type="aa{sv}" access="read"/>
</interface></node>`;
const dialer = {
    calls: [],
    get Calls() {
        return new GLib.Variant('aa{sv}', this.calls.map(state => ({state: GLib.Variant.new_uint32(state)})));
    },
    set(states) {
        this.calls = states;
        this.exported.emit_property_changed('Calls', this.Calls);
    },
};
dialer.exported = Gio.DBusExportedObject.wrapJSObject(DIALER_XML, dialer);
dialer.exported.export(bus, '/vibeos/Dialer/Calls');

const MM = 'org.freedesktop.ModemManager1';
const MANAGER_XML = `<node><interface name="org.freedesktop.DBus.ObjectManager">
  <method name="GetManagedObjects"><arg type="a{oa{sa{sv}}}" direction="out"/></method>
  <signal name="InterfacesAdded"><arg type="o"/><arg type="a{sa{sv}}"/></signal>
  <signal name="InterfacesRemoved"><arg type="o"/><arg type="as"/></signal>
</interface></node>`;
const CALL_XML = `<node><interface name="${MM}.Call">
  <property name="State" type="i" access="read"/>
</interface></node>`;
const modem = {
    calls: new Map(),
    GetManagedObjects() {
        const objects = {};
        for (const [path, call] of this.calls)
            objects[path] = {[`${MM}.Call`]: {State: GLib.Variant.new_int32(call.State)}};
        return new GLib.Variant('(a{oa{sa{sv}}})', [objects]);
    },
    add(n, state) {
        const call = {State: state};
        call.exported = Gio.DBusExportedObject.wrapJSObject(CALL_XML, call);
        const path = `/org/freedesktop/ModemManager1/Call/${n}`;
        call.exported.export(bus, path);
        this.calls.set(path, call);
        this.exported.emit_signal('InterfacesAdded', new GLib.Variant('(oa{sa{sv}})',
            [path, {[`${MM}.Call`]: {State: GLib.Variant.new_int32(state)}}]));
    },
    set(n, state) {
        const call = this.calls.get(`/org/freedesktop/ModemManager1/Call/${n}`);
        call.State = state;
        call.exported.emit_property_changed('State', GLib.Variant.new_int32(state));
    },
    remove(n) {
        const path = `/org/freedesktop/ModemManager1/Call/${n}`;
        this.calls.get(path).exported.unexport();
        this.calls.delete(path);
        this.exported.emit_signal('InterfacesRemoved', new GLib.Variant('(oas)', [path, [`${MM}.Call`]]));
    },
};
modem.exported = Gio.DBusExportedObject.wrapJSObject(MANAGER_XML, modem);
modem.exported.export(bus, '/org/freedesktop/ModemManager1');

// the names, as the services own them: the watchers do not start anything, so nothing answers until they are there
const owned = [];
for (const name of ['vibeos.Dialer.Calls', MM])
    owned.push(Gio.bus_own_name_on_connection(bus, name, Gio.BusNameOwnerFlags.NONE, null, null));

{
    const watch = new CallWatch({session: Gio.DBus.session, system: Gio.DBus.session});
    await sleep(300);
    check('calls: none to begin with', !watch.busy);
    dialer.set([4]);
    check('calls: the Phone\'s service has one up', await waitFor(() => watch.busy));
    dialer.set([7]);
    check('calls: and it is over', await waitFor(() => !watch.busy));
    dialer.set([2, 7]);
    check('calls: one incoming among ended ones', await waitFor(() => watch.busy));
    dialer.set([]);
    check('calls: none again', await waitFor(() => !watch.busy));

    modem.add(1, 3);
    check('calls: ModemManager has one ringing', await waitFor(() => watch.busy));
    modem.set(1, 4);
    await sleep(100);
    check('calls: it is answered', watch.busy);
    modem.set(1, 7);
    check('calls: it is terminated', await waitFor(() => !watch.busy));
    modem.add(2, 1);
    check('calls: another is dialling', await waitFor(() => watch.busy));
    modem.remove(2);
    check('calls: it is gone', await waitFor(() => !watch.busy));
    watch.destroy();

    // services that were never there: nothing is started, nothing is busy
    const absent = new CallWatch({session: Gio.DBus.session, system: Gio.DBus.session});
    await sleep(100);
    check('calls: a watcher that starts with a call already up knows it', (dialer.set([4]), await waitFor(() => {
        return absent.busy;
    })));
    absent.destroy();
}

print(failures ? `${failures} FAILED` : 'all passed');
System.exit(failures ? 1 : 0);
