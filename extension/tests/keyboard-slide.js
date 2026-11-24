// GNOME Console under the on-screen keyboard, in a headless GNOME Shell of the test's own (keyboard-slide.sh runs it as
// the shell's automation script): the extension's keyboard swapped in, its touch-focus opening, and a Console of the
// test's own. Touches and keys come from virtual devices of this shell's own, text from its input method.
//
//   A  a tap on the terminal opens the keyboard over it: the window keeps its size, and with the prompt at its top it
//      stays where it is.
//   B  output takes the cursor down to the last line: the window slides up until the cursor's line is just above the
//      keys, and keeps its size.
//   C  `clear` takes the cursor back up: the window slides back down, the keyboard still up.
//   D  the terminal's menu (Shift+F10) opened on the slid window: drawn at the cursor, on the window as it is drawn
//      (mutter places it on the window unslid), and still on it a moment later.
//   E  Console's main menu, its button tapped with the window slid less than the header bar's height: drawn under the
//      button as the button is drawn. (A long press, a finger's way to the terminal's menu, is reported.)
//   F  Back closes the keyboard: the window slides back down, and it never changed its size.
import Clutter from 'gi://Clutter';
import Gio from 'gi://Gio';
import GLib from 'gi://GLib';
import Meta from 'gi://Meta';
import Shell from 'gi://Shell';

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

const keyboard = () => Main.keyboard._keyboard;
function drawn() {
    const k = keyboard();
    return !!k && Main.layoutManager.keyboardBox.visible && k.mapped && k.opacity > 0 &&
        k.translation_y < k.height - 1;
}
const terminalFocused = () => Main.inputMethod.currentFocus !== null &&
    Main.inputMethod.content_purpose === Clutter.InputContentPurpose.TERMINAL;

let touch = null, keys = null;
const now = () => GLib.get_monotonic_time();
const seat = () => global.stage.context.get_backend().get_default_seat();
async function tap(x, y) {
    touch ??= seat().create_virtual_device(Clutter.InputDeviceType.TOUCHSCREEN_DEVICE);
    touch.notify_touch_down(now(), 0, x, y);
    await sleep(60);
    touch.notify_touch_up(now(), 0);
}
// the finger kept down for ms, trembling by a pixel, then lifted
async function longPress(x, y, ms, whileDown) {
    touch ??= seat().create_virtual_device(Clutter.InputDeviceType.TOUCHSCREEN_DEVICE);
    touch.notify_touch_down(now(), 0, x, y);
    const end = Date.now() + ms;
    for (let n = 0; Date.now() < end; n++) {
        await sleep(40);
        touch.notify_touch_motion(now(), 0, x, y + n % 2);
    }
    await whileDown?.();
    touch.notify_touch_up(now(), 0);
}
async function keyCombo(modifiers, keyval) {
    keys ??= seat().create_virtual_device(Clutter.InputDeviceType.KEYBOARD_DEVICE);
    for (const m of modifiers)
        keys.notify_keyval(now(), m, Clutter.KeyState.PRESSED);
    keys.notify_keyval(now(), keyval, Clutter.KeyState.PRESSED);
    await sleep(30);
    keys.notify_keyval(now(), keyval, Clutter.KeyState.RELEASED);
    for (const m of modifiers.reverse())
        keys.notify_keyval(now(), m, Clutter.KeyState.RELEASED);
}
const key = keyval => keyCombo([], keyval);

