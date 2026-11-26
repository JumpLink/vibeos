// The home screen: the VibeOS launcher, a Vulkan Wayland client the shell starts itself, so it knows the launcher's
// window from every other. That window is made the desktop (under every other window, on every workspace, in no
// window list) and comes back if the launcher dies.
import Gio from 'gi://Gio';
import GLib from 'gi://GLib';
import GObject from 'gi://GObject';
import Meta from 'gi://Meta';

import * as Main from 'resource:///org/gnome/shell/ui/main.js';

import {SingleStack} from './stack.js';

export const LAUNCHER = ['/usr/local/bin/vibeos-launcher'];
const RESPAWN_MS = 1000;

// mutter's suspend state of a window seen (META_WINDOW_SUSPEND_STATE_ACTIVE; the enum is private to mutter, not in its
// typelib: 1 active, 2 hidden, 3 suspended)
const SUSPEND_ACTIVE = 1;

let hasSuspendState;

// a window's suspend state, or null where this mutter has none. The property is not in Meta's typelib either, and gjs
// shows only the typelib's properties: win.suspend_state is undefined whatever the state (gjs 1.90), so it is read
// through a GValue.
function suspendState(win) {
    try {
        hasSuspendState ??= GObject.Object.find_property.call(Meta.Window, 'suspend-state') !== null;
        if (!hasSuspendState)
            return null;
        const value = new GObject.Value();
        win.get_property('suspend-state', value);
        return value.get_enum();
    } catch {
        return null;
    }
}

export class Home {
    constructor() {
        this._client = null;
        this._window = null;
        this._respawnId = 0;
        this._fitId = 0;
        this._destroyed = false;
        // the display's last user time as the phone last went home: no app comes up by itself after it until the user
        // touches something again (attention.js)
        this.homeAt = 0;

        global.display.connectObject('window-created', (_d, win) => this._windowCreated(win), this);
        // A turn of the screen: the launcher's window is asked for the turned screen's size. Left alone it kept the
        // size it had, cut to fit (800 × 800 on the tablet turned upright), and the launcher drew in that square.
        Main.layoutManager.connectObject('monitors-changed', () => {
            if (this._window)
                this._fitWindow(this._window, 'the screen changed');
        }, this);
        this._spawn();
        this._takeOverview();
        this._stack = new SingleStack(this);
    }

    // The overview was the mobile shell's home: whatever would show it goes home instead
    _takeOverview() {
        const overview = Main.overview;
        this._overview = {show: overview.show, showApps: overview.showApps, toggle: overview.toggle};
        overview.show = () => this.goHome();
        overview.showApps = () => this.goHome();
        overview.toggle = () => this.goHome();
        const hide = () => {
            if (overview.visible)
                overview.hide();
        };
        if (Main.layoutManager._startingUp)
            Main.layoutManager.connectObject('startup-complete', hide, this);
        else
            hide();
    }

    _giveOverview() {
        Object.assign(Main.overview, this._overview);
        Main.layoutManager.disconnectObject(this);
    }

    // Home: every app window on the phone's own screen goes to recents (minimized), the launcher takes the focus. A
    // window on a TV or monitor plugged into the phone (a game played there) stays where it is: Home is the phone's.
    goHome() {
        this.appsToRecents(false);
        this._window?.activate(global.get_current_time());
        this.markHome();
    }

    // Home's rule, for every way home (the Home key, the swipe up, back from an app's first screen, the swipe up of a
    // sideways app): the launcher is the desktop, under every window, so a window left unminimized on the phone's
    // screen is in front of it. skipEffect: the windows go without the minimize animation (a swipe flies its own).
    appsToRecents(skipEffect) {
        const workspace = global.workspace_manager.get_active_workspace();
        const phone = Main.layoutManager.primaryIndex;
        for (const win of workspace.list_windows()) {
            if (win === this._window || win.minimized || win.get_window_type() !== Meta.WindowType.NORMAL ||
                win.get_monitor() !== phone)
                continue;
            const actor = skipEffect ? win.get_compositor_private() : null;
            if (actor)
                Main.wm.skipNextEffect(actor);
            win.minimize();
        }
    }

