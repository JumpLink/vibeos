// A window under the on-screen keyboard, in a headless GNOME Shell of the test's own (keyboard-overlay.sh runs it as
// the shell's automation script): the extension's keyboard swapped in, its touch-focus opening, and a full-height
// window of the test's own (keyboard-overlay-client.js) with a field at its top and one at its bottom. Touches come
// from a virtual touchscreen of this shell's own. The monitor is OSK_MONITOR (default 1080x2280) at the scale
// OSK_SCALE (default 1).
//
// An ordinary app:
//   A  the top field focused, the keyboard opens over the window: it neither moves nor changes size, and the client
//      is sent no new size.
//   B  the bottom field takes the focus (keyboard closed before): the window slides up until the field's line is just above
//      the keys (the margin between), and its size is the same, for the shell and for the client.
//   C  the focus moves to the top field with the keys up: the window slides back down.
//   D  the keyboard closed: the window is back at its place, and it never changed its size.
// An app whose desktop file says X-Vibeos-Keyboard=overlay:
//   E  the bottom field takes the focus and the keyboard opens: the window does not move at all, and keeps its size.
// An app whose desktop file says X-Vibeos-Keyboard=fit:
//   F  the keyboard fits the window above the keys, as it did for every app once, and the window grows back.
import Clutter from 'gi://Clutter';
import Gio from 'gi://Gio';
import GLib from 'gi://GLib';
import Meta from 'gi://Meta';
import Shell from 'gi://Shell';

import * as Main from 'resource:///org/gnome/shell/ui/main.js';

import {standInForTheMobileShell} from './keyboard-standin.js';

export const METRICS = {};
const DIR = GLib.path_get_dirname(GLib.filename_from_uri(import.meta.url)[0]);
const SCALE = Number(GLib.getenv('OSK_SCALE') ?? '1');
const MONITOR = GLib.getenv('OSK_MONITOR') ?? '1080x2280';
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

// a client: its lines kept, commands sent
function spawnClient(appId) {
    const lines = [];
    const launcher = new Gio.SubprocessLauncher({
        flags: Gio.SubprocessFlags.STDIN_PIPE | Gio.SubprocessFlags.STDOUT_PIPE,
    });
    // no portal answers on the test's bus (GTK waits for one), and the software renderer will do
    launcher.setenv('GDK_DEBUG', 'no-portals', true);
    launcher.setenv('GSK_RENDERER', 'cairo', true);
    launcher.setenv('GTK_A11Y', 'none', true);
    const proc = launcher.spawnv(['gjs', '-m', `${DIR}/keyboard-overlay-client.js`, appId]);
    const out = new Gio.DataInputStream({base_stream: proc.get_stdout_pipe()});
    const read = () => out.read_line_async(GLib.PRIORITY_DEFAULT, null, (stream, res) => {
        const [line] = stream.read_line_finish_utf8(res);
        if (line === null)
            return;
        lines.push(line);
        read();
    });
    read();
    return {
        proc, lines,
        send: cmd => proc.get_stdin_pipe().write_all(new TextEncoder().encode(`${cmd}\n`), null),
        // the window's sizes, as the client was given them
        layouts: () => lines.filter(l => l.startsWith('layout ')),
        allocs: () => lines.filter(l => l.startsWith('alloc ')),
    };
}

const keyboard = () => Main.keyboard._keyboard;
function drawn() {
    const k = keyboard();
    return !!k && Main.layoutManager.keyboardBox.visible && k.mapped && k.opacity > 0 &&
        k.translation_y < k.height - 1;
}

let touch = null;
const now = () => GLib.get_monotonic_time();
async function tap(x, y) {
    touch ??= global.stage.context.get_backend().get_default_seat()
        .create_virtual_device(Clutter.InputDeviceType.TOUCHSCREEN_DEVICE);
    touch.notify_touch_down(now(), 0, x, y);
    await sleep(60);
    touch.notify_touch_up(now(), 0);
}

