// restpicture.js against a stand-in launcher (rest-picture.sh): the lock screen's picture of the wallpaper at rest is
// the launcher's, made once for each picture, screen, density and launcher and then taken from the cache; made again
// when any of them changes; never by a launcher that could reach the session's Wayland display; a failed, stopped or
// too slow one (ended after the timeout) leaves nothing that is taken for it later. And the lock screen's side of it
// (lockWallpaper()) with stand-ins for the keyguard's decoding and showing: the launcher's picture first when it is
// made, else the picture itself and then the launcher's; the first picture never waits for the launcher, and counts
// as loaded after the cap even when it never decodes, so the boot splash goes.
import Gio from 'gi://Gio';
import GLib from 'gi://GLib';
import System from 'system';

import {FIRST_PICTURE_MS, lockWallpaper, MAKE_TIMEOUT_MS, restPicture} from './restpicture.js';

// under emulation (VIBEOS_TEST_EMULATED: the arm64 package build under qemu-user) a process takes far longer to
// start, so the short timeouts below are stretched: the stand-in launcher is running before it is ended
const SLOW = GLib.getenv('VIBEOS_TEST_EMULATED') ? 4 : 1;

const T = System.programArgs[0];
const LAUNCHER = `${T}/launcher`;
const RUNS = `${T}/runs`;
const MODE = `${T}/mode`;
const SET = `${T}/set`;
const PICTURE = `${SET}/wallpaper.jpg`;
const CACHE = GLib.build_filenamev([GLib.get_user_cache_dir(), 'vibeos-shell', 'lock-wallpaper']);

function check(ok, what) {
    if (!ok)
        throw new Error(`failed: ${what}`);
    print(`ok - ${what}`);
}

function sleep(ms) {
    return new Promise(resolve => GLib.timeout_add(GLib.PRIORITY_DEFAULT, ms, () => {
        resolve();
        return GLib.SOURCE_REMOVE;
    }));
}

function write(path, text, mode = 0o644) {
    GLib.file_set_contents(path, text);
    GLib.chmod(path, mode);
}

function read(path) {
    try {
        return new TextDecoder().decode(GLib.file_get_contents(path)[1]);
    } catch {
        return null;
    }
}

const runs = () => (read(RUNS) ?? '').split('\n').filter(l => l);

// until @path is there (a hanging stand-in has written its pid), at most @limit ms
async function until(path, limit) {
    const end = GLib.get_monotonic_time() + limit * 1000;
    while (!GLib.file_test(path, GLib.FileTest.EXISTS) && GLib.get_monotonic_time() < end)
        await sleep(20);
}

// the stand-in launchers still running: the one that wrote its pid, and any not that far yet (its command line
// names it)
function leftover() {
    const left = [];
    const pid = read(`${T}/pid`)?.trim();
    if (pid && GLib.file_test(`/proc/${pid}`, GLib.FileTest.EXISTS))
        left.push(pid);
    const proc = Gio.File.new_for_path('/proc').enumerate_children('standard::name', Gio.FileQueryInfoFlags.NONE,
        null);
    for (let info; (info = proc.next_file(null));) {
        const name = info.get_name();
        if (/^\d+$/.test(name) && name !== pid && (read(`/proc/${name}/cmdline`) ?? '').includes(LAUNCHER))
            left.push(name);
    }
    return left;
}

// a file's time moved on by @s seconds, as a package upgrade or a new picture moves it
function touch(path, s) {
    const file = Gio.File.new_for_path(path);
    const info = file.query_info('time::modified', Gio.FileQueryInfoFlags.NONE, null);
    file.set_attribute_uint64('time::modified', info.get_attribute_uint64('time::modified') + s,
        Gio.FileQueryInfoFlags.NONE, null);
}

const ms = t0 => (GLib.get_monotonic_time() - t0) / 1000;

// made, and how many runs of the launcher it took
async function made(w, h, density, cancellable = null, timeoutMs = undefined) {
    const before = runs().length;
    const rest = await restPicture(LAUNCHER, PICTURE, w, h, density, cancellable, timeoutMs);
    const ok = rest.made === true ? true : await rest.made;
    return {rest, ok, ran: runs().length - before};
}

