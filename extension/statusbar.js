// The status bar: 116 px tall, clock and notification icons on the left, mobile signal, Wi-Fi and the
// unified battery on the right, ending where a camera hole at the bar's end begins (the S10e's), or clear of a punch
// hole in its middle (the S20 FE's). A tablet's is 84 px tall whichever way it is turned, as a phone's turned one.
// It replaces the mobile shell's top
// panel, reserves its height for maximized windows (strut), is transparent with white icons over the launcher and
// black over an app, and hides under a fullscreen app.
import Clutter from 'gi://Clutter';
import Gio from 'gi://Gio';
import GLib from 'gi://GLib';
import GnomeDesktop from 'gi://GnomeDesktop';
import GObject from 'gi://GObject';
import Meta from 'gi://Meta';
import Pango from 'gi://Pango';
import PangoCairo from 'gi://PangoCairo';
import St from 'gi://St';

import * as Main from 'resource:///org/gnome/shell/ui/main.js';

import {appCircle, sourceApp} from './appicon.js';
import {Canvas, paintIcon} from './icons.js';
import {Color, cogl, font, setSource, Type} from './palette.js';
import {px, screenPixels, SCALE, SCREEN_W, TABLET} from './units.js';

// --- geometry, screen px of the 1080x2280 reference
export const BAR_HEIGHT = 116;
const CLOCK_INK_X = 26;                 // glyph left edge
const CLOCK_INK_TOP = 40;               // digit top
const NOTIF_GAP = 22;                   // clock ink to the first icon's box
const NOTIF_ICON = 45;
const NOTIF_GLYPH = 38;                 // smaller: a symbolic icon has less padding, so it inks as much as the box
const NOTIF_PITCH = 71;
const NOTIF_MAX = 4;                    // then the overflow dot
const NOTIF_ALPHA = 0.9;
const DOT_RADIUS = 6;                   // the overflow dot
const CENTER_Y = 57.5;
const SYSTEM_END = 889;                 // the battery's right edge, the cutout begins here
const CUTOUT_GAP = 931 - SYSTEM_END;    // from there to the hole (cutout.js), on whichever side it is
// Turned (hole at the start): the bar is LAND_BAR_HEIGHT and its content LAND_RAISE higher. The clock starts
// PAD_START past the hole; at an end without one the battery stops OPEN_END short of the edge, in any rotation
// (StatusBarContentInsetsProvider: the hole's depth, else a minimum padding, plus the bar's own padding)
const LAND_BAR_HEIGHT = 84;
const LAND_RAISE = 16;
const PAD_START = 25;
const OPEN_END = 85;
const TABLET_END = 74;                  // a tablet's: the battery's charging bolt ends this far from the edge
export const SYSTEM_SPAN = 543;         // the microphone and camera off icons, Bluetooth, the data type, signal,
                                        // Wi-Fi and the widest battery (228) end to end, and room
const ICONS_TAIL = 2;                   // the battery's antialiased edge past its end
const ICON_GAP = 19;                    // between signal, Wi-Fi and battery
const ICON_H = 36;                      // signal and Wi-Fi
const SIGNAL_W = 51;                    // 4 bars, SIGNAL_PAD apart, SIGNAL_BASE tall and SIGNAL_STEP more a level
const SIGNAL_PAD = 6;
const SIGNAL_BASE = 18;
const SIGNAL_STEP = 6;
const WIFI_W = ICON_H * 16.21 / 12;     // ic_wifi_N viewport 16.21 x 12
const PRIVACY_W = 42;                   // ic_mic_off, ic_device_camera_off: 24 x 24 icons, the size Android draws them
const BLUETOOTH_W = 42;                 // stat_sys_data_bluetooth_connected: a 17 dp system icon, drawn as those are
// the network type left of the signal (5G, LTE, H+…): a technical label, the same in every language, its capitals as
// tall as the signal's third bar; DATA_TYPE_GAP from the signal, the widest ("LTE") in DATA_TYPE_SPAN's 72 px
const DATA_TYPE_FONT = 'Google Sans Flex, VibeOS Roboto 37px @wght=600,wdth=100,ROND=100,opsz=17';
const DATA_TYPE_GAP = 6;
const BATTERY_H = 39;                   // the body's vector is 24 x 13 units, r 4: BATTERY_VP px a unit
const BATTERY_VP = BATTERY_H / 13;

