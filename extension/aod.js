// The always-on display, and the panel's low power mode under it: Phosh's AOD (Neo's aod.c) as the owner signed it
// off, moved into the shell.
//
// While the screen is "off" with de.yesman.neo aod on, power.js's black layer shows this: the date, the battery and
// one icon per app with notifications, dim grey on black, which costs next to nothing on OLED. With a panel that
// keeps a clock of its own (the S10e's S6E3FA7: a `self_clock` attribute next to `lpm`), the AOD is Samsung's: once
// it shows, the panel goes into its low power mode and draws analog hands over the dial drawn here, ticking by itself
// while the phone sleeps, so nothing has to wake the phone for the time (s10e-sleepd sees the clock run). Against
// burn-in the content then moves a step every two hours, at the first wake after (s10e-sleepd's hourly one at the
// latest), and the hands go with it. If the panel refuses its clock, the AOD shows the time instead and wanders a
// little every minute.
import Clutter from 'gi://Clutter';
import Gio from 'gi://Gio';
import GLib from 'gi://GLib';
import GnomeDesktop from 'gi://GnomeDesktop';
import GObject from 'gi://GObject';
import St from 'gi://St';

import * as Main from 'resource:///org/gnome/shell/ui/main.js';

import {Canvas} from './icons.js';
import {chargingText} from './state.js';
import {notificationIcon} from './statusbar.js';
import {px, screenPixels, SCALE} from './units.js';

Gio._promisify(Gio.File.prototype, 'open_readwrite_async');
Gio._promisify(Gio.OutputStream.prototype, 'write_bytes_async');
Gio._promisify(Gio.IOStream.prototype, 'close_async');

const LPM_NITS = '10';
const SHIFT = 24;                       // px the content wanders, against burn-in
// with the panel's clock: a step of DRIFT px (9 panel pixels on the S10e) every DRIFT_PERIOD s, around a ring of 8
const DRIFT = 3;
const DRIFT_PERIOD = 2 * 60 * 60;
const RING = [[-1, -1], [0, -1], [1, -1], [1, 0], [1, 1], [0, 1], [-1, 1], [-1, 0]];
const TOP = 80;                         // under the camera hole
// the dial around the panel's hands, in panel pixels: they reach 207 px from the centre (s6e3fa7 hand image)
const DIAL_TICK_OUT = 236;
const DIAL_TICK_IN = 218;
const DIAL_QUARTER_IN = 204;
const DIAL_DOT = 9;
// the frame with the dial reaches the panel before its clock starts over it
const CLOCK_DELAY_MS = 300;
const MAX_ICONS = 6;
const GREY = '#8a8a8a';
const ICON_OPACITY = 0x8a;              // white icons over black: #8a8a8a

// --- the panel

function writable(path) {
    try {
        return Gio.File.new_for_path(path).query_info('access::can-write', Gio.FileQueryInfoFlags.NONE, null)
            .get_attribute_boolean('access::can-write');
    } catch (e) {
        return false;
    }
}

// the attribute files named @attr of the devices under @base that we may write
function findAttributes(base, attr) {
    const found = [];
    try {
        const children = Gio.File.new_for_path(base).enumerate_children('standard::name',
            Gio.FileQueryInfoFlags.NONE, null);
        for (let info; (info = children.next_file(null));) {
            const path = GLib.build_filenamev([base, info.get_name(), attr]);
            if (writable(path))
                found.push(path);
        }
    } catch (e) {
    }
    return found;
}

// The display panel's own power states, written to sysfs one at a time and in order: entering low power mode takes
// the panel ~150 ms, uploading the hands longer, and leaving it must not overtake a start still on its way
export class Panel {
    constructor() {
        // a udev rule gives both to the video group
        this._lpm = findAttributes('/sys/bus/mipi-dsi/devices', 'lpm')[0] ?? null;
        const clock = this._lpm && GLib.build_filenamev([GLib.path_get_dirname(this._lpm), 'self_clock']);
        this._clock = clock && writable(clock) ? clock : null;
        // touchscreens that wake on a double tap (s6sy761's wake_gesture)
        this._gestures = findAttributes('/sys/bus/i2c/devices', 'wake_gesture');
        this._queue = Promise.resolve(true);
        // unknown: the first call writes, whatever a crashed shell left behind
        this._lowPower = null;
        this._gesture = null;
    }

