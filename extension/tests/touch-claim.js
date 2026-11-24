// A system swipe's touch, taken from the app under it (claim.js), in a headless GNOME Shell of the test's own
// (touch-claim.sh runs it as the shell's automation script): a fullscreen app that says what it is given of every
// touch (touch-claim-client.js), and a swipe made as navigation.js makes its own (a Clutter.PanGesture on the stage,
// in the capture phase, for touches that start in the bottom strip), whose recognize takes a TouchClaim and whose end
// and cancel let it go. Touches come from a virtual touchscreen of this shell's own.
//
//   A  the swipe up for home: the app has the touch's begin, then its cancel as the swipe is recognised, and nothing
//      after it: no move, and no lift (the release the launcher or the app went on to act on). The swipe has every
//      move after it and its end, and the claim is gone with it.
//   B  the quick switch along the bar (sideways, from the strip): the same; the app under it does not page.
//   C  a tap, and a drag, in the middle of the app, after those: the app has them, begin to end, and no cancel.
//   D  a swipe cancelled while the finger is still down (the claim let go then): the rest of that touch, its lift
//      included, still reaches no app.
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

// the client: its lines kept
const lines = [];
function spawnClient() {
    const launcher = new Gio.SubprocessLauncher({
        flags: Gio.SubprocessFlags.STDIN_PIPE | Gio.SubprocessFlags.STDOUT_PIPE,
    });
    // no portal answers on the test's bus (GTK waits for one), and the software renderer will do
    launcher.setenv('GDK_DEBUG', 'no-portals', true);
    launcher.setenv('GSK_RENDERER', 'cairo', true);
    launcher.setenv('GTK_A11Y', 'none', true);
    const proc = launcher.spawnv(['gjs', '-m', `${DIR}/touch-claim-client.js`]);
    const out = new Gio.DataInputStream({base_stream: proc.get_stdout_pipe()});
    const read = () => out.read_line_async(GLib.PRIORITY_DEFAULT, null, (stream, res) => {
        const [line] = stream.read_line_finish_utf8(res);
        if (line === null)
            return;
        lines.push(line);
        read();
    });
    read();
    return {proc, quit: () => proc.get_stdin_pipe().write_all(new TextEncoder().encode('quit\n'), null)};
}
// what the client was given of the touches since line n: begin, update, end, cancel, in order
const given = n => lines.slice(n).filter(l => l.startsWith('client touch ')).map(l => l.slice(13));
const describe = list => list.filter((t, i) => t !== 'update' || list[i - 1] !== 'update').join(' ') || 'nothing';

let device = null;
const now = () => GLib.get_monotonic_time();
function touchscreen() {
    device ??= global.stage.context.get_backend().get_default_seat()
        .create_virtual_device(Clutter.InputDeviceType.TOUCHSCREEN_DEVICE);
    return device;
}
const down = (slot, x, y) => touchscreen().notify_touch_down(now(), slot, x, y);
const lift = slot => touchscreen().notify_touch_up(now(), slot);
async function move(slot, [x0, y0], [x1, y1], ms) {
    const steps = Math.max(2, Math.round(ms / 16));
    for (let i = 1; i <= steps; i++) {
        await sleep(16);
        touchscreen().notify_touch_motion(now(), slot, x0 + (x1 - x0) * i / steps, y0 + (y1 - y0) * i / steps);
    }
}

