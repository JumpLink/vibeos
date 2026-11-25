// The on-screen keyboard closed under a finger, in a headless GNOME Shell of the test's own (keyboard-stale.sh runs it
// as the shell's automation script): the extension's keyboard swapped in, its touch-focus close holding, and a window
// with a URL field (keyboard-stale-client.js). Touches come from a virtual touchscreen of this shell's own.
//
//   A  the field loses the focus, no finger down: the keyboard goes.
//   B  the field loses the focus while a finger drags above the keys, which the keyboard's swipe down takes, and the
//      finger lifts while the keyboard slides down: it goes. (The shell's end of the swipe slid the closing keyboard
//      back up, where it stayed, closed, over the app and then the launcher.)
//   C  a keyboard left on the screen closed: Back sees it (keyboardShown) and takes it away (dismissKeyboard).
//   D  the same, touched on a key: it goes, and the keys never have the touch (on the open keyboard they do).
import Clutter from 'gi://Clutter';
import Gio from 'gi://Gio';
import GLib from 'gi://GLib';

import * as Main from 'resource:///org/gnome/shell/ui/main.js';

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

// the client: its lines kept, commands sent
const lines = [];
function spawnClient() {
    const launcher = new Gio.SubprocessLauncher({
        flags: Gio.SubprocessFlags.STDIN_PIPE | Gio.SubprocessFlags.STDOUT_PIPE,
    });
    // no portal answers on the test's bus (GTK waits for one), and the software renderer will do
    launcher.setenv('GDK_DEBUG', 'no-portals', true);
    launcher.setenv('GSK_RENDERER', 'cairo', true);
    launcher.setenv('GTK_A11Y', 'none', true);
    const proc = launcher.spawnv(['gjs', '-m', `${DIR}/keyboard-stale-client.js`]);
    const out = new Gio.DataInputStream({base_stream: proc.get_stdout_pipe()});
    const read = () => out.read_line_async(GLib.PRIORITY_DEFAULT, null, (stream, res) => {
        const [line] = stream.read_line_finish_utf8(res);
        if (line === null)
            return;
        lines.push(line);
        read();
    });
    read();
    return {proc, send: cmd => proc.get_stdin_pipe().write_all(new TextEncoder().encode(`${cmd}\n`), null)};
}
async function fieldText(client) {
    const n = lines.length;
    client.send('text');
    await until(() => lines.slice(n).some(l => l.startsWith('client text')), 5000);
    return lines.slice(n).find(l => l.startsWith('client text'))?.replace(/^client text /, '') ?? null;
}

const keyboard = () => Main.keyboard._keyboard;
// the client's URL field has the input focus (not a field of the shell's own)
const fieldFocused = () => Main.inputMethod.currentFocus !== null &&
    Main.inputMethod.content_purpose === Clutter.InputContentPurpose.URL;
// on the screen: its box shown, the keys not see-through, not slid off the bottom
function drawn() {
    const k = keyboard();
    return !!k && Main.layoutManager.keyboardBox.visible && k.mapped && k.opacity > 0 &&
        k.translation_y < k.height - 1;
}
function state() {
    const k = keyboard();
    return k ? `open=${!!Main.keyboard.visible} drawn=${drawn()} slid=${k.translation_y.toFixed(0)}/` +
        `${k.height.toFixed(0)} opacity=${k.opacity} swipe=${k._panGesture?.enabled} ` +
        `field=${fieldFocused()} overview=${Main.overview.visible}` : 'no keyboard';
}

let device = null;
const now = () => GLib.get_monotonic_time();
function touchscreen() {
    device ??= global.stage.context.get_backend().get_default_seat()
        .create_virtual_device(Clutter.InputDeviceType.TOUCHSCREEN_DEVICE);
    return device;
}
async function drag(slot, x, y0, y1, ms) {
    touchscreen().notify_touch_down(now(), slot, x, y0);
    const steps = Math.max(2, Math.round(ms / 16));
    for (let i = 1; i <= steps; i++) {
        await sleep(16);
        touchscreen().notify_touch_motion(now(), slot, x, y0 + (y1 - y0) * i / steps);
    }
}
// the finger kept down, trembling by a pixel: for ms, or until done() says so
async function hold(slot, x, y, ms) {
    const end = Date.now() + (typeof ms === 'number' ? ms : 60000);
    for (let n = 0; Date.now() < end && !(typeof ms === 'function' && ms()); n++) {
        await sleep(40);
        touchscreen().notify_touch_motion(now(), slot, x, y + n % 2);
    }
}
const lift = slot => touchscreen().notify_touch_up(now(), slot);

// the field loses the focus (the client takes its time on a slow machine)
async function unfocus(client) {
    client.send('unfocus');
    await until(() => Main.inputMethod.currentFocus === null, 15000);
}

async function openOn(client) {
    client.send('focus');
    await until(fieldFocused, 15000);
    Main.keyboard.open(Main.layoutManager.primaryIndex);
    await until(() => Main.keyboard.visible && drawn(), 5000);
    await sleep(400);
}

