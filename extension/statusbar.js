// The Android 16 status bar: 116 px tall, clock and notification icons on the left, mobile signal, Wi-Fi and the
// unified battery on the right, ending where the S10e's camera hole begins. It replaces the mobile shell's top
// panel, reserves its height for maximized windows (strut), is transparent with white icons over the launcher and
// black over an app, and hides under a fullscreen app.
import Clutter from 'gi://Clutter';
import Cogl from 'gi://Cogl';
import Gio from 'gi://Gio';
import GLib from 'gi://GLib';
import GnomeDesktop from 'gi://GnomeDesktop';
import GObject from 'gi://GObject';
import Meta from 'gi://Meta';
import St from 'gi://St';

import * as Main from 'resource:///org/gnome/shell/ui/main.js';

import {paintIcon} from './icons.js';
import {Color, cogl, font, setSource, Type} from './palette.js';
import {px, screenPixels, SCALE} from './units.js';

// --- geometry, screen px of the 1080x2280 reference (spec §1)
export const BAR_HEIGHT = 116;
const CLOCK_INK_X = 26;                 // glyph left edge
const CLOCK_INK_TOP = 40;               // digit top
const NOTIF_GAP = 22;                   // clock ink to the first icon's box
const NOTIF_ICON = 45;                  // 15 dp drawing size
const NOTIF_GLYPH = 38;                 // a 16 px symbolic icon inks what a 24 dp Android one does at 45
const NOTIF_PITCH = 71;
const NOTIF_MAX = 4;                    // then the overflow dot
const NOTIF_ALPHA = 0.9;
const DOT_RADIUS = 6;                   // 2 sp
const CENTER_Y = 57.5;
const SYSTEM_END = 889;                 // the battery's right edge, the cutout begins here
const ICON_GAP = 19;                    // between signal, Wi-Fi and battery
const ICON_H = 36;                      // signal and Wi-Fi are 12 sp tall
const SIGNAL_W = 51;                    // 17 sp: 4 bars, 2 sp apart, 6 sp + 2 sp per level tall
const SIGNAL_PAD = 6;
const SIGNAL_BASE = 18;
const SIGNAL_STEP = 6;
const WIFI_W = ICON_H * 16.21 / 12;     // ic_wifi_N viewport 16.21 x 12
const BATTERY_H = 39;                   // 13 sp; the body is 24 x 13 vp, r 4, so 1 vp = 3 px
const BATTERY_VP = BATTERY_H / 13;

const FILL = {
    default: [255, 255, 255],
    charging: [0x18, 0xcc, 0x47],
    error: [0xff, 0x0e, 0x01],
    powerSave: [0xff, 0xc9, 0x17],
};

// The charging bolt and the power-save plus (BatteryGlyph.kt), viewport units
const BOLT = {w: 8, h: 9, d: 'M7.672,3.375L4.302,3.375L5.038,0.563C5.113,0.281 4.91,0 4.633,0C4.515,0 4.398,0.056 ' +
    '4.324,0.146L0.09,5.051C-0.102,5.276 0.047,5.625 0.335,5.625L3.705,5.625L2.969,8.438C2.895,8.719 3.097,9 3.374,' +
    '9C3.492,9 3.609,8.944 3.684,8.854L7.917,3.949C8.109,3.724 7.96,3.375 7.672,3.375Z'};
const PLUS = {w: 8.5, h: 8.5, d: 'M4.248,0C4.745,0 5.148,0.403 5.148,0.9V3.35H7.6C8.097,3.35 8.5,3.753 8.5,4.25C8.5,' +
    '4.747 8.097,5.149 7.6,5.149H5.148V7.6C5.148,8.097 4.745,8.5 4.248,8.5C3.751,8.5 3.349,8.097 3.349,7.6V5.149H0.9C' +
    '0.403,5.149 0,4.747 0,4.25C0,3.753 0.403,3.35 0.9,3.35H3.349V0.9C3.349,0.403 3.751,0 4.248,0Z'};

