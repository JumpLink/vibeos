// One window stack, as on Android: every app on the one workspace, maximized and held in place, the launcher under
// them all. The mobile shell gives each window a workspace of its own instead, and removes it the moment the window
// closes: with a window still on its list, so mutter aborts the shell (workspace.c assert_workspace_empty) whenever
// an app's last window goes. With dynamic workspaces off the shell keeps out of it, and one workspace is all there is.
import Gio from 'gi://Gio';
import GLib from 'gi://GLib';
import Meta from 'gi://Meta';

import * as Main from 'resource:///org/gnome/shell/ui/main.js';

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

    // full screen and fixed: no dragging an app off its place by its header bar
    _hold(win) {
        if (this._home.isHome(win) || win.is_override_redirect())
            return;
        const tracker = Main.wm.workspaceTracker;
        tracker?._maximizeInSingleWindowWorkspace?.(win);
        if (win.get_window_type() === Meta.WindowType.NORMAL && !win.get_transient_for())
            win.set_can_grab?.(false);
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
        for (const actor of global.get_window_actors())
            actor.meta_window.set_can_grab?.(true);
    }
}
