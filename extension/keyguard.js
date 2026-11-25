// The keyguard: the lock screen over everything while the phone is locked, its bouncer, and the unlock
// (in screen px of the 1080x2280 reference). A tablet's is laid out as Android 17 lays out the Galaxy Tab S5e's,
// either way it is turned, and again whenever it turns.
//
// Locked, the shell is in the 'unlock-dialog' session mode, as GNOME's own screen shield puts it: windows hidden,
// gestures, heads-ups and keybindings off, and this view holds a modal grab. It locks when the screen goes off
// (power.js) and when the session starts. vibeos.Shell lock-type picks the bouncer: the PIN pad, the pattern, or none
// (swipe). The secret is the user's password, checked by vibeos-auth through PAM; wrong attempts are throttled as the
// AOSP gatekeeper does, kept across restarts in ~/.local/state/vibeos/keyguard.json. That file also keeps the
// length of the last PIN that unlocked, for Android 14's auto-confirm: a PIN of 6 or more digits unlocks as its last
// digit is typed.
import Clutter from 'gi://Clutter';
import Cogl from 'gi://Cogl';
import GdkPixbuf from 'gi://GdkPixbuf';
import Gio from 'gi://Gio';
import GLib from 'gi://GLib';
import GnomeBG from 'gi://GnomeBG?version=4.0';
import GnomeDesktop from 'gi://GnomeDesktop';
import GObject from 'gi://GObject';
import Shell from 'gi://Shell';
import St from 'gi://St';

import * as Main from 'resource:///org/gnome/shell/ui/main.js';
import * as Signals from 'resource:///org/gnome/shell/misc/signals.js';

import {PatternView, PinView, Strings, touchSlot, tryAgainIn} from './bouncer.js';
import {Canvas, paintIcon} from './icons.js';
import {animate, clamp, EMPHASIZED_ACCELERATE, FAST_OUT_SLOW_IN, lerp, LINEAR, MIN_FLING_VELOCITY,
    TOUCH_RESPONSE, VelocityTracker} from './motion.js';
import {Color, rgba, SHADE_PANEL} from './palette.js';
import {chargingText} from './state.js';
import {notificationIcon, SystemIcons, systemEnd} from './statusbar.js';
import {AndroidText} from './text.js';
import {wholeFrames} from './redraws.js';
import {geometry, HEIGHT, landscape, px, screenPixels, SCALE, SCREEN_SCALE, SCREEN_W, TABLET, WIDTH} from './units.js';

Gio._promisify(Gio.Subprocess.prototype, 'communicate_utf8_async');
Gio._promisify(Gio.File.prototype, 'load_contents_async');
Gio._promisify(Gio.File.prototype, 'replace_contents_async');

const GP_AUTH = ['/usr/local/libexec/vibeos-auth', '/usr/libexec/vibeos-auth'];

// --- the lock screen
const CLOCK_SIZE = 86 * 0.98;           // dp, the small Flex clock
const CLOCK_WGHT = 550;
// a soft grey-white, the owner's choice for the launcher's clock (2026-11-04), not the accent
const CLOCK_COLOUR = [224, 224, 224];
const CLOCK_LEFT = 84;
const CLOCK_INK_TOP = 246;
const DATE_SIZE = 16;
const DATE_WGHT = 630;
const DATE_INK_TOP = 541.5;
const CARRIER_LEFT = 48;
const CARRIER_INK_TOP = 42;
const SYSTEM_INSET = 9;                 // the icons end this much left of the home status bar's (880 on the S10e)
const PILL = {x: 48, y: 664, h: 144, pad: 39, slot: 66, icon: 45, max: 3};
const PILL_COLOUR = [19, 27, 28];
const PILL_ICON = [221, 231, 233];
const LOCK_ICON = {cx: 540, cy: 1950, size: 65 * 2.54};
const INDICATION_SIZE = 14;
const INDICATION_WGHT = 700;
const INDICATION_INK_TOP = 2083;

// A tablet's (screen px; W and H the screen as it is turned now). Text Android places by its logical box is placed
// here so that a sample's ink lands where Android's did: the font may not be Android's.
const TABLET_DATE_SAMPLE = 'Mon, Nov 16';
const TABLET_CLOCK_SAMPLE = '12:28';
// landscape: the date at the top left; the large clock in two lines, hours over minutes, centred on the left half;
// the notification icons' pill at the top right
const TABLET_LAND = {
    date: {sampleInkLeft: 172.5, inkTop: 249},
    largeClock: {inkTop: 766.5, pitch: 429, digitHeight: 395.25},
    pillRightGap: 120, pillY: 216,
    lockIconAboveBottom: 288,
    indicationAboveBottom: 162,
};
// portrait: the small clock with the date under it, as on a phone, further in; the pill under them
const TABLET_PORT = {
    clock: {sampleInkLeft: 457.5, inkTop: 400.5},
    date: {sampleInkLeft: 472.5, inkTop: 699},
    pillX: 360, pillY: 1144.5,
    lockIconAboveBottom: 330,
    indicationAboveBottom: 183,
};
const TABLET_SYSTEM_END = 74;           // from the end, as the home status bar's
const TABLET_STATUS_CENTRE_Y = 42;

// --- the swipe up: f is the panel's expansion, 1 at rest; f = 1 - (d + 142) / 1590 for a drag of d px
const SLOP = 24;
const F_OFFSET = 142;
const F_RANGE = 1590;
const BOUNCER_SHIFT = 240;              // how far below its place the bouncer starts as it comes in
const BLUR_MAX = 102;
const SETTLE_MS = 300;
const CANCEL_MS = 100;
const DISMISS_SWIPE = 30;               // a bouncer dragged down this far goes on release
const BACK_EDGE = 90;                   // the back gesture's edge: the bouncer leaves these touches to back
const BOUNCER_TO_LOCK_MS = 450;
const LOCK_ALPHA_BACK_MS = 250;

// --- the unlock
const GONE_SHORT_MS = 200;
const GONE_MS = 500;
const SURFACE_DELAY_MS = 67;
const SURFACE_MS = 300;
const SURFACE_SCALE = 0.95;
const SURFACE_SHIFT = 0.05;             // of the screen's height: 114 on 2280
const SURFACE_PIVOT_Y = 0.66;
// swipe (no secret): dismissing past 0.3 unlocks; the content fades out by 0.18 and rises up to SWIPE_RISE
const SWIPE_FINISH = 0.3;
const SWIPE_RISE = 540;

// auto-confirm: the shortest PIN it is offered for
const MIN_AUTO_CONFIRM = 6;
// frames the bouncer is painted at opacity 1/255 when it is built, so its canvases are drawn and uploaded then
const PRERENDER_FRAMES = 3;

const showBouncerProgress = f => f >= 0.9 ? 1 : f < 0.6 ? 0 : (f - 0.6) / 0.3;
// lock screen -> bouncer: progress over f 1 -> 0.88, the lock screen gone by half of it
const toBouncerProgress = f => 1 - clamp((f - 0.88) / 0.12);