// A minimal path tracer for the two glyphs above (absolute M L H V C Z)
function traceSvg(cr, d, x0, y0, s) {
    const tokens = d.match(/[MLHVCZ]|-?[0-9]*\.?[0-9]+/g);
    let cmd = '', i = 0, cx = 0, cy = 0;
    const num = () => parseFloat(tokens[i++]);
    cr.newPath();
    while (i < tokens.length) {
        if (/[MLHVCZ]/.test(tokens[i]))
            cmd = tokens[i++];
        switch (cmd) {
        case 'M':
            cx = num();
            cy = num();
            cr.moveTo(x0 + cx * s, y0 + cy * s);
            cmd = 'L';
            break;
        case 'L':
            cx = num();
            cy = num();
            cr.lineTo(x0 + cx * s, y0 + cy * s);
            break;
        case 'H':
            cx = num();
            cr.lineTo(x0 + cx * s, y0 + cy * s);
            break;
        case 'V':
            cy = num();
            cr.lineTo(x0 + cx * s, y0 + cy * s);
            break;
        case 'C': {
            const c = [num(), num(), num(), num(), num(), num()];
            cr.curveTo(x0 + c[0] * s, y0 + c[1] * s, x0 + c[2] * s, y0 + c[3] * s, x0 + c[4] * s, y0 + c[5] * s);
            [cx, cy] = [c[4], c[5]];
            break;
        }
        case 'Z':
            cr.closePath();
            break;
        }
    }
}

function roundRect(cr, x, y, w, h, r) {
    r = Math.min(r, w / 2, h / 2);
    cr.newSubPath();
    cr.arc(x + w - r, y + r, r, -Math.PI / 2, 0);
    cr.arc(x + w - r, y + h - r, r, 0, Math.PI / 2);
    cr.arc(x + r, y + h - r, r, Math.PI / 2, Math.PI);
    cr.arc(x + r, y + r, r, Math.PI, 1.5 * Math.PI);
    cr.closePath();
}

// The 4-bar mobile signal (MobileIcon.kt): bars of the current level white, the others at 30 %
export function paintSignal(cr, x, bottom, level, color, alpha = 1) {
    const barW = (SIGNAL_W - SIGNAL_PAD * 3) / 4;
    for (let bar = 1; bar <= 4; bar++) {
        const h = SIGNAL_BASE + SIGNAL_STEP * (bar - 1);
        roundRect(cr, x + (bar - 1) * (barW + SIGNAL_PAD), bottom - h, barW, h, barW / 2);
        setSource(cr, color, alpha * (level >= bar ? 1 : 0.3));
        cr.fill();
    }
}

// The Wi-Fi fan, ic_wifi_0..3 (the new icons squash 5 levels into 4: 3 -> 2, 4 -> 3)
export function paintWifi(cr, x, top, level, color, alpha = 1) {
    const drawn = [0, 1, 2, 2, 3][level] ?? 0;
    paintIcon(cr, `ic_wifi_${drawn}`, x, top - (WIFI_W - ICON_H) / 2, WIFI_W, color, alpha);
}