async function main() {
    GLib.mkdir_with_parents(SET, 0o755);
    write(PICTURE, 'a picture');
    write(`${SET}/parallax.json`, '{"layers": []}');
    write(MODE, 'make');
    // the stand-in: how it was run, then OUT made, or a failure, or a launcher that never ends
    write(LAUNCHER, `#!/bin/sh
echo "$* WAYLAND_DISPLAY=\${WAYLAND_DISPLAY-} WAYLAND_SOCKET=\${WAYLAND_SOCKET-}" >> "${RUNS}"
case $(cat "${MODE}") in fail) exit 1 ;; hang) echo $$ > "${T}/pid"; exec sleep 30 ;; esac
printf 'the rest picture of %s at %sx%s, %s\\n' "$2" "$3" "$4" "$5" > "$6"
`, 0o755);

    let r = await made(1600, 2560, 2);
    const out = `${CACHE}/1600x2560.jpg`;
    check(r.ok && r.ran === 1, 'not in the cache: the launcher makes it, once');
    check(runs()[0] === `--rest-picture ${PICTURE} 1600 2560 2 ${out} WAYLAND_DISPLAY=vibeos-rest-picture-none ` +
        'WAYLAND_SOCKET=', 'the launcher is asked for the picture, the screen and the density, and kept off Wayland');
    check(r.rest.uri === Gio.File.new_for_path(out).get_uri(), 'the picture is the cache\'s file for the shape');
    check(read(out) === `the rest picture of ${PICTURE} at 1600x2560, 2\n`, 'the launcher\'s picture is in the cache');
    const mode = Gio.File.new_for_path(CACHE).query_info('unix::mode', 0, null).get_attribute_uint32('unix::mode');
    check((mode & 0o777) === 0o700, 'the cache folder is the user\'s alone');

    r = await made(1600, 2560, 2);
    check(r.rest.made === true && r.ran === 0, 'made already: taken from the cache, the launcher not run');

    r = await made(2560, 1600, 2);
    check(r.ok && r.ran === 1 && read(`${CACHE}/2560x1600.jpg`) !== null, 'the screen turned: its own picture');
    r = await made(1600, 2560, 2);
    check(r.rest.made === true && r.ran === 0, 'turned back: the first one still from the cache');

    touch(PICTURE, 10);
    r = await made(1600, 2560, 2);
    check(r.ok && r.ran === 1, 'the picture changed: made again');
    touch(`${SET}/parallax.json`, 10);
    r = await made(1600, 2560, 2);
    check(r.ok && r.ran === 1, 'its set changed: made again');
    touch(LAUNCHER, 10);
    r = await made(1600, 2560, 2);
    check(r.ok && r.ran === 1, 'the launcher changed: made again');
    r = await made(1600, 2560, 3);
    check(r.ok && r.ran === 1, 'another density: made again');
    r = await made(1600, 2560, 3);
    check(r.rest.made === true && r.ran === 0, 'and then from the cache');

    write(MODE, 'fail');
    touch(PICTURE, 10);
    r = await made(1600, 2560, 3);
    check(!r.ok && r.ran === 1, 'the launcher fails: not made');
    check(read(`${out}.key`) === null, 'a failure leaves no key: the old picture is not taken for the new one');
    write(MODE, 'make');
    r = await made(1600, 2560, 3);
    check(r.ok && r.ran === 1, 'the next time it is made');

    write(MODE, 'hang');
    touch(PICTURE, 10);
    const cancellable = new Gio.Cancellable();
    GLib.unlink(`${T}/pid`);
    const pending = made(1600, 2560, 3, cancellable);
    await until(`${T}/pid`, 10000 * SLOW);
    const t0 = GLib.get_monotonic_time();
    cancellable.cancel();
    r = await pending;
    check(!r.ok && GLib.get_monotonic_time() - t0 < 5e6, 'stopped: the launcher is ended at once, not made');
    check(read(`${out}.key`) === null, 'stopped: no key either');
    const pid = read(`${T}/pid`)?.trim();
    check(pid && leftover().length === 0, 'stopped: no launcher left running');

    check(MAKE_TIMEOUT_MS === 15000 && FIRST_PICTURE_MS === 2000,
        'a launcher is ended after 15 s, and the first picture counts as loaded after 2 s at the latest');
    // (the timeouts short here, as everywhere below)
    touch(PICTURE, 10);
    let since = GLib.get_monotonic_time();
    GLib.unlink(`${T}/pid`);
    r = await made(1600, 2560, 3, null, 800 * SLOW);
    const late = ms(since);
    check(!r.ok && r.ran === 1 && late >= 790 * SLOW && late < Math.min(5000 * SLOW, MAKE_TIMEOUT_MS - 1000),
        `a launcher that never ends is ended after the timeout ` +
        `(${Math.round(late)} ms of ${800 * SLOW}), not made`);
    check(read(`${out}.key`) === null, 'ended: no key');
    const pid2 = read(`${T}/pid`)?.trim();
    check(pid2 && pid2 !== pid && leftover().length === 0, 'ended: no launcher left');

    // the keyguard's side
    write(MODE, 'make');
    r = await made(1600, 2560, 2);
    check(r.ok, '(the picture made for the lock screen\'s tests)');
    let k = keyguard();
    await k.done;
    check(k.ran() === 0 && k.shown.length === 1 && k.shown[0].rest && k.shown[0].uri === r.rest.uri &&
        k.firstMs <= k.shown[0].ms + 50, 'made already: the lock screen shows the launcher\'s picture, at once');

    touch(PICTURE, 10);
    k = keyguard({turned: true});
    await k.done;
    check(k.shown.length === 2 && !k.shown[0].rest && k.shown[0].text === 'a picture' && k.shown[1].rest &&
        k.firstMs <= k.shown[0].ms + 50 && k.shown[0].ms <= k.shown[1].ms,
    'not made yet: the picture itself first (the first picture), then the launcher\'s');
    check(k.ran() === 2 && runs().at(-1).startsWith(`--rest-picture ${PICTURE} 2560 1600 2 `),
        'a tablet: its turned shape made next');

    write(MODE, 'hang');
    touch(PICTURE, 10);
    GLib.unlink(`${T}/pid`);
    k = keyguard({timeoutMs: 1000 * SLOW});
    await k.first;
    check(k.shown.length === 1 && !k.shown[0].rest && k.firstMs < 1000 * SLOW,
        `the launcher never ends: the first picture does not wait for it (${Math.round(k.firstMs)} ms)`);
    await k.done;
    const pid3 = read(`${T}/pid`)?.trim();
    check(k.shown.length === 1 && k.failed.length === 0 && read(`${out}.key`) === null && pid3 &&
        leftover().length === 0 && ms(k.t0) >= 990 * SLOW && ms(k.t0) < Math.min(6000 * SLOW, MAKE_TIMEOUT_MS - 1000),
    `and it is ended after the timeout (${Math.round(ms(k.t0))} ms of ${1000 * SLOW}), nothing more shown, no key`);

    touch(PICTURE, 10);
    GLib.unlink(`${T}/pid`);
    // the cap stays under FIRST_PICTURE_MS even stretched, so that it is told from the default
    const cap = Math.min(500 * SLOW, FIRST_PICTURE_MS - 500);
    k = keyguard({timeoutMs: 5000 * SLOW, firstMs: cap, stuck: true});
    await k.first;
    check(k.shown.length === 0 && k.firstMs >= cap - 10 && k.firstMs < FIRST_PICTURE_MS,
        `the launcher never ends and the picture never decodes: loaded after the cap all the same, so the boot ` +
        `splash goes (${Math.round(k.firstMs)} ms of ${cap})`);
    k.cancellable.cancel();
    since = GLib.get_monotonic_time();
    await k.done;
    const pid4 = read(`${T}/pid`)?.trim();
    check(ms(since) < 1000 * SLOW && k.shown.length === 0 && k.failed.length === 0 && pid4 && pid4 !== pid3 &&
        leftover().length === 0,
    'then stopped: over at once, nothing shown, no launcher left');
}