// Where a hole sits in a status row W px wide and @height tall, from its box there ([x, y, width, height], screen
// px): 'start' (the clock clears it), 'end' (the system icons stop CUTOUT_GAP before it), 'middle' (a punch hole
// near the centre: the clock and the notification icons stay left of it and the system icons end where they would
// without one, as Android's StatusBarContentInsetsProvider splits the bar around it), or null (not in the row)
export function holePlace([x, y, w, h], W, height = BAR_HEIGHT) {
    if (h <= 0 || y >= height)
        return null;
    if (x + w < W / 3)
        return 'start';
    if (x > W * 2 / 3)
        return 'end';
    return 'middle';
}

// Where the system icons end in a status row W px wide, screen px: CUTOUT_GAP before a hole at the row's end, else
// OPEN_END short of the edge (TABLET_END on a tablet). Upright on the S10e (hole from 931) 889, on the S20 FE (hole
// in the middle) 995.
export function systemEnd(hole, W, height = BAR_HEIGHT) {
    return holePlace(hole, W, height) === 'end' ? hole[0] - CUTOUT_GAP : W - (TABLET ? TABLET_END : OPEN_END);
}

const FILL = {
    default: [255, 255, 255],
    charging: [0x18, 0xcc, 0x47],
    error: [0xff, 0x0e, 0x01],
    powerSave: [0xff, 0xc9, 0x17],
};

// The charging bolt and the power-save plus, viewport units
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

// The 4-bar mobile signal: bars of the current level white, the others at 30 %
export function paintSignal(cr, x, bottom, level, color, alpha = 1) {
    const barW = (SIGNAL_W - SIGNAL_PAD * 3) / 4;
    for (let bar = 1; bar <= 4; bar++) {
        const h = SIGNAL_BASE + SIGNAL_STEP * (bar - 1);
        roundRect(cr, x + (bar - 1) * (barW + SIGNAL_PAD), bottom - h, barW, h, barW / 2);
        setSource(cr, color, alpha * (level >= bar ? 1 : 0.3));
        cr.fill();
    }
}

// The network type, right-aligned at @right and its capitals centred on @centerY. Returns the width drawn.
let dataTypeFont = null;
export function paintDataType(cr, text, right, centerY, color, alpha = 1) {
    dataTypeFont ??= Pango.FontDescription.from_string(DATA_TYPE_FONT);
    const layout = PangoCairo.create_layout(cr);
    layout.set_font_description(dataTypeFont);
    layout.set_text(text, -1);
    const [ink] = layout.get_pixel_extents();
    cr.moveTo(Math.round(right - ink.x - ink.width), Math.round(centerY - ink.y - ink.height / 2));
    setSource(cr, color, alpha);
    PangoCairo.show_layout(cr, layout);
    cr.newPath();
    return ink.width;
}

// The Wi-Fi fan, ic_wifi_0..3 (the new icons squash 5 levels into 4: 3 -> 2, 4 -> 3)
export function paintWifi(cr, x, top, level, color, alpha = 1) {
    const drawn = [0, 1, 2, 2, 3][level] ?? 0;
    paintIcon(cr, `ic_wifi_${drawn}`, x, top - (WIFI_W - ICON_H) / 2, WIFI_W, color, alpha);
}

// The battery: a 24 x 13 rounded body filled to the level, then the
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

