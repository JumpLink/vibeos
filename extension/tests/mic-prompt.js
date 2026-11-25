// The microphone's question (micprompt.js) in a headless GNOME Shell of the test's own (mic-prompt.sh runs it as the
// shell's automation script), with stand-ins on the test's session bus for the two things it talks to: the permission
// store (a service of this script that records what is written) and WirePlumber's script (a name it owns, as
// sandbox-access.lua does while it holds a stream). Touches come from a virtual touchscreen of this shell's.
//
//   A  a name for an app: "Allow <the desktop file's Name> to use the microphone?" over a dim; Allow writes "yes" to
//      "vibeos-sandbox"/"microphone" for that app, and the name going takes the reply entry away
//   B  Don't allow writes "no"
//   C  dismissed (Escape): "dismissed" in the reply table, the microphone's entry untouched
//   D  asked while locked: no dialog, "dismissed"; locking with the dialog up takes it away, "dismissed"
//   E  the name going while the dialog is up (the script's own time ran out): the dialog goes, nothing is answered
//   F  no answer in the time given: "dismissed"
//   G  one dialog per app however often the name is announced; names that are not the script's, or whose app id does
//      not decode, are left alone; a question already open when the shell starts is asked
import Clutter from 'gi://Clutter';
import Gio from 'gi://Gio';
import GLib from 'gi://GLib';
import St from 'gi://St';

import * as Main from 'resource:///org/gnome/shell/ui/main.js';
import * as Signals from 'resource:///org/gnome/shell/misc/signals.js';

export const METRICS = {};
const DIR = GLib.path_get_dirname(GLib.filename_from_uri(import.meta.url)[0]);
let failures = 0;
const say = (...a) => print('TEST', ...a);
function check(name, ok, detail = '') {
    say(ok ? 'PASS' : 'FAIL', name, detail);
    if (!ok)
        failures++;
}
const sleep = ms => new Promise(resolve => GLib.timeout_add(GLib.PRIORITY_DEFAULT, ms, () => {
    resolve();
    return GLib.SOURCE_REMOVE;
}));
async function until(cond, ms) {
    const end = Date.now() + ms;
    while (Date.now() < end) {
        if (cond())
            return true;
        await sleep(50);
    }
    return cond();
}

let device = null;
const now = () => GLib.get_monotonic_time();
function touchscreen() {
    device ??= global.stage.context.get_backend().get_default_seat()
        .create_virtual_device(Clutter.InputDeviceType.TOUCHSCREEN_DEVICE);
    return device;
}
async function tap(actor) {
    const [x, y] = actor.get_transformed_position();
    const [w, h] = actor.get_transformed_size();
    touchscreen().notify_touch_down(now(), 0, x + w / 2, y + h / 2);
    await sleep(60);
    touchscreen().notify_touch_up(now(), 0);
    await sleep(100);
}
async function escape() {
    const keyboard = global.stage.context.get_backend().get_default_seat()
        .create_virtual_device(Clutter.InputDeviceType.KEYBOARD_DEVICE);
    keyboard.notify_keyval(now(), Clutter.KEY_Escape, Clutter.KeyState.PRESSED);
    keyboard.notify_keyval(now(), Clutter.KEY_Escape, Clutter.KeyState.RELEASED);
    await sleep(200);
}

class Keyguard extends Signals.EventEmitter {
    constructor() {
        super();
        this.showing = false;
    }

    set(showing) {
        this.showing = showing;
        this.emit('changed');
    }
}

function* actors(root) {
    for (const child of root.get_children()) {
        yield child;
        yield* actors(child);
    }
}
const button = (root, label) => [...actors(root)].find(a => a instanceof St.Button && a.accessible_name === label);
const texts = root => [...actors(root)].filter(a => a instanceof Clutter.Text).map(a => a.text);

function dialogLayer() {
    return Main.layoutManager.modalDialogGroup.get_children().find(a => a.visible && a.get_n_children() === 1 &&
        a.get_first_child().constructor.name.includes('BoxLayout')) ?? null;
}

// the permission store: what is written and taken away, in order
const STORE = 'org.freedesktop.impl.portal.PermissionStore';
const STORE_XML = `<node><interface name="${STORE}">
  <method name="SetPermission">
    <arg type="s" direction="in"/><arg type="b" direction="in"/><arg type="s" direction="in"/>
    <arg type="s" direction="in"/><arg type="as" direction="in"/>
  </method>
  <method name="DeletePermission">
    <arg type="s" direction="in"/><arg type="s" direction="in"/><arg type="s" direction="in"/>
  </method>
</interface></node>`;
const writes = [];
const store = {
    SetPermission(table, create, id, app, value) {
        writes.push(['set', table, id, app, value.join(',')]);
    },
    DeletePermission(table, id, app) {
        writes.push(['delete', table, id, app]);
    },
};
const written = (kind, table, app) => writes.filter(w => w[0] === kind && w[1] === table && w[3] === app);