export async function run() {
    await sleep(1000);
    new Gio.Settings({schema_id: 'org.gnome.desktop.a11y.applications'}).set_boolean('screen-keyboard-enabled', true);
    new Gio.Settings({schema_id: 'vibeos.Shell'}).set_string('keyboard', 'vibeos');
    await sleep(300);
    const kb = await import(`file://${DIR}/ext/keyboard/keyboard.js`);
    const {dp} = await import(`file://${DIR}/ext/units.js`);
    const {TouchFocusKeyboard} = await import(`file://${DIR}/ext/oskfocus.js`);
    const swap = new kb.KeyboardSwap();
    const touchFocus = new TouchFocusKeyboard();
    check('the extension\'s keyboard is in', swap.active && keyboard()?.constructor.name.includes('VibeosKeyboard'));

    // the text cursor as mutter has it (the window unslid)
    let cursor = null;
    Main.inputMethod.connect('cursor-location-changed', (_im, rect) => {
        if (Number.isFinite(rect.get_y()) && Number.isFinite(rect.get_height()))
            cursor = {y: rect.get_y(), height: rect.get_height()};
    });

    const launcher = new Gio.SubprocessLauncher({flags: Gio.SubprocessFlags.NONE});
    launcher.setenv('GDK_DEBUG', 'no-portals', true);
    launcher.setenv('GSK_RENDERER', 'cairo', true);
    launcher.setenv('GTK_A11Y', 'none', true);
    launcher.set_cwd(GLib.get_home_dir());
    const proc = launcher.spawnv(['kgx']);
    const tracker = Shell.WindowTracker.get_default();
    const console_ = () => global.display.list_all_windows().find(w => w.get_window_type() === Meta.WindowType.NORMAL &&
        tracker.get_window_app(w)?.get_id() === 'org.gnome.Console.desktop');
    await until(() => console_()?.get_compositor_private()?.mapped, 120000);
    const win = console_();
    Main.overview.hide();
    await until(() => !Main.overview.visible, 10000);
    global.stage.set_key_focus(null);
    win?.activate(global.get_current_time());
    await until(() => global.display.focus_window === win && terminalFocused(), 15000);
    check('Console is up, its terminal focused', !!win && global.display.focus_window === win && terminalFocused(),
        global.display.list_all_windows().map(w => `"${w.title}"`).join(' '));
    check('Console slides (keyboard-slide-apps)', kb.keyboardSlides(win));
    // over the whole work area, as the phone's shell puts every window (this shell's session leaves it floating)
    win.maximize();
    const tall = () => win.get_frame_rect().height > Main.layoutManager.primaryMonitor.height / 2;
    await until(() => win.maximized_vertically && tall(), 10000);
    await sleep(1500);
    check('Console is maximized', win.maximized_vertically && tall(),
        `${win.get_frame_rect().width}x${win.get_frame_rect().height}`);
    global.force_animations = true;

    const actor = win.get_compositor_private();
    const frame0 = win.get_frame_rect();
    const frameText = r => `${r.x},${r.y} ${r.width}x${r.height}`;
    const sameFrame = () => {
        const r = win.get_frame_rect();
        return r.x === frame0.x && r.y === frame0.y && r.width === frame0.width && r.height === frame0.height;
    };
    let resized = 0;
    win.connect('size-changed', () => resized++);
    const monitor = Main.layoutManager.primaryMonitor;
    const x = monitor.x + monitor.width / 2;
    const top = () => monitor.y + monitor.height - keyboard().height;
    const settled = async () => {
        await until(() => !actor.get_transition('translation-y'), 5000);
        await sleep(300);
    };
    const cursorText = () => cursor ? `${cursor.y.toFixed(0)}+${cursor.height.toFixed(0)}` : '-';
    const state = () => `open=${!!Main.keyboard.visible} drawn=${drawn()} slid=${actor.translation_y.toFixed(1)} ` +
        `frame=${frameText(win.get_frame_rect())} cursor=${cursorText()} ` +
        `top=${keyboard() ? top().toFixed(0) : '-'} resized=${resized}`;
    say('INFO', 'before', state());

    // A
    await tap(x, frame0.y + frame0.height / 3);
    await until(() => Main.keyboard.visible && drawn(), 5000);
    await settled();
    check('A: a tap on the terminal opens the keyboard', Main.keyboard.visible && drawn(), state());
    check('A: the window keeps its size', sameFrame() && resized === 0, state());
    check('A: the prompt at the top: not slid', actor.translation_y === 0, state());

    // B
    const type = text => Main.inputMethod.commit(text);
    const slidTo = () => {
        const covered = Math.max(0, frame0.y + frame0.height - top());
        return -Math.round(Math.min(covered, Math.max(0, cursor.y + cursor.height + dp(8) - top())));
    };
    type('seq 200\r');
    await until(() => actor.translation_y < 0, 10000);
    await settled();
    check('B: the output\'s last line: slid up',
        actor.translation_y < 0 && Math.abs(actor.translation_y - slidTo()) <= 1, `${state()} expected ${slidTo()}`);
    check('B: the cursor\'s line just above the keys',
        cursor.y + cursor.height + actor.translation_y <= top() &&
        cursor.y + cursor.height + actor.translation_y >= top() - dp(8) - 1, state());
    check('B: the window keeps its size', sameFrame() && resized === 0, state());

    // C
    type('clear\r');
    await until(() => actor.translation_y === 0 && !actor.get_transition('translation-y'), 10000);
    await settled();
    check('C: `clear`: back down, the keyboard still up', actor.translation_y === 0 && Main.keyboard.visible, state());

    // D
    type('seq 200\r');
    await until(() => actor.translation_y < 0, 10000);
    await settled();
    const menuOf = () => global.get_window_actors().find(a => a.meta_window.find_root_ancestor() === win &&
        a.meta_window.get_window_type() === Meta.WindowType.DROPDOWN_MENU);
    const menuText = a => a ? `menu ${a.x.toFixed(0)},${(a.y + a.translation_y).toFixed(0)} ${a.width.toFixed(0)}x` +
        `${a.height.toFixed(0)} lift=${a.translation_y.toFixed(1)}` : 'no menu';
    // on the window as it is drawn: moved as far as the window, or further where the keyboard lifted it clear of the
    // keys, and on the screen
    const attached = a => !!a && a.translation_y <= actor.translation_y + 0.5 &&
        Math.abs(a.translation_y - Math.min(a.meta_window.__vibeosLift ?? 0, actor.translation_y)) < 0.5 &&
        a.y + a.translation_y >= monitor.y;
    // (mutter places it, above the cursor's line, where there is no room under it)
    const cursorY = cursor.y;
    await keyCombo([Clutter.KEY_Shift_L], Clutter.KEY_F10);
    await until(() => menuOf()?.mapped, 5000);
    await sleep(100);
    let menu = menuOf();
    check('D: Shift+F10 opens the terminal\'s menu, at the cursor', !!menu &&
        Math.abs(menu.y + menu.height - cursorY) < 60, `${menuText(menu)} the cursor's line at ` +
        `${(cursorY + actor.translation_y).toFixed(0)} ${state()}`);
    check('D: drawn on the slid window', attached(menu), `${menuText(menu)} ${state()}`);
    await sleep(1500);
    menu = menuOf();
    check('D: still on the window a moment later', attached(menu), `${menuText(menu)} ${state()}`);
    await key(Clutter.KEY_Escape);
    await until(() => !menuOf(), 5000);
    check('D: Escape closes it', !menuOf(), state());
    await sleep(500);

    // E: slid less than the header bar's height, its main menu button half on the screen, tapped
    if (!Main.keyboard.visible) {
        await tap(x, monitor.y + monitor.height / 4);
        await until(() => Main.keyboard.visible && drawn(), 5000);
    }
    type('clear\r');
    await until(() => actor.translation_y === 0 && !actor.get_transition('translation-y'), 10000);
    await settled();
    // the prompt on the first line, the command on it, then n lines and the next prompt: its cursor (n + 1) lines
    // down, and the window slid by less than a line
    const first = cursor.y, line = cursor.height;
    const n = Math.floor((top() - dp(8) - first) / line) - 1;
    type(`seq ${n}\r`);
    await until(() => actor.translation_y < 0, 10000);
    await settled();
    const slidE = actor.translation_y;
    // (in this shell's layout: new tab, main menu, minimize, maximize and close at the header bar's end)
    const button = {x: frame0.x + frame0.width - 138, y: frame0.y + 23};
    say('INFO', `E: slid ${slidE.toFixed(0)}, the menu button at ${button.x},${(button.y + slidE).toFixed(0)}`,
        state());
    await tap(button.x, button.y + slidE + 4);
    await until(() => menuOf()?.mapped, 5000);
    await sleep(100);
    menu = menuOf();
    check('E: the main menu opens', !!menu, `${menuText(menu)} ${state()}`);
    check('E: under its button as it is drawn', !!menu && attached(menu) &&
        Math.abs(menu.y + menu.translation_y - (button.y + 17 + actor.translation_y)) < 40,
    `${menuText(menu)} ${state()}`);
    await sleep(1500);
    menu = menuOf();
    say('INFO', 'E: a moment later', menuText(menu), state());
    check('E: still on the window', !menu || attached(menu), `${menuText(menu)} ${state()}`);
    await key(Clutter.KEY_Escape);
    await until(() => !menuOf(), 5000);
    check('E: Escape closes it', !menuOf(), state());
    await sleep(500);

    // a long press, as a finger would ask for the terminal's menu: what Console does with it (reported)
    if (!Main.keyboard.visible) {
        await tap(x, monitor.y + monitor.height / 4);
        await until(() => Main.keyboard.visible && drawn(), 5000);
    }
    await longPress(x, top() - 300, 1500, async () => {
        await until(() => menuOf()?.mapped, 2000);
        say('INFO', 'a long press:', menuText(menuOf()), state(), global.get_window_actors().map(a =>
            `${a.meta_window.get_window_type()}:"${a.meta_window.title}"`).join(' '));
    });
    await sleep(500);
    if (menuOf()) {
        await key(Clutter.KEY_Escape);
        await until(() => !menuOf(), 5000);
    }
    await sleep(500);

    // F
    if (!Main.keyboard.visible) {
        await tap(x, monitor.y + monitor.height / 4);
        await until(() => Main.keyboard.visible && drawn(), 5000);
    }
    type('seq 200\r');
    await until(() => cursor.y > top() && Math.abs(actor.translation_y - slidTo()) <= 1, 10000);
    await settled();
    check('F: slid with the keyboard up', Main.keyboard.visible && actor.translation_y < 0 &&
        Math.abs(actor.translation_y - slidTo()) <= 1, `${state()} expected ${slidTo()}`);
    kb.dismissKeyboard();
    await until(() => !Main.keyboard.visible && actor.translation_y === 0, 5000);
    await settled();
    check('F: Back: the keyboard gone, the window back down', !Main.keyboard.visible && !drawn() &&
        actor.translation_y === 0, state());
    check('F: the window never changed its size', sameFrame() && resized === 0, state());

    proc.force_exit();
    touchFocus.destroy();
    swap.destroy();
    say(failures ? `FAILED ${failures}` : 'ALL PASSED');
    if (failures)
        throw new Error(`${failures} checks failed`);
}
