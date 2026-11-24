// The Terminal under a finger, in a headless GNOME Shell of the test's own (touch.sh runs it as the shell's automation
// script): the extension's keyboard swapped in, its touch-focus opening, and a Terminal of the test's own. Touches
// and keys come from virtual devices of this shell's own, text from its input method. A program of the test's
// (probe.py) asks for a program's modes and logs what the terminal sends it.
//
//   A  a tap opens the keyboard over the window: it keeps its size, and output to the bottom slides it up.
//   B  on the alternate screen (less) two fingers going up 5½ rows send 5 Down keys, and back down 3½ rows 3 Up; one
//      finger going up 5½ rows past the touch slop 5 more Down keys; with application cursor keys (DECCKM) they are
//      ESC O B.
//   C  a flung two-finger scroll goes on after the fingers lift: more keys than the fingers' travel.
//   D  a program that tracks the mouse (htop) gets wheel buttons, SGR and the original encoding, at the cell under
//      the fingers, from two fingers and from one; a tap is a click there.
//   E  a long press selects the word under the finger; Ctrl+Shift+C copies it.
//   F  two fingers going down 10 rows scroll the history back 10 rows: the word under the same spot is 10 lines up;
//      one finger going down 10 rows past the slop, 10 more.
//   G  the selection's end handle dragged 2 rows down takes the selection to there.
//   H  a tap ends the selection; Ctrl+Shift+V pastes into the shell.
import Clutter from 'gi://Clutter';
import Gio from 'gi://Gio';
import GLib from 'gi://GLib';
import Meta from 'gi://Meta';
import Shell from 'gi://Shell';
import St from 'gi://St';

import * as Main from 'resource:///org/gnome/shell/ui/main.js';

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
const finger = () => (touch ??= seat().create_virtual_device(Clutter.InputDeviceType.TOUCHSCREEN_DEVICE));
async function tap(x, y) {
    finger().notify_touch_down(now(), 0, x, y);
    await sleep(60);
    touch.notify_touch_up(now(), 0);
}
// the finger kept down for ms, trembling by a pixel, then (after whileDown) moved to `to` and lifted
async function longPress(x, y, ms, to = null) {
    finger().notify_touch_down(now(), 0, x, y);
    const end = Date.now() + ms;
    for (let n = 0; Date.now() < end; n++) {
        await sleep(40);
        touch.notify_touch_motion(now(), 0, x, y + n % 2);
    }
    if (to) {
        for (let i = 1; i <= 10; i++) {
            await sleep(16);
            touch.notify_touch_motion(now(), 0, x + (to.x - x) * i / 10, y + (to.y - y) * i / 10);
        }
        await sleep(100);
    }
    touch.notify_touch_up(now(), 0);
}
// two fingers 120 px apart, centred on x, going dy from y over ms; held still for `hold` ms before they lift (no
// fling), or lifted at once (a fling)
async function swipe2(x, y, dy, ms, hold = 150) {
    const steps = Math.max(3, Math.round(ms / 16));
    finger().notify_touch_down(now(), 0, x - 60, y);
    touch.notify_touch_down(now(), 1, x + 60, y);
    await sleep(30);
    for (let i = 1; i <= steps; i++) {
        await sleep(ms / steps);
        touch.notify_touch_motion(now(), 0, x - 60, y + dy * i / steps);
        touch.notify_touch_motion(now(), 1, x + 60, y + dy * i / steps);
    }
    if (hold)
        await sleep(hold);
    touch.notify_touch_up(now(), 0);
    touch.notify_touch_up(now(), 1);
}
// one finger going dy from y over ms, held still before it lifts as swipe2's
async function swipe1(x, y, dy, ms, hold = 150) {
    const steps = Math.max(3, Math.round(ms / 16));
    finger().notify_touch_down(now(), 0, x, y);
    await sleep(30);
    for (let i = 1; i <= steps; i++) {
        await sleep(ms / steps);
        touch.notify_touch_motion(now(), 0, x, y + dy * i / steps);
    }
    if (hold)
        await sleep(hold);
    touch.notify_touch_up(now(), 0);
}
// the Terminal's touch slop (term-view.c): a one-finger scroll moves the text by the finger's travel less this
const SLOP = 8;
async function keyCombo(modifiers, keyval) {
    keys ??= seat().create_virtual_device(Clutter.InputDeviceType.KEYBOARD_DEVICE);
    for (const m of modifiers)
        keys.notify_keyval(now(), m, Clutter.KeyState.PRESSED);
    keys.notify_keyval(now(), keyval, Clutter.KeyState.PRESSED);
    await sleep(30);
    keys.notify_keyval(now(), keyval, Clutter.KeyState.RELEASED);
    for (const m of [...modifiers].reverse())
        keys.notify_keyval(now(), m, Clutter.KeyState.RELEASED);
}

