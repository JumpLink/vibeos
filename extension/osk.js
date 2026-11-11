// sm.puri.OSK0 on the session bus, as squeekboard serves it under Phosh: Android's keyboard requests (Waydroid's bridge
// IME, through gnome/device/waydroid/waydroid-oskbridge) open GNOME's own on-screen keyboard. Waydroid's windows have
// no input method, so the shell never learns of their text fields by itself; the keyboard types into them as key
// presses, through its virtual keyboard. It opens only over an Android window in front, and not locked, and closes
// once that window is no longer in front: nothing else would close it. Docked as on the phone; docked it needs the
// screen keyboard switched on in Accessibility, as GNOME's own apps do.
import Gio from 'gi://Gio';
import GLib from 'gi://GLib';

import * as Main from 'resource:///org/gnome/shell/ui/main.js';

const IFACE = `<node>
  <interface name="sm.puri.OSK0">
    <method name="SetVisible">
      <arg type="b" name="visible" direction="in"/>
    </method>
    <property name="Visible" type="b" access="read"/>
  </interface>
</node>`;

const isAndroid = win => win?.get_wm_class()?.startsWith('waydroid.') ?? false;

export class AndroidKeyboard {
    constructor() {
        this._window = null;        // the Android window the keyboard is open for
        this._impl = Gio.DBusExportedObject.wrapJSObject(IFACE, this);
        this._impl.export(Gio.DBus.session, '/sm/puri/OSK0');
        // squeekboard, if it runs, keeps the name
        this._nameId = Gio.bus_own_name(Gio.BusType.SESSION, 'sm.puri.OSK0', Gio.BusNameOwnerFlags.NONE,
            null, null, null);
        global.display.connectObject('notify::focus-window', () => this._sync(), this);
        Main.sessionMode.connectObject('updated', () => this._sync(), this);
        Main.keyboard.connectObject('visibility-changed', () => this._visibilityChanged(), this);
    }

    get Visible() {
        return !!Main.keyboard.visible;
    }

    SetVisible(visible) {
        const win = global.display.focus_window;
        if (visible && isAndroid(win) && !Main.sessionMode.isLocked) {
            this._window = win;
            Main.keyboard.open(win.get_monitor());
        } else if (!visible && this._window) {
            // only the keyboard Android asked for: one a GNOME app's text field opened stays
            this._close();
        }
    }

    _sync() {
        if (this._window && (global.display.focus_window !== this._window || Main.sessionMode.isLocked))
            this._close();
    }

    _close() {
        this._window = null;
        Main.keyboard.close();
    }

    _visibilityChanged() {
        if (!Main.keyboard.visible)
            this._window = null;
        this._impl.emit_property_changed('Visible', GLib.Variant.new_boolean(this.Visible));
    }

    destroy() {
        global.display.disconnectObject(this);
        Main.sessionMode.disconnectObject(this);
        Main.keyboard.disconnectObject(this);
        if (this._window)
            this._close();
        Gio.bus_unown_name(this._nameId);
        this._impl.unexport();
    }
}
