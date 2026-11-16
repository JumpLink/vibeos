// Docked: while a display is plugged in, vibeos-dock runs the session as gnome-desktop.service, GNOME's own desktop on
// the display, and none of the phone's UI belongs there (its lock screen, AOD and launcher would come up on the TV,
// where nothing can unlock them). What the extension does in that session instead: the desktop never blanks, locks or
// sleeps (an idle and suspend inhibitor, which s10e-sleepd honours too; a suspended phone drops the display), the power
// key does nothing (gnome-settings-daemon's would suspend), and a game controller works the shell, as a console's
// does: the D-pad and left stick move, A chooses, B goes back, Home or Start opens and closes the app grid. A game in
// front reads the controller itself, so then only Home is the shell's.
import Clutter from 'gi://Clutter';
import Gio from 'gi://Gio';
import GLib from 'gi://GLib';
import Shell from 'gi://Shell';

import * as Main from 'resource:///org/gnome/shell/ui/main.js';

import {HardwareKeys} from './keys.js';

const DESKTOP_UNIT = 'gnome-desktop.service';

const INHIBIT_SUSPEND = 4;      // GsmInhibitorFlag
const INHIBIT_IDLE = 8;

// linux/input-event-codes.h, as libmanette reports a mapped controller's buttons and axes
const BTN_SOUTH = 0x130;
const BTN_EAST = 0x131;
const BTN_START = 0x13b;
const BTN_MODE = 0x13c;
const BTN_DPAD_UP = 0x220;
const BTN_DPAD_DOWN = 0x221;
const BTN_DPAD_LEFT = 0x222;
const BTN_DPAD_RIGHT = 0x223;
const ABS_X = 0x00;
const ABS_Y = 0x01;
const ABS_HAT0X = 0x10;
const ABS_HAT0Y = 0x11;

// a stick counts as pushed past PRESS and as let go under RELEASE
const PRESS = 0.6;
const RELEASE = 0.35;
// a direction held repeats, as a held arrow key does
const REPEAT_DELAY_MS = 400;
const REPEAT_MS = 110;

// Is this session vibeos-dock's desktop? Both sessions are the same gnome-session and shell, so ask systemd which unit
// started it (the two units conflict: one is active at a time).
export function isDesktopSession() {
    try {
        const bus = Gio.DBus.system;
        const [path] = bus.call_sync('org.freedesktop.systemd1', '/org/freedesktop/systemd1',
            'org.freedesktop.systemd1.Manager', 'GetUnit', new GLib.Variant('(s)', [DESKTOP_UNIT]),
            new GLib.VariantType('(o)'), Gio.DBusCallFlags.NONE, 2000, null).deepUnpack();
        const [state] = bus.call_sync('org.freedesktop.systemd1', path, 'org.freedesktop.DBus.Properties', 'Get',
            new GLib.Variant('(ss)', ['org.freedesktop.systemd1.Unit', 'ActiveState']),
            new GLib.VariantType('(v)'), Gio.DBusCallFlags.NONE, 2000, null).deepUnpack();
        return ['active', 'activating', 'reloading'].includes(state.unpack());
    } catch (e) {
        // not loaded (NoSuchUnit): not docked
        return false;
    }
}

export class Desktop {
    constructor() {
        this._cookie = 0;
        this._cancellable = new Gio.Cancellable();
        Gio.DBus.session.call('org.gnome.SessionManager', '/org/gnome/SessionManager', 'org.gnome.SessionManager',
            'Inhibit', new GLib.Variant('(susu)', ['org.gnome.Shell', 0, 'Docked to a display',
                INHIBIT_SUSPEND | INHIBIT_IDLE]),
            new GLib.VariantType('(u)'), Gio.DBusCallFlags.NONE, -1, this._cancellable, (bus, res) => {
                try {
                    [this._cookie] = bus.call_finish(res).deepUnpack();
                } catch (e) {
                    if (!e.matches(Gio.IOErrorEnum, Gio.IOErrorEnum.CANCELLED))
                        logError(e, 'vibeos: keeping the docked desktop awake');
                }
            });

        this._keys = new HardwareKeys({onPower: () => {}, onWake: () => {}});
        // the controller is a nicety: without it the desktop still stays awake and the power key still does nothing
        try {
            this._pad = new PadNavigation();
        } catch (e) {
            logError(e, 'vibeos: a game controller in the docked desktop');
            this._pad = null;
        }
    }

    destroy() {
        this._cancellable.cancel();
        if (this._cookie) {
            Gio.DBus.session.call('org.gnome.SessionManager', '/org/gnome/SessionManager',
                'org.gnome.SessionManager', 'Uninhibit', new GLib.Variant('(u)', [this._cookie]),
                null, Gio.DBusCallFlags.NONE, -1, null, null);
        }
        this._cookie = 0;
        this._keys.destroy();
        this._pad?.destroy();
    }
}

// A game controller as the shell's keyboard, through libmanette (gir1.2-manette-0.2; without it, no controller)
class PadNavigation {
    constructor() {
        this._devices = new Set();
        this._held = new Map();         // direction key -> its repeat's source id
        this._stick = {x: 0, y: 0};
        const seat = global.stage.context.get_backend().get_default_seat();
        this._keyboard = seat.create_virtual_device(Clutter.InputDeviceType.KEYBOARD_DEVICE);
        import('gi://Manette?version=0.2').then(({default: Manette}) => {
            if (this._destroyed)
                return;
            this._monitor = new Manette.Monitor();
            this._monitor.connectObject('device-connected', (monitor, device) => this._add(device), this);
            const iter = this._monitor.iterate();
            for (let [ok, device] = iter.next(); ok; [ok, device] = iter.next())
                this._add(device);
        }).catch(e => log(`vibeos: no game controller in the docked desktop: ${e.message}`));
    }