// the script's side: a name while a question is open
const hex = app => [...new TextEncoder().encode(app)].map(b => b.toString(16).padStart(2, '0')).join('');
const NAME = app => `org.freedesktop.ReserveDevice1.VibeOSMicAsk${hex(app)}`;
const owned = new Map();
function ask(app, name = NAME(app)) {
    owned.set(app, Gio.bus_own_name(Gio.BusType.SESSION, name, Gio.BusNameOwnerFlags.NONE, null, null, null));
}
function withdraw(app) {
    Gio.bus_unown_name(owned.get(app));
    owned.delete(app);
}

export async function run() {
    await sleep(1000);
    const units = await import(`file://${DIR}/ext/units.js`);
    units.geometry.start();
    const {MicrophonePrompts, appOfName} = await import(`file://${DIR}/ext/micprompt.js`);
    Main.overview.hide();
    await until(() => !Main.overview.visible, 10000);

    const exported = Gio.DBusExportedObject.wrapJSObject(STORE_XML, store);
    exported.export(Gio.DBus.session, '/org/freedesktop/impl/portal/PermissionStore');
    Gio.bus_own_name(Gio.BusType.SESSION, STORE, Gio.BusNameOwnerFlags.NONE, null, null, null);
    await sleep(300);

    // the name's decoding
    check('names: an app id', appOfName(NAME('org.example.Voice')) === 'org.example.Voice');
    check('names: another name', appOfName('org.freedesktop.ReserveDevice1.Audio0') === null);
    check('names: odd hex', appOfName('org.freedesktop.ReserveDevice1.VibeOSMicAskabc') === null);
    check('names: not hex', appOfName('org.freedesktop.ReserveDevice1.VibeOSMicAskzz') === null);
    check('names: no app id', appOfName('org.freedesktop.ReserveDevice1.VibeOSMicAsk') === null);

    const keyguard = new Keyguard();
    const prompts = new MicrophonePrompts(keyguard, Gio.DBus.session, 2);
    await sleep(300);

    // A: Allow
    ask('org.example.Voice');
    await until(() => prompts.showing, 3000);
    let layer = dialogLayer();
    check('A: the dialog is up', prompts.showing && !!layer);
    const said = texts(layer ?? new St.Widget());
    check('A: it names the app by its desktop file', said.includes('Allow Voice Recorder to use the microphone?'),
        said.join(' | '));
    check('A: the old answer is taken away first',
        written('delete', 'vibeos-sandbox-ask', 'org.example.Voice').length === 1);
    check('A: nothing answered yet', written('set', 'vibeos-sandbox', 'org.example.Voice').length === 0);
    await tap(button(layer, 'Allow'));
    await until(() => written('set', 'vibeos-sandbox', 'org.example.Voice').length > 0, 2000);
    let sets = written('set', 'vibeos-sandbox', 'org.example.Voice');
    check('A: Allow writes yes', sets.length === 1 && sets[0][2] === 'microphone' && sets[0][4] === 'yes',
        JSON.stringify(sets));
    check('A: nothing in the reply table', written('set', 'vibeos-sandbox-ask', 'org.example.Voice').length === 0);
    await until(() => !dialogLayer(), 2000);
    check('A: the dialog is gone', !prompts.showing && !dialogLayer());
    withdraw('org.example.Voice');
    await until(() => written('delete', 'vibeos-sandbox-ask', 'org.example.Voice').length === 2, 2000);
    check('A: the name going takes the reply entry away',
        written('delete', 'vibeos-sandbox-ask', 'org.example.Voice').length === 2);

    // B: Don't allow
    ask('org.example.Voice');
    await until(() => prompts.showing, 3000);
    layer = dialogLayer();
    await tap(button(layer, "Don't allow"));
    await until(() => written('set', 'vibeos-sandbox', 'org.example.Voice').length > 1, 2000);
    sets = written('set', 'vibeos-sandbox', 'org.example.Voice');
    check('B: Don\'t allow writes no', sets.length === 2 && sets[1][4] === 'no', JSON.stringify(sets));
    withdraw('org.example.Voice');
    await sleep(300);

    // C: dismissed
    ask('org.example.Voice');
    await until(() => prompts.showing, 3000);
    await escape();
    await until(() => written('set', 'vibeos-sandbox-ask', 'org.example.Voice').length > 0, 2000);
    let replies = written('set', 'vibeos-sandbox-ask', 'org.example.Voice');
    check('C: dismissed is written to the reply table', replies.length === 1 && replies[0][2] === 'microphone' &&
        replies[0][4] === 'dismissed', JSON.stringify(replies));
    check('C: the microphone entry is untouched', written('set', 'vibeos-sandbox', 'org.example.Voice').length === 2);
    await until(() => !dialogLayer(), 2000);
    check('C: the dialog is gone', !prompts.showing && !dialogLayer());
    withdraw('org.example.Voice');
    await sleep(300);

    // D: locked
    keyguard.set(true);
    ask('org.example.Voice');
    await sleep(800);
    check('D: no dialog while locked', !prompts.showing && !dialogLayer());
    replies = written('set', 'vibeos-sandbox-ask', 'org.example.Voice');
    check('D: dismissed', replies.length === 2, JSON.stringify(replies));
    withdraw('org.example.Voice');
    await sleep(300);
    keyguard.set(false);
    ask('org.example.Voice');
    await until(() => prompts.showing, 3000);
    keyguard.set(true);
    await until(() => !dialogLayer(), 2000);
    check('D: locking takes the dialog away', !prompts.showing && !dialogLayer());
    await until(() => written('set', 'vibeos-sandbox-ask', 'org.example.Voice').length === 3, 2000);
    check('D: ... dismissed', written('set', 'vibeos-sandbox-ask', 'org.example.Voice').length === 3);
    withdraw('org.example.Voice');
    await sleep(300);
    keyguard.set(false);
    check('D: no microphone entry in all that', written('set', 'vibeos-sandbox', 'org.example.Voice').length === 2);

    // E: the script takes the question away
    const before = writes.length;
    ask('org.example.Voice');
    await until(() => prompts.showing, 3000);
    withdraw('org.example.Voice');
    await until(() => !dialogLayer(), 2000);
    check('E: the dialog goes with the name', !prompts.showing && !dialogLayer());
    await sleep(300);
    check('E: nothing answered', writes.slice(before).every(w => w[0] === 'delete'), JSON.stringify(writes.slice(before)));

    // F: no answer in time (2 s here)
    ask('org.example.Voice');
    await until(() => prompts.showing, 3000);
    await until(() => !dialogLayer(), 6000);
    check('F: the dialog is gone after the time', !prompts.showing && !dialogLayer());
    check('F: dismissed', written('set', 'vibeos-sandbox-ask', 'org.example.Voice').length === 4);
    withdraw('org.example.Voice');
    await sleep(300);

    // G: one dialog per app; other names left alone
    const asks = writes.length;
    const bus = Gio.DBus.session;
    ask('org.example.Voice');
    await until(() => prompts.showing, 3000);
    // (the same name announced again by a second owner in the queue is no new name)
    const second = Gio.bus_own_name(Gio.BusType.SESSION, 'org.freedesktop.ReserveDevice1.Audio7',
        Gio.BusNameOwnerFlags.NONE, null, null, null);
    const odd = Gio.bus_own_name(Gio.BusType.SESSION, 'org.freedesktop.ReserveDevice1.VibeOSMicAsk0g',
        Gio.BusNameOwnerFlags.NONE, null, null, null);
    await sleep(500);
    const layers = Main.layoutManager.modalDialogGroup.get_children().filter(a => a.visible &&
        a.get_n_children() === 1 && a.get_first_child().constructor.name.includes('BoxLayout'));
    check('G: one dialog', layers.length === 1, `${layers.length}`);
    check('G: other names left alone', writes.slice(asks).every(w => w[3] === 'org.example.Voice'));
    Gio.bus_unown_name(second);
    Gio.bus_unown_name(odd);
    withdraw('org.example.Voice');
    await until(() => !prompts.showing, 2000);

    // a question already open when the shell starts
    prompts.destroy();
    ask('org.example.Voice');
    await sleep(300);
    const late = new MicrophonePrompts(keyguard, bus, 30);
    await until(() => late.showing, 3000);
    check('G: a question open before the shell started is asked', late.showing && !!dialogLayer());
    late.destroy();
    await sleep(300);
    check('G: destroyed: the dialog is gone', !dialogLayer());
    withdraw('org.example.Voice');

    say(failures ? `FAILED ${failures}` : 'ALL PASSED');
    if (failures)
        throw new Error(`${failures} checks failed`);
}
