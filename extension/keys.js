// The power key and the touchscreen's double tap, for the phone's own screen on and off (power.js).
//
// gnome-settings-daemon grabs XF86PowerOff and, with no suspend to go to, answers it with gnome-session's power-off
// dialog, which shuts the phone down after 60 s (2026-11-04). Here the key is taken from it: its grab released and
// grabbed again with the release reported too, as Android needs the press (wake) and how long it is held (the power
// menu). XF86WakeUp is the s6sy761's double tap from a dark screen. Both are allowed in every action mode, under the
// lock screen's modal grab too. Unloading gives the power key back to gnome-settings-daemon.
import Clutter from 'gi://Clutter';
import Meta from 'gi://Meta';
import Shell from 'gi://Shell';

import * as Main from 'resource:///org/gnome/shell/ui/main.js';

const POWER = 'XF86PowerOff';
const WAKE = 'XF86WakeUp';
// the keys' XKB keycodes (evdev KEY_POWER 116, KEY_WAKEUP 143, + 8), to find a grab of them without a press
const POWER_KEYCODE = 124;

export class HardwareKeys {
    // onPower(pressed, timeMs) for each press and release of the power key; onWake() for a double tap
    constructor({onPower, onWake}) {
        this._onPower = onPower;
        this._onWake = onWake;
        this._dbus = Main.shellDBusService;

        // gnome-settings-daemon's grab, given back on destroy
        const theirs = global.display.get_keybinding_action(POWER_KEYCODE, 0);
        if (theirs) {
            const name = Meta.external_binding_name_for_action(theirs);
            this._theirs = {sender: this._dbus._grabbedAccelerators.get(theirs), modes: Main.wm._allowedKeybindings[name]};
            this._dbus._ungrabAccelerator(theirs);
        }

        this._power = this._grab(POWER, Meta.KeyBindingFlags.TRIGGER_RELEASE);
        this._wake = this._grab(WAKE, Meta.KeyBindingFlags.NONE);
        global.display.connectObject(
            'accelerator-activated', (display, action, device, timestamp) => this._activated(action, true, timestamp),
            'accelerator-deactivated', (display, action, device, timestamp) => this._activated(action, false, timestamp),
            this);
    }

    _grab(accelerator, flags) {
        const action = global.display.grab_accelerator(accelerator, flags);
        if (action === Meta.KeyBindingAction.NONE) {
            log(`vibeos: could not grab ${accelerator}`);
            return 0;
        }
        Main.wm.allowKeybinding(Meta.external_binding_name_for_action(action), Shell.ActionMode.ALL);
        return action;
    }

    _activated(action, pressed, timestamp) {
        if (action === this._power && action) {
            // a held key repeats after org.gnome.desktop.peripherals.keyboard's delay (500 ms, the long press's own
            // timeout): each repeat would start the press over, so a hold never opened the menu and its up slept
            const event = Clutter.get_current_event();
            if (pressed && event && event.get_flags() & Clutter.EventFlags.FLAG_REPEATED)
                return;
            this._onPower(pressed, timestamp);
        }
        else if (action === this._wake && action && pressed)
            this._onWake();
    }

    destroy() {
        global.display.disconnectObject(this);
        for (const action of [this._power, this._wake]) {
            if (!action)
                continue;
            Main.wm.allowKeybinding(Meta.external_binding_name_for_action(action), Shell.ActionMode.NONE);
            global.display.ungrab_accelerator(action);
        }
        // back to gnome-settings-daemon, if it is still there
        if (this._theirs?.sender && this._dbus._grabbers.has(this._theirs.sender)) {
            this._dbus._grabAcceleratorForSender(POWER, this._theirs.modes ?? Shell.ActionMode.ALL,
                Meta.KeyBindingFlags.NONE, this._theirs.sender);
        }
    }
}

// The Bixby key under the volume keys opens recents. The device tree reports it as
// KEY_COFFEE, which XKB makes XF86ScreenSaver and gnome-settings-daemon a screen lock; the udev hwdb entry
// (61-s10e-bixby.hwdb) makes it KEY_APPSELECT, XF86AppSelect, which nothing else in GNOME takes. onPress() once per
// press, repeats dropped. In recents (a modal grab, action mode POPUP) the shell filters the binding out (it is
// allowed in NORMAL only), and the key reaches recents itself (recents.js _onKey).
const APP_SWITCH = 'XF86AppSelect';

export class AppSwitchKey {
    constructor(onPress) {
        this._onPress = onPress;
        this._action = global.display.grab_accelerator(APP_SWITCH, Meta.KeyBindingFlags.IGNORE_AUTOREPEAT);
        if (this._action === Meta.KeyBindingAction.NONE) {
            log(`vibeos: could not grab ${APP_SWITCH}`);
            this._action = 0;
            return;
        }
        Main.wm.allowKeybinding(Meta.external_binding_name_for_action(this._action), Shell.ActionMode.NORMAL);
        global.display.connectObject('accelerator-activated', (display, action) => {
            if (action === this._action)
                this._onPress();
        }, this);
    }

    destroy() {
        global.display.disconnectObject(this);
        if (!this._action)
            return;
        Main.wm.allowKeybinding(Meta.external_binding_name_for_action(this._action), Shell.ActionMode.NONE);
        global.display.ungrab_accelerator(this._action);
        this._action = 0;
    }
}