// The unified battery (UnifiedBattery.kt, BatteryFrame.kt): a 24 x 13 rounded body filled to the level, then the
// cap, or an attribution glyph (bolt, plus) overlapping the body by 20 % with a clear 2 vp stroke around it.
// Returns the width drawn.
export function paintBattery(cr, x, top, battery, powerSave, scale = BATTERY_VP) {
    const charging = battery.plugged;
    const attribution = powerSave ? PLUS : charging ? BOLT : null;
    const profile = powerSave ? 'powerSave' : charging ? 'charging' : battery.level <= 20 ? 'error' : 'default';
    const full = battery.level >= 100;
    const bodyW = 24 * scale, bodyH = 13 * scale;

    cr.pushGroup();
    // body: background, then the fill clipped to the level
    roundRect(cr, x, top, bodyW, bodyH, 4 * scale);
    if (full) {
        setSource(cr, FILL[profile]);
        cr.fill();
    } else {
        cr.setSourceRGBA(1, 1, 1, attribution ? 0.55 : 0.45);
        cr.fillPreserve();
        cr.save();
        cr.clip();
        cr.rectangle(x, top, Math.ceil(battery.level / 100 * 24) * scale, bodyH);
        cr.clip();
        roundRect(cr, x, top, bodyW, bodyH, 4 * scale);
        setSource(cr, FILL[profile]);
        cr.fill();
        cr.restore();
    }

    let width;
    if (attribution) {
        const gw = attribution.w * scale, gh = attribution.h * scale;
        const gx = x + bodyW - Math.round(0.2 * gw);
        const gy = top + Math.round((bodyH - gh) / 2);
        traceSvg(cr, attribution.d, gx, gy, scale);
        cr.setOperator(0);                            // CLEAR
        cr.setLineWidth(2 * scale);
        cr.setLineJoin(1);
        cr.strokePreserve();
        cr.setOperator(2);                            // OVER
        cr.setSourceRGBA(1, 1, 1, 1);
        cr.fill();
        width = bodyW + Math.round(gw * 0.8);
    } else {
        // the cap: 1.5 x 6.03 vp after a 1 vp gap
        const capX = x + bodyW + Math.round(scale);
        const capH = 6.03 * scale, capW = 1.5 * scale;
        const capY = top + Math.round((bodyH - capH) / 2);
        cr.newPath();
        cr.moveTo(capX, capY);
        cr.lineTo(capX + capW - 1.167 * scale, capY);
        cr.arc(capX + capW - 1.167 * scale, capY + 1.167 * scale, 1.167 * scale, -Math.PI / 2, 0);
        cr.arc(capX + capW - 1.167 * scale, capY + capH - 1.167 * scale, 1.167 * scale, 0, Math.PI / 2);
        cr.lineTo(capX, capY + capH);
        cr.closePath();
        if (full)
            setSource(cr, FILL[profile]);
        else
            cr.setSourceRGBA(1, 1, 1, 0.45);
        cr.fill();
        width = bodyW + Math.round(scale) + Math.round(capW);
    }
    cr.popGroupToSource();
    cr.paint();
    return width;
}

export const BATTERY_WIDTH = battery => battery.plugged
    ? 24 * BATTERY_VP + Math.round(8 * BATTERY_VP * 0.8) : 24 * BATTERY_VP + Math.round(BATTERY_VP) + 1.5 * BATTERY_VP;

// The ink rectangle of a Clutter.Text in its own logical pixels (its Pango layout is at the resource scale)
export function inkBox(text) {
    const [ink] = text.get_layout().get_extents();
    const k = 1024 * text.get_resource_scale();
    return {x: ink.x / k, y: ink.y / k, width: ink.width / k, height: ink.height / k};
}

// Paints colour-icon notification icons white, keeping their alpha (Android tints the small icons)
const WhiteMaskEffect = GObject.registerClass(
class WhiteMaskEffect extends Clutter.ShaderEffect {
    vfunc_get_static_snippet() {
        return Cogl.Snippet.new(Cogl.SnippetHook.FRAGMENT, '',
            'cogl_color_out = vec4(cogl_color_out.a);');
    }
});

let iconTheme = null;

// The -symbolic variant of a themed icon when the theme has one, else null
export function symbolicIcon(gicon) {
    if (!(gicon instanceof Gio.ThemedIcon))
        return null;
    // a symbolic name may be one of the shell's own icons, which the icon theme does not list
    if (gicon.get_names()[0]?.endsWith('-symbolic'))
        return gicon;
    iconTheme ??= new St.IconTheme();
    const name = gicon.get_names().map(n => n.endsWith('-symbolic') ? n : `${n}-symbolic`)
        .find(n => iconTheme.has_icon(n));
    return name ? new Gio.ThemedIcon({name}) : null;
}

// The notification icon for a source: its -symbolic variant when the theme has one, else the icon masked white
export function notificationIcon(source, size) {
    const symbolic = symbolicIcon(source.icon);
    const icon = new St.Icon({
        gicon: symbolic ?? source.icon,
        icon_size: size,
        style: 'color: white;',
        opacity: Math.round(NOTIF_ALPHA * 255),
    });
    if (!symbolic)
        icon.add_effect(new WhiteMaskEffect());
    return icon;
}