// the AOSP gatekeeper's retry timeout after n failures, s
function lockoutSeconds(n) {
    if (n < 5)
        return 0;
    if (n < 11)
        return n === 5 || n === 10 ? 30 : 0;
    if (n < 30)
        return 30;
    if (n < 140)
        return 30 * 2 ** Math.floor((n - 30) / 10);
    return 86400;
}

function vibeosSettings() {
    const schema = Gio.SettingsSchemaSource.get_default()?.lookup('vibeos.Shell', true);
    return schema ? new Gio.Settings({settings_schema: schema}) : null;
}

// One line of AndroidText placed by its ink: left at its logical start (or centred), its ink top at a y
function placeText(text, {left, centre, inkTop}) {
    const x = centre !== undefined ? centre - text.textWidth / 2 : left;
    text.set_position(px(x - text.inset), px(inkTop - text.inset - text.ink.y));
}

// the logical start that puts @sample's ink, in @text's font, at @inkLeft; @text is left showing @sample
function sampleLeft(text, sample, inkLeft) {
    text.setText(sample);
    return inkLeft - text.ink.x;
}

const LockIcon = GObject.registerClass(
class LockIcon extends Canvas {
    constructor() {
        super({width: px(LOCK_ICON.size), height: px(LOCK_ICON.size), reactive: true});
        this.place(LOCK_ICON.cx, LOCK_ICON.cy);
        this._name = 'keyguard_ic_lock';
    }

    // centred on (cx, cy)
    place(cx, cy) {
        this.set_position(px(cx - LOCK_ICON.size / 2), px(cy - LOCK_ICON.size / 2));
    }

    set unlocked(on) {
        const name = on ? 'keyguard_ic_unlocked' : 'keyguard_ic_lock';
        if (name !== this._name) {
            this._name = name;
            this.queue_repaint();
        }
    }

    draw(cr) {
        screenPixels(cr);
        paintIcon(cr, this._name, 0, 0, LOCK_ICON.size, Color.accent1_100);
    }
});


// the screen as it is turned now in its own pixels: what a wallpaper is decoded for
function panelPixels() {
    const k = SCREEN_SCALE / SCALE;
    return [Math.round(WIDTH * k), Math.round(HEIGHT * k)];
}

// GNOME's wallpaper as a picture St can draw. A slideshow (an .xml: Debian's default, desktop-base's
// desktop-background.xml, is one) is its picture for now at the panel's size, as the shell's own background takes it;
// St would try to load the XML as an image on every frame.
function pictureUri(uri) {
    if (!uri?.toLowerCase().endsWith('.xml'))
        return uri;
    try {
        const show = new GnomeBG.BGSlideShow({file: Gio.File.new_for_uri(uri)});
        show.load();
        const [, , , file] = show.get_current_slide(...panelPixels());
        return file ? Gio.File.new_for_path(file).get_uri() : '';
    } catch (e) {
        logError(e, `vibeos: the wallpaper slideshow ${uri}`);
        return '';
    }
}

// The wallpaper covering the panel as CSS "background-size: cover" does (scaled to fill it, the middle kept), decoded
// in GdkPixbuf's worker thread and uploaded once, in the panel's proportions and at most its pixels: a picture larger
// than the panel is decoded at the size that covers it, a smaller one as it is, and the GPU scales it as it draws
// (GdkPixbuf scaling a 1080x2280 picture up for a 1080x2400 panel doubled the decode).
// As a CSS background-image, St loaded it in the paint that first showed it, on the main loop, at the file's size
// times the stage's scale (3240x6840 for a 1080x2280 picture at scale 3): 2 s of the first lock screen at boot on an
// S10e, with the boot splash up all that time, and an 88 MB texture kept in St's cache for good.
// Resolves to a St.ImageContent; rejects for a picture it cannot read, or one that is not a local file.
async function loadCover(uri, width, height, cancellable) {
    const file = Gio.File.new_for_uri(uri);
    const path = file.get_path();
    if (!path)
        throw new Error(`not a local file: ${uri}`);
    const [, w, h] = await new Promise((resolve, reject) => {
        GdkPixbuf.Pixbuf.get_file_info_async(path, cancellable, (_o, res) => {
            try {
                resolve(GdkPixbuf.Pixbuf.get_file_info_finish(res));
            } catch (e) {
                reject(e);
            }
        });
    });
    if (!w || !h)
        throw new Error(`not a picture: ${uri}`);
    const stream = await new Promise((resolve, reject) => {
        file.read_async(GLib.PRIORITY_DEFAULT, cancellable, (_o, res) => {
            try {
                resolve(file.read_finish(res));
            } catch (e) {
                reject(e);
            }
        });
    });
    let pixbuf = await new Promise((resolve, reject) => {
        const done = (_o, res) => {
            try {
                resolve(GdkPixbuf.Pixbuf.new_from_stream_finish(res));
            } catch (e) {
                reject(e);
            } finally {
                stream.close_async(GLib.PRIORITY_DEFAULT, null, null);
            }
        };
        if (w > width && h > height) {
            // the side short of the panel's proportions fills it, the other is cut
            const [toW, toH] = w * height > h * width ? [-1, height] : [width, -1];
            GdkPixbuf.Pixbuf.new_from_stream_at_scale_async(stream, toW, toH, true, cancellable, done);
        } else {
            GdkPixbuf.Pixbuf.new_from_stream_async(stream, cancellable, done);
        }
    });
    pixbuf = pixbuf.apply_embedded_orientation() ?? pixbuf;
    const [pw, ph] = [pixbuf.width, pixbuf.height];
    const [cw, ch] = pw * height > ph * width
        ? [Math.round(ph * width / height), ph] : [pw, Math.round(pw * height / width)];
    const cut = pixbuf.new_subpixbuf(Math.floor((pw - cw) / 2), Math.floor((ph - ch) / 2), cw, ch).copy();
    const content = St.ImageContent.new_with_preferred_size(cw, ch);
    content.set_bytes(global.stage.context.get_backend().get_cogl_context(), cut.read_pixel_bytes(),
        cut.has_alpha ? Cogl.PixelFormat.RGBA_8888 : Cogl.PixelFormat.RGB_888, cw, ch, cut.rowstride);
    return content;
}