// the stage as a PNG in shots/ (OVERLAY_SHOTS in keyboard-overlay.sh keeps them)
async function shot(name) {
    const shooter = new Shell.Screenshot();
    const stream = Gio.File.new_for_path(`${DIR}/shots/${name}.png`).replace(null, false, Gio.FileCreateFlags.NONE, null);
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

async function setScale() {
    Gio._promisify(Gio.DBusConnection.prototype, 'call', 'call_finish');
    if (SCALE === 1)
        return;
    const reply = await Gio.DBus.session.call('org.gnome.Mutter.DisplayConfig', '/org/gnome/Mutter/DisplayConfig',
        'org.gnome.Mutter.DisplayConfig', 'GetCurrentState', null, null, Gio.DBusCallFlags.NONE, -1, null);
    const serial = reply.get_child_value(0).get_uint32();
    const config = GLib.Variant.parse(null, `(uint32 ${serial}, uint32 1, [(0, 0, ${SCALE.toFixed(1)}, uint32 0, true, ` +
        `[('Meta-0', '${MONITOR}@60.000', @a{sv} {})])], @a{sv} {})`, null, null);
    await Gio.DBus.session.call('org.gnome.Mutter.DisplayConfig', '/org/gnome/Mutter/DisplayConfig',
        'org.gnome.Mutter.DisplayConfig', 'ApplyMonitorsConfig', config, null, Gio.DBusCallFlags.NONE, -1, null);
}

export async function run() {
    await sleep(1000);
    new Gio.Settings({schema_id: 'org.gnome.desktop.a11y.applications'}).set_boolean('screen-keyboard-enabled', true);
    new Gio.Settings({schema_id: 'vibeos.Shell'}).set_string('keyboard', 'vibeos');
    await setScale();
    await sleep(500);
    const kb = await import(`file://${DIR}/ext/keyboard/keyboard.js`);
    const {dp} = await import(`file://${DIR}/ext/units.js`);
    standInForTheMobileShell();
    const swap = new kb.KeyboardSwap();
    check('the extension\'s keyboard is in', swap.active && keyboard()?.constructor.name.includes('VibeosKeyboard'));
    const monitor = Main.layoutManager.primaryMonitor;
    say('INFO', `monitor ${monitor.width}x${monitor.height} at scale ${monitor.geometry_scale}, dp(8)=${dp(8)}`);

    // the text cursor as mutter has it (the window unslid), each time its client says
    let cursor = null, cursorEvents = 0;
    Main.inputMethod.connect('cursor-location-changed', (_im, rect) => {
        if (Number.isFinite(rect.get_y()) && Number.isFinite(rect.get_height())) {
            cursor = {y: rect.get_y(), height: rect.get_height()};
            cursorEvents++;
        }
    });

    const tracker = Shell.WindowTracker.get_default();
    const appWindow = id => global.display.list_all_windows().find(w =>
        w.get_window_type() === Meta.WindowType.NORMAL && tracker.get_window_app(w)?.get_id() === `${id}.desktop`) ??
        global.display.list_all_windows().find(w => w.get_window_type() === Meta.WindowType.NORMAL &&
        w.get_wm_class() === id);

    async function scenario(appId, label, expect) {
        const client = spawnClient(appId);
        await until(() => appWindow(appId)?.get_compositor_private()?.mapped, 60000);
        const win = appWindow(appId);
        Main.overview.hide();
        await until(() => !Main.overview.visible, 10000);
        win?.activate(global.get_current_time());
        // over the whole work area, as the phone's shell puts every window (this shell's session leaves it floating)
        win.maximize();
        const tall = () => win.get_frame_rect().height > monitor.height / 2;
        await until(() => win.maximized_vertically && tall(), 10000);
        await sleep(1500);
        check(`${label}: the window is up, maximized, ${expect === 'fit' ? 'fit' : kb.keyboardOverlays(win) ? 'overlay' : 'plain'}`,
            win.maximized_vertically && tall() && kb.keyboardOverlays(win) === (expect === 'overlay') &&
            kb.keyboardFits(win) === (expect === 'fit'), `${win.get_frame_rect().width}x${win.get_frame_rect().height}`);
        global.force_animations = true;
        const actor = win.get_compositor_private();
        const frame0 = win.get_frame_rect();
        const frameText = r => `${r.x},${r.y} ${r.width}x${r.height}`;
        const sameFrame = () => {
            const r = win.get_frame_rect();
            return r.x === frame0.x && r.y === frame0.y && r.width === frame0.width && r.height === frame0.height;
        };
        let resized = 0;
        let smallest = frame0.height;
        win.connect('size-changed', () => {
            resized++;
            smallest = Math.min(smallest, win.get_frame_rect().height);
        });
        const layouts0 = client.layouts().length, allocs0 = client.allocs().length;
        const top = () => monitor.y + monitor.height - keyboard().height;
        const settled = async () => {
            await until(() => !actor.get_transition('translation-y'), 5000);
            await sleep(400);
        };
        const state = () => `open=${!!Main.keyboard.visible} drawn=${drawn()} y=${(actor.y + actor.translation_y).toFixed(1)} ` +
            `slid=${actor.translation_y.toFixed(1)} frame=${frameText(win.get_frame_rect())} ` +
            `cursor=${cursor ? `${cursor.y.toFixed(0)}+${cursor.height.toFixed(0)}` : '-'} ` +
            `keys=${keyboard() ? top().toFixed(0) : '-'} resized=${resized} ` +
            `client-sizes=${client.layouts().length - layouts0}/${client.allocs().length - allocs0}`;
        // the size the client has, and was given
        const clientKeeps = () => client.layouts().length === layouts0 && client.allocs().length === allocs0;

        await until(() => client.lines.includes('client up'), 60000);
        client.send('bounds');
        await until(() => client.lines.filter(l => l.startsWith('bounds ')).length === 2, 10000);
        const bounds = {};
        for (const l of client.lines.filter(l => l.startsWith('bounds '))) {
            const [, name, x, y, w, h] = l.split(' ');
            bounds[name] = {x: frame0.x + Number(x) + Number(w) / 2, y: frame0.y + Number(y) + Number(h) / 2};
        }
        say('INFO', `${label}: fields`, JSON.stringify(bounds), 'before', state());
        const close = async () => {
            kb.dismissKeyboard();
            await until(() => !Main.keyboard.visible && !drawn(), 5000);
            client.send('unfocus');
            await settled();
        };
        // The field takes the focus (as a tap gives it) and the keyboard opens: a stock shell's input method has the
        // focus whenever the window has, so the touch-focus opening (oskfocus.js) sees no field take it.
        const open = async name => {
            const events = cursorEvents, n = client.lines.length;
            client.send(`focus-${name}`);
            await until(() => client.lines.slice(n).includes(`field ${name} has the focus`), 10000);
            await until(() => cursorEvents > events, 5000);
            await sleep(200);
            // (the VibeOS shell's keyboard learns of the window in front itself)
            keyboard()._setFocusWindow(win);
            Main.keyboard.open(Main.layoutManager.primaryIndex);
            await until(() => Main.keyboard.visible && drawn(), 5000);
            await settled();
            return cursorEvents - events;
        };
        const cursorBottom = () => cursor.y + cursor.height;

        // A
        let events = await open('top');
        // (a fitted window is resized as the keys come, and the stock shell's input method may close them for that)
        check(`${label} A: the top field focused, the keyboard opens`, expect === 'fit' || Main.keyboard.visible && drawn(),
            state());
        say('INFO', `${label} A: cursor events since the tap: ${events}`);
        if (expect === 'fit') {
            await shot(`${label}-A-fitted`);
            // (the window went from the top of the work area down to the keys)
            check(`${label} A: the window is fitted above the keys`,
                smallest <= top() - frame0.y + 1 && smallest < frame0.height && actor.translation_y === 0,
                `${state()} smallest height ${smallest}`);
            await close();
            await until(() => sameFrame(), 5000);
            check(`${label} D: the keyboard closed: the window grew back`, sameFrame() && actor.translation_y === 0, state());
            client.proc.force_exit();
            return;
        }
        await shot(`${label}-A-top-field`);
        check(`${label} A: the window did not move`, actor.translation_y === 0 && actor.y === frame0.y, state());
        check(`${label} A: the window keeps its size, and the client is sent none`,
            sameFrame() && resized === 0 && clientKeeps(), state());
        await close();
        check(`${label} A: closed: still in place`, actor.translation_y === 0 && sameFrame() && clientKeeps(), state());

        // B
        events = await open('bottom');
        say('INFO', `${label} B: cursor events since the tap: ${events}`);
        await shot(`${label}-B-bottom-field`);
        const covered = Math.max(0, frame0.y + frame0.height - top());
        check(`${label} B: the bottom field is under the keys`, bounds.bottom.y > top(), `field ${bounds.bottom.y} keys ${top()}`);
        if (expect === 'overlay') {
            check(`${label} E: the window does not move`, actor.translation_y === 0 && actor.y === frame0.y, state());
            check(`${label} E: the window keeps its size`, sameFrame() && resized === 0 && clientKeeps(), state());
            await close();
            check(`${label} E: closed: still in place`, actor.translation_y === 0 && sameFrame() && clientKeeps(), state());
            client.proc.force_exit();
            return;
        }
        const want = -Math.round(Math.min(covered, Math.max(0, cursorBottom() + dp(8) - top())));
        check(`${label} B: the window slid up`, actor.translation_y < 0 && Math.abs(actor.translation_y - want) <= 1,
            `${state()} expected ${want}`);
        check(`${label} B: the field's line just above the keys`,
            cursorBottom() + actor.translation_y <= top() && cursorBottom() + actor.translation_y >= top() - dp(8) - 1,
            state());
        check(`${label} B: the window keeps its size, and the client is sent none`,
            sameFrame() && resized === 0 && clientKeeps(), state());

        // C
        client.send('focus-top');
        await until(() => actor.translation_y === 0 && !actor.get_transition('translation-y'), 5000);
        await settled();
        check(`${label} C: the top field focused with the keys up: back down`,
            Main.keyboard.visible && actor.translation_y === 0 && sameFrame() && clientKeeps(), state());
        client.send('focus-bottom');
        await until(() => actor.translation_y < 0, 5000);
        await settled();
        check(`${label} C: and up again for the bottom field`, actor.translation_y < 0 &&
            Math.abs(actor.translation_y - want) <= 1, `${state()} expected ${want}`);

        // no cursor known (the client sends none): the window stays where the keys find it, and slides for the next one
        keyboard()._kbCursor = null;
        keyboard()._kbSlideSoon();
        await until(() => actor.translation_y === 0 && !actor.get_transition('translation-y'), 5000);
        await settled();
        check(`${label} C: no cursor known: back down, not moved by the whole covered part`,
            actor.translation_y === 0 && sameFrame(), state());
        client.send('focus-top');
        await sleep(300);
        client.send('focus-bottom');
        await until(() => actor.translation_y < 0, 5000);
        await settled();
        check(`${label} C: and a cursor that comes later slides it`, Math.abs(actor.translation_y - want) <= 1,
            `${state()} expected ${want}`);

        // D
        await close();
        await shot(`${label}-D-closed`);
        await until(() => actor.translation_y === 0, 5000);
        check(`${label} D: the keyboard closed: the window is back at y=${frame0.y}`,
            !Main.keyboard.visible && actor.translation_y === 0 && actor.y === frame0.y, state());
        check(`${label} D: it never changed its size`, sameFrame() && resized === 0 && clientKeeps(), state());
        client.proc.force_exit();
    }

    const only = GLib.getenv('OSK_ONLY');
    for (const [id, label, expect] of [['Plain', 'plain', 'slide'], ['Overlay', 'overlay', 'overlay'], ['Fit', 'fit', 'fit']]) {
        if (!only || only === label)
            await scenario(`test.vibeos.Keyboard${id}`, label, expect);
    }

    swap.destroy();
    say(failures ? `FAILED ${failures}` : 'ALL PASSED');
    if (failures)
        throw new Error(`${failures} checks failed`);
}