// lockWallpaper() as the keyguard calls it, for PICTURE on a 1600x2560 screen of density 2; its decoding (@stuck:
// never in, until it is stopped) and showing are stand-ins that note what was put on, when (ms after the start)
function keyguard({turned = false, timeoutMs = undefined, firstMs = undefined, stuck = false} = {}) {
    const before = runs().length;
    const k = {shown: [], failed: [], t0: GLib.get_monotonic_time(), cancellable: new Gio.Cancellable(),
        ran: () => runs().length - before};
    const {first, done} = lockWallpaper({
        launcher: LAUNCHER, path: PICTURE, uri: Gio.File.new_for_path(PICTURE).get_uri(), w: 1600, h: 2560,
        density: 2, turned, cancellable: k.cancellable, timeoutMs, firstMs,
        load: uri => {
            if (stuck)
                return new Promise((resolve, reject) => k.cancellable.connect(() => reject(new Error('stopped'))));
            const text = read(Gio.File.new_for_uri(uri).get_path());
            return text === null ? Promise.reject(new Error(`no ${uri}`)) : Promise.resolve({uri, text});
        },
        show: (picture, rest) => k.shown.push({...picture, rest, ms: ms(k.t0)}),
        fail: e => k.failed.push(e),
    });
    k.first = first.then(() => (k.firstMs = ms(k.t0)));
    k.done = done.then(() => k.first);
    return k;
}

let status = 0;
const loop = GLib.MainLoop.new(null, false);
main().catch(e => {
    logError(e);
    status = 1;
}).finally(() => loop.quit());
loop.run();
System.exit(status);
