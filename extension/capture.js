// Frame capture for checking animations against the Android recordings (development only).
// With ~/.config/gnome-phone/capture present, an animation can save the stage as it draws: one PNG at a time to
// /tmp/gp-cap/<name>-<ms>.png, labelled with the animation's own clock, so frames line up with a spec's timeline
// whatever the capture costs (run it with the shell's slow-down factor raised).
import Gio from 'gi://Gio';
import GLib from 'gi://GLib';
import Shell from 'gi://Shell';

Gio._promisify(Shell.Screenshot.prototype, 'screenshot_stage_to_content');
Gio._promisify(Shell.Screenshot, 'composite_to_stream');

const DIR = '/tmp/gp-cap';
let busy = false;

export function capturing() {
    return GLib.file_test(GLib.build_filenamev([GLib.get_user_config_dir(), 'gnome-phone', 'capture']),
        GLib.FileTest.EXISTS);
}

// Saves the next stage frame as <name>-<ms>.png, unless a capture is still being written
export async function capture(name, ms) {
    if (busy)
        return;
    busy = true;
    try {
        GLib.mkdir_with_parents(DIR, 0o755);
        const shooter = new Shell.Screenshot();
        const [content] = await shooter.screenshot_stage_to_content();
        const texture = content.get_texture();
        const file = Gio.File.new_for_path(`${DIR}/${name}-${String(Math.round(ms)).padStart(4, '0')}.png`);
        const stream = file.replace(null, false, Gio.FileCreateFlags.NONE, null);
        await Shell.Screenshot.composite_to_stream(texture, 0, 0, -1, -1, 1, null, 0, 0, 1, stream);
        stream.close(null);
    } catch (e) {
        logError(e, 'gnome-phone: capture');
    } finally {
        busy = false;
    }
}
