// The lock screen's wallpaper as the home shows it at rest: the launcher's own picture of it (vibeos-launcher
// --rest-picture FILE W H DENSITY OUT: a parallax set's layers where the launcher puts them, and the home's zoom), so
// that unlocking into the home moves nothing. A set's flat picture (its wallpaper.jpg) is the set at rest on a phone's
// screen only: on the Xiaomi Pad 5 (1600x2560) the lock screen showed the phone's middle 1.36 times larger than the
// home, 2.15 times turned, and any still picture 1.10 times smaller than the home's zoom (2026-11-25).
//
// Made once for each picture, screen and launcher, in a process of its own (a second of CPU or so), and kept in the
// cache: $XDG_CACHE_HOME/vibeos-shell/lock-wallpaper/WxH.jpg, beside WxH.jpg.key, what it was made from (the picture's
// path and time, its set's parallax.json's time, the launcher's, the screen and the density). A launcher that takes
// longer than MAKE_TIMEOUT_MS is ended. Until the picture is made the lock screen shows the wallpaper itself, and the
// first picture on counts as loaded after FIRST_PICTURE_MS at the latest: the boot splash waits for it
// (lockWallpaper()). Imports nothing of the shell's (tests/rest-picture.sh).
import Gio from 'gi://Gio';
import GLib from 'gi://GLib';

Gio._promisify(Gio.File.prototype, 'query_info_async');
Gio._promisify(Gio.File.prototype, 'load_contents_async');
Gio._promisify(Gio.File.prototype, 'replace_contents_bytes_async', 'replace_contents_finish');
Gio._promisify(Gio.Subprocess.prototype, 'wait_check_async');

// how long the launcher may take to make a picture (about 1 s of a desktop's CPU for the Xiaomi Pad 5's screen): one
// that takes longer is ended, and the lock screen keeps the wallpaper itself
export const MAKE_TIMEOUT_MS = 15000;
// how long the lock screen's first picture may take before it counts as loaded (the boot splash waits for it, and has
// its own 2 s too: extension.js WALLPAPER_WAIT_MS); it is put on when it is in
export const FIRST_PICTURE_MS = 2000;

// a file's modification time (s.µs), '' when there is no file
async function modified(path, cancellable) {
    try {
        const info = await Gio.File.new_for_path(path).query_info_async('time::modified,time::modified-usec',
            Gio.FileQueryInfoFlags.NONE, GLib.PRIORITY_DEFAULT, cancellable);
        return `${info.get_attribute_uint64('time::modified')}.${info.get_attribute_uint32('time::modified-usec')}`;
    } catch {
        return '';
    }
}

async function readText(file, cancellable) {
    try {
        const [bytes] = await file.load_contents_async(cancellable);
        return new TextDecoder().decode(bytes);
    } catch {
        return null;
    }
}

async function make(launcher, args, keyFile, key, cancellable, timeoutMs) {
    let late = false;
    try {
        GLib.mkdir_with_parents(keyFile.get_parent().get_path(), 0o700);
        // the key goes first: a picture made for another key is never taken for this one's
        try {
            keyFile.delete(null);
        } catch {}
        const sub = new Gio.SubprocessLauncher({flags: Gio.SubprocessFlags.STDOUT_SILENCE});
        // never a Wayland client: a launcher too old to know --rest-picture would start a second home
        sub.setenv('WAYLAND_DISPLAY', 'vibeos-rest-picture-none', true);
        sub.unsetenv('WAYLAND_SOCKET');
        const proc = sub.spawnv(['nice', '-n', '10', launcher, '--rest-picture', ...args]);
        // (g_cancellable_connect: at once when it is cancelled already)
        const id = cancellable?.connect(() => proc.force_exit()) ?? 0;
        let timeoutId = GLib.timeout_add(GLib.PRIORITY_DEFAULT, timeoutMs, () => {
            timeoutId = 0;
            late = true;
            proc.force_exit();
            return GLib.SOURCE_REMOVE;
        });
        try {
            await proc.wait_check_async(null);
        } finally {
            if (timeoutId)
                GLib.source_remove(timeoutId);
            if (id)
                cancellable.disconnect(id);
        }
        await keyFile.replace_contents_bytes_async(new GLib.Bytes(new TextEncoder().encode(key)), null, false,
            Gio.FileCreateFlags.NONE, cancellable);
        return true;
    } catch (e) {
        if (!cancellable?.is_cancelled()) {
            logError(e, `vibeos: the lock screen's picture of ${args[0]}${
                late ? `: not made in ${timeoutMs} ms, the launcher ended` : ''}`);
        }
        return false;
    }
}