export class Keyguard extends Signals.EventEmitter {
    // @state: the SystemState (carrier, battery, the status icons); @cutout: the camera hole the status icons keep
    // clear of
    constructor(state, cutout) {
        super();
        this._state = state;
        this._cutout = cutout;
        this._vibeos = vibeosSettings();
        // the wallpaper is the launcher's (GNOME's background, its dark picture under the dark style), as Shell 51's
        // own unlock dialog has it: Settings' Background panel no longer sets the screensaver's picture
        this._background = new Gio.Settings({schema_id: 'org.gnome.desktop.background'});
        this._interface = new Gio.Settings({schema_id: 'org.gnome.desktop.interface'});
        // Settings' "Show on the lock screen" (Notifications), for the pill's icons
        this._notificationSettings = new Gio.Settings({schema_id: 'org.gnome.desktop.notifications'});
        this.showing = false;
        this.bouncerShowing = false;
        this._bouncerActive = false;
        this._pinLength = 0;
        this._f = 1;
        this._bouncer = null;
        this._modeLocked = false;
        this._grab = null;
        this._afterRestart = true;
        this._failures = 0;
        this._lockedUntil = 0;          // realtime, ms
        this._countdownId = 0;
        this._checking = false;
        // what runs once the phone is unlocked (dismissThen), and what runs when the bouncer leaves without it
        this._afterUnlock = null;
        this._afterCancel = null;
        // an app's window over the lock screen (setOccluded), and the other windows' actors hidden meanwhile
        this.occludedBy = null;
        this._occlusionHidden = new Set();
        this._occlusion = {};           // the connections that last while occluded
        // the wallpaper (_syncWallpaper()): its St.ImageContent or CSS, and the load under way
        this._wallpaperContent = null;
        this._wallpaperStyle = null;
        this._wallpaperLoad = null;
        this._wallpaperShape = '';
        this._covers = new Map();       // the decoded wallpaper by the screen's shape it covers ("WxH")
        this.wallpaperLoaded = Promise.resolve();
        this._loadThrottle();

        this._build();
        this._layout();
        // GNOME's screen shield stays out of the way: the session going idle (gnome-session's presence) faded its
        // lightbox in and locked it under this keyguard, which has the screen timeout (power.js) and the lock. Its
        // handler is stubbed on the instance while this runs; destroy() gives the prototype's back.
        if (Main.screenShield)
            Main.screenShield._onStatusChanged = () => {};
        // turned: laid out again, the wallpaper covering the screen as it is now
        geometry.connectObject('changed', () => this._layout(), this);
        this._wallClock = new GnomeDesktop.WallClock({time_only: true});
        this._wallClock.connectObject('notify::clock', () => this._syncClock(), this);
        this._interface.connectObject('changed::clock-format', () => this._syncClock(),
            'changed::color-scheme', () => this._syncWallpaper(true), this);
        this._background.connectObject('changed', () => this._syncWallpaper(true), this);
        this._notificationSettings.connectObject('changed::show-in-lock-screen', () => this._syncPill(), this);
        state.connectObject('changed', () => this._syncState(), this);
        // another lock type: the next lock builds its bouncer
        this._vibeos?.connectObject('changed::lock-type', () => {
            if (this._bouncerActive)
                return;
            this._endPrerender();
            this._bouncer?.destroy();
            this._bouncer = null;
        }, this);
        Main.messageTray.connectObject(
            'source-added', (_tray, source) => this._addSource(source),
            'source-removed', (_tray, source) => {
                source.disconnectObject(this);
                this._syncPill();
            }, this);
        for (const source of Main.messageTray.getSources())
            this._addSource(source);
        this._syncClock();
        this._syncState();
        this._syncWallpaper();
    }

    get _lockType() {
        return this._vibeos?.get_string('lock-type') ?? 'pin';
    }

    get _secure() {
        return this._lockType !== 'swipe';
    }

    // --- the view

    _build() {
        this.actor = new St.Widget({
            name: 'vibeosKeyguard',
            reactive: true,
            visible: false,
        });
        // The same children first and last, always shown: what shows and hides between them (the bouncer, the
        // wallpaper) never changes the first or last visible child. St restyles a child that gains or loses
        // :first-child or :last-child, and a restyle redraws every canvas under it: the bouncer showing restyled all
        // its keys, mid-swipe.
        // First, black under the wallpaper. The unlock takes it away (opacity: no restyle) with the wallpaper, and
        // the screen behind shows through: as the keyguard's own background it stayed over the launcher coming up,
        // and every unlock went black for half a second (2026-11-13).
        this._black = new St.Widget({style: 'background-color: black;'});
        this.actor.add_child(this._black);
        this._wallpaper = new St.Widget();
        this.actor.add_child(this._wallpaper);

        // the bouncer's backdrop: the wallpaper blurred, under the shade panel's tint; in the unlock, the screen coming
        // up behind. Drawn in whole frames while it shows (redraws.js). A tablet's is the tint alone, opaque, as
        // Android has it where the GPU does no blur (the Galaxy Tab S5e's).
        this._blurLayer = new St.Widget();
        this._blur = new Shell.BlurEffect({mode: Shell.BlurMode.BACKGROUND, radius: 0, brightness: 1});
        this._blur.enabled = false;
        this._blurLayer.add_effect(this._blur);
        this.actor.add_child(this._blurLayer);
        const flat = SHADE_PANEL[0].map(v => Math.round(v * SHADE_PANEL[1]));
        this._scrim = new St.Widget({opacity: 0,
            style: TABLET ? `background-color: rgb(${flat.join(', ')});`
                : `background-color: ${rgba(SHADE_PANEL[0].map(Math.round), SHADE_PANEL[1])};`});
        this.actor.add_child(this._scrim);

        // what the lock screen shows
        this._content = new St.Widget();
        this.actor.add_child(this._content);
        this._carrier = new AndroidText('', 14, 700, {colour: Color.white, opsz: 16});
        this._content.add_child(this._carrier);
        this._systemIcons = new SystemIcons(this._state, {end: this._systemEnd()});
        this._content.add_child(this._systemIcons);
        this._cutout.connectObject('changed', () => this._systemIcons.setEnd(this._systemEnd()), this);
        this._clock = new AndroidText('', CLOCK_SIZE, CLOCK_WGHT, {colour: CLOCK_COLOUR});
        this._content.add_child(this._clock);
        // a tablet's large clock, turned to landscape: hours over minutes, each line centred
        this._largeClock = [];
        if (TABLET) {
            // its size: the digits as tall as Android's in this font
            const probe = new AndroidText('27', 100, CLOCK_WGHT, {colour: CLOCK_COLOUR});
            const size = 100 * TABLET_LAND.largeClock.digitHeight / probe.ink.height;
            probe.destroy();
            this._largeClock = [0, 1].map(() => {
                const line = new AndroidText('', size, CLOCK_WGHT, {colour: CLOCK_COLOUR});
                this._content.add_child(line);
                return line;
            });
        }
        this._date = new AndroidText('', DATE_SIZE, DATE_WGHT, {colour: Color.white});
        this._content.add_child(this._date);
        this._pill = new St.Widget({x: px(PILL.x), y: px(PILL.y), height: px(PILL.h), visible: false,
            style: `background-color: rgb(${PILL_COLOUR.join(', ')}); border-radius: ${px(PILL.h / 2)}px;`});
        this._content.add_child(this._pill);
        this._lockIcon = new LockIcon();
        this._content.add_child(this._lockIcon);
        this._lockIcon.connect('button-release-event', () => this._lockIconTapped());
        this._lockIcon.connect('touch-event', (a, event) => {
            if (event.type() === Clutter.EventType.TOUCH_END && !this._tracking)
                this._lockIconTapped();
            return Clutter.EVENT_PROPAGATE;
        });
        this._indication = new AndroidText('', INDICATION_SIZE, INDICATION_WGHT, {colour: Color.white, opsz: 16});
        this._content.add_child(this._indication);

        this._bouncerBox = new St.Widget({opacity: 0, visible: false});
        this.actor.add_child(this._bouncerBox);
        this.actor.add_child(new Clutter.Actor());

        this._tracker = new VelocityTracker();
        this._down = null;
        this.actor.connect('event', (a, event) => this._onEvent(event));
    }