    get hasClock() {
        return this._clock !== null;
    }

    // resolves to whether the panel took it
    _write(path, value) {
        this._queue = this._queue.then(async () => {
            try {
                const stream = await Gio.File.new_for_path(path).open_readwrite_async(GLib.PRIORITY_DEFAULT, null);
                try {
                    // a GBytes: write_all_async does not keep a JS buffer alive for its thread, and the kernel got
                    // garbage (EINVAL)
                    await stream.get_output_stream().write_bytes_async(new GLib.Bytes(new TextEncoder().encode(value)),
                        GLib.PRIORITY_DEFAULT, null);
                } finally {
                    await stream.close_async(GLib.PRIORITY_DEFAULT, null);
                }
                return true;
            } catch (e) {
                log(`gnome-phone: ${path} <- ${value}: ${e.message}`);
                return false;
            }
        });
        return this._queue;
    }

    // the panel's low power mode, at its lowest brightness; leaving it stops the panel's clock
    lowPower(on) {
        if (!this._lpm || on === this._lowPower)
            return;
        this._lowPower = on;
        this._write(this._lpm, on ? LPM_NITS : '0');
    }

    // the panel's analog clock centred at @x, @y panel pixels; resolves to whether the panel took it
    startClock(x, y, utcOffset) {
        if (!this._clock)
            return Promise.resolve(false);
        this.lowPower(true);
        return this._write(this._clock, `analog ${x} ${y} ${utcOffset} 24`);
    }

    wakeGesture(on) {
        if (on === this._gesture)
            return;
        this._gesture = on;
        for (const path of this._gestures)
            this._write(path, on ? '1' : '0');
    }
}

// --- the AOD

function localUtcOffset() {
    return Math.round(GLib.DateTime.new_now_local().get_utc_offset() / 1e6);
}

// the dial's ticks and centre dot; the panel draws the hands over it
const Dial = GObject.registerClass(
class Dial extends Canvas {
    constructor() {
        const size = Math.ceil(2 * (DIAL_TICK_OUT + 6) / SCALE);
        super({width: size, height: size, x_align: Clutter.ActorAlign.CENTER});
    }

    draw(cr, width, height) {
        screenPixels(cr);
        const cx = width * SCALE / 2, cy = height * SCALE / 2;
        cr.setSourceRGB(0x6a / 255, 0x6a / 255, 0x6a / 255);
        cr.setLineCap(1);                              // ROUND
        for (let i = 0; i < 12; i++) {
            const a = i * Math.PI / 6, inner = i % 3 ? DIAL_TICK_IN : DIAL_QUARTER_IN;
            cr.setLineWidth(i % 3 ? 3 : 6);
            cr.moveTo(cx + inner * Math.sin(a), cy - inner * Math.cos(a));
            cr.lineTo(cx + DIAL_TICK_OUT * Math.sin(a), cy - DIAL_TICK_OUT * Math.cos(a));
            cr.stroke();
        }
        cr.arc(cx, cy, DIAL_DOT, 0, 2 * Math.PI);
        cr.fill();
    }
});

