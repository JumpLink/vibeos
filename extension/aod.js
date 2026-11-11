// The always-on display, and the panel's low power mode under it: Phosh's AOD (Neo's aod.c) as the owner signed it
// off, moved into the shell.
//
// While the screen is "off" with de.yesman.neo aod on, power.js's black layer shows this: the date, one icon per app
// with notifications, and the battery at the bottom where the lock screen says it charges, dim grey on black, which costs next to nothing on OLED. With a panel that
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
const CLOCK_TRIES = 3;                  // failed starts in a row before the time stays for good
const MAX_ICONS = 6;
// the battery line's centre: the lock screen's charging line (keyguard.js INDICATION_INK_TOP 2083, half its cap
// height of 32 below), screen px
const BATTERY_CENTRE_Y = 2099;
const BATTERY_ROW_H = 60;               // the icon's height; the label is centred on it
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

    // the hands' image goes to the panel again at the next start
    reloadClock() {
        if (this._clock)
            this._write(this._clock, 'reload');
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
        this._clockFailures = 0;        // in a row
        this._running = false;          // the panel took its clock
        this._utcOffset = 0;
        this._startId = 0;
        this._step = 0;
        this._replace = false;          // the dial moved: the hands are checked once it is laid out
        this._placed = null;            // where the panel's hands are centred, in panel pixels

        // the clock near the top, under the camera hole, the date and the app icons under it; the battery at the
        // bottom (sizes in screen px: they were set as logical px at scale 2.5)
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

        this._icons = new St.BoxLayout({style: `spacing: ${px(25)}px;`, x_align: Clutter.ActorAlign.CENTER,
            margin_top: Math.round(px(45))});

        this._battery = new St.BoxLayout({style: `spacing: ${px(15)}px;`, x_align: Clutter.ActorAlign.CENTER,
            height: px(BATTERY_ROW_H)});
        this._batteryIcon = new St.Icon({icon_size: Math.round(px(60)), style: `color: ${GREY};`,
            y_align: Clutter.ActorAlign.CENTER});
        this._batteryLabel = new St.Label({style: `color: ${GREY}; font-size: ${px(37.5)}px;`,
            y_align: Clutter.ActorAlign.CENTER});
        this._battery.add_child(this._batteryIcon);
        this._battery.add_child(this._batteryLabel);

        this._box.add_child(this._time);
        this._box.add_child(this._dial);
        this._box.add_child(this._date);
        this._box.add_child(this._icons);
        // top to bottom: the hole's room, the clock's column, the room left, the battery, what is under it
        this.add_child(new St.Widget({height: TOP}));
        this.add_child(this._box);
        this.add_child(new St.Widget({y_expand: true}));
        this.add_child(this._battery);
        this.add_child(new St.Widget({height: px(monitor.height * SCALE - BATTERY_CENTRE_Y - BATTERY_ROW_H / 2)}));
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
        // a start the panel refused gets another go each time the screen goes off: the first AOD after a boot
        // timed out on its DSI command after the hands' upload (2026-11-09), and without the clock the AOD stayed
        // the time, outside the panel's low power mode and so brighter, until the shell restarted. The hands go up
        // again too: the image from that start came out as noise (the panel driver's upload after a boot, again)
        if (!this._useClock && this._panel.hasClock && this._clockFailures < CLOCK_TRIES) {
            this._panel.reloadClock();
            this._useClock = true;
            this._showPanelClock(true);
        }
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
        this._moveTo(dx * DRIFT, dy * DRIFT);
    }

    // The content to (dx, dy), with the panel's hands. The hands go first and the dial follows in the next frame: the
    // other way round, the frame with the moved dial reached the panel long before the hands did, and they sat off
    // the dial's centre for a moment (2026-11-08)
    async _moveTo(dx, dy) {
        const fromX = this._box.margin_left - SHIFT, fromY = this._box.margin_top - SHIFT;
        if (fromX === dx && fromY === dy)
            return;
        const centre = this._running && this._dialCentre();
        if (centre) {
            const scale = global.stage.peek_stage_views()[0].get_scale();
            const placed = await this._placeClock(centre[0] + Math.round((dx - fromX) * scale),
                centre[1] + Math.round((dy - fromY) * scale));
            if (!placed && this._running)
                this._clockFailed();
        }
        if (this._shift(dx, dy))
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
        // the battery line wanders as far, apart from the column
        this._battery.set_translation(dx, dy, 0);
        return true;
    }

    _syncBattery() {
        const {battery} = this._state;
        this._batteryIcon.icon_name = battery.icon;
        // plugged in, how fast it charges, as the lock screen says it
        this._batteryLabel.text = battery.plugged && battery.level < 100
            ? `${battery.level}% • ${chargingText(battery.speed, battery.amps)}` : `${battery.level}%`;
        this._battery.visible = battery.present;
    }

    _addSource(source) {
        source.connectObject(
            'notification-added', () => this._syncIcons(),
            'notification-removed', () => this._syncIcons(), this);
        this._syncIcons();
    }

    // one icon per app with notifications, of the apps allowed on the lock screen (as keyguard.js's pill)
    _syncIcons() {
        this._icons.destroy_all_children();
        const seen = new Set();
        for (const source of Main.messageTray.getSources()) {
            if (seen.size >= MAX_ICONS)
                break;
            const key = source.icon?.to_string();
            if (!source.notifications.length || !source.policy.showInLockScreen || !key || seen.has(key))
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
        if (await this._placeClock(...centre))
            this._clockFailures = 0;
        else
            this._clockFailed();
    }

    _placeClock(x, y) {
        this._placed = [x, y];
        return this._panel.startClock(x, y, this._utcOffset);
    }

    // the dial is laid out somewhere new: if it landed a pixel away from where the hands went ahead, they follow
    _onMoved() {
        if (!this._replace || !this._running)
            return;
        this._replace = false;
        // after this layout, when the dial's position is its new one
        GLib.idle_add(GLib.PRIORITY_DEFAULT, () => {
            const centre = this._running && this._dialCentre();
            if (centre && (centre[0] !== this._placed?.[0] || centre[1] !== this._placed?.[1]))
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
        this._clockFailures++;
        this._running = false;
        this._showPanelClock(false);
        this._shift(0, 0);
        log(`gnome-phone: the panel's clock is off, showing the time (${this._clockFailures} of ${CLOCK_TRIES})`);
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
