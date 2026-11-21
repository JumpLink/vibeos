// The panel's camera hole (or notch), wherever the panel's rotation puts it. Where it is comes from gmobile's display
// panel data for the device (the device tree's compatibles, as Phosh reads them), or, for a panel gmobile has not
// built in, from a description a device package installs as display-panels/<compatible>.json under /etc/gmobile or
// gmobile/ in the data dirs (gmobile 0.7.5 reads only its built-in ones, but parses such a file the same way). No
// data: no hole. On the S10e (beyond0lte) it is the path LineageOS describes it with, "M 0,0 H -49.67 V 38.67 H 0 V 0 Z
// @dp @right": a 149 x 116 px box in the top right corner of the upright panel. On the S20 FE (r8s) it is LineageOS's
// box around the punch hole, "M -60,0 h 120 v 120 h -120 Z": 120 x 120 px in the top centre. Upright, the status bar
// is over it.
// Turned, it is on a short edge, and apps are kept clear of it: a black band
// along that edge, which CutoutBand reserves as a strut, and a status bar whose content starts after the hole when
// the hole is at its end.
import Gio from 'gi://Gio';
import GLib from 'gi://GLib';
import GObject from 'gi://GObject';
import Meta from 'gi://Meta';
import St from 'gi://St';

import * as Main from 'resource:///org/gnome/shell/ui/main.js';

import {px, SCALE, SCREEN_W} from './units.js';

// MtkMonitorTransform (0-3, unflipped) -> the edge the upright panel's top is on: upright the top, 90 (the phone's
// left side up) the right, 180 the bottom, 270 (right side up) the left
const SIDES = ['top', 'right', 'bottom', 'left'];

const STATE_TYPE = '(ua((ssss)a(siiddada{sv})a{sv})a(iiduba(ssss)a{sv})a{sv})';

// gmobile (gir1.2-gm-0) is optional: without it there is no hole
async function loadGm() {
    const {default: Gm} = await import('gi://Gm?version=0');
    Gm.init();
    return Gm;
}

// A device package's own panel description, for the first compatible that has one
function readPanelFile(Gm, compatibles) {
    const dirs = ['/etc/gmobile', ...GLib.get_system_data_dirs().map(d => GLib.build_filenamev([d, 'gmobile']))];
    for (const c of compatibles) {
        for (const dir of dirs) {
            const path = GLib.build_filenamev([dir, 'display-panels', `${c}.json`]);
            if (!GLib.file_test(path, GLib.FileTest.EXISTS))
                continue;
            try {
                const [, bytes] = GLib.file_get_contents(path);
                return Gm.DisplayPanel.new_from_data(new TextDecoder().decode(bytes));
            } catch (e) {
                logError(e, `vibeos: ${path}`);
            }
        }
    }
    return null;
}

// The hole on the upright panel, screen px: [x, y, width, height] around every cutout in the panel's top half, or
// null. gmobile gives them in the panel's own pixels (x-res wide), scaled here to the mode it runs at.
async function readHole() {
    let Gm;
    try {
        Gm = await loadGm();
    } catch {
        return null;
    }
    let compatibles;
    try {
        compatibles = Gm.device_tree_get_compatibles('/sys');
    } catch {
        return null;            // no device tree: not a phone gmobile knows
    }
    const panel = new Gm.DeviceInfo({compatibles}).get_display_panel() ?? readPanelFile(Gm, compatibles);
    if (!panel)
        return null;
    const k = panel.get_x_res() > 0 ? SCREEN_W / panel.get_x_res() : 1;
    const cutouts = panel.get_cutouts();
    let x0 = Infinity, y0 = Infinity, x1 = -Infinity, y1 = -Infinity;
    for (let i = 0; i < cutouts.get_n_items(); i++) {
        const b = cutouts.get_item(i).get_bounds();
        if (b.width <= 0 || b.height <= 0 || b.y > panel.get_y_res() / 2)
            continue;
        x0 = Math.min(x0, b.x);
        y0 = Math.min(y0, b.y);
        x1 = Math.max(x1, b.x + b.width);
        y1 = Math.max(y1, b.y + b.height);
    }
    if (x1 <= x0)
        return null;
    return [x0 * k, y0 * k, (x1 - x0) * k, (y1 - y0) * k].map(Math.round);
}

