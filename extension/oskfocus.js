// A text field that a tap gives focus opens the keyboard, as on Android (InputMethodManager.showSoftInput from the
// view the finger touched). GTK asks for the keyboard itself when its own text widget is tapped
// (gtk_im_context_activate_osk), but a WebKit page's fields never ask (a site's password box stayed without keys), and
// neither does a field an app focuses in answer to a tap elsewhere (the browser's address bar opened by its pill):
// their text input is enabled without show_input_panel, so mutter keeps the keyboard closed.
//
// So: a finger comes down while no client text field has the input focus; within FOCUS_WAIT_MS of it lifting, one
// does → the keyboard opens, on that window's monitor. The input method's focus is polled for that short while only
// (there is no signal for it): nothing runs between touches. A field that keeps the focus across the tap (a key on the
// keyboard, the send button) opens nothing, and the focus leaving the field closes the keyboard as before (the input
// method's own focus-out).
import Clutter from 'gi://Clutter';
import GLib from 'gi://GLib';

import * as Main from 'resource:///org/gnome/shell/ui/main.js';

const FOCUS_WAIT_MS = 800;      // a page's field: the tap goes to WebKit's web process and the focus comes back
const POLL_MS = 40;

export class TouchFocusKeyboard {
    constructor() {
        this._hadFocus = true;
        this._pollId = 0;
        global.stage.connectObject('captured-event', (_stage, event) => this._onEvent(event), this);
    }

    _onEvent(event) {
        const type = event.type();
        if (type === Clutter.EventType.TOUCH_BEGIN) {
            this._stop();
            this._hadFocus = Main.inputMethod.currentFocus !== null;
        } else if (type === Clutter.EventType.TOUCH_END) {
            this._watch();
        }
        return Clutter.EVENT_PROPAGATE;
    }

    _watch() {
        this._stop();
        const end = GLib.get_monotonic_time() + FOCUS_WAIT_MS * 1000;
        this._pollId = GLib.timeout_add(GLib.PRIORITY_DEFAULT, POLL_MS, () => {
            if (Main.inputMethod.currentFocus === null) {
                // the focus left a field: it may come back to another one
                this._hadFocus = false;
            } else if (!this._hadFocus) {
                this._pollId = 0;
                this._open();
                return GLib.SOURCE_REMOVE;
            }
            if (GLib.get_monotonic_time() < end)
                return GLib.SOURCE_CONTINUE;
            this._pollId = 0;
            return GLib.SOURCE_REMOVE;
        });
        GLib.Source.set_name_by_id(this._pollId, '[gnome-phone] touch focus keyboard');
    }

    _open() {
        if (Main.sessionMode.isLocked || Main.keyboard.visible)
            return;
        const win = global.display.focus_window;
        Main.keyboard.open(win ? win.get_monitor() : Main.layoutManager.primaryIndex);
    }

    _stop() {
        if (this._pollId)
            GLib.source_remove(this._pollId);
        this._pollId = 0;
    }

    destroy() {
        this._stop();
        global.stage.disconnectObject(this);
    }
}
