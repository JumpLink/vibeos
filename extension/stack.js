// One window stack, as on Android: every app on the one workspace, maximized and held in place, the launcher under
// them all. The mobile shell gives each window a workspace of its own instead, and removes it the moment the window
// closes: with a window still on its list, so mutter aborts the shell (workspace.c assert_workspace_empty) whenever
// an app's last window goes. With dynamic workspaces off the shell keeps out of it, and one workspace is all there is.
import Gio from 'gi://Gio';
import GLib from 'gi://GLib';
import Meta from 'gi://Meta';

import * as Main from 'resource:///org/gnome/shell/ui/main.js';

import {TABLET} from './units.js';

// the keys this changes, saved as the user had them (null: the default)
const KEYS = [
    ['org.gnome.mutter', 'dynamic-workspaces', 'b', false],
    ['org.gnome.desktop.wm.preferences', 'num-workspaces', 'i', 1],
];

export class SingleStack {
    constructor(home) {
        this._home = home;
        this._saved = [];
        // dynamic workspaces off first: with them on, a window leaving its workspace is what removes it
        this._set(...KEYS[0]);
        // mutter's own prefs take the change from the same signal as the shell's tracker, in no set order: once
        // both have it, the stack is put together
        this._idleId = GLib.idle_add(GLib.PRIORITY_DEFAULT, () => {
            this._idleId = 0;
            this._gather();
            return GLib.SOURCE_REMOVE;
        });
        global.display.connectObject('window-created', (_d, win) => this._hold(win), this);
    }

    _set(schema, key, type, value) {
        const settings = new Gio.Settings({schema_id: schema});
        this._saved.push({settings, key, user: settings.get_user_value(key)});
        settings.set_value(key, new GLib.Variant(type, value));
    }

    // every window to the first workspace, then the others go: mutter has nothing left to relocate
    _gather() {
        Main.wm.workspaceTracker?._redoLayout?.();
        const first = global.workspace_manager.get_workspace_by_index(0);
        for (const actor of global.get_window_actors()) {
            const win = actor.meta_window;
            if (!win.on_all_workspaces && !win.get_transient_for() && win.get_workspace() !== first)
                win.change_workspace(first);
        }
        if (!first.active)
            first.activate(global.display.get_current_time_roundtrip());
        this._set(...KEYS[1]);
        for (const actor of global.get_window_actors())
            this._hold(actor.meta_window);
    }

    // full screen and fixed: no dragging an app off its place by its header bar. The mobile shell's tracker
    // maximizes a window once, as it comes; on a phone's screen no app's window is smaller than that anyway. A
    // tablet's screen is bigger than many an app's window wants to be, and one that let go of its maximized state
    // stayed a window in the middle of the screen (Console, 11-19): there it is held maximized as on a stock shell.
    _hold(win) {
        if (this._home.isHome(win) || win.is_override_redirect())
            return;
        const tracker = Main.wm.workspaceTracker;
        if (tracker?._maximizeInSingleWindowWorkspace)
            tracker._maximizeInSingleWindowWorkspace(win);
        if (TABLET || !tracker?._maximizeInSingleWindowWorkspace)
            this._maximize(win);
        if (win.get_window_type() === Meta.WindowType.NORMAL && !win.get_transient_for())
            win.set_can_grab?.(false);
    }

    // A stock shell maximizes nothing by itself (the mobile shell's tracker above does): an app's window is maximized
    // as it comes, and again whenever it lets go (a double click on its header bar)
    _maximize(win) {
        if (win.get_window_type() !== Meta.WindowType.NORMAL || win.get_transient_for())
            return;
        // Not while the keyboard has it: with the keyboard up, the shell unmaximizes the focused window and fits it
        // from the top down to the keys (keyboard.js _animateWindow, which marks it __keyboardMoved), and maximizes it
        // again as the keyboard goes. Held maximized through that, the window stayed under the keys, and so did the
        // dialog centred in it: Settings' "Add a printer" and Wi-Fi password fields typed blind (owner, 11-21).
        const maximize = () => {
            if (!win.is_maximized() && !win.fullscreen && win.can_maximize() && !win.__keyboardMoved)
                win.maximize();
        };
        maximize();
        // not from inside mutter's own change of the state: once it is done
        let laterId = 0;
        const again = () => {
            if (!laterId) {
                laterId = global.compositor.get_laters().add(Meta.LaterType.BEFORE_REDRAW, () => {
                    laterId = 0;
                    maximize();
                    return GLib.SOURCE_REMOVE;
                });
            }
        };
        win.connectObject('notify::maximized-horizontally', again, 'notify::maximized-vertically', again,
            'unmanaged', () => {
                if (laterId)
                    global.compositor.get_laters().remove(laterId);
                laterId = 0;
                win.disconnectObject(this);
            }, this);
    }

    destroy() {
        if (this._idleId)
            GLib.source_remove(this._idleId);
        global.display.disconnectObject(this);
        for (const {settings, key, user} of this._saved) {
            if (user)
                settings.set_value(key, user);
            else
                settings.reset(key);
        }
        for (const actor of global.get_window_actors()) {
            actor.meta_window.disconnectObject(this);
            actor.meta_window.set_can_grab?.(true);
        }
    }
}