    // The wallpaper loads off the main loop (loadCover()); the one before stays until it is in. wallpaperLoaded
    // resolves once it is on, or once it failed and the CSS background took its place. Each shape of the screen gets
    // its own cover, kept while the picture stays: a turned tablet has its other one at once. @changed: another
    // picture, every cover made again.
    _syncWallpaper(changed = false) {
        const [w, h] = panelPixels();
        const shape = `${w}x${h}`;
        if (changed)
            this._covers.clear();
        else if (shape === this._wallpaperShape)
            return;
        this._wallpaperShape = shape;
        const cover = this._covers.get(shape);
        if (cover) {
            this._wallpaperLoad?.cancel();
            this._wallpaperLoad = null;
            this._setWallpaper(cover, null);
            this.wallpaperLoaded = Promise.resolve();
            return;
        }
        const dark = this._interface.get_string('color-scheme') === 'prefer-dark';
        const uri = pictureUri((dark && this._background.get_string('picture-uri-dark')) ||
            this._background.get_string('picture-uri'));
        this._wallpaperLoad?.cancel();
        this._wallpaperLoad = null;
        if (!uri) {
            this._setWallpaper(null, 'background-color: black;');
            this.wallpaperLoaded = Promise.resolve();
            return;
        }
        const cancellable = new Gio.Cancellable();
        this._wallpaperLoad = cancellable;
        this.wallpaperLoaded = loadCover(uri, w, h, cancellable).then(content => {
            if (cancellable.is_cancelled())
                return;
            this._covers.set(shape, content);
            this._setWallpaper(content, null);
        }, e => {
            if (cancellable.is_cancelled())
                return;
            logError(e, `vibeos: the lock screen's wallpaper ${uri}`);
            this._setWallpaper(null, `background-image: url("${uri}"); background-size: cover;`);
        }).finally(() => {
            if (this._wallpaperLoad === cancellable)
                this._wallpaperLoad = null;
        });
    }

    // @content: the St.ImageContent loadCover() made, or null for @style, CSS (black, or the picture as St loads it)
    _setWallpaper(content, style) {
        this._wallpaperContent = content;
        this._wallpaperStyle = style;
        this._wallpaper.set_content(content);
        this._wallpaper.style = style;
    }

    _syncClock() {
        const now = GLib.DateTime.new_now_local();
        const h24 = this._interface.get_string('clock-format') === '24h';
        const time = now.format(h24 ? '%H:%M' : '%-I:%M');
        this._clock.setText(time);
        this._date.setText(now.format('%a, %b %-e'));
        const L = this._L;
        if (!L) {
            placeText(this._clock, {left: CLOCK_LEFT, inkTop: CLOCK_INK_TOP});
            placeText(this._date, {left: CLOCK_LEFT, inkTop: DATE_INK_TOP});
            return;
        }
        placeText(this._date, {left: L.dateLeft, inkTop: L.date.inkTop});
        const large = !!L.largeClock;
        this._clock.visible = !large;
        for (const line of this._largeClock)
            line.visible = large;
        if (large) {
            const [hours, minutes] = time.split(':');
            const {inkTop, pitch} = L.largeClock;
            [hours, minutes].forEach((part, i) => {
                this._largeClock[i].setText(part);
                placeText(this._largeClock[i], {centre: L.largeClockCentre, inkTop: inkTop + pitch * i});
            });
        } else {
            placeText(this._clock, {left: L.clockLeft, inkTop: L.clock.inkTop});
        }
    }

    // --- laying out for the screen as it is now

    // Everything over the whole screen, and on a tablet the lock screen's parts where Android has them for the way
    // it is turned. The phone's are where it was built with them (the phone's lock screen is always upright).
    _layout() {
        const monitor = Main.layoutManager.primaryMonitor;
        if (!monitor)
            return;
        this.actor.set_position(monitor.x, monitor.y);
        this.actor.set_size(monitor.width, monitor.height);
        for (const layer of [this._black, this._wallpaper, this._blurLayer, this._scrim, this._content,
            this._bouncerBox])
            layer.set_size(monitor.width, monitor.height);
        this._bouncer?.relayout();
        this._syncWallpaper();
        if (!TABLET) {
            this._L = null;
            return;
        }
        const W = WIDTH, H = HEIGHT;
        const land = landscape();
        const T = land ? TABLET_LAND : TABLET_PORT;
        this._L = {
            ...T,
            dateLeft: sampleLeft(this._date, TABLET_DATE_SAMPLE, T.date.sampleInkLeft),
            clockLeft: T.clock ? sampleLeft(this._clock, TABLET_CLOCK_SAMPLE, T.clock.sampleInkLeft) : 0,
            largeClockCentre: W / 4,
            pill: land ? {right: W - T.pillRightGap, y: T.pillY} : {x: T.pillX, y: T.pillY},
        };
        // no carrier line: a tablet has no SIM to name
        this._carrier.visible = false;
        this._systemIcons.setEnd(W - TABLET_SYSTEM_END);
        this._systemIcons.setCenterY(TABLET_STATUS_CENTRE_Y);
        this._lockIcon.place(W / 2, H - T.lockIconAboveBottom);
        this._indicationAt = {centre: W / 2, inkTop: H - T.indicationAboveBottom};
        placeText(this._indication, this._indicationAt);
        this._placePill();
        this._syncClock();
    }

    _placePill() {
        const pill = this._L?.pill;
        if (!pill)
            return;
        this._pill.set_position(px(pill.right !== undefined ? pill.right - (this._pillWidth ?? 0) : pill.x), px(pill.y));
    }

    _syncState() {
        const {mobile, wifi, battery} = this._state;
        // the mobile network, then (the owner's addition) the Wi-Fi network, joined with " | " as two SIMs' names
        // are
        const names = [mobile.present ? mobile.operator || 'Emergency calls only' : 'No SIM'];
        if (wifi.connected && wifi.name)
            names.push(wifi.name);
        const carrier = names.join(' | ');
        if (carrier !== this._carrierText && !TABLET) {
            this._carrierText = carrier;
            this._carrier.setText(carrier);
            placeText(this._carrier, {left: CARRIER_LEFT, inkTop: CARRIER_INK_TOP});
        }
        // the charging line, with how fast (One UI's names for the speeds)
        let indication = '';
        if (battery.present && battery.plugged)
            indication = battery.level >= 100 ? 'Charged'
                : `${battery.level}% • ${chargingText(battery.speed, battery.amps)}`;
        if (indication !== this._indicationText) {
            this._indicationText = indication;
            this._indication.setText(indication);
            this._indication.visible = !!indication;
            placeText(this._indication, this._indicationAt ?? {centre: 540, inkTop: INDICATION_INK_TOP});
        }
    }