// The notification icon for a source, @size logical px: the app's logo in a circle, as the shade's rows draw it
// (appicon.js); a source with no app (the shell's own, a D-Bus client with no desktop file) its -symbolic icon in
// @glyph (a CSS colour) when the theme has one, else its own icon in the circle too. Masked white, a colour icon was a
// white blob: a square for every tile. @grey: the circle in greys (the always-on display)
export function notificationIcon(source, size, {glyph = 'white', grey = false} = {}) {
    const app = sourceApp(source);
    // a source with no icon (a D-Bus client with no desktop file, Waydroid's apps): the fallback the shade's rows
    // show. Not a notification's own picture: the one icon stands for all of the source's notifications
    const gicon = app?.get_icon() ?? source.icon ?? new Gio.ThemedIcon({name: 'dialog-information'});
    // an app's logo, unless the app's icon is a symbolic one itself: drawn in the shell's light text colour, a white
    // disc would lose it
    const logo = app && !gicon.get_names?.()[0]?.endsWith('-symbolic');
    const symbolic = logo ? null : symbolicIcon(gicon);
    if (!symbolic) {
        const circle = appCircle(gicon, Math.round(size));
        if (grey)
            circle.add_effect(new Clutter.DesaturateEffect({factor: 1}));
        return circle;
    }
    return new St.Icon({
        gicon: symbolic,
        icon_size: size,
        style: `color: ${glyph};`,
        opacity: Math.round(NOTIF_ALPHA * 255),
    });
}

// The right-hand system icons: Bluetooth, the data type and the mobile signal, Wi-Fi, battery, right-aligned at @end
// (screen px) and centred on @centerY; the shade header draws the same cluster
export function paintSystemIcons(cr, state, end, centerY) {
    const {wifi, mobile, battery, powerSave, micOff, cameraOff, bluetooth} = state;
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
        // while mobile data is connected, its network type, as Android draws it
        if (mobile.dataType)
            x -= DATA_TYPE_GAP + paintDataType(cr, mobile.dataType, x - DATA_TYPE_GAP, centerY, Color.white);
        x -= ICON_GAP;
    }
    // a device connected over Bluetooth (a car, headphones), as Android 12 on shows it: left of the mobile network,
    // or of Wi-Fi without one; nothing for Bluetooth that is only on
    if (bluetooth?.connected) {
        x -= BLUETOOTH_W;
        paintIcon(cr, 'stat_sys_data_bluetooth_connected', x, centerY - BLUETOOTH_W / 2, BLUETOOTH_W, Color.white);
        x -= ICON_GAP;
    }
    // Settings' Camera and Microphone switches off: no app gets them (privacy.js)
    for (const [off, icon] of [[cameraOff, 'ic_device_camera_off'], [micOff, 'ic_mic_off']]) {
        if (!off)
            continue;
        x -= PRIVACY_W;
        paintIcon(cr, icon, x, centerY - PRIVACY_W / 2, PRIVACY_W, Color.white);
        x -= ICON_GAP;
    }
}

// What paintSystemIcons draws. The state changes far more often than that (an access point's strength within a bar,
// UPower's energy rate), and a repaint is a texture across the whole bar converted on the CPU on its way to the GPU:
// 15 ms of the shell's frame, wherever it lands in an animation
function systemIconsKey({wifi, mobile, battery, powerSave, micOff, cameraOff, bluetooth}) {
    return [battery.present, battery.level, battery.plugged, !!powerSave, wifi.connected, wifi.level, mobile.present,
        mobile.level, mobile.dataType, !!micOff, !!cameraOff, !!bluetooth?.connected].join();
}

export const SystemIcons = GObject.registerClass(
class SystemIcons extends Canvas {
    // @end, @centerY in screen px of the area. With @span the canvas is only the @span px left of @end, not the whole
    // area from 0: a landscape bar is 2280 px, and the repaint costs by the width
    constructor(state, {end = SYSTEM_END, centerY = CENTER_Y, height = BAR_HEIGHT, span = 0} = {}) {
        super({x: 0, y: 0, width: px(SCREEN_W), height: px(height)});
        this._state = state;
        this._span = span;
        this._left = 0;
        this._centerY = centerY;
        this._drawn = null;
        this.setEnd(end);
        state.connectObject('changed', () => {
            if (systemIconsKey(state) !== this._drawn)
                this.queue_repaint();
        }, this);
    }

    // where the icons end and their vertical centre, screen px of the area
    get end() {
        return this._end;
    }

    get centerY() {
        return this._centerY;
    }

    setCenterY(centerY) {
        if (centerY === this._centerY)
            return;
        this._centerY = centerY;
        this.queue_repaint();
    }

    // where the icons end, screen px of the area; without a span the canvas runs from 0 to there
    setEnd(end) {
        this._end = end;
        if (this._span) {
            this._left = Math.max(0, end - this._span);
            this.set_position(px(this._left), 0);
            this.set_width(px(end + ICONS_TAIL - this._left));
        } else if (TABLET) {
            this.set_width(px(end + ICONS_TAIL));
        }
        this.queue_repaint();
    }

    draw(cr) {
        screenPixels(cr);
        cr.translate(-this._left, 0);
        paintSystemIcons(cr, this._state, this._end, this._centerY);
        this._drawn = systemIconsKey(this._state);
    }
});