    // the phone is home now, the launcher activated: every way home says so (the launcher's activation is a user
    // time too, so this comes after it)
    markHome() {
        this.homeAt = global.display.get_last_user_time();
    }

    get window() {
        return this._window;
    }

    isHome(win) {
        return win !== null && win === this._window;
    }

    _spawn() {
        // the launcher gets the shell's environment: a GPU whose Vulkan driver needs a variable (panvk's
        // PAN_I_WANT_A_BROKEN_VULKAN_DRIVER on a Mali) has it set for the session, in environment.d, by its device
        // package; the launcher picks a GPU over a software rasterizer itself
        const launcher = new Gio.SubprocessLauncher({flags: Gio.SubprocessFlags.NONE});
        try {
            this._client = Meta.WaylandClient.new_subprocess(global.context, launcher, LAUNCHER);
        } catch (e) {
            logError(e, 'vibeos: cannot start the launcher');
            return;
        }
        const proc = this._client.get_subprocess();
        proc.wait_async(null, () => {
            if (this._destroyed)
                return;
            console.warn(`vibeos: the launcher exited (${proc.get_exit_status()}), starting it again`);
            this._client = null;
            this._window = null;
            this._respawnId = GLib.timeout_add(GLib.PRIORITY_DEFAULT, RESPAWN_MS, () => {
                this._respawnId = 0;
                this._spawn();
                return GLib.SOURCE_REMOVE;
            });
        });
    }

    _windowCreated(win) {
        if (!this._client?.owns_window(win))
            return;
        this._window = win;
        win.set_type(Meta.WindowType.DESKTOP);
        win.stick();
        win.connectObject('unmanaged', () => {
            if (this._window === win)
                this._window = null;
        }, this);
        win.connectObject('shown', () => this._fitWindow(win, 'shown'),
            'notify::suspend-state', () => {
                const state = suspendState(win);
                if (state === SUSPEND_ACTIVE || state === null)
                    this._fitWindow(win, 'seen again');
            }, this);
    }

    // A turn of the screen before the launcher's first frame: the shell keeps its window at the turned screen's size,
    // but tells the launcher, when the window shows, the size it gave before the turn, and the launcher draws at that
    // size until the next turn. Asking for the screen's size once the window shows sends the launcher the right one;
    // when it already has it nothing is sent. The same when an app stops covering the launcher: mutter tells a
    // window seen again the size it last sent it, which can be from before a turn and back while it was covered (the
    // Xiaomi Pad 5's home drawn portrait on its landscape panel, 2026-11-25); and after every change of the screen.
    _fitWindow(win, why) {
        if (this._fitId)
            GLib.source_remove(this._fitId);
        this._fitId = GLib.idle_add(GLib.PRIORITY_DEFAULT_IDLE, () => {
            this._fitId = 0;
            if (win !== this._window)
                return GLib.SOURCE_REMOVE;
            const monitor = global.display.get_monitor_geometry(win.get_monitor());
            const rect = win.get_frame_rect();
            if (rect.width !== monitor.width || rect.height !== monitor.height) {
                console.log(`vibeos: home: fit (${why}): the launcher's window ${rect.width}x${rect.height}, the ` +
                    `screen ${monitor.width}x${monitor.height}`);
            }
            win.move_resize_frame(false, monitor.x, monitor.y, monitor.width, monitor.height);
            return GLib.SOURCE_REMOVE;
        });
    }

    destroy() {
        this._destroyed = true;
        this._stack.destroy();
        this._giveOverview();
        global.display.disconnectObject(this);
        this._window?.disconnectObject(this);
        if (this._respawnId)
            GLib.source_remove(this._respawnId);
        if (this._fitId)
            GLib.source_remove(this._fitId);
        this._client?.get_subprocess()?.force_exit();
        this._client = null;
    }
}