    _addSource(source) {
        source.connectObject(
            'notification-added', () => this._syncPill(),
            'notification-removed', () => this._syncPill(), this);
        this._syncPill();
    }

    // the notification icons in their pill: one per app, three and the overflow dot at most;
    // none after a lockdown. Only the apps allowed on the lock screen: a source's policy says it for the setting and
    // the app's own switch together (show-in-lock-screen, both), as for Shell's own lock screen (unlockDialog.js).
    // An app's switch is read again at each lock (lock() syncs the pill), the setting at once.
    _syncPill() {
        this._pill.destroy_all_children();
        const sources = this._lockedDown ? []
            : Main.messageTray.getSources().filter(s => s.notifications.length && s.policy.showInLockScreen);
        const n = Math.min(sources.length, PILL.max + 1);
        this._pill.visible = n > 0;
        if (!n)
            return;
        this._pillWidth = (26 + 22 * n) * 3;
        this._pill.width = px(this._pillWidth);
        this._placePill();
        sources.slice(0, sources.length > PILL.max + 1 ? PILL.max : n).forEach((source, k) => {
            const icon = notificationIcon(source, px(PILL.icon), {glyph: `rgb(${PILL_ICON.join(', ')})`});
            icon.opacity = 255;
            icon.set_position(px(PILL.pad + PILL.slot * k + (PILL.slot - PILL.icon) / 2), px((PILL.h - PILL.icon) / 2));
            this._pill.add_child(icon);
        });
        if (sources.length > PILL.max + 1) {
            const dot = new St.Widget({width: px(12), height: px(12),
                style: `background-color: rgba(255, 255, 255, ${0xdd / 255}); border-radius: ${px(6)}px;`});
            dot.set_position(px(PILL.pad + PILL.slot * PILL.max + PILL.slot / 2 - 6), px(PILL.h / 2 - 6));
            this._pill.add_child(dot);
        }
    }

    // --- locking

    lock() {
        if (this.showing) {
            // the screen went off over the bouncer: back to the lock screen
            this._hideBouncer(false);
            return;
        }
        this.showing = true;
        this._unlocking?.stop();
        // whatever the screen did since the last lock
        this._layout();
        this._lockIcon.unlocked = !this._secure;
        this._syncClock();
        this._syncState();
        this._syncPill();
        this._setF(1);
        this._content.opacity = 255;
        this._content.translation_y = 0;
        this._wallpaper.show();
        this.actor.show();
        if (!this._modeLocked) {
            Main.sessionMode.pushMode('unlock-dialog');
            this._modeLocked = true;
        }
        this._grab ??= Main.pushModal(this.actor, {actionMode: Shell.ActionMode.LOCK_SCREEN});
        this._prepareBouncer();
        this.emit('changed');
    }

    // the power menu's Lockdown: locked at once, and the notifications kept off the lock screen until the next unlock
    lockdown() {
        this._lockedDown = true;
        this.lock();
        this._syncPill();
    }

    // --- occluded: an app's window over the lock screen (whenlocked.js)

    // @win: the window that shows over the lock screen, or null for the lock screen itself. Only the keyguard's view
    // and its grab step aside for it: every other window stays hidden, and the phone stays locked (showing stays
    // true), so home, recents and the shade stay shut. Unlocked, nothing is occluded.
    setOccluded(win) {
        if (this._unlockingNow)
            return;
        win = this.showing ? win : null;
        if (win === this.occludedBy)
            return;
        this._showHiddenWindows();
        this.occludedBy = win;
        if (win) {
            if (this.bouncerShowing)
                this._hideBouncer(false);
            this.actor.hide();
            if (this._grab)
                Main.popModal(this._grab);
            this._grab = null;
            // the locked session mode hides the window group (LayoutManager._updateVisibility): shown for this window,
            // with every other window in it hidden, the ones that come later too
            const group = global.window_group;
            group.connectObject(
                'notify::visible', () => group.show(),
                'child-added', (_group, actor) => this._hideForOcclusion(actor), this._occlusion);
            group.show();
            for (const actor of global.get_window_actors())
                this._hideForOcclusion(actor);
        } else if (this.showing) {
            this.actor.show();
            this._grab ??= Main.pushModal(this.actor, {actionMode: Shell.ActionMode.LOCK_SCREEN});
            Main.layoutManager._updateVisibility();
        }
        this.emit('changed');
    }

    _hideForOcclusion(actor) {
        if (actor === this.occludedBy?.get_compositor_private() || !actor.visible)
            return;
        actor.hide();
        this._occlusionHidden.add(actor);
        actor.connectObject('destroy', () => this._occlusionHidden.delete(actor), this._occlusion);
    }

    _showHiddenWindows() {
        global.window_group.disconnectObject(this._occlusion);
        for (const actor of this._occlusionHidden) {
            actor.disconnectObject(this._occlusion);
            actor.show();
        }
        this._occlusionHidden.clear();
    }

    // the screen came on: the lock screen as it was, the clock up to date
    wokeUp() {
        if (!this.showing)
            return;
        this._layout();
        this._syncClock();
        this._syncState();
    }

    // --- the swipe up and the bouncer

    _local(event) {
        const [sx, sy] = event.get_coords();
        return [sx * SCALE, sy * SCALE];
    }

    _onEvent(event) {
        if (!this.showing || this._unlockingNow)
            return Clutter.EVENT_PROPAGATE;
        const type = event.type();
        const seq = touchSlot(event);
        const isTouch = type >= Clutter.EventType.TOUCH_BEGIN && type <= Clutter.EventType.TOUCH_CANCEL;
        if (type === Clutter.EventType.TOUCH_BEGIN || type === Clutter.EventType.BUTTON_PRESS) {
            if (this._down)
                return Clutter.EVENT_STOP;
            const [x, y] = this._local(event);
            // a touch stopped here cancels the back gesture before it can recognize
            if (this.bouncerShowing && isTouch && (x < BACK_EDGE || x > WIDTH - BACK_EDGE))
                return Clutter.EVENT_PROPAGATE;
            this._settle?.stop();
            this._settle = null;
            this._down = {seq: isTouch ? seq : 'pointer', x, y, t: event.get_time(), f: this._f};
            this._tracking = false;
            this._tracker.clear();
            this._tracker.add(event.get_time(), x, y);
            return Clutter.EVENT_STOP;
        }
        const down = this._down;
        const mine = down && (isTouch ? down.seq !== 'pointer' && down.seq === seq
            : down.seq === 'pointer' && (type === Clutter.EventType.MOTION || type === Clutter.EventType.BUTTON_RELEASE));
        if (!mine)
            return Clutter.EVENT_PROPAGATE;
        const [x, y] = this._local(event);
        this._tracker.add(event.get_time(), x, y);
        if (type === Clutter.EventType.TOUCH_UPDATE || type === Clutter.EventType.MOTION) {
            this._drag(down, x, y);
            return Clutter.EVENT_STOP;
        }
        this._down = null;
        this._release(down, x, y, type !== Clutter.EventType.TOUCH_CANCEL);
        return Clutter.EVENT_STOP;
    }