// The right-hand system icons: mobile signal, Wi-Fi, battery, right-aligned at @end (screen px) and centred on
// @centerY; the shade header draws the same cluster
export function paintSystemIcons(cr, state, end, centerY) {
    const {wifi, mobile, battery, powerSave} = state;
    let x = end;
    if (battery.present) {
        x -= BATTERY_WIDTH(battery);
        paintBattery(cr, x, centerY - BATTERY_H / 2, battery, !!powerSave);
        x -= ICON_GAP;
    }
    if (wifi.connected) {
        x -= WIFI_W;
        paintWifi(cr, x, centerY - ICON_H / 2, wifi.level, Color.white);
        x -= ICON_GAP;
    }
    if (mobile.present) {
        x -= SIGNAL_W;
        paintSignal(cr, x, centerY + ICON_H / 2, mobile.level, Color.white);
    }
}

export const SystemIcons = GObject.registerClass(
class SystemIcons extends St.DrawingArea {
    // @end, @centerY in screen px of the area
    constructor(state, {end = SYSTEM_END, centerY = CENTER_Y, height = BAR_HEIGHT} = {}) {
        super({x: 0, y: 0, width: px(1080), height: px(height)});
        this._state = state;
        this._end = end;
        this._centerY = centerY;
        state.connectObject('changed', () => this.queue_repaint(), this);
    }

    vfunc_repaint() {
        const cr = this.get_context();
        screenPixels(cr);
        paintSystemIcons(cr, this._state, this._end, this._centerY);
        cr.$dispose();
    }
});