export async function run() {
    await sleep(1000);
    const {TouchClaim} = await import(`file://${DIR}/ext/claim.js`);
    // the mobile shell's own swipes off, as navigation.js turns them off: its swipe up to its overview, its home bar,
    // its overview and workspace swipes
    if (Main.overview._singleFingerOverviewEdgeDrag)
        Main.overview._singleFingerOverviewEdgeDrag.enabled = false;
    for (const name of ['_singleFingerOverviewGesture', '_threeFingerOverviewGesture',
        '_singleFingerWorkspacesGesture', '_threeFingerWorkspacesGesture']) {
        if (Main.overview[name])
            Main.overview[name].enabled = false;
    }
    Main.layoutManager.bottomPanelBox?.hide();

    const client = spawnClient();
    const window = () => global.display.list_all_windows().find(w => w.title === 'touch-claim');
    await until(() => window()?.get_compositor_private()?.mapped, 120000);
    Main.overview.hide();
    await until(() => !Main.overview.visible, 10000);
    window()?.activate(global.get_current_time());
    await until(() => global.display.focus_window === window(), 5000);
    const monitor = Main.layoutManager.primaryMonitor;
    await until(() => {
        const r = window()?.get_frame_rect();
        return r && r.width === monitor.width && r.height === monitor.height;
    }, 10000);
    const r = window()?.get_frame_rect();
    check('the app is up, fullscreen, focused', global.display.focus_window === window() &&
        r.width === monitor.width && r.height === monitor.height, `${r?.width}x${r?.height}`);
    await sleep(500);

    // the swipe, as navigation.js's: touches from the bottom strip, recognised past the touch slop
    const {x: mx, y: my, width: W, height: H} = monitor;
    const STRIP = 100;
    const claim = new TouchClaim();
    const swipe = new Clutter.PanGesture({name: 'test swipe up', begin_threshold: 34, max_n_points: 1});
    let movesClaimed = 0, ends = 0, cancels = 0;
    swipe.connect('should-handle-sequence', (_g, event) => event.get_coords()[1] >= my + H - STRIP);
    swipe.connect('recognize', () => claim.take());
    swipe.connect('pan-update', () => {
        if (claim.held)
            movesClaimed++;
    });
    swipe.connect('end', () => {
        ends++;
        claim.release();
    });
    swipe.connect('cancel', () => {
        cancels++;
        claim.release();
    });
    swipe.connect('notify::state', () => {
        if (swipe.state !== Clutter.GestureState.RECOGNIZING)
            claim.release();
    });
    global.stage.add_action_full('test-swipe-up', Clutter.EventPhase.CAPTURE, swipe);

    // a whole stroke: down, moves, up, then time for the client to say what it had
    async function stroke(from, to, ms) {
        const n = lines.length;
        down(0, ...from);
        await move(0, from, to, ms);
        await sleep(50);
        lift(0);
        await sleep(800);
        return given(n);
    }
    // the client's touch after its cancel: nothing
    const afterCancel = list => list.slice(list.indexOf('cancel') + 1);

    // the virtual touchscreen's first touch, which comes as the device is being added, reaches no client
    down(0, mx + W / 2, my + H / 2);
    await sleep(60);
    lift(0);
    await until(() => given(0).includes('end'), 5000);

    // A: up from the strip
    movesClaimed = ends = 0;
    let had = await stroke([mx + W / 2, my + H - 40], [mx + W / 2, my + H - 900], 300);
    check('A: the app had the touch\'s begin, then its cancel', had[0] === 'begin' && had.includes('cancel'),
        describe(had));
    check('A: and nothing after it, no lift', had.includes('cancel') && afterCancel(had).length === 0,
        describe(had));
    check('A: the swipe had the moves after it, and its end', movesClaimed > 3 && ends === 1,
        `${movesClaimed} moves, ${ends} ends`);
    check('A: the claim gone with it', !claim.held && !global.stage.get_grab_actor());

    // B: along the bar
    movesClaimed = ends = 0;
    had = await stroke([mx + W * 0.15, my + H - 40], [mx + W * 0.9, my + H - 70], 300);
    check('B: the quick switch along the bar: the app had begin, cancel', had[0] === 'begin' &&
        had.includes('cancel'), describe(had));
    check('B: and nothing after it, no lift', had.includes('cancel') && afterCancel(had).length === 0,
        describe(had));
    check('B: the swipe had its moves and its end', movesClaimed > 3 && ends === 1,
        `${movesClaimed} moves, ${ends} ends`);

    // C: the app's own touches, after those
    ends = 0;
    let n = lines.length;
    down(0, mx + W / 2, my + H / 2);
    await sleep(60);
    lift(0);
    await sleep(800);
    had = given(n);
    check('C: a tap in the app: the app has it, begin to end', had[0] === 'begin' && had.at(-1) === 'end' &&
        !had.includes('cancel'), describe(had));
    had = await stroke([mx + W / 2, my + H / 2], [mx + W / 2, my + H / 4], 250);
    check('C: a drag in the app: the app has it, begin to end', had[0] === 'begin' && had.includes('update') &&
        had.at(-1) === 'end' && !had.includes('cancel'), describe(had));
    check('C: the swipe took neither', ends === 0, `${ends} ends`);

    // D: cancelled with the finger down
    cancels = 0;
    n = lines.length;
    const from = [mx + W / 2, my + H - 40], mid = [mx + W / 2, my + H - 500];
    down(0, ...from);
    await move(0, from, mid, 200);
    await until(() => claim.held, 2000);
    const claimed = claim.held;
    swipe.cancel();
    await sleep(100);
    const released = !claim.held && !global.stage.get_grab_actor();
    await move(0, mid, [mx + W / 2, my + H / 3], 200);
    lift(0);
    await sleep(800);
    had = given(n);
    check('D: claimed, then let go with the finger still down', claimed && released && cancels === 1,
        `claimed ${claimed}, released ${released}, ${cancels} cancels`);
    check('D: the rest of the touch reached no app, its lift neither', had[0] === 'begin' &&
        had.includes('cancel') && afterCancel(had).length === 0, describe(had));

    global.stage.remove_action(swipe);
    claim.release();
    client.quit();
    say(failures ? `FAILED ${failures}` : 'ALL PASSED');
    if (failures)
        throw new Error(`${failures} checks failed`);
}
