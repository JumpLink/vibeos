// Bluetooth's pairing dialogs (pairdialog.js) in a headless GNOME Shell of the test's own (bt-dialog.sh runs it as the
// shell's automation script), the extension's keyboard swapped in, touches from a virtual touchscreen of this shell's.
//
//   A  a PIN: the dialog over a dim, the field focused, the keyboard up and the dialog above it; letters kept out,
//      Pair only with a PIN, tapped: the PIN is the answer
//   B  "PIN contains letters or symbols" lets them in; Cancel answers no
//   C  asked while locked: a notification and no dialog; unlocked, the dialog and no notification; Pair: yes
//   D  a code shown, locked over it: behind a notification again, back on unlock; close() takes it away
//   E  a notification swiped away while locked answers no
//   F  a service question; the agent made and taken down where no BlueZ is
import Clutter from 'gi://Clutter';
import Gio from 'gi://Gio';
import GLib from 'gi://GLib';
import Shell from 'gi://Shell';
import St from 'gi://St';

import * as Main from 'resource:///org/gnome/shell/ui/main.js';
import * as MessageTray from 'resource:///org/gnome/shell/ui/messageTray.js';
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

function shot(name) {
    return new Promise(resolve => {
        try {
            const file = Gio.File.new_for_path(`${DIR}/shots/${name}.png`);
            const stream = file.replace(null, false, Gio.FileCreateFlags.NONE, null);
            new Shell.Screenshot().screenshot(false, stream, (o, res) => {
                try {
                    o.screenshot_finish(res);
                } catch (e) {
                    say('note: no picture', name, e.message);
                }
                stream.close(null);
                resolve();
            });
        } catch (e) {
            say('note: no picture', name, e.message);
            resolve();
        }
    });
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

// every actor under @root
function* actors(root) {
    for (const child of root.get_children()) {
        yield child;
        yield* actors(child);
    }
}
const buttons = root => [...actors(root)].filter(a => a instanceof St.Button);
const button = (root, label) => buttons(root).find(b => b.accessible_name === label);

function dialogLayer() {
    return Main.layoutManager.modalDialogGroup.get_children().find(a => a.visible && a.get_n_children() === 1 &&
        a.get_first_child().constructor.name.includes('BoxLayout')) ?? null;
}

function ourNotifications() {
    return Main.messageTray.getSources().filter(s => s.title === 'Bluetooth').flatMap(s => s.notifications);
}

export async function run() {
    await sleep(1000);
    new Gio.Settings({schema_id: 'org.gnome.desktop.a11y.applications'}).set_boolean('screen-keyboard-enabled', true);
    new Gio.Settings({schema_id: 'vibeos.Shell'}).set_string('keyboard', 'vibeos');
    await sleep(300);
    const units = await import(`file://${DIR}/ext/units.js`);
    units.geometry.start();
    const kb = await import(`file://${DIR}/ext/keyboard/keyboard.js`);
    const swap = new kb.KeyboardSwap();
    check('the extension\'s keyboard is in', swap.active);
    const {PairingUI} = await import(`file://${DIR}/ext/pairdialog.js`);
    const {Bluetooth} = await import(`file://${DIR}/ext/bluetooth.js`);
    Main.overview.hide();
    await until(() => !Main.overview.visible, 10000);

    const keyguard = new Keyguard();
    const ui = new PairingUI(keyguard);

    // A: a PIN
    let h = ui.ask({kind: 'pin', device: '/test/dev_1', name: 'Test car'});
    await until(() => ui.showing, 3000);
    let layer = dialogLayer();
    check('A: the dialog is up', ui.showing && !!layer);
    const focus = global.stage.key_focus;
    check('A: the PIN field has the focus', focus instanceof Clutter.Text && focus.editable,
        `${focus?.constructor.name}`);
    check('A: digits asked for', focus?.input_purpose === Clutter.InputContentPurpose.DIGITS);
    if (!Main.keyboard.visible)
        Main.keyboard.open(Main.layoutManager.primaryIndex);
    const kbUp = await until(() => Main.keyboard.visible && Main.layoutManager.keyboardBox.visible, 5000);
    await sleep(700);
    check('A: the keyboard is up', kbUp);
    const panel = layer?.get_first_child();
    if (panel && kbUp) {
        const [, py] = panel.get_transformed_position();
        const [, ph] = panel.get_transformed_size();
        const keys = Main.keyboard.keyboardActor;
        const [, ky] = keys.get_transformed_position();
        check('A: the dialog above the keyboard', py + ph <= ky + 1, `dialog ${py.toFixed(0)}+${ph.toFixed(0)} keys ${ky.toFixed(0)}`);
    }
    await shot('pin');
    const pair = button(layer, 'Pair');
    check('A: Pair not tappable without a PIN', pair && !pair.reactive);
    focus.text = '12ab34';
    await sleep(100);
    check('A: letters kept out', focus.text === '1234', focus.text);
    check('A: Pair tappable with a PIN', pair.reactive);
    await shot('pin-typed');
    await tap(pair);
    let answer = await Promise.race([h.answer, sleep(2000).then(() => 'none')]);
    check('A: Pair answers the PIN', answer === '1234', `${answer}`);
    await until(() => !dialogLayer(), 2000);
    check('A: the dialog gone', !ui.showing && !dialogLayer());

    // B: letters and symbols, then Cancel
    h = ui.ask({kind: 'pin', device: '/test/dev_1', name: 'Test car'});
    await until(() => ui.showing, 3000);
    await sleep(600);
    layer = dialogLayer();
    const box = button(layer, 'PIN contains letters or symbols');
    await tap(box);
    const field = global.stage.key_focus;
    check('B: the box ticked', box.checked);
    check('B: the field asks for text', field?.input_purpose === Clutter.InputContentPurpose.NORMAL);
    field.text = 'ab12';
    await sleep(100);
    check('B: letters kept', field.text === 'ab12', field.text);
    await shot('pin-letters');
    await tap(button(layer, 'Cancel'));
    answer = await Promise.race([h.answer, sleep(2000).then(() => 'none')]);
    check('B: Cancel answers no', answer === null, `${answer}`);
    Main.keyboard.close?.();

    // C: asked while locked
    keyguard.set(true);
    h = ui.ask({kind: 'confirm', device: '/test/dev_2', name: 'Headphones', code: '012345'});
    await sleep(300);
    check('C: no dialog while locked', !ui.showing && !dialogLayer());
    let notes = ourNotifications();
    check('C: a notification instead', notes.length === 1 && notes[0].title === 'Pair with Headphones?',
        notes.map(n => n.title).join());
    keyguard.set(false);
    await until(() => ui.showing, 3000);
    check('C: unlocked, the dialog', ui.showing);
    check('C: ... and the notification gone', ourNotifications().length === 0);
    await sleep(400);
    await shot('confirm');
    await tap(button(dialogLayer(), 'Pair'));
    answer = await Promise.race([h.answer, sleep(2000).then(() => 'none')]);
    check('C: Pair answers yes', answer === true, `${answer}`);

    // D: a code shown, locked over it, closed by BlueZ
    h = ui.ask({kind: 'display', device: '/test/dev_3', name: 'Keyboard', code: '654321'});
    await until(() => ui.showing, 3000);
    await sleep(400);
    await shot('display');
    keyguard.set(true);
    await sleep(200);
    check('D: locked: the dialog hidden', !ui.showing && !dialogLayer());
    check('D: ... behind a notification', ourNotifications().length === 1);
    keyguard.set(false);
    await until(() => ui.showing, 3000);
    check('D: unlocked: the dialog back', ui.showing && ourNotifications().length === 0);
    h.close();
    answer = await Promise.race([h.answer, sleep(2000).then(() => 'none')]);
    await until(() => !dialogLayer(), 2000);
    check('D: close() takes it away unanswered', answer === null && !ui.showing && !dialogLayer(), `${answer}`);

    // E: a notification swiped away
    keyguard.set(true);
    h = ui.ask({kind: 'pin', device: '/test/dev_4', name: 'Speaker'});
    await sleep(200);
    notes = ourNotifications();
    notes[0]?.destroy(MessageTray.NotificationDestroyedReason.DISMISSED);
    answer = await Promise.race([h.answer, sleep(2000).then(() => 'none')]);
    check('E: a notification swiped away answers no', answer === null, `${answer}`);
    keyguard.set(false);
    await sleep(200);
    check('E: ... and no dialog after unlock', !ui.showing && !dialogLayer());

    // F: a service question; the agent on a bus with no BlueZ
    h = ui.ask({kind: 'service', device: '/test/dev_5', name: 'Phone of someone', uuid: '0000111e'});
    await until(() => ui.showing, 3000);
    await sleep(400);
    await shot('service');
    await tap(button(dialogLayer(), "Don't allow"));
    answer = await Promise.race([h.answer, sleep(2000).then(() => 'none')]);
    check('F: Don\'t allow answers no', answer === null, `${answer}`);
    let made = false;
    try {
        const bt = new Bluetooth({ui});
        await sleep(500);
        bt.destroy();
        made = true;
    } catch (e) {
        say('error', e.message);
    }
    check('F: the agent made and taken down without BlueZ', made);
    ui.destroy();
    swap.destroy();

    say(failures ? `FAILED ${failures}` : 'ALL PASSED');
    if (failures)
        throw new Error(`${failures} checks failed`);
}
