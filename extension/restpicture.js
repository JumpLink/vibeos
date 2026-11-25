// The lock screen's wallpaper as the home shows it at rest: the launcher's own picture of it (vibeos-launcher
// --rest-picture FILE W H DENSITY OUT: a parallax set's layers where the launcher puts them, and the home's zoom), so
// that unlocking into the home moves nothing. A set's flat picture (its wallpaper.jpg) is the set at rest on a phone's
// screen only: on the Xiaomi Pad 5 (1600x2560) the lock screen showed the phone's middle 1.36 times larger than the
// home, 2.15 times turned, and any still picture 1.10 times smaller than the home's zoom (2026-11-25).
//
// Made once for each picture, screen and launcher, in a process of its own (a second of CPU or so), and kept in the
// cache: $XDG_CACHE_HOME/vibeos-shell/lock-wallpaper/WxH.jpg, beside WxH.jpg.key, what it was made from (the picture's
// path and time, its set's parallax.json's time, the launcher's, the screen and the density). Imports nothing of the
// shell's (tests/rest-picture.sh).
import Gio from 'gi://Gio';
import GLib from 'gi://GLib';

Gio._promisify(Gio.File.prototype, 'query_info_async');
Gio._promisify(Gio.File.prototype, 'load_contents_async');
Gio._promisify(Gio.File.prototype, 'replace_contents_bytes_async', 'replace_contents_finish');
Gio._promisify(Gio.Subprocess.prototype, 'wait_check_async');

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

async function make(launcher, args, keyFile, key, cancellable) {
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
        try {
            await proc.wait_check_async(null);
        } finally {
            if (id)
                cancellable.disconnect(id);
        }
        await keyFile.replace_contents_bytes_async(new GLib.Bytes(new TextEncoder().encode(key)), null, false,
            Gio.FileCreateFlags.NONE, cancellable);
        return true;
    } catch (e) {
        if (!cancellable?.is_cancelled())
            logError(e, `vibeos: the lock screen's picture of ${args[0]}`);
        return false;
    }
}

// The rest picture of the wallpaper @path (a local file) for a @w x @h px screen of @density px per dp, made by
// @launcher (the launcher's path). Resolves to {uri, made}: made is true when the picture is in the cache already,
// else a promise that resolves to true once it is made, or to false when it could not be (the launcher failed, or
// @cancellable stopped it).
export async function restPicture(launcher, path, w, h, density, cancellable = null) {
    const out = GLib.build_filenamev([GLib.get_user_cache_dir(), 'vibeos-shell', 'lock-wallpaper', `${w}x${h}.jpg`]);
    const keyFile = Gio.File.new_for_path(`${out}.key`);
    const set = GLib.build_filenamev([GLib.path_get_dirname(path), 'parallax.json']);
    const times = await Promise.all([path, set, launcher].map(p => modified(p, cancellable)));
    const key = `${[path, ...times, `${w}x${h}`, density].join('\n')}\n`;
    const uri = Gio.File.new_for_path(out).get_uri();
    if (await readText(keyFile, cancellable) === key && GLib.file_test(out, GLib.FileTest.EXISTS))
        return {uri, made: true};
    return {uri, made: make(launcher, [path, `${w}`, `${h}`, `${density}`, out], keyFile, key, cancellable)};
}