// what the shell's end of a cancelled swipe down left: closed, and drawn where an open keyboard is
function makeStale() {
    const k = keyboard();
    k._keyboardVisible = false;
    k._keyboardRequested = false;
    k._panGesture.enabled = false;
}

export async function run() {
    await sleep(1000);
    new Gio.Settings({schema_id: 'org.gnome.desktop.a11y.applications'}).set_boolean('screen-keyboard-enabled', true);
    new Gio.Settings({schema_id: 'vibeos.Shell'}).set_string('keyboard', 'vibeos');
    await sleep(300);
    const kb = await import(`file://${DIR}/ext/keyboard/keyboard.js`);
    const {TouchFocusKeyboard} = await import(`file://${DIR}/ext/oskfocus.js`);
    const swap = new kb.KeyboardSwap();
    const touchFocus = new TouchFocusKeyboard();
    check('the extension\'s keyboard is in', swap.active && keyboard()?.constructor.name.includes('VibeosKeyboard'));

    const client = spawnClient();
    const window = () => global.display.list_all_windows().find(w => w.title === 'keyboard-stale');
    await until(() => window()?.get_compositor_private()?.mapped, 120000);
    // the shell's home and its search field away, and the window given the focus (a window of a process the shell did
    // not start may not be given it by itself)
    Main.overview.hide();
    await until(() => !Main.overview.visible, 10000);
    global.stage.set_key_focus(null);
    window()?.activate(global.get_current_time());
    await until(() => global.display.focus_window === window(), 5000);
    check('the window is up, focused', !!window() && global.display.focus_window === window(),
        global.display.list_all_windows().map(w => `"${w.title}"`).join(' '));
    await sleep(500);
    const monitor = Main.layoutManager.primaryMonitor;
    const x = monitor.x + monitor.width / 2;
    // the keyboard's slides run as on a phone (a shell drawing in software has its animations off, and an ease then
    // ends at once)
    global.force_animations = true;

    await openOn(client);
    check('A: open, for the client\'s field', fieldFocused() && Main.keyboard.visible && drawn(), state());
    await unfocus(client);
    await until(() => !Main.keyboard.visible && !drawn(), 5000);
    check('A: closed, gone', !Main.keyboard.visible && !drawn(), state());

    await openOn(client);
    const top = monitor.y + monitor.height - keyboard().height;
    await drag(0, x, top - 500, top - 380, 250);
    await until(() => keyboard()._panGesture.state === Clutter.GestureState.RECOGNIZING, 5000);
    check('B: the swipe down has the finger', keyboard()._panGesture.state === Clutter.GestureState.RECOGNIZING,
        `state ${keyboard()._panGesture.state}`);
    // the finger kept down while the field loses the focus, until the keyboard closes (at once, or when a close held
    // for the finger gives up), and lifted while it slides down
    let unfocused = false;
    const holding = hold(0, x, top - 380, () => unfocused);
    await unfocus(client);
    unfocused = true;
    await holding;
    await hold(0, x, top - 380, () => !Main.keyboard.visible);
    check('B: closed under the finger', !Main.keyboard.visible, state());
    lift(0);
    await sleep(1500);
    check('B: lifted, gone', !Main.keyboard.visible && !drawn(), state());
    check('B: no finger counted down', touchFocus._touches === 0, `${touchFocus._touches}`);

    await openOn(client);
    makeStale();
    check('C: left drawn, closed', !Main.keyboard.visible && drawn(), state());
    check('C: Back sees it', kb.keyboardShown());
    kb.dismissKeyboard();
    await until(() => !drawn(), 5000);
    check('C: Back took it away', !drawn() && !kb.keyboardShown(), state());

    // a tap on the middle of the keys: the keys have it on the open keyboard (so that "the keys never had it" below
    // means something)
    await openOn(client);
    let keyTouches = 0;
    const onTouch = keyboard()._onTouch;
    keyboard()._onTouch = function (event) {
        keyTouches++;
        return onTouch.call(this, event);
    };
    const middle = monitor.y + monitor.height - keyboard().height / 2;
    const tap = async () => {
        touchscreen().notify_touch_down(now(), 1, x, middle);
        await sleep(60);
        lift(1);
    };
    await tap();
    await until(() => keyTouches >= 2, 5000);
    check('D: the open keyboard\'s keys have the tap', keyTouches >= 2, `${keyTouches} touch events`);
    await sleep(500);
    const text = await fieldText(client);
    makeStale();
    keyTouches = 0;
    await tap();
    await until(() => !drawn(), 5000);
    await sleep(500);
    check('D: touched, gone', !drawn(), state());
    check('D: the keys never had the touch', keyTouches === 0, `${keyTouches} touch events`);
    const after = await fieldText(client);
    check('D: nothing typed', after === text, `${text} -> ${after}`);

    client.send('quit');
    touchFocus.destroy();
    swap.destroy();
    say(failures ? `FAILED ${failures}` : 'ALL PASSED');
    if (failures)
        throw new Error(`${failures} checks failed`);
}
