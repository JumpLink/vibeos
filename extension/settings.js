// Settings, opened on a page: VibeOS Settings (the phone's settings app), which answers GNOME Settings' panel names;
// GNOME Settings only where VibeOS Settings is not installed.
import Gio from 'gi://Gio';
import GLib from 'gi://GLib';
import Shell from 'gi://Shell';

// Told that a window of an app is on its way, to raise it as it asks for attention (the shell's launch transition
// sets it); null while the shell is off
let expectWindow = null;

export function setExpectWindow(fn) {
    expectWindow = fn;
}

// @panel a GNOME Settings panel name (wifi, bluetooth, notifications, ...), or null for the main page
export function openSettings(panel = null) {
    const vibeos = GLib.find_program_in_path('vibeos-settings');
    const argv = vibeos ? [vibeos, ...(panel ? ['--page', panel] : [])]
        : ['gnome-control-center', ...(panel ? [panel] : [])];
    expectSettings();
    spawn(argv);
}

// "App info": the app's own page (VibeOS Settings --app, GNOME Settings' applications panel)
// @id the app's desktop file id
export function openAppInfo(id) {
    const vibeos = GLib.find_program_in_path('vibeos-settings');
    expectSettings();
    spawn(vibeos ? [vibeos, '--app', id] : ['gnome-control-center', 'applications', id.replace(/\.desktop$/, '')]);
}

// Settings' window, new or already open (and then minimized under the home screen), only asks for attention when
// it is presented: without this it stayed where it was, and the button seemed to do nothing
function expectSettings() {
    const app = settingsApp();
    if (app)
        expectWindow?.(app);
}

// The Settings app these start, as the shell knows it, or null. Its window maps without focus (no activation token
// that mutter takes, from VibeOS Settings' own or one passed to it): openSettings and openAppInfo have the launch
// transition expect it, which raises it.
export function settingsApp() {
    const apps = Shell.AppSystem.get_default();
    return apps.lookup_app(GLib.find_program_in_path('vibeos-settings') ? 'vibeos.Settings.desktop'
        : 'org.gnome.Settings.desktop');
}

function spawn(argv) {
    try {
        Gio.Subprocess.new(argv, Gio.SubprocessFlags.NONE);
    } catch (e) {
        logError(e, 'vibeos: cannot start Settings');
    }
}
