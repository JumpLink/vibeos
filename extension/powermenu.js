// The power menu, as LineageOS 23.2 has it: a black dim over everything, and a panel holding two columns of round
// buttons, each with its icon and a label under it; a short last row is packed in the middle. It comes in over 220 ms
// on the standard curve, sliding 60 px in from the power key's side as it fades in with the dim, and goes in 150 ms
// (standard-accelerate), 30 px the other way. A pressed button shrinks to 0.97 in 50 ms and comes back in 250. A tap
// outside the panel closes it.
//
// The items are the reference's that this phone can back: Lockdown, shown only with a PIN or a pattern
// set (the lock screen at once, its notifications kept off it until the next unlock), Power off and Restart
// (logind's: gnome-session 51 has only its dialog's Shutdown and Reboot, no RequestShutdown/RequestReboot any more).
// Emergency is left out: nothing here can place an emergency call. Between them, this device's own: Android, a restart
// into the LineageOS it dual-boots with, the way its device package writes in /etc/vibeos/android (a plain restart
// comes back here):
//   reboot=<mode>   logind's restart with this reboot parameter, the kernel's reboot mode ("android" on the Galaxy
//                   S10e; "recovery" on the S20 FE, whose recovery partition holds LineageOS's boot image)
//   helper=<path>   a program run through pkexec instead, for a device that also has to set the bootloader message
//                   in misc (the Tab S5e, the POCO X3); its polkit action lets the active session run it
// A device without the file (no Android beside VibeOS) has no Android item.
import Gio from 'gi://Gio';
import GLib from 'gi://GLib';
import Shell from 'gi://Shell';
import St from 'gi://St';

import * as Main from 'resource:///org/gnome/shell/ui/main.js';

import {VectorIcon} from './icons.js';
import {animate, bezier, LINEAR} from './motion.js';
import {Color, cogl, rgba} from './palette.js';
import {AndroidText} from './text.js';
import {HEIGHT, px, WIDTH} from './units.js';

Gio._promisify(Gio.DBusConnection.prototype, 'call');

// screen px
const PAD = 72;                 // around the grid and between its cells
const BUTTON = 288;
const ICON = 60;
const LABEL_GAP = 24;           // between a button and its label
const CORNER = 84;              // the panel's corners
const COLUMNS = 2;
const LABEL_SP = 14;
const LABEL_WGHT = 700;
const DIM = 0.6;                // the black dim behind the panel
const ENTER_MS = 220;           // the standard duration of a window coming in
const EXIT_MS = 150;            // and the short one, going
const ENTER_DX = 60;            // a popup's rise, turned to run along x: the power key is on the right
const EXIT_DX = -30;            // and its fall, going
const STANDARD = bezier(0.2, 0, 0, 1);
const STANDARD_ACCELERATE = bezier(0.3, 0, 1, 1);
const PRESSED_SCALE = 0.97;
const PRESS_MS = 50;
const RELEASE_MS = 250;

function shapeStyle(color, radius) {
    return `background-color: ${rgba(color)}; border-radius: ${px(radius).toFixed(2)}px;`;
}

function login1(method, params) {
    return Gio.DBus.system.call('org.freedesktop.login1', '/org/freedesktop/login1',
        'org.freedesktop.login1.Manager', method, params, null, Gio.DBusCallFlags.NONE, -1, null);
}

// PowerOff or Reboot, a reboot with the kernel's reboot mode `mode` ('' for the default: this system again); the
// shell's session may (polkit says yes without asking)
async function logind(method, mode = '') {
    try {
        if (method === 'Reboot')
            await login1('SetRebootParameter', new GLib.Variant('(s)', [mode]));
        await login1(method, new GLib.Variant('(b)', [false]));
    } catch (e) {
        log(`vibeos: ${method}: ${e.message}`);
    }
}

const ANDROID_CONF = '/etc/vibeos/android';

// the Android item's action, or null without one
function androidRestart() {
    let conf;
    try {
        const [, bytes] = GLib.file_get_contents(ANDROID_CONF);
        conf = new TextDecoder().decode(bytes);
    } catch {
        return null;
    }
    const keys = {};
    for (const line of conf.split('\n')) {
        const m = line.match(/^\s*(reboot|helper)\s*=\s*(\S+)\s*$/);
        if (m)
            keys[m[1]] = m[2];
    }
    if (keys.helper) {
        return () => {
            try {
                Gio.Subprocess.new(['pkexec', keys.helper], Gio.SubprocessFlags.NONE);
            } catch (e) {
                log(`vibeos: ${keys.helper}: ${e.message}`);
            }
        };
    }
    return keys.reboot ? () => logind('Reboot', keys.reboot) : null;
}

export class PowerMenu {
    // secure(): a PIN or pattern is set; lockdown(): lock now
    constructor({secure, lockdown}) {
        this._secure = secure;
        this._lockdown = lockdown;
        this._closing = false;
        this._grab = null;
        this._anim = null;

        this.layer = new St.Widget({visible: false});
        // the dim, which a tap closes the menu on
        this._scrim = new St.Button({button_mask: St.ButtonMask.PRIMARY});
        this._scrim.connect('clicked', () => this.close());
        this.layer.add_child(this._scrim);
        this._panel = new St.Widget({reactive: true, style: shapeStyle(Color.surfaceContainerLow, CORNER)});
        this.layer.add_child(this._panel);
    }