    _drag(down, x, y) {
        const d = down.y - y;
        if (this.bouncerShowing) {
            // a bouncer dragged down goes back to the lock screen on release
            this._bouncerBox.translation_y = px(Math.max(0, -d) * 0.5);
            return;
        }
        if (!this._tracking) {
            if (d < SLOP)
                return;
            this._tracking = true;
            if (this._secure)
                this._ensureBouncer();
        }
        this._setF(Math.min(1, 1 - (d + F_OFFSET) / F_RANGE));
        if (!this._secure && 1 - this._f >= SWIPE_FINISH)
            this._goingAway();
    }

    _release(down, x, y, lifted) {
        if (this.bouncerShowing) {
            const dragged = y - down.y;
            if (lifted && dragged > DISMISS_SWIPE && !this._bouncer?.ownsPoint(px(down.x), px(down.y)))
                this._hideBouncer(true);
            else
                this._springBouncerBack();
            return;
        }
        if (!this._tracking)
            return;
        this._tracking = false;
        const v = this._tracker.velocityY;
        const up = lifted && (Math.abs(v) >= MIN_FLING_VELOCITY ? v < 0 : this._f < 0.5);
        if (!this._secure) {
            if (up)
                this._goingAway();
            else
                this._settleTo(1, CANCEL_MS * 2);
            return;
        }
        if (up)
            this._settleTo(0, SETTLE_MS, () => this._bouncerShown());
        else
            this._settleTo(1, CANCEL_MS, () => this._dropBouncer());
    }

    _springBouncerBack() {
        const from = this._bouncerBox.translation_y;
        if (!from)
            return;
        animate(this._bouncerBox, {from, to: 0, duration: 150, interpolator: FAST_OUT_SLOW_IN,
            onUpdate: v => {
                this._bouncerBox.translation_y = v;
            }});
    }

    _settleTo(f, ms, onDone) {
        this._settle?.stop();
        const from = this._f;
        this._settle = animate(this.actor, {from, to: f, duration: ms * Math.min(1, Math.abs(f - from) / 0.5),
            interpolator: FAST_OUT_SLOW_IN, onUpdate: v => this._setF(v),
            onDone: () => {
                this._settle = null;
                onDone?.();
            }});
    }

    // The panel's expansion f: the lock screen's content, the bouncer's progress, the blur and the tint
    _setF(f) {
        this._f = f;
        if (!this._secure) {
            // dismissing: the content fades out by 18 % and rises
            this._content.opacity = Math.round(255 * clamp((f - 0.82) / 0.18));
            this._content.translation_y = px(-SWIPE_RISE * (1 - f));
            return;
        }
        const p = toBouncerProgress(f);
        this._content.opacity = Math.round(255 * (1 - clamp(p / 0.5)));
        this._content.translation_y = 0;
        this._setBackdrop(p);
        if (!this._bouncerActive)
            return;
        const sp = showBouncerProgress(f);
        this._bouncerBox.opacity = Math.round(255 * (1 - sp));
        this._bouncerBox.translation_y = px(BOUNCER_SHIFT * sp);
        if (sp < 1 && !this._appeared) {
            this._appeared = true;
            this._bouncer.appear();
        }
    }

    _setBackdrop(p) {
        const blur = TABLET ? 0 : Math.trunc(lerp(0, BLUR_MAX, p));
        this._blur.enabled = blur > 0;
        wholeFrames(this, blur > 0);
        if (blur > 0 && this._blur.radius !== blur)
            this._blur.radius = blur;
        this._scrim.opacity = Math.round(255 * p);
    }

    // The bouncer is built once and kept (a new one only when the lock type changes), and built when the phone locks,
    // not when a swipe starts: building it and drawing its canvases for the first time cost the swipe's first 100 ms.
    // Painted for a few frames at opacity 1/255 as it is built, so every canvas in it is drawn then, under the screen
    // going off, and then hidden: hidden widgets keep their drawings.
    _prepareBouncer() {
        const kind = this._lockType === 'pattern' ? 'pattern' : 'pin';
        if (this._bouncer?.kind === kind || !this._secure)
            return;
        if (this._bouncerActive)
            return;
        this._bouncer?.destroy();
        this._bouncer = kind === 'pattern' ? new PatternView() : new PinView();
        this._strings = Strings[this._bouncer.kind];
        this._bouncer.connect('submit', (view, secret) => this._submit(secret, view.autoConfirmed));
        this._bouncerBox.add_child(this._bouncer);
        this._bouncer.setMessage(this._strings.prompt, '');
        this._bouncer.setEnabled(false);
        this._bouncerBox.translation_y = 0;
        this._bouncerBox.opacity = 1;
        this._bouncerBox.show();
        let frames = 0;
        this._prerenderId = global.stage.connect('after-paint', () => {
            if (++frames < PRERENDER_FRAMES)
                return;
            this._endPrerender();
        });
    }

    _endPrerender() {
        if (!this._prerenderId)
            return;
        global.stage.disconnect(this._prerenderId);
        this._prerenderId = 0;
        if (!this._bouncerActive) {
            this._bouncerBox.hide();
            this._bouncerBox.opacity = 0;
            this._bouncer?.setEnabled(true);
        }
    }

    _ensureBouncer() {
        if (this._bouncerActive)
            return;
        this._endPrerender();
        this._prepareBouncer();
        this._bouncerActive = true;
        this._autoSuspended = false;
        this._syncAutoConfirm();
        this._bouncerBox.translation_y = px(BOUNCER_SHIFT);
        this._bouncerBox.opacity = 0;
        this._bouncerBox.show();
        this._appeared = false;
        this._showPrompt();
        this._syncLockout();
    }

    _dropBouncer() {
        const cancelled = this._afterCancel;
        this._afterUnlock = null;
        this._afterCancel = null;
        this._bouncerActive = false;
        this._bouncer?.park();
        this._bouncerBox.hide();
        this._bouncerBox.translation_y = 0;
        this._appeared = false;
        if (this.bouncerShowing) {
            this.bouncerShowing = false;
            this.emit('changed');
        }
        cancelled?.();
    }