const type = text => Main.inputMethod.commit(text);
function readFile(path) {
    try {
        return new TextDecoder().decode(GLib.file_get_contents(path)[1]);
    } catch {
        return '';
    }
}
// what probe.py logged: the bytes the terminal sent it, as a string
function sent(log) {
    const lines = readFile(log).split('\n').slice(1).filter(l => l);
    return lines.map(l => l.match(/../g).map(h => String.fromCharCode(parseInt(h, 16))).join('')).join('');
}
const count = (text, part) => text.split(part).length - 1;
const show = text => JSON.stringify(text).replace(/\\u001b/g, 'ESC');
function clipboard() {
    return new Promise(resolve => St.Clipboard.get_default().get_text(St.ClipboardType.CLIPBOARD,
        (_c, text) => resolve(text)));
}
async function shot(name) {
    const shooter = new Shell.Screenshot();
    const file = Gio.File.new_for_path(`${DIR}/shots/${name}.png`);
    const stream = file.replace(null, false, Gio.FileCreateFlags.NONE, null);
    await new Promise(resolve => shooter.screenshot(false, stream, (o, res) => {
        try {
            o.screenshot_finish(res);
        } catch (e) {
            say('INFO', 'shot failed', name, e.message);
        }
        stream.close(null);
        resolve();
    }));
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

    // the text cursor's cell as mutter has it (the window unslid)
    let cursor = null, cursorMoves = 0;
    Main.inputMethod.connect('cursor-location-changed', (_im, rect) => {
        if (Number.isFinite(rect.get_y()) && Number.isFinite(rect.get_height())) {
            cursor = {x: rect.get_x(), y: rect.get_y(), width: rect.get_width(), height: rect.get_height()};
            cursorMoves++;
        }
    });

    const launcher = new Gio.SubprocessLauncher({flags: Gio.SubprocessFlags.NONE});
    launcher.setenv('GDK_DEBUG', 'no-portals', true);
    launcher.setenv('GSK_RENDERER', 'cairo', true);
    launcher.setenv('GTK_A11Y', 'none', true);
    launcher.set_cwd(GLib.get_home_dir());
    const proc = launcher.spawnv([readFile(`${DIR}/binary`).trim()]);
    const tracker = Shell.WindowTracker.get_default();
    const terminal = () => global.display.list_all_windows().find(w =>
        w.get_window_type() === Meta.WindowType.NORMAL && tracker.get_window_app(w)?.get_id() === 'vibeos.Terminal.desktop');
    await until(() => terminal()?.get_compositor_private()?.mapped, 120000);
    const win = terminal();
    Main.overview.hide();
    await until(() => !Main.overview.visible, 10000);
    global.stage.set_key_focus(null);
    win?.activate(global.get_current_time());
    await until(() => global.display.focus_window === win && terminalFocused(), 15000);
    check('the Terminal is up, its terminal focused', !!win && global.display.focus_window === win &&
        terminalFocused(), global.display.list_all_windows().map(w => `"${w.title}"`).join(' '));
    check('it slides under the keyboard (X-Vibeos-Keyboard=slide)', kb.keyboardSlides(win));
    win.maximize();
    const tall = () => win.get_frame_rect().height > Main.layoutManager.primaryMonitor.height / 2;
    await until(() => win.maximized_vertically && tall(), 10000);
    await sleep(1500);
    check('it is maximized', win.maximized_vertically && tall(),
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
    const midX = monitor.x + monitor.width / 2;
    const top = () => monitor.y + monitor.height - keyboard().height;
    const settled = async () => {
        await until(() => !actor.get_transition('translation-y'), 5000);
        await sleep(300);
    };
    const state = () => `open=${!!Main.keyboard.visible} slid=${actor.translation_y.toFixed(1)} ` +
        `frame=${frameText(win.get_frame_rect())} cursor=${cursor ? `${cursor.x.toFixed(0)},` +
        `${cursor.y.toFixed(0)} ${cursor.width.toFixed(0)}x${cursor.height.toFixed(0)}` : '-'} resized=${resized}`;
    const closeKeyboard = async () => {
        if (Main.keyboard.visible)
            kb.dismissKeyboard();
        await until(() => !Main.keyboard.visible && actor.translation_y === 0, 5000);
        await settled();
    };
    say('INFO', 'before', state());
    await shot('0-start');

    // A
    await tap(midX, frame0.y + frame0.height / 3);
    await until(() => Main.keyboard.visible && drawn(), 5000);
    await settled();
    check('A: a tap on the terminal opens the keyboard', Main.keyboard.visible && drawn(), state());
    check('A: the window keeps its size', sameFrame() && resized === 0, state());
    type('seq 300\r');
    await until(() => actor.translation_y < 0, 10000);
    await settled();
    check('A: output to the bottom slides it up, the cursor\'s line above the keys',
        actor.translation_y < 0 && cursor.y + cursor.height + actor.translation_y <= top() + 1, state());
    check('A: still its size', sameFrame() && resized === 0, state());
    await shot('1-keyboard');
    await closeKeyboard();
    check('A: the keyboard closed, back down', !Main.keyboard.visible && actor.translation_y === 0, state());

    // the grid: probe.py parks the cursor at row 10, column 5
    let grid = null;
    const runProbe = async (mode, log) => {
        const moves = cursorMoves;
        type(`python3 ${DIR}/probe.py ${mode} ${log}\r`);
        await until(() => readFile(log).startsWith('READY') && cursorMoves > moves, 15000);
        await sleep(300);
        const [, rows, columns] = readFile(log).split('\n')[0].split(' ').map(Number);
        grid = {cw: cursor.width, ch: cursor.height, x: cursor.x - 4 * cursor.width, y: cursor.y - 9 * cursor.height,
            rows, columns};
        return readFile(log).startsWith('READY');
    };
    const cellX = col => grid.x + (col + 0.5) * grid.cw;
    const cellY = row => grid.y + (row + 0.5) * grid.ch;
    const quit = async log => {
        type('q');
        await until(() => readFile(log).includes('71'), 5000);
        await sleep(500);
    };

    // B
    const altLog = `${DIR}/alt.log`;
    check('B: the probe is up on the alternate screen', await runProbe('alt', altLog), readFile(altLog));
    say('INFO', 'grid', JSON.stringify(grid));
    await swipe2(midX, cellY(25), -5.5 * grid.ch, 400);
    await until(() => count(sent(altLog), '\x1b[B') >= 5, 3000);
    await sleep(300);
    let s = sent(altLog);
    check('B: two fingers up 5½ rows: 5 Down keys', count(s, '\x1b[B') === 5 && count(s, '\x1b[A') === 0, show(s));
    await swipe2(midX, cellY(15), 3.5 * grid.ch, 300);
    await sleep(500);
    s = sent(altLog);
    check('B: down 3½ rows: 3 Up keys', count(s, '\x1b[B') === 5 && count(s, '\x1b[A') === 3, show(s));
    await swipe1(midX, cellY(25), -(5.5 * grid.ch + SLOP), 400);
    await sleep(500);
    s = sent(altLog);
    check('B: one finger up 5½ rows past the slop: 5 more Down keys',
        count(s, '\x1b[B') === 10 && count(s, '\x1b[A') === 3, show(s));
    await quit(altLog);
    const appLog = `${DIR}/altapp.log`;
    check('B: the probe with application cursor keys', await runProbe('altapp', appLog), readFile(appLog));
    await swipe2(midX, cellY(25), -2.5 * grid.ch, 300);
    await sleep(500);
    s = sent(appLog);
    check('B: DECCKM: ESC O B', count(s, '\x1bOB') === 2 && !s.includes('\x1b[B'), show(s));

    // C
    await swipe2(midX, cellY(25), -3 * grid.ch, 50, 0);
    await sleep(2500);
    s = sent(appLog);
    check('C: a fling goes on after the fingers lift', count(s, '\x1bOB') - 2 > 4, show(s));
    await quit(appLog);

    // D
    const mouseLog = `${DIR}/mouse.log`;
    check('D: the probe tracking the mouse', await runProbe('mouse', mouseLog), readFile(mouseLog));
    const centreCol = Math.floor((midX - grid.x) / grid.cw);
    await swipe2(midX, cellY(20) - grid.ch / 2 + 1, -4.5 * grid.ch, 400);
    await sleep(500);
    s = sent(mouseLog);
    // the reports' cell: where the fingers' centre is at each step, a row up each
    const wheels = s.match(/\x1b\[<65;\d+;\d+M/g) ?? [];
    check('D: 4 wheel-down reports (SGR)', wheels.length === 4 && wheels.every(w => w.includes(`;${centreCol + 1};`)),
        show(s));
    await swipe1(midX, cellY(20) - grid.ch / 2 + 1, -(2.5 * grid.ch + SLOP), 300);
    await sleep(500);
    s = sent(mouseLog);
    check('D: one finger up 2½ rows: 2 more', (s.match(/\x1b\[<65;\d+;\d+M/g) ?? []).length === 6, show(s));
    await tap(cellX(7), cellY(3));
    await sleep(500);
    s = sent(mouseLog);
    check('D: a tap is a click at its cell', s.includes('\x1b[<0;8;4M\x1b[<0;8;4m'), show(s));
    await closeKeyboard();
    await quit(mouseLog);
    const legacyLog = `${DIR}/legacy.log`;
    check('D: the probe tracking the mouse, without SGR', await runProbe('mouse-legacy', legacyLog));
    await swipe2(midX, cellY(20) - grid.ch / 2 + 1, 1.5 * grid.ch, 300);
    await sleep(500);
    s = sent(legacyLog);
    check('D: a wheel-up report in the original encoding', s.startsWith(`\x1b[M${String.fromCharCode(32 + 64)}`) &&
        count(s, '\x1b[M') === 1, show(s));
    await quit(legacyLog);
    await closeKeyboard();

    // E: lines L0001 to L0300, the prompt under them
    type('clear; seq -f L%04g 300\r');
    await sleep(2000);
    const promptRow = Math.round((cursor.y - grid.y) / grid.ch);
    const lineAt = row => 300 - (promptRow - 1 - row);
    say('INFO', 'the prompt on row', promptRow, state());
    const row = 12;
    await longPress(cellX(1), cellY(row), 900);
    await sleep(300);
    await shot('2-selected');
    await keyCombo([Clutter.KEY_Control_L, Clutter.KEY_Shift_L], Clutter.KEY_C);
    await sleep(300);
    let copied = await clipboard();
    const name = n => `L${String(n).padStart(4, '0')}`;
    check('E: a long press selects the word; Ctrl+Shift+C copies it', copied === name(lineAt(row)),
        `${show(copied)} expected ${name(lineAt(row))}`);

    // F
    await swipe2(midX, cellY(10), 10 * grid.ch, 500);
    await sleep(500);
    await longPress(cellX(1), cellY(row), 900);
    await sleep(300);
    await keyCombo([Clutter.KEY_Control_L, Clutter.KEY_Shift_L], Clutter.KEY_C);
    await sleep(300);
    copied = await clipboard();
    check('F: two fingers down 10 rows: the history 10 lines back', copied === name(lineAt(row) - 10),
        `${show(copied)} expected ${name(lineAt(row) - 10)}`);
    await swipe1(midX, cellY(10), 10 * grid.ch + SLOP, 500);
    await sleep(500);
    await longPress(cellX(1), cellY(row), 900);
    await sleep(300);
    await keyCombo([Clutter.KEY_Control_L, Clutter.KEY_Shift_L], Clutter.KEY_C);
    await sleep(300);
    copied = await clipboard();
    check('F: one finger down 10 rows past the slop: 10 more', copied === name(lineAt(row) - 20),
        `${show(copied)} expected ${name(lineAt(row) - 20)}`);

    // G: the end handle (at the word's end, under its row) 2 rows down
    await longPress(cellX(1), cellY(row), 900);
    await sleep(300);
    const handle = {x: grid.x + 5 * grid.cw + 11, y: grid.y + (row + 1) * grid.ch + 11};
    await longPress(handle.x, handle.y, 100, {x: handle.x, y: handle.y + 2 * grid.ch});
    await sleep(300);
    await shot('3-handle');
    await keyCombo([Clutter.KEY_Control_L, Clutter.KEY_Shift_L], Clutter.KEY_C);
    await sleep(300);
    copied = await clipboard();
    const first = lineAt(row) - 20;
    const want = [name(first), name(first + 1), name(first + 2)].join('\n');
    check('G: the end handle 2 rows down: three lines', copied === want, `${show(copied)} expected ${show(want)}`);

    // H
    await longPress(cellX(1), cellY(row), 900);
    await sleep(300);
    await tap(cellX(20), cellY(row + 5));
    await sleep(500);
    St.Clipboard.get_default().set_text(St.ClipboardType.CLIPBOARD, 'unchanged');
    await keyCombo([Clutter.KEY_Control_L, Clutter.KEY_Shift_L], Clutter.KEY_C);
    await sleep(300);
    copied = await clipboard();
    check('H: a tap ends the selection (nothing left to copy)', copied === 'unchanged', show(copied));
    await closeKeyboard();
    St.Clipboard.get_default().set_text(St.ClipboardType.CLIPBOARD, `echo pasted-ok > ${DIR}/pasted`);
    await sleep(200);
    await keyCombo([Clutter.KEY_Control_L, Clutter.KEY_Shift_L], Clutter.KEY_V);
    await sleep(500);
    type('\r');
    await until(() => readFile(`${DIR}/pasted`).trim() === 'pasted-ok', 5000);
    check('H: Ctrl+Shift+V pastes into the shell', readFile(`${DIR}/pasted`).trim() === 'pasted-ok',
        show(readFile(`${DIR}/pasted`)));
    // the view was left 10 rows back in the history (F): typing took it back to the end, the new prompt on the window
    const onWindow = () => cursor.x < grid.x + 20 * grid.cw && cursor.y >= grid.y &&
        cursor.y + cursor.height <= frame0.y + frame0.height;
    await until(onWindow, 5000);
    check('H: typing goes back from the history to the prompt', onWindow(), state());
    check('the window never changed its size', sameFrame() && resized === 0, state());
    await shot('4-end');

    proc.force_exit();
    touchFocus.destroy();
    swap.destroy();
    say(failures ? `FAILED ${failures}` : 'ALL PASSED');
    if (failures)
        throw new Error(`${failures} checks failed`);
}
