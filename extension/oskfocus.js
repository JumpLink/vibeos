// A text field that a tap gives focus opens the keyboard, as a phone's does. GTK asks for the keyboard itself when its
// own text widget is tapped (gtk_im_context_activate_osk), but a WebKit page's fields never ask (a site's password box
// stayed without keys), and neither does a field an app focuses in answer to a tap elsewhere (the browser's address bar
// opened by its pill): their text input is enabled without show_input_panel, so mutter keeps the keyboard closed.
//
// So: a finger comes down while no client text field has the input focus; within FOCUS_WAIT_MS of it lifting, one
// does → the keyboard opens, on that window's monitor. The input method's focus is polled for that short while only
// (there is no signal for it): nothing runs between touches. A field that keeps the focus across the tap (a key on the
// keyboard, the send button) opens nothing, and the focus leaving the field closes the keyboard as before (the input
// method's own focus-out).
//
// That close waits for the finger, though. The shell closes the keyboard the moment the field loses the focus, and the
// window it had shrunk grows back at once. GTK moves the focus on a button's PRESS (focus-on-click), so a dialog's
// Cancel or Rename touched while the keyboard was up moved down the screen before the finger lifted: the release
// missed the button and the dialog stayed, re-centred lower, as if it had opened again (neo-settings' phone name,
// 2026-11-12). On Android a button takes no focus in touch mode and the keyboard goes once the tap is done. So while a
// finger is down, Keyboard.close only marks the keyboard to close; the last finger lifting closes it, unless a field
// took the focus meanwhile (open).
import Clutter from 'gi://Clutter';
import GLib from 'gi://GLib';

import * as KeyboardUI from 'resource:///org/gnome/shell/ui/keyboard.js';
import * as Main from 'resource:///org/gnome/shell/ui/main.js';

const FOCUS_WAIT_MS = 800;      // a page's field: the tap goes to WebKit's web process and the focus comes back
const POLL_MS = 40;
const CLOSE_AFTER_LIFT_MS = 60; // the app has the lift (and its button its click) before its window grows back
const CLOSE_HELD_MAX_MS = 2000; // a lift that never came (a grab took the touch) cannot keep the keyboard up

export class TouchFocusKeyboard {
    constructor() {
        this._hadFocus = true;
        this._pollId = 0;
        this._touches = 0;
        this._closeId = 0;
        this._holdClose();
        global.stage.connectObject('captured-event', (_stage, event) => this._onEvent(event), this);
    }

    // Keyboard.close and open, wrapped while the extension runs (the keyboard is made again whenever the OSK setting
    // changes, so the class, not the instance)
    _holdClose() {
        const proto = KeyboardUI.Keyboard?.prototype;
        if (!proto?.close || !proto?.open)
            return;
        const close = proto.close;
        const open = proto.open;
        const self = this;
        proto.close = function (...args) {
            if (self._touches > 0) {
                this.__gnomePhoneCloseHeld = true;
                self._closeLater(CLOSE_HELD_MAX_MS);
                return undefined;
            }
            this.__gnomePhoneCloseHeld = false;
            return close.apply(this, args);
        };
        proto.open = function (...args) {
            this.__gnomePhoneCloseHeld = false;
            return open.apply(this, args);
        };
        this._unholdClose = () => {
            proto.close = close;
            proto.open = open;
        };
        this._closeHeld = () => {
            const keyboard = Main.keyboard._keyboard;
            if (keyboard?.__gnomePhoneCloseHeld) {
                keyboard.__gnomePhoneCloseHeld = false;
                close.call(keyboard);
            }
        };
    }

    _closeLater(ms) {
        if (!this._closeHeld)
            return;
        if (this._closeId)
            GLib.source_remove(this._closeId);
        this._closeId = GLib.timeout_add(GLib.PRIORITY_DEFAULT, ms, () => {
            this._closeId = 0;
            this._touches = 0;
            this._closeHeld();
            return GLib.SOURCE_REMOVE;
        });
        GLib.Source.set_name_by_id(this._closeId, '[gnome-phone] keyboard close after the lift');
    }

    _onEvent(event) {
        const type = event.type();
        if (type === Clutter.EventType.TOUCH_BEGIN) {
            this._touches++;
            this._stop();
            this._hadFocus = Main.inputMethod.currentFocus !== null;
        } else if (type === Clutter.EventType.TOUCH_END || type === Clutter.EventType.TOUCH_CANCEL) {
            this._touches = Math.max(0, this._touches - 1);
            if (this._touches === 0 && Main.keyboard._keyboard?.__gnomePhoneCloseHeld)
                this._closeLater(CLOSE_AFTER_LIFT_MS);
            if (type === Clutter.EventType.TOUCH_END)
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
        if (this._closeId)
            GLib.source_remove(this._closeId);
        this._closeId = 0;
        this._closeHeld?.();
        this._unholdClose?.();
    }
}