    // auto-confirm on while the last PIN that unlocked was 6 digits or more, and off for the rest of this bouncer once
    // a PIN it confirmed was wrong: then the PIN may have changed, and only enter can take one of another length
    _syncAutoConfirm() {
        const length = this._pinLength >= MIN_AUTO_CONFIRM && !this._autoSuspended ? this._pinLength : 0;
        if (this._bouncer)
            this._bouncer.autoConfirm = length;
    }

    _bouncerShown() {
        if (this.bouncerShowing)
            return;
        this.bouncerShowing = true;
        this.emit('changed');
    }

    // the lock icon: the bouncer, or with no secret the unlock
    _lockIconTapped() {
        if (!this.showing || this.bouncerShowing || this._tracking || this._unlockingNow)
            return;
        if (!this._secure) {
            this._goingAway();
            return;
        }
        this._ensureBouncer();
        this._settleTo(0, SETTLE_MS * 2, () => this._bouncerShown());
    }

    // Android's KeyguardManager.requestDismissKeyguard, for an app a key opens (keylaunch.js): the bouncer comes up
    // (with no secret, the unlock itself), and `then` runs once the phone is unlocked. The bouncer leaving without an
    // unlock (back, a swipe down, the screen going off) drops it, and runs `cancelled` if there is one; a later call
    // replaces it, the earlier one cancelled.
    dismissThen(then, cancelled = null) {
        if (!this.showing) {
            then();
            return;
        }
        if (this._unlockingNow || this.occludedBy) {
            cancelled?.();
            return;
        }
        const earlier = this._afterCancel;
        this._afterUnlock = then;
        this._afterCancel = cancelled;
        if (earlier !== cancelled)
            earlier?.();
        if (!this._secure) {
            this._goingAway();
            return;
        }
        if (this.bouncerShowing)
            return;
        this._settle?.stop();
        this._settle = null;
        this._tracking = false;
        this._ensureBouncer();
        this._settleTo(0, SETTLE_MS * 2, () => this._bouncerShown());
    }

    // the back gesture: the bouncer goes, the lock screen comes back
    back() {
        if (this.bouncerShowing && !this._unlockingNow)
            this._hideBouncer(true);
    }

    // bouncer -> lock screen: the bouncer fades, the lock screen comes back in 250 ms, the blur clears in 450
    _hideBouncer(animated) {
        this._settle?.stop();
        this._settle = null;
        if (!this._bouncerActive) {
            this._setF(1);
            return;
        }
        if (!animated) {
            this._dropBouncer();
            this._setF(1);
            return;
        }
        const content = this._content.opacity / 255, box = this._bouncerBox.opacity / 255;
        const start = this._f;
        this._settle = animate(this.actor, {from: 0, to: BOUNCER_TO_LOCK_MS, duration: BOUNCER_TO_LOCK_MS,
            interpolator: LINEAR,
            onUpdate: ms => {
                this._content.opacity = Math.round(255 * lerp(content, 1, clamp(ms / LOCK_ALPHA_BACK_MS)));
                this._bouncerBox.opacity = Math.round(255 * box * (1 - clamp(ms / GONE_SHORT_MS)));
                this._setBackdrop(toBouncerProgress(start) * (1 - ms / BOUNCER_TO_LOCK_MS));
            },
            onDone: () => {
                this._settle = null;
                this._dropBouncer();
                this._f = 1;
                this._setBackdrop(0);
            }});
    }

    // --- the secret

    _showPrompt() {
        if (!this._bouncerActive)
            return;
        this._bouncer.setMessage(this._strings.prompt, this._afterRestart ? this._strings.restart : '');
    }

    // @secret: what was entered, or null for any input (the message goes back to the prompt); @auto: a PIN that
    // confirmed itself
    async _submit(secret, auto = false) {
        if (secret === null) {
            if (!this._lockedOut && !this._checking)
                this._showPrompt();
            return;
        }
        if (this._lockedOut) {
            this._bouncer?.setEnabled(false);
            return;
        }
        // an entry of 3 characters or fewer is no attempt
        if (secret.length <= 3) {
            this._bouncer?.wrong();
            this._bouncer?.setEnabled(true);
            return;
        }
        this._checking = true;
        const result = await this._check(secret);
        this._checking = false;
        if (!this._bouncerActive)
            return;
        if (result === 0) {
            this._failures = 0;
            this._lockedUntil = 0;
            // the length auto-confirm goes by: this PIN's, if it is digits only and long enough
            if (this._bouncer.kind === 'pin')
                this._pinLength = /^[0-9]+$/.test(secret) && secret.length >= MIN_AUTO_CONFIRM ? secret.length : 0;
            this._saveThrottle();
            this._afterRestart = false;
            this._bouncer.disappear(() => this._goingAway());
            return;
        }
        if (result === 2) {
            // PAM could not run: no attempt counted
            this._bouncer.setMessage('Could not check', 'Try again');
            this._bouncer.wrong();
            this._bouncer.setEnabled(true);
            return;
        }
        this._failures++;
        if (auto) {
            this._autoSuspended = true;
            this._syncAutoConfirm();
        }
        const timeout = lockoutSeconds(this._failures);
        this._lockedUntil = timeout ? Date.now() + timeout * 1000 : 0;
        this._saveThrottle();
        this._bouncer.wrong();
        if (timeout) {
            this._syncLockout();
        } else {
            this._bouncer.setMessage(this._strings.wrong, '');
            this._bouncer.setEnabled(true);
        }
    }

    async _check(secret) {
        const helper = GP_AUTH.find(p => GLib.file_test(p, GLib.FileTest.IS_EXECUTABLE));
        if (!helper) {
            log('vibeos: no vibeos-auth to check the lock screen secret with');
            return 2;
        }
        try {
            const proc = Gio.Subprocess.new([helper],
                Gio.SubprocessFlags.STDIN_PIPE | Gio.SubprocessFlags.STDOUT_SILENCE | Gio.SubprocessFlags.STDERR_SILENCE);
            await proc.communicate_utf8_async(`${secret}\n`, null);
            return proc.get_if_exited() ? proc.get_exit_status() : 2;
        } catch (e) {
            logError(e, 'vibeos: vibeos-auth');
            return 2;
        }
    }

    get _lockedOut() {
        return this._lockedUntil > Date.now();
    }

    // a lockout counts down a second at a time, the input off meanwhile
    _syncLockout() {
        if (this._countdownId)
            GLib.source_remove(this._countdownId);
        this._countdownId = 0;
        if (!this._bouncerActive)
            return;
        if (!this._lockedOut) {
            this._bouncer.setEnabled(true);
            return;
        }
        this._bouncer.setEnabled(false);
        const tick = () => {
            const left = Math.round((this._lockedUntil - Date.now()) / 1000);
            if (left <= 0 || !this._bouncerActive) {
                this._countdownId = 0;
                this._showPrompt();
                this._bouncer?.setEnabled(true);
                return GLib.SOURCE_REMOVE;
            }
            this._bouncer.setMessage(tryAgainIn(left), this._strings.tooMany);
            return GLib.SOURCE_CONTINUE;
        };
        tick();
        this._countdownId = GLib.timeout_add(GLib.PRIORITY_DEFAULT, 1000, tick);
    }