export const StatusBar = GObject.registerClass(
class StatusBar extends St.Widget {
    constructor(state, home) {
        const monitor = Main.layoutManager.primaryMonitor;
        super({
            name: 'gnomePhoneStatusBar',
            reactive: true,
            x: monitor.x,
            y: monitor.y,
            width: monitor.width,
            // the strut is whole logical pixels: 47 = 117.5 px, the bar's content is laid out on 116
            height: Math.ceil(px(BAR_HEIGHT)),
        });
        this._state = state;
        this._home = home;

        // black over apps, nothing over the launcher
        this._background = new St.Widget({
            style: 'background-color: black;',
            opacity: 0,
            width: this.width,
            height: this.height,
        });
        this.add_child(this._background);

        this._clock = new Clutter.Text({
            font_name: font(...Type.labelLargeEmphasized),
            color: cogl(Color.white),
        });
        this.add_child(this._clock);
        this._wallClock = new GnomeDesktop.WallClock({time_only: true});
        this._wallClock.connectObject('notify::clock', () => this._syncClock(), this);
        this._syncClock();

        this._notifications = new St.Widget({x: 0, y: 0});
        this.add_child(this._notifications);

        this._system = new SystemIcons(state);
        this.add_child(this._system);

        this._sources = new Set();
        Main.messageTray.connectObject(
            'source-added', (t, source) => this._addSource(source),
            'source-removed', (t, source) => this._removeSource(source),
            this);
        for (const source of Main.messageTray.getSources())
            this._addSource(source);

        global.display.connectObject(
            'restacked', () => this._queueSyncMode(),
            'in-fullscreen-changed', () => this._queueSyncMode(),
            'notify::focus-window', () => this._queueSyncMode(),
            this);
        global.window_manager.connectObject(
            'minimize', () => this._queueSyncMode(),
            'unminimize', () => this._queueSyncMode(),
            'map', () => this._queueSyncMode(),
            'destroy', () => this._queueSyncMode(),
            this);
        this._overAppOverride = null;
        this._syncMode();
    }

    // the time in the reference's format, 24 h with a leading zero ("05:09")
    _syncClock() {
        const now = GLib.DateTime.new_now_local();
        this._clock.text = now.format('%H:%M');
        // place the digits' ink, not the text box: digit tops at y 40, the first glyph at x 26
        const ink = inkBox(this._clock);
        this._clock.set_position(px(CLOCK_INK_X) - ink.x, px(CLOCK_INK_TOP) - ink.y);
        this._clockInkRight = CLOCK_INK_X + ink.width * SCALE;
        this._layoutNotifications();
    }

    // For gestures that move windows (the home gesture): true or false overrides whether the bar draws as over an
    // app (black) or over the launcher (clear); null follows the windows again
    setOverApp(value) {
        this._overAppOverride = value;
        this._syncMode();
    }

    // an app (not the launcher) is on screen under the bar
    get overApp() {
        return !!this._overApp;
    }

    get clockInkRight() {
        return this._clockInkRight;
    }

    // --- notification icons, one per source with a non-silent notification

    _addSource(source) {
        if (this._sources.has(source))
            return;
        this._sources.add(source);
        source.connectObject(
            'notification-added', () => this._syncNotifications(),
            'notification-removed', () => this._syncNotifications(),
            'notify::icon', () => this._syncNotifications(),
            this);
        this._syncNotifications();
    }

    _removeSource(source) {
        source.disconnectObject(this);
        this._sources.delete(source);
        this._syncNotifications();
    }

    _syncNotifications() {
        this._notifications.destroy_all_children();
        const sources = [...this._sources].filter(s =>
            s.notifications.some(n => n.urgency !== 0 /* LOW: silent */));
        const size = px(NOTIF_GLYPH);
        sources.slice(0, NOTIF_MAX).forEach(source => {
            const icon = notificationIcon(source, size);
            this._notifications.add_child(icon);
        });
        if (sources.length > NOTIF_MAX) {
            const dot = new St.DrawingArea({width: px(DOT_RADIUS * 2), height: px(DOT_RADIUS * 2)});
            dot.connect('repaint', area => {
                const cr = area.get_context();
                const [w, h] = area.get_surface_size();
                cr.arc(w / 2, h / 2, w / 2, 0, 2 * Math.PI);
                cr.setSourceRGBA(1, 1, 1, NOTIF_ALPHA);
                cr.fill();
                cr.$dispose();
            });
            this._notifications.add_child(dot);
        }
        this._layoutNotifications();
    }

    _layoutNotifications() {
        if (!this._notifications)
            return;
        let x = this._clockInkRight + NOTIF_GAP;
        for (const child of this._notifications) {
            if (child instanceof St.Icon) {
                child.set_position(px(x + (NOTIF_ICON - NOTIF_GLYPH) / 2), px(CENTER_Y - NOTIF_GLYPH / 2));
                x += NOTIF_PITCH;
            } else {
                child.set_position(px(x), px(CENTER_Y - DOT_RADIUS));
                x += DOT_RADIUS * 2;
            }
        }
    }

    // --- over the launcher, over an app, under a fullscreen app

    _queueSyncMode() {
        if (this._syncModeId)
            return;
        this._syncModeId = global.compositor.get_laters().add(Meta.LaterType.BEFORE_REDRAW, () => {
            this._syncModeId = 0;
            this._syncMode();
            return GLib.SOURCE_REMOVE;
        });
    }

    _syncMode() {
        const workspace = global.workspace_manager.get_active_workspace();
        const apps = workspace.list_windows().filter(w =>
            !w.minimized && !this._home.isHome(w) && w.get_window_type() === Meta.WindowType.NORMAL);
        const overApp = this._overAppOverride ?? apps.length > 0;
        const fullscreen = apps.some(w => w.is_fullscreen());
        // an ease is 1.5-7 ms of the shell's frame here, and this runs on every restack and focus change
        if (overApp !== this._overApp) {
            this._background.ease({
                opacity: overApp ? 255 : 0,
                duration: 200,
                mode: Clutter.AnimationMode.EASE_OUT_QUAD,
            });
        }
        this._overApp = overApp;
        this.visible = !fullscreen;
    }

    destroy() {
        if (this._syncModeId)
            global.compositor.get_laters().remove(this._syncModeId);
        for (const source of this._sources)
            source.disconnectObject(this);
        this._sources.clear();
        Main.messageTray.disconnectObject(this);
        global.display.disconnectObject(this);
        global.window_manager.disconnectObject(this);
        this._wallClock.disconnectObject(this);
        this._wallClock = null;
        super.destroy();
    }
});

// Takes the mobile shell's top panel out: hidden, no strut, no fullscreen tracking. Returns the undo.
export function hideShellPanel() {
    const layout = Main.layoutManager;
    const data = layout._trackedActors.find(a => a.actor === layout.panelBox);
    const saved = data ? {affectsStruts: data.affectsStruts, trackFullscreen: data.trackFullscreen} : null;
    if (data) {
        data.affectsStruts = false;
        data.trackFullscreen = false;
    }
    layout.panelBox.hide();
    layout._queueUpdateRegions();
    return () => {
        if (data)
            Object.assign(data, saved);
        layout.panelBox.show();
        layout._updateVisibility();
        layout._queueUpdateRegions();
    };
}