export const Cutout = GObject.registerClass({
    Signals: {'changed': {}},
}, class Cutout extends GObject.Object {
    constructor() {
        super();
        this._transform = 0;
        this._hole = null;              // [x, y, width, height] on the upright panel, screen px
        readHole().then(hole => {
            if (this._destroyed || !hole)
                return;
            this._hole = hole;
            this.emit('changed');
        }).catch(e => logError(e, 'vibeos: the display cutout'));
        this._sensor = global.backend.get_orientation_manager();
        this._cancellable = new Gio.Cancellable();
        Main.layoutManager.connectObject('monitors-changed', () => this._update(), this);
        this._update();
    }

    // 'top', 'right', 'bottom' or 'left'; null when the primary monitor is not the phone's own panel (a display
    // made the primary one: the phone docked)
    get side() {
        return this._transform === null ? null : SIDES[this._transform];
    }

    // the hole on the upright panel, screen px: [x, y, width, height] ([0, 0, 0, 0]: none)
    get upright() {
        return this._hole ?? [0, 0, 0, 0];
    }

    // how far the hole reaches in from the upright panel's top edge, screen px (0: no hole)
    get depth() {
        return this._hole ? this._hole[1] + this._hole[3] : 0;
    }

    // how far content keeps off each edge for the hole, screen px (Android's safe insets)
    get insets() {
        const side = this.side, depth = this.depth;
        return {
            top: side === 'top' ? depth : 0,
            right: side === 'right' ? depth : 0,
            bottom: side === 'bottom' ? depth : 0,
            left: side === 'left' ? depth : 0,
        };
    }

    // the hole's box on the turned panel, screen px: [x, y, width, height] ([0, 0, 0, 0]: none)
    get rect() {
        const m = Main.layoutManager.primaryMonitor;
        if (!this._hole || !m)
            return [0, 0, 0, 0];
        const W = m.width * SCALE, H = m.height * SCALE;
        const [x, y, w, h] = this._hole;
        switch (this.side) {
        case 'top':
            return [x, y, w, h];
        case 'right':
            return [W - y - h, x, h, w];
        case 'bottom':
            return [W - x - w, H - y - h, w, h];
        case 'left':
            return [y, H - x - w, h, w];
        default:
            return [0, 0, 0, 0];
        }
    }

    // The transform at once from the panel's shape and the sensor (mutter turns the panel by the sensor's reading),
    // then mutter's own word for it (GNOME's orientation lock can hold a reading the sensor has left). With a display
    // plugged in as well the panel keeps its hole for as long as it is the primary monitor: the status bar is still
    // on it (2026-11-20: a TV on the S10e put the battery under the camera); mutter says which one that is.
    _update() {
        const m = Main.layoutManager.primaryMonitor;
        if (!m) {
            this._set(null);
            return;
        }
        if (Main.layoutManager.monitors.length === 1) {
            if (m.height >= m.width) {
                this._set(this._transform === 2 ? 2 : 0);
            } else {
                const o = this._sensor.get_orientation();
                this._set(o === Meta.Orientation.LEFT_UP ? 1 : o === Meta.Orientation.RIGHT_UP ? 3
                    : this._transform === 1 || this._transform === 3 ? this._transform : 1);
            }
        }
        Gio.DBus.session.call('org.gnome.Mutter.DisplayConfig', '/org/gnome/Mutter/DisplayConfig',
            'org.gnome.Mutter.DisplayConfig', 'GetCurrentState', null, new GLib.VariantType(STATE_TYPE),
            Gio.DBusCallFlags.NONE, -1, this._cancellable, (bus, res) => {
                try {
                    const [, monitors, logical] = bus.call_finish(res).recursiveUnpack();
                    const builtin = monitors.filter(([, , props]) => props['is-builtin']).map(([[c]]) => c);
                    const primary = logical.find(l => l[4]) ?? (logical.length === 1 ? logical[0] : null);
                    if (!primary || !primary[5].some(([c]) => builtin.includes(c)))
                        this._set(null);
                    else
                        this._set(primary[3] & 3);
                } catch (e) {
                    if (!e.matches(Gio.IOErrorEnum, Gio.IOErrorEnum.CANCELLED))
                        logError(e, 'vibeos: the panel\'s rotation');
                }
            });
    }

    _set(transform) {
        if (transform === this._transform)
            return;
        this._transform = transform;
        this.emit('changed');
    }

    destroy() {
        this._destroyed = true;
        this._cancellable.cancel();
        Main.layoutManager.disconnectObject(this);
    }
});

// Android's black band along the hole's edge when the hole is not under the status bar: apps keep clear of it (a
// strut), and the status bar draws over its top end. Gone with the status bar (a fullscreen app covers the hole).
export const CutoutBand = GObject.registerClass(
class CutoutBand extends St.Widget {
    constructor(cutout, statusBar) {
        super({name: 'vibeosCutoutBand', style: 'background-color: black;', reactive: false});
        this._cutout = cutout;
        this._statusBar = statusBar;
        cutout.connectObject('changed', () => this._sync(), this);
        statusBar.connectObject('notify::visible', () => this._sync(), this);
        Main.layoutManager.connectObject('monitors-changed', () => this._sync(), this);
        this._sync();
    }

    _sync() {
        const m = Main.layoutManager.primaryMonitor;
        const side = this._cutout.side;
        const d = Math.ceil(px(this._cutout.depth));
        // the shell takes struts from hidden actors too: no band is no size
        if (!m || !side || side === 'top' || !d || !this._statusBar.visible) {
            this.hide();
            this.set_position(m?.x ?? 0, m?.y ?? 0);
            this.set_size(0, 0);
            return;
        }
        if (side === 'left') {
            this.set_position(m.x, m.y);
            this.set_size(d, m.height);
        } else if (side === 'right') {
            this.set_position(m.x + m.width - d, m.y);
            this.set_size(d, m.height);
        } else {
            this.set_position(m.x, m.y + m.height - d);
            this.set_size(m.width, d);
        }
        this.show();
    }

    destroy() {
        this._cutout.disconnectObject(this);
        this._statusBar.disconnectObject(this);
        Main.layoutManager.disconnectObject(this);
        super.destroy();
    }
});