    get _throttleFile() {
        return Gio.File.new_for_path(GLib.build_filenamev([GLib.get_user_state_dir(), 'vibeos', 'keyguard.json']));
    }

    _loadThrottle() {
        try {
            const [, bytes] = this._throttleFile.load_contents(null);
            const {failures, until, pinLength} = JSON.parse(new TextDecoder().decode(bytes));
            this._failures = failures | 0;
            this._lockedUntil = Number(until) || 0;
            this._pinLength = pinLength | 0;
        } catch (e) {
        }
    }

    async _saveThrottle() {
        const file = this._throttleFile;
        try {
            file.get_parent().make_directory_with_parents(null);
        } catch (e) {
        }
        try {
            const data = JSON.stringify({failures: this._failures, until: this._lockedUntil,
                pinLength: this._pinLength});
            await file.replace_contents_async(new TextEncoder().encode(data), null, false,
                Gio.FileCreateFlags.PRIVATE | Gio.FileCreateFlags.REPLACE_DESTINATION, null);
        } catch (e) {
            logError(e, 'vibeos: keyguard state');
        }
    }

    // --- the unlock: the keyguard goes, the screen behind comes up from 95 %

    // The shell's "unlock-dialog" mode, left: only while it is still the current one. GNOME's own screen shield,
    // locking alongside on a suspend or a logind Lock, pops the same mode when it unlocks; popping it a second time
    // threw, the unlock stopped half way and the right PIN left a black screen.
    _leaveLockMode() {
        if (this._modeLocked && Main.sessionMode.currentMode === 'unlock-dialog')
            Main.sessionMode.popMode('unlock-dialog');
        this._modeLocked = false;
    }

    _goingAway() {
        if (this._unlockingNow || !this.showing)
            return;
        this._unlockingNow = true;
        this._settle?.stop();
        this._settle = null;
        if (this._countdownId)
            GLib.source_remove(this._countdownId);
        this._countdownId = 0;

        // the wallpaper under the windows now: the launcher draws the same one, apps come up over it
        const monitor = Main.layoutManager.primaryMonitor;
        const under = new St.Widget({x: monitor.x, y: monitor.y, width: monitor.width, height: monitor.height,
            style: this._wallpaperStyle});
        under.set_content(this._wallpaperContent);
        Main.layoutManager.uiGroup.insert_child_below(under, global.window_group);
        this._wallpaper.hide();
        this._black.opacity = 0;

        const group = global.window_group;
        const redirect = group.offscreen_redirect;
        group.offscreen_redirect = Clutter.OffscreenRedirect.AUTOMATIC_FOR_OPACITY;
        group.set_pivot_point(0.5, SURFACE_PIVOT_Y);
        const surface = a => {
            group.opacity = Math.round(255 * a);
            group.scale_x = group.scale_y = lerp(SURFACE_SCALE, 1, a);
            group.translation_y = px(Math.round(HEIGHT * SURFACE_SHIFT) * (1 - a));
        };
        surface(0);
        this._leaveLockMode();

        const content = this._content.opacity / 255, box = this._bouncerBox.opacity / 255;
        const blur = this._blur.enabled ? this._blur.radius : 0, scrim = this._scrim.opacity / 255;
        const end = SURFACE_DELAY_MS + SURFACE_MS;
        this._unlocking = animate(this.actor, {from: 0, to: Math.max(GONE_MS, end), duration: Math.max(GONE_MS, end),
            interpolator: LINEAR,
            onUpdate: ms => {
                const short = clamp(ms / GONE_SHORT_MS);
                this._content.opacity = Math.round(255 * content * (1 - clamp(ms / (this._secure ? 50 : 126))));
                this._bouncerBox.opacity = Math.round(255 * box * (1 - short));
                const radius = Math.trunc(blur * (1 - short));
                this._blur.enabled = radius > 0;
                wholeFrames(this, radius > 0);
                if (radius > 0)
                    this._blur.radius = radius;
                this._scrim.opacity = Math.round(255 * scrim * (1 - EMPHASIZED_ACCELERATE(clamp(ms / GONE_MS))));
                surface(TOUCH_RESPONSE(clamp((ms - SURFACE_DELAY_MS) / SURFACE_MS)));
            },
            onDone: () => {
                this._unlocking = null;
                surface(1);
                group.set_pivot_point(0, 0);
                group.offscreen_redirect = redirect;
                under.destroy();
                this._finishUnlock();
            }});
    }

    _finishUnlock() {
        const then = this._afterUnlock;
        this._afterCancel = null;
        this._unlockingNow = false;
        this._lockedDown = false;
        this._dropBouncer();
        this._setBackdrop(0);
        this._f = 1;
        this.actor.hide();
        this._wallpaper.show();
        this._black.opacity = 255;
        if (this._grab)
            Main.popModal(this._grab);
        this._grab = null;
        this.showing = false;
        this.emit('changed');
        then?.();
    }

    // for development (gp-eval): the lock screen goes as if the secret was right
    unlockForDevelopment() {
        if (!this.showing)
            return;
        this._afterRestart = false;
        this._goingAway();
    }

    // where the status icons end, screen px: as the home status bar's on the upright panel, less SYSTEM_INSET; on a
    // tablet TABLET_SYSTEM_END from the end whichever way it is turned (_relayout), a turn's cutout change included
    _systemEnd() {
        if (TABLET)
            return WIDTH - TABLET_SYSTEM_END;
        return systemEnd(this._cutout.upright, SCREEN_W) - SYSTEM_INSET;
    }

    destroy() {
        if (Main.screenShield)
            delete Main.screenShield._onStatusChanged;
        this._wallpaperLoad?.cancel();
        this._wallpaperLoad = null;
        this._settle?.stop();
        this._unlocking?.stop();
        wholeFrames(this, false);
        if (this._prerenderId)
            global.stage.disconnect(this._prerenderId);
        this._prerenderId = 0;
        this._vibeos?.disconnectObject(this);
        if (this._countdownId)
            GLib.source_remove(this._countdownId);
        this._wallClock.disconnectObject(this);
        this._interface.disconnectObject(this);
        this._background.disconnectObject(this);
        this._notificationSettings.disconnectObject(this);
        this._cutout.disconnectObject(this);
        this._state.disconnectObject(this);
        Main.messageTray.disconnectObject(this);
        for (const source of Main.messageTray.getSources())
            source.disconnectObject(this);
        if (this._grab)
            Main.popModal(this._grab);
        this._grab = null;
        this._showHiddenWindows();
        this._leaveLockMode();
        const group = global.window_group;
        group.opacity = 255;
        group.scale_x = group.scale_y = 1;
        group.translation_y = 0;
        this.actor.destroy();
    }
}
