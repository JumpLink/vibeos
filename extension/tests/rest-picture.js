// restpicture.js against a stand-in launcher (rest-picture.sh): the lock screen's picture of the wallpaper at rest is
// the launcher's, made once for each picture, screen, density and launcher and then taken from the cache; made again
// when any of them changes; never by a launcher that could reach the session's Wayland display; a failed or stopped
// one leaves nothing that is taken for it later.
import Gio from 'gi://Gio';
import GLib from 'gi://GLib';
import System from 'system';

import {restPicture} from './restpicture.js';

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

// a file's time moved on by @s seconds, as a package upgrade or a new picture moves it
function touch(path, s) {
    const file = Gio.File.new_for_path(path);
    const info = file.query_info('time::modified', Gio.FileQueryInfoFlags.NONE, null);
    file.set_attribute_uint64('time::modified', info.get_attribute_uint64('time::modified') + s,
        Gio.FileQueryInfoFlags.NONE, null);
}

// made, and how many runs of the launcher it took
async function made(w, h, density, cancellable = null) {
    const before = runs().length;
    const rest = await restPicture(LAUNCHER, PICTURE, w, h, density, cancellable);
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
    check((Gio.File.new_for_path(CACHE).query_info('unix::mode', 0, null).get_attribute_uint32('unix::mode') & 0o777) ===
        0o700, 'the cache folder is the user\'s alone');

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
    const t0 = GLib.get_monotonic_time();
    const pending = made(1600, 2560, 3, cancellable);
    await sleep(300);
    cancellable.cancel();
    r = await pending;
    check(!r.ok && GLib.get_monotonic_time() - t0 < 5e6, 'stopped: the launcher is ended at once, not made');
    check(read(`${out}.key`) === null, 'stopped: no key either');
    const pid = read(`${T}/pid`)?.trim();
    check(pid && !GLib.file_test(`/proc/${pid}`, GLib.FileTest.EXISTS), 'stopped: no launcher left running');
}

let status = 0;
const loop = GLib.MainLoop.new(null, false);
main().catch(e => {
    logError(e);
    status = 1;
}).finally(() => loop.quit());
loop.run();
System.exit(status);
