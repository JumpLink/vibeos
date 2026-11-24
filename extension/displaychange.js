// The shell's "Keep these display settings?" on the phone's own panel. Mutter has the shell ask it after every display
// change made to stay (ApplyMonitorsConfig "persistent": Settings' Display page, GNOME Settings), and puts the old
// configuration back by itself when nobody answers in 20 seconds. GNOME Shell opens the question on the monitor the
// pointer is on, which with a TV plugged in can be the TV, and a TV may have nothing to touch or click with: there the
// question goes unanswered and every change (the TV made the main display, Mirror) goes back. On the built-in panel a
// finger answers it. Without a built-in panel that is on, it stays where the shell put it.
//
// A Display size step (Settings writes vibeos.Interface display-size, then asks mutter for the panel's new scale) is
// kept without asking, as Android's Display size is: the same screens stay on at the same resolution, and the panel
// answers a finger at any scale. Any other change still asks.
import Gio from 'gi://Gio';
import GLib from 'gi://GLib';

import * as Main from 'resource:///org/gnome/shell/ui/main.js';

// how long after Settings' Display size write its display change is taken for one
const DISPLAY_SIZE_MS = 3000;

export class DisplayChangeOnPanel {
    constructor() {
        // the screens before the last change and now: the shell has the new ones by the time it is asked
        this._now = screens();
        this._before = this._now;
        Main.layoutManager.connectObject('monitors-changed', () => {
            this._before = this._now;
            this._now = screens();
        }, this);
        this._sizeAt = 0;
        const source = Gio.SettingsSchemaSource.get_default();
        if (source.lookup('vibeos.Interface', true)?.has_key('display-size')) {
            this._interface = new Gio.Settings({schema_id: 'vibeos.Interface'});
            this._interface.connectObject('changed::display-size', () => {
                this._sizeAt = GLib.get_monotonic_time() / 1000;
            }, this);
        }
        // the shell's own handler (windowManager.js, connected at startup) has opened the dialog by the time this runs
        global.window_manager.connectObject('confirm-display-change', () => this._confirm(), this);
    }

    _confirm() {
        // windowManager.js's DisplayChangeDialog is not exported: a ModalDialog, which keeps to the monitor its
        // MonitorConstraint names (set from the pointer as it opens)
        const dialogs = Main.layoutManager.modalDialogGroup.get_children()
            .filter(actor => actor.constructor.name === 'DisplayChangeDialog' && actor._monitorConstraint);
        if (!dialogs.length) {
            log('vibeos: no "Keep these display settings?" dialog found to show on the built-in panel');
            return;
        }
        const displaySize = GLib.get_monotonic_time() / 1000 - this._sizeAt < DISPLAY_SIZE_MS;
        if (displaySize && this._now === this._before && dialogs.every(dialog => dialog._onSuccess)) {
            this._sizeAt = 0;
            // the dialog's own "Keep Changes": mutter is told, the dialog closes
            for (const dialog of dialogs)
                dialog._onSuccess();
            return;
        }
        const index = builtinMonitorIndex();
        if (index < 0)
            return;
        for (const dialog of dialogs)
            dialog._monitorConstraint.index = index;
    }

    destroy() {
        Main.layoutManager.disconnectObject(this);
        this._interface?.disconnectObject(this);
        global.window_manager.disconnectObject(this);
    }
}

// the screens that are on, each with its size in its own pixels (the logical size times the scale: the same at
// any scale), and which is the main one: "DSI-2 1080x2280*"
function screens() {
    const manager = global.backend.get_monitor_manager();
    const display = global.display;
    const primary = display.get_primary_monitor();
    return manager.get_monitors().filter(monitor => monitor.is_active()).map(monitor => {
        const connector = monitor.get_connector();
        const index = manager.get_monitor_for_connector(connector);
        if (index < 0)
            return connector;
        const rect = display.get_monitor_geometry(index);
        const scale = display.get_monitor_scale(index);
        return `${connector} ${Math.round(rect.width * scale)}x${Math.round(rect.height * scale)}` +
            `${index === primary ? '*' : ''}`;
    }).sort().join(', ');
}

// the logical monitor (the shell's monitor index) the built-in panel is in, -1 while it is off or there is none
function builtinMonitorIndex() {
    const manager = global.backend.get_monitor_manager();
    const panel = manager.get_monitors().find(monitor => monitor.is_builtin() && monitor.is_active());
    return panel ? manager.get_monitor_for_connector(panel.get_connector()) : -1;
}
