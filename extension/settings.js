// Settings, opened on a page: Neo Settings (the phone's settings app), which answers GNOME Settings' panel names;
// GNOME Settings only where Neo Settings is not installed.
import Gio from 'gi://Gio';
import GLib from 'gi://GLib';

// @panel a GNOME Settings panel name (wifi, bluetooth, notifications, ...), or null for the main page
export function openSettings(panel = null) {
    const neo = GLib.find_program_in_path('neo-settings');
    const argv = neo ? [neo, ...(panel ? ['--page', panel] : [])]
        : ['gnome-control-center', ...(panel ? [panel] : [])];
    try {
        Gio.Subprocess.new(argv, Gio.SubprocessFlags.NONE);
    } catch (e) {
        logError(e, 'gnome-phone: cannot start Settings');
    }
}