export const StatusBar = GObject.registerClass({
    // the clock's text or the bar's layout changed (contentLine)
    Signals: {'content-changed': {}},
}, class StatusBar extends St.Widget {
    constructor(state, home, cutout) {
        const monitor = Main.layoutManager.primaryMonitor;
        super({
            name: 'vibeosStatusBar',
            reactive: true,
            x: monitor.x,
            y: monitor.y,
            width: monitor.width,
            // the strut is whole logical pixels (39 = 117 px at scale 3, 47 = 117.5 px at 2.5); the bar's content is
            // laid out on 116
            height: Math.ceil(px(BAR_HEIGHT)),
        });
        this._state = state;
        this._home = home;
        this._cutout = cutout;
        this._clockInkX = CLOCK_INK_X;
        this._notificationsEnd = Infinity;  // where the notification icons stop: before a hole in the bar's middle
        this._notificationCount = 0;        // sources with a non-silent notification
        this._raise = 0;                // how much higher than upright the content sits (turned: LAND_RAISE)

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
        // Settings' "24-hour time" (Date and time), as the lock screen follows it
        this._interface = new Gio.Settings({schema_id: 'org.gnome.desktop.interface'});
        this._interface.connectObject('changed::clock-format', () => this._syncClock(), this);
        this._syncClock();

        this._notifications = new St.Widget({x: 0, y: 0});
        this.add_child(this._notifications);

        this._system = new SystemIcons(state, {span: SYSTEM_SPAN});
        this.add_child(this._system);

        // turned with the panel: across its whole width, the clock after the hole when it is at the bar's start
        cutout.connectObject('changed', () => this._relayout(), this);
        Main.layoutManager.connectObject('monitors-changed', () => {
            this._relayout();
            this._queueSyncMode();      // the windows on its screen may be others now
        }, this);
        this._relayout();

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
            'window-entered-monitor', () => this._queueSyncMode(),
            'window-left-monitor', () => this._queueSyncMode(),
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

    // the time in the reference's format: 24 h with a leading zero ("05:09"), or 12 h as Android's status bar has
    // it, without a leading zero or AM/PM ("5:09")
    _syncClock() {
        const now = GLib.DateTime.new_now_local();
        const h24 = this._interface.get_string('clock-format') === '24h';
        this._clock.text = now.format(h24 ? '%H:%M' : '%-I:%M');
        // place the digits' ink, not the text box: digit tops at y 40, the first glyph at x 26
        const ink = inkBox(this._clock);
        this._clock.set_position(px(this._clockInkX) - ink.x, px(CLOCK_INK_TOP - this._raise) - ink.y);
        this._clockInkRight = this._clockInkX + ink.width * SCALE;
        this._layoutNotifications();
        this.emit('content-changed');
    }

    // Upright the hole is at the bar's end (the S10e: the battery ends CUTOUT_GAP before it) or in its middle (the S20
    // FE: the notification icons stop CUTOUT_GAP before it, the battery ends as without one). Turned the bar is lower
    // and runs the panel's whole length, the hole at its start (the clock clears it), at its end (as upright) or on
    // an edge the bar is not on
    _relayout() {
        const m = Main.layoutManager.primaryMonitor;
        if (!m)
            return;
        const turned = TABLET || m.width > m.height;
        const height = turned ? LAND_BAR_HEIGHT : BAR_HEIGHT;
        this._raise = turned ? LAND_RAISE : 0;
        this.set_position(m.x, m.y);
        this.set_size(m.width, Math.ceil(px(height)));
        this._background.set_size(m.width, this.height);
        const hole = this._cutout.rect;
        const W = m.width * SCALE;
        const place = holePlace(hole, W, height);
        this._clockInkX = place === 'start' ? hole[0] + hole[2] + PAD_START : CLOCK_INK_X;
        this._notificationsEnd = place === 'middle' ? hole[0] - CUTOUT_GAP : Infinity;
        this._system.setEnd(systemEnd(hole, W, height));
        this._system.setCenterY(CENTER_Y - this._raise);
        this._syncClock();
    }

    // Where the clock and the system icons are, for a tablet's shade header to draw its own at the same place: the
    // clock's text and font, its ink's start and top, the icons' end and centre line (screen px)
    get contentLine() {
        return {
            clockText: this._clock.text,
            clockFont: this._clock.font_name,
            clockInkX: this._clockInkX,
            clockInkTop: CLOCK_INK_TOP - this._raise,
            systemEnd: this._system.end,
            centerY: this._system.centerY,
        };
    }

    // The clock and the system icons hidden while a tablet's shade draws them over the bar at the same place: one
    // set drawn over the other would thicken their edges
    setLineHidden(hidden) {
        this._clock.visible = !hidden;
        this._system.visible = !hidden;
    }

    // the bar's height in screen px: 116 upright, 84 turned or on a tablet
    get barHeight() {
        return this._raise ? LAND_BAR_HEIGHT : BAR_HEIGHT;
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

    // the camera hole the bar keeps clear of
    get cutout() {
        return this._cutout;
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
        this._notificationCount = sources.length;
        const size = px(NOTIF_GLYPH);
        sources.slice(0, NOTIF_MAX).forEach(source => {
            const icon = notificationIcon(source, size);
            this._notifications.add_child(icon);
        });
        // the dot stands for the sources without an icon (_layoutNotifications shows it)
        this._dot = null;
        if (sources.length > 0) {
            const dot = new Canvas({width: px(DOT_RADIUS * 2), height: px(DOT_RADIUS * 2)});
            dot.draw = (cr, w, h) => {
                cr.arc(w / 2, h / 2, w / 2, 0, 2 * Math.PI);
                cr.setSourceRGBA(1, 1, 1, NOTIF_ALPHA);
                cr.fill();
            };
            this._notifications.add_child(dot);
            this._dot = dot;
        }
        this._layoutNotifications();
    }

    // up to NOTIF_MAX icons after the clock, as many as end before _notificationsEnd, then the dot for the rest
    _layoutNotifications() {
        if (!this._notifications)
            return;
        const x0 = this._clockInkRight + NOTIF_GAP;
        const total = this._notificationCount;
        const right = n => n < total ? x0 + n * NOTIF_PITCH + DOT_RADIUS * 2 : x0 + (n - 1) * NOTIF_PITCH + NOTIF_ICON;
        let shown = Math.min(total, NOTIF_MAX);
        while (shown > 0 && right(shown) > this._notificationsEnd)
            shown--;
        const centerY = CENTER_Y - this._raise;
        let i = 0;
        for (const child of this._notifications) {
            if (child !== this._dot) {
                child.visible = i < shown;
                child.set_position(px(x0 + i * NOTIF_PITCH + (NOTIF_ICON - NOTIF_GLYPH) / 2),
                    px(centerY - NOTIF_GLYPH / 2));
                i++;
            } else {
                child.visible = shown < total && right(shown) <= this._notificationsEnd;
                child.set_position(px(x0 + shown * NOTIF_PITCH), px(centerY - DOT_RADIUS));
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
        // the windows on its own screen only: a fullscreen game on a plugged-in TV does not hide it on the device's
        // panel, nor does an app there make it black over the launcher
        const here = Main.layoutManager.primaryIndex;
        const workspace = global.workspace_manager.get_active_workspace();
        const apps = workspace.list_windows().filter(w =>
            !w.minimized && !this._home.isHome(w) && w.get_window_type() === Meta.WindowType.NORMAL &&
            w.get_monitor() === here);
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
        Main.layoutManager.disconnectObject(this);
        this._cutout.disconnectObject(this);
        global.display.disconnectObject(this);
        global.window_manager.disconnectObject(this);
        this._wallClock.disconnectObject(this);
        this._wallClock = null;
        this._interface.disconnectObject(this);
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
