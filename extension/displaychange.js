// The shell's "Keep these display settings?" on the phone's own panel. Mutter has the shell ask it after every display
// change made to stay (ApplyMonitorsConfig "persistent": Settings' Display page, GNOME Settings), and puts the old
// configuration back by itself when nobody answers in 20 seconds. GNOME Shell opens the question on the monitor the
// pointer is on, which with a TV plugged in can be the TV, and a TV may have nothing to touch or click with: there the
// question goes unanswered and every change (the TV made the main display, Mirror) goes back. On the built-in panel a
// finger answers it. Without a built-in panel that is on, it stays where the shell put it.
import * as Main from 'resource:///org/gnome/shell/ui/main.js';

export class DisplayChangeOnPanel {
    constructor() {
        // the shell's own handler (windowManager.js, connected at startup) has opened the dialog by the time this runs
        global.window_manager.connectObject('confirm-display-change', () => this._toPanel(), this);
    }

    _toPanel() {
        const index = builtinMonitorIndex();
        if (index < 0)
            return;
        // windowManager.js's DisplayChangeDialog is not exported: a ModalDialog, which keeps to the monitor its
        // MonitorConstraint names (set from the pointer as it opens)
        const dialogs = Main.layoutManager.modalDialogGroup.get_children()
            .filter(actor => actor.constructor.name === 'DisplayChangeDialog' && actor._monitorConstraint);
        if (!dialogs.length) {
            log('vibeos: no "Keep these display settings?" dialog found to show on the built-in panel');
            return;
        }
        for (const dialog of dialogs)
            dialog._monitorConstraint.index = index;
    }

    destroy() {
        global.window_manager.disconnectObject(this);
    }
}

// the logical monitor (the shell's monitor index) the built-in panel is in, -1 while it is off or there is none
function builtinMonitorIndex() {
    const manager = global.backend.get_monitor_manager();
    const panel = manager.get_monitors().find(monitor => monitor.is_builtin() && monitor.is_active());
    return panel ? manager.get_monitor_for_connector(panel.get_connector()) : -1;
}