// The rest picture of the wallpaper @path (a local file) for a @w x @h px screen of @density px per dp, made by
// @launcher (the launcher's path). Resolves to {uri, made}: made is true when the picture is in the cache already,
// else a promise that resolves to true once it is made, or to false when it could not be (the launcher failed, took
// longer than @timeoutMs and was ended, or @cancellable stopped it).
export async function restPicture(launcher, path, w, h, density, cancellable = null, timeoutMs = MAKE_TIMEOUT_MS) {
    const out = GLib.build_filenamev([GLib.get_user_cache_dir(), 'vibeos-shell', 'lock-wallpaper', `${w}x${h}.jpg`]);
    const keyFile = Gio.File.new_for_path(`${out}.key`);
    const set = GLib.build_filenamev([GLib.path_get_dirname(path), 'parallax.json']);
    const times = await Promise.all([path, set, launcher].map(p => modified(p, cancellable)));
    const key = `${[path, ...times, `${w}x${h}`, density].join('\n')}\n`;
    const uri = Gio.File.new_for_path(out).get_uri();
    if (await readText(keyFile, cancellable) === key && GLib.file_test(out, GLib.FileTest.EXISTS))
        return {uri, made: true};
    return {uri, made: make(launcher, [path, `${w}`, `${h}`, `${density}`, out], keyFile, key, cancellable, timeoutMs)};
}

// The lock screen's wallpaper @uri (@path: its local file, or null) on a @w x @h px screen of @density px per dp: the
// launcher's rest picture of it when that is made already, else @uri itself until it is, and then the launcher's.
// @load(uri) resolves to the picture decoded (throws when it cannot); @show(picture, rest) puts it on, @rest true for
// the launcher's (the one to keep for the shape); @fail(error) when @uri itself could not be loaded either. @turned (a
// tablet): the other shape (@h x @w) made next, so that turning the screen finds it made. After @cancellable nothing
// more is shown. Returns {first, done}: @first resolves once the first picture is on or failed, and after @firstMs at
// the latest, however long the launcher takes; @done once all of it is over (the launcher's pictures made or not).
export function lockWallpaper({launcher, path, uri, w, h, density, load, show, fail, turned = false, cancellable = null,
    firstMs = FIRST_PICTURE_MS, timeoutMs = MAKE_TIMEOUT_MS}) {
    const cancelled = () => cancellable?.is_cancelled() ?? false;
    let shown;
    const first = new Promise(resolve => (shown = resolve));
    let capId = GLib.timeout_add(GLib.PRIORITY_DEFAULT, firstMs, () => {
        capId = 0;
        shown();
        return GLib.SOURCE_REMOVE;
    });
    first.then(() => {
        if (capId)
            GLib.source_remove(capId);
        capId = 0;
    });

    const makeTurned = async () => {
        if (!turned || w === h || cancelled())
            return;
        const other = await restPicture(launcher, path, h, w, density, cancellable, timeoutMs);
        if (other.made !== true)
            await other.made;
    };
    let rest = null;
    const done = (async () => {
        rest = path ? await restPicture(launcher, path, w, h, density, cancellable, timeoutMs) : null;
        if (cancelled())
            return;
        const showRest = async () => {
            try {
                const picture = await load(rest.uri);
                if (cancelled())
                    return false;
                show(picture, true);
                shown();
                return true;
            } catch (e) {
                if (!cancelled())
                    logError(e, `vibeos: the lock screen's picture of ${uri}`);
                return false;
            }
        };
        if (rest?.made === true && await showRest()) {
            await makeTurned();
            return;
        }
        if (cancelled())
            return;
        try {
            const picture = await load(uri);
            if (cancelled())
                return;
            show(picture, false);
        } catch (e) {
            if (cancelled())
                return;
            fail(e);
        }
        shown();
        if (rest && rest.made !== true && await rest.made && !cancelled() && await showRest())
            await makeTurned();
    })().catch(e => {
        if (!cancelled())
            logError(e, `vibeos: the lock screen's wallpaper ${uri}`);
    }).finally(() => {
        shown();
        // over only once the launcher is (ended, when it was stopped)
        return rest && rest.made !== true ? rest.made : null;
    });
    return {first, done};
}