    get showing() {
        return this.layer.visible && !this._closing;
    }

    open() {
        if (this.layer.visible)
            return;
        this._build();
        this.layer.show();
        this._grab = Main.pushModal(this.layer, {actionMode: Shell.ActionMode.SYSTEM_MODAL});
        this._animate(true);
    }

    // then: what the pressed item does, once the menu is gone
    close(then = null) {
        if (!this.layer.visible || this._closing)
            return;
        this._closing = true;
        this._animate(false, () => this._hide(then));
    }

    closeNow() {
        if (!this.layer.visible)
            return;
        this._anim?.stop();
        this._hide(null);
    }

    _hide(then) {
        this._anim = null;
        this._closing = false;
        this.layer.hide();
        if (this._grab)
            Main.popModal(this._grab);
        this._grab = null;
        this._panel.destroy_all_children();
        then?.();
    }

    _animate(enter, onDone = null) {
        this._anim?.stop();
        this._anim = animate(this.layer, {
            from: 0, to: 1,
            duration: enter ? ENTER_MS : EXIT_MS,
            interpolator: enter ? STANDARD : STANDARD_ACCELERATE,
            onUpdate: p => {
                const alpha = enter ? p : 1 - p;
                this._panel.opacity = Math.round(alpha * 255);
                this._panel.translation_x = px(enter ? ENTER_DX * (1 - p) : EXIT_DX * p);
                this.layer.background_color = cogl(Color.black, DIM * alpha);
            },
            onDone,
        });
    }

    _build() {
        const items = [];
        if (this._secure())
            items.push(['Lockdown', 'ic_lock_lockdown_round', () => this._lockdown()]);
        items.push(['Power off', 'ic_lock_power_off_round', () => logind('PowerOff')]);
        const android = androidRestart();
        if (android)
            items.push(['Android', 'ic_android', android]);
        items.push(['Restart', 'ic_restart_round', () => logind('Reboot')]);

        const labels = items.map(([label]) =>
            new AndroidText(label, LABEL_SP, LABEL_WGHT, {colour: Color.onSurface}));
        const cellH = BUTTON + LABEL_GAP + Math.max(...labels.map(l => l.textHeight));
        const columns = Math.min(items.length, COLUMNS);
        const rows = Math.ceil(items.length / COLUMNS);
        const w = PAD + columns * (BUTTON + PAD);
        const h = PAD + rows * (cellH + PAD);
        // over the whole screen as it is turned now, the panel in its middle
        const monitor = Main.layoutManager.primaryMonitor;
        this.layer.set_position(monitor.x, monitor.y);
        this.layer.set_size(monitor.width, monitor.height);
        this._scrim.set_size(monitor.width, monitor.height);
        this._panel.set_position(px(Math.round((WIDTH - w) / 2)), px(Math.round((HEIGHT - h) / 2)));
        this._panel.set_size(px(w), px(h));
        items.forEach(([label, icon, action], i) => {
            const row = Math.floor(i / COLUMNS);
            const inRow = Math.min(COLUMNS, items.length - row * COLUMNS);
            const rowX = (w - inRow * BUTTON - (inRow - 1) * PAD) / 2;
            const x = rowX + (i % COLUMNS) * (BUTTON + PAD);
            this._panel.add_child(this._item(label, labels[i], icon, action, x, PAD + row * (cellH + PAD), cellH));
        });
    }

    _item(label, text, icon, action, x, y, height) {
        const button = new St.Button({
            x: px(x), y: px(y), width: px(BUTTON), height: px(height),
            can_focus: true,
            button_mask: St.ButtonMask.PRIMARY,
            accessible_name: label,
        });
        button.set_pivot_point(0.5, 0.5);
        const box = new St.Widget({width: px(BUTTON), height: px(height)});
        box.add_child(new St.Widget({width: px(BUTTON), height: px(BUTTON),
            style: shapeStyle(Color.surfaceContainerHighest, BUTTON / 2)}));
        const glyph = new VectorIcon(icon, px(ICON), Color.onSurface);
        glyph.set_position(px((BUTTON - ICON) / 2), px((BUTTON - ICON) / 2));
        box.add_child(glyph);
        text.set_position(px(BUTTON / 2 - text.textWidth / 2 - text.inset), px(BUTTON + LABEL_GAP - text.inset));
        box.add_child(text);
        button.set_child(box);

        let scale = null;
        button.connect('notify::pressed', () => {
            scale?.stop();
            scale = animate(button, {
                from: button.scale_x, to: button.pressed ? PRESSED_SCALE : 1,
                duration: button.pressed ? PRESS_MS : RELEASE_MS,
                interpolator: LINEAR,
                onUpdate: s => button.set_scale(s, s),
            });
        });
        button.connect('clicked', () => this.close(action));
        return button;
    }

    destroy() {
        this.closeNow();
        this.layer.destroy();
    }
}