export const Aod = GObject.registerClass(
class Aod extends St.BoxLayout {
    // @panel: the Panel; @state: the SystemState, for the battery
    constructor(panel, state) {
        const monitor = Main.layoutManager.primaryMonitor;
        super({
            name: 'gnomePhoneAod',
            orientation: Clutter.Orientation.VERTICAL,
            width: monitor.width,
            height: monitor.height,
        });
        this._panel = panel;
        this._state = state;
        this._useClock = panel.hasClock;
        this._running = false;          // the panel took its clock
        this._utcOffset = 0;
        this._startId = 0;
        this._step = 0;
        this._replace = false;          // the dial moved: the hands follow once it is laid out

        // the clock near the top, under the camera hole; the date, the battery and the app icons under it
        // (sizes in screen px: they were set as logical px at scale 2.5)
        this._box = new St.BoxLayout({
            orientation: Clutter.Orientation.VERTICAL,
            style: `spacing: ${px(15)}px;`,
            x_expand: true,
            x_align: Clutter.ActorAlign.CENTER,
            y_align: Clutter.ActorAlign.START,
        });
        this._time = new St.Label({
            style: `color: ${GREY}; font-size: ${px(160)}px; font-weight: 300;`,
            x_align: Clutter.ActorAlign.CENTER,
        });
        this._dial = new Dial();
        this._date = new St.Label({style: `color: ${GREY}; font-size: ${px(42.5)}px;`, x_align: Clutter.ActorAlign.CENTER});

        const bottom = new St.BoxLayout({style: `spacing: ${px(30)}px;`, x_align: Clutter.ActorAlign.CENTER,
            margin_top: Math.round(px(45))});
        const battery = new St.BoxLayout({style: `spacing: ${px(15)}px;`});
        this._batteryIcon = new St.Icon({icon_size: Math.round(px(60)), style: `color: ${GREY};`, y_align: Clutter.ActorAlign.CENTER});
        this._batteryLabel = new St.Label({style: `color: ${GREY}; font-size: ${px(37.5)}px;`,
            y_align: Clutter.ActorAlign.CENTER});
        battery.add_child(this._batteryIcon);
        battery.add_child(this._batteryLabel);
        this._icons = new St.BoxLayout({style: `spacing: ${px(25)}px;`, y_align: Clutter.ActorAlign.CENTER});
        bottom.add_child(battery);
        bottom.add_child(this._icons);

        this._box.add_child(this._time);
        this._box.add_child(this._dial);
        this._box.add_child(this._date);
        this._box.add_child(bottom);
        this.add_child(new St.Widget({height: TOP}));
        this.add_child(this._box);
        this._box.connectObject('notify::allocation', () => this._onMoved(), this);

        this._wallClock = new GnomeDesktop.WallClock({time_only: true});
        this._wallClock.connectObject('notify::clock', () => this._tick(), this);
        state.connectObject('changed', () => this._syncBattery(), this);
        Main.messageTray.connectObject(
            'source-added', (_tray, source) => this._addSource(source),
            'source-removed', (_tray, source) => {
                source.disconnectObject(this);
                this._syncIcons();
            }, this);
        for (const source of Main.messageTray.getSources())
            this._addSource(source);

        this._showPanelClock(this._useClock);
        this.prepare();
    }

    // --- the content

    // what it shows, brought up to date as the screen goes off
    prepare() {
        this._tick();
        this._syncBattery();
        this._syncIcons();
    }

    _tick() {
        this._time.text = this._wallClock.clock;
        this._date.text = GLib.DateTime.new_now_local().format('%A, %B %-e');

        if (this._useClock) {
            // the content drifts every two hours; a new UTC offset (summer time) goes to the panel's clock
            this._drift();
            if (this._running && localUtcOffset() !== this._utcOffset)
                this._startClock();
            return;
        }
        // a slow walk around a small square, one step a minute
        this._step = (this._step + 1) % 16;
        this._shift((this._step % 4 - 2) * SHIFT / 2, (Math.floor(this._step / 4) - 2) * SHIFT / 2);
    }

    // the spot for now on a ring of eight around the middle; the clock checks it every minute, and the first minute
    // after a wake is the first after the spot's time
    _drift() {
        const now = Math.floor(GLib.get_real_time() / 1e6);
        const [dx, dy] = RING[Math.floor(now / DRIFT_PERIOD) % RING.length];
        if (this._shift(dx * DRIFT, dy * DRIFT))
            this._replace = true;
    }

    _shift(dx, dy) {
        const box = this._box;
        if (box.margin_left === SHIFT + dx && box.margin_top === SHIFT + dy)
            return false;
        box.margin_left = SHIFT + dx;
        box.margin_right = SHIFT - dx;
        box.margin_top = SHIFT + dy;
        box.margin_bottom = SHIFT - dy;
        return true;
    }

    _syncBattery() {
        const {battery} = this._state;
        this._batteryIcon.icon_name = battery.icon;
        // plugged in, how fast it charges, as One UI's AOD says it under the clock
        this._batteryLabel.text = battery.plugged && battery.level < 100
            ? `${battery.level}% • ${chargingText(battery.speed)}` : `${battery.level}%`;
        this._batteryIcon.get_parent().visible = battery.present;
    }

    _addSource(source) {
        source.connectObject(
            'notification-added', () => this._syncIcons(),
            'notification-removed', () => this._syncIcons(), this);
        this._syncIcons();
    }

    // one icon per app with notifications
    _syncIcons() {
        this._icons.destroy_all_children();
        const seen = new Set();
        for (const source of Main.messageTray.getSources()) {
            if (seen.size >= MAX_ICONS)
                break;
            const key = source.icon?.to_string();
            if (!source.notifications.length || !key || seen.has(key))
                continue;
            seen.add(key);
            const icon = notificationIcon(source, 24);
            icon.opacity = ICON_OPACITY;
            this._icons.add_child(icon);
        }
    }

    // --- the panel's clock

    _showPanelClock(on) {
        this._dial.visible = on;
        this._time.visible = !on;
    }

    // fully faded in: the panel's clock starts, after a moment for the frame with the dial to get there
    shown() {
        this._clearStart();
        if (!this._useClock || this._running)
            return;
        this._startId = GLib.timeout_add(GLib.PRIORITY_DEFAULT, CLOCK_DELAY_MS, () => {
            this._startId = 0;
            this._startClock();
            return GLib.SOURCE_REMOVE;
        });
    }

    // starting to go: the panel leaves its low power mode (power.js), which stops its clock
    hidden() {
        this._clearStart();
        this._running = false;
        this._replace = false;
    }

    _clearStart() {
        if (this._startId)
            GLib.source_remove(this._startId);
        this._startId = 0;
    }

    // the dial's centre in panel pixels, or null if the panel is not upright
    _dialCentre() {
        const view = global.stage.peek_stage_views()[0];
        if (!view || view.transform !== 0 || !this._dial.is_mapped())
            return null;
        const [x, y] = this._dial.get_transformed_position();
        const [width, height] = this._dial.get_transformed_size();
        const scale = view.get_scale();
        return [Math.round((x + width / 2) * scale), Math.round((y + height / 2) * scale)];
    }

    async _startClock() {
        const centre = this._dialCentre();
        if (!centre) {
            this._clockFailed();
            return;
        }
        this._utcOffset = localUtcOffset();
        this._running = true;
        if (!await this._panel.startClock(...centre, this._utcOffset))
            this._clockFailed();
    }

    // the dial is laid out somewhere new: the panel's hands go there too
    _onMoved() {
        if (!this._replace || !this._running)
            return;
        this._replace = false;
        // after this layout, when the dial's position is its new one
        GLib.idle_add(GLib.PRIORITY_DEFAULT, () => {
            if (this._running)
                this._startClock();
            return GLib.SOURCE_REMOVE;
        });
    }

    // the panel refused its clock (no hand image, say), or it cannot be placed: the time as before
    _clockFailed() {
        if (!this._useClock)
            return;
        this._clearStart();
        this._useClock = false;
        this._running = false;
        this._showPanelClock(false);
        this._shift(0, 0);
        log("gnome-phone: the panel's clock is off, showing the time");
    }

    destroy() {
        this._clearStart();
        this._wallClock.disconnectObject(this);
        this._wallClock = null;
        this._state.disconnectObject(this);
        Main.messageTray.disconnectObject(this);
        for (const source of Main.messageTray.getSources())
            source.disconnectObject(this);
        super.destroy();
    }
});
