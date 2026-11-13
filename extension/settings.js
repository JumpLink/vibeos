// Settings, opened on a page: Neo Settings (the phone's settings app), which answers GNOME Settings' panel names;
// GNOME Settings only where Neo Settings is not installed.
import Gio from 'gi://Gio';
import GLib from 'gi://GLib';
import Shell from 'gi://Shell';

// @panel a GNOME Settings panel name (wifi, bluetooth, notifications, ...), or null for the main page
export function openSettings(panel = null) {
    const neo = GLib.find_program_in_path('neo-settings');
    const argv = neo ? [neo, ...(panel ? ['--page', panel] : [])]
        : ['gnome-control-center', ...(panel ? [panel] : [])];
    spawn(argv);
}

// "App info": the app's own page (Neo Settings --app, GNOME Settings' applications panel)
// @id the app's desktop file id
export function openAppInfo(id) {
    const neo = GLib.find_program_in_path('neo-settings');
    spawn(neo ? [neo, '--app', id] : ['gnome-control-center', 'applications', id.replace(/\.desktop$/, '')]);
}

// The Settings app these start, as the shell knows it, or null. Its window maps without focus (no activation token
// that mutter takes, from Neo Settings' own or one passed to it): whoever opens it raises it (LaunchTransition.expect).
export function settingsApp() {
    const apps = Shell.AppSystem.get_default();
    return apps.lookup_app(GLib.find_program_in_path('neo-settings') ? 'neo.Settings.desktop'
        : 'org.gnome.Settings.desktop');
}

function spawn(argv) {
    try {
        Gio.Subprocess.new(argv, Gio.SubprocessFlags.NONE);
    } catch (e) {
        logError(e, 'gnome-phone: cannot start Settings');
    }
}