    _add(device) {
        if (this._devices.has(device))
            return;
        this._devices.add(device);
        device.connectObject(
            'button-press-event', (d, event) => this._button(event, true),
            'button-release-event', (d, event) => this._button(event, false),
            'absolute-axis-event', (d, event) => this._axis(event),
            'hat-axis-event', (d, event) => this._hat(event),
            'disconnected', () => {
                device.disconnectObject(this);
                this._devices.delete(device);
                this._releaseAll();
            }, this);
    }

    // a game in front takes the controller itself: one its desktop file calls a game, or anything fullscreen (a
    // window the shell can't place with its desktop file has no categories)
    get _shellsTurn() {
        if (Main.overview.visible)
            return true;
        const win = global.display.focus_window;
        if (!win)
            return true;
        if (win.is_fullscreen())
            return false;
        const categories = Shell.WindowTracker.get_default().get_window_app(win)?.get_app_info()?.get_categories();
        return !categories?.split(';').includes('Game');
    }

    _button(event, pressed) {
        const [ok, button] = event.get_button();
        if (!ok)
            return;
        if (button === BTN_MODE) {
            if (pressed)
                this._toggleAppGrid();
            return;
        }
        if (!this._shellsTurn) {
            this._releaseAll();
            return;
        }
        switch (button) {
        case BTN_DPAD_UP:
            this._direction(Clutter.KEY_Up, pressed);
            break;
        case BTN_DPAD_DOWN:
            this._direction(Clutter.KEY_Down, pressed);
            break;
        case BTN_DPAD_LEFT:
            this._direction(Clutter.KEY_Left, pressed);
            break;
        case BTN_DPAD_RIGHT:
            this._direction(Clutter.KEY_Right, pressed);
            break;
        case BTN_SOUTH:
            if (pressed)
                this._tap(Clutter.KEY_Return);
            break;
        case BTN_EAST:
            if (pressed)
                this._tap(Clutter.KEY_Escape);
            break;
        case BTN_START:
            if (pressed)
                this._toggleAppGrid();
            break;
        }
    }

    // the left stick as the D-pad, with some play between pushed and let go
    _axis(event) {
        const [ok, axis, value] = event.get_absolute();
        if (!ok || (axis !== ABS_X && axis !== ABS_Y))
            return;
        if (!this._shellsTurn) {
            this._releaseAll();
            return;
        }
        const [negative, positive] = axis === ABS_X ? [Clutter.KEY_Left, Clutter.KEY_Right]
            : [Clutter.KEY_Up, Clutter.KEY_Down];
        const was = axis === ABS_X ? this._stick.x : this._stick.y;
        let now = was;
        if (value <= -PRESS)
            now = -1;
        else if (value >= PRESS)
            now = 1;
        else if (Math.abs(value) < RELEASE)
            now = 0;
        if (now === was)
            return;
        if (axis === ABS_X)
            this._stick.x = now;
        else
            this._stick.y = now;
        if (was)
            this._direction(was < 0 ? negative : positive, false);
        if (now)
            this._direction(now < 0 ? negative : positive, true);
    }

    // a D-pad libmanette has no mapping for comes as a hat
    _hat(event) {
        const [ok, axis, value] = event.get_hat();
        if (!ok || (axis !== ABS_HAT0X && axis !== ABS_HAT0Y))
            return;
        if (!this._shellsTurn) {
            this._releaseAll();
            return;
        }
        const [negative, positive] = axis === ABS_HAT0X ? [Clutter.KEY_Left, Clutter.KEY_Right]
            : [Clutter.KEY_Up, Clutter.KEY_Down];
        this._direction(negative, value < 0);
        this._direction(positive, value > 0);
    }

    _toggleAppGrid() {
        if (Main.overview.visible)
            Main.overview.hide();
        else
            Main.overview.showApps();
    }

    _tap(keyval) {
        const time = GLib.get_monotonic_time();
        this._keyboard.notify_keyval(time, keyval, Clutter.KeyState.PRESSED);
        this._keyboard.notify_keyval(time, keyval, Clutter.KeyState.RELEASED);
    }

    // a direction pushed: one step, then steps on while held
    _direction(keyval, pressed) {
        if (!pressed) {
            const id = this._held.get(keyval);
            if (id)
                GLib.source_remove(id);
            this._held.delete(keyval);
            return;
        }
        if (this._held.has(keyval))
            return;
        this._tap(keyval);
        const id = GLib.timeout_add(GLib.PRIORITY_DEFAULT, REPEAT_DELAY_MS, () => {
            this._tap(keyval);
            this._held.set(keyval, GLib.timeout_add(GLib.PRIORITY_DEFAULT, REPEAT_MS, () => {
                this._tap(keyval);
                return GLib.SOURCE_CONTINUE;
            }));
            return GLib.SOURCE_REMOVE;
        });
        this._held.set(keyval, id);
    }

    _releaseAll() {
        for (const id of this._held.values())
            GLib.source_remove(id);
        this._held.clear();
        this._stick = {x: 0, y: 0};
    }

    destroy() {
        this._destroyed = true;
        this._releaseAll();
        for (const device of this._devices)
            device.disconnectObject(this);
        this._devices.clear();
        this._monitor?.disconnectObject(this);
        this._monitor = null;
        this._keyboard.run_dispose();
    }
}
