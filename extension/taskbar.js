// A tablet's taskbar: Android 17's transient taskbar, as it is on the Galaxy Tab S5e.
//
// Over an app it is stashed: a handle, 220 × 4 dp, centred 16 dp above the screen's bottom. A swipe up from the bottom
// unstashes it as the swipe begins (navigation.js says so): a pill 77 dp high, its bottom 24 dp above the screen's,
// with the all-apps button, a divider, the dock's apps (the launcher's layout) and, after another divider, the apps
// running that are not in the dock, the most recent first. It stays out while recents is up, and over the app a swipe
// went back to until a touch lands beside it. At home, under the lock screen and over a fullscreen window there is
// none. A tap on an app opens it or brings its window back; the all-apps button goes home with the drawer open.
//
// Unstashing, the handle grows into the pill on EMPHASIZED in 430 ms; stashing, the icons go in 50 ms and the pill
// shrinks back into the handle in 410 ms. The handle is light over a dark app and dark over a light one: the screen
// just above it is read when it shows, when the app in front changes and every second while it stays (a change
// crosses over in 120 ms). Lengths are dp (dp() makes them the stage's).
import Clutter from 'gi://Clutter';
import Gio from 'gi://Gio';
import GLib from 'gi://GLib';
import GObject from 'gi://GObject';
import Meta from 'gi://Meta';
import Shell from 'gi://Shell';
import St from 'gi://St';

import * as Main from 'resource:///org/gnome/shell/ui/main.js';

import {Canvas} from './icons.js';
import {PathInterpolator} from './launch.js';
import {TabletTaskbar, rgba, setSource} from './palette.js';
import {animate, appGicon, clamp, lerp, lerpRect} from './task.js';
import {dp, geometry} from './units.js';

const HANDLE_W = 220, HANDLE_H = 4, HANDLE_ABOVE = 16;     // the handle's centre above the screen's bottom
const PILL_H = 77, PILL_BOTTOM = 24;
// along the pill from its left edge: the all-apps button's centre, the divider after it, the first app's centre;
// then an app every PITCH, a divider half way to the next group, and the right edge half a pitch past the last
const ALL_APPS_X = 47, DIVIDER_X = 94.5, FIRST_APP_X = 151.5, PITCH = 76, END = 38;
const GROUP_GAP = 57;                   // an app's centre to the divider after it, and that divider to the next app
const ALL_APPS = 34;                    // the all-apps glyph's box
const ICON = 53;                        // an app's icon (its touch box is the pitch wide)
const DIVIDER_W = 3, DIVIDER_H = 29;
const RECENT_MAX = 3;                   // the running apps shown past the dock's
const PRESSED_SCALE = 0.92;

const UNSTASH_MS = 430;
const STASH_MS = 410;
const ICONS_OUT_MS = 50;
const FADE_MS = 150;                    // shown or hidden all at once (home, the lock screen)
const TAP_SLOP = 12;
const SAMPLE_MS = 1000;
const SAMPLE_ABOVE = 6;                 // the screen read this far above the handle's top, at its middle and ends
const HANDLE_COLOR_MS = 120;
const LUMA_DARK = 0.45, LUMA_LIGHT = 0.55;  // a light handle below the first, a dark one above the second
const EMPHASIZED = new PathInterpolator('M0,0 C0.05,0 0.133333,0.06 0.166666,0.4 C0.208333,0.82 0.25,1 1,1');

const Mode = {HIDDEN: 0, STASHED: 1, UNSTASHED: 2};

// the all-apps button's glyph: three dots and a magnifier where the fourth would be
const AllAppsGlyph = GObject.registerClass(
class AllAppsGlyph extends Canvas {
    draw(cr, w, h) {
        cr.setOperator(0);
        cr.paint();
        cr.setOperator(2);
        const s = Math.min(w, h) / ALL_APPS;
        setSource(cr, TabletTaskbar.glyph);
        for (const [x, y] of [[8.5, 8.5], [25.5, 8.5], [8.5, 25.5]]) {
            cr.arc(x * s, y * s, 7.25 * s, 0, 2 * Math.PI);
            cr.fill();
        }
        cr.setLineWidth(2.75 * s);
        cr.setLineCap(1);
        cr.arc(24 * s, 24 * s, 5.25 * s, 0, 2 * Math.PI);
        cr.stroke();
        cr.moveTo(27.9 * s, 27.9 * s);
        cr.lineTo(32.25 * s, 32.25 * s);
        cr.stroke();
    }
});

export class Taskbar {
    constructor(nav, home) {
        this._nav = nav;
        this._home = home;
        this._mode = Mode.HIDDEN;
        this._p = 0;                    // the handle (0) to the pill (1)
        this._alpha = 0;                // all of it
        this._iconsAlpha = 0;
        this._items = [];               // {kind: 'apps' | 'app', app, actor, x (its centre along the pill)}
        this._pillW = 0;
        this._touch = null;
        this._motions = new Map();

        this.actor = new Clutter.Actor({name: 'vibeosTaskbar', reactive: false});
        this._handleDark = 0;           // the light handle (0) to the dark one (1)
        this._handle = new St.Widget({style: this._handleStyle()});
        this._pill = new St.Widget({
            reactive: true,
            style: `background-color: ${rgba(TabletTaskbar.fill)}; border: 1px solid ${rgba(TabletTaskbar.outline)};` +
                ` border-radius: ${dp(PILL_H)}px;`,
        });
        this._content = new Clutter.Actor();
        this.actor.add_child(this._handle);
        this.actor.add_child(this._pill);
        this.actor.add_child(this._content);
        this._pill.connectObject('event', (_a, event) => this._onEvent(event), this);

        global.display.connectObject('notify::focus-window', () => (this._later(), this._sampleSoon()),
            'window-created', () => this._later(), this);
        global.window_manager.connectObject('minimize', () => this._later(), 'unminimize', () => this._later(),
            'size-change', () => (this._later(), this._sampleSoon()), this);
        Shell.AppSystem.get_default().connectObject('app-state-changed', () => this._later(true),
            'installed-changed', () => this._later(true), this);
        geometry.connectObject('changed', () => this._layout(), this);
        Main.layoutManager.connectObject('monitors-changed', () => this._layout(), this);

        const dir = GLib.build_filenamev([GLib.get_user_config_dir(), 'vibeos-launcher']);
        this._layoutFile = Gio.File.new_for_path(GLib.build_filenamev([dir, 'layout.json']));
        try {
            this._monitor = this._layoutFile.monitor_file(Gio.FileMonitorFlags.NONE, null);
            this._monitor.connectObject('changed', () => this._later(true), this);
        } catch {
            this._monitor = null;
        }
        this._rebuild();
        this._layout();
    }

    // ---- what the navigation says ------------------------------------------------------------------------------

    // out as a pill: a swipe up began over an app, recents is coming
    unstash() {
        this._keep = false;
        this._set(Mode.UNSTASHED);
    }

    // the swipe went back to the app: the pill stays until a touch lands beside it
    keepOpen() {
        this._keep = true;
        this._set(Mode.UNSTASHED);
        this._listenBeside(true);
    }

    // going home: gone
    hide() {
        this._keep = false;
        this._set(Mode.HIDDEN);
    }

    // what the screen in front calls for now: none at home, under the lock screen or over a fullscreen window; the
    // handle over an app (the pill while it is kept out)
    sync() {
        if (this._destroyed || this._nav._g || this._nav._recents)
            return;
        const win = this._nav._appWindow();
        if (!win || win.is_fullscreen() || this._nav._keyguard?.showing) {
            this._keep = false;
            this._set(Mode.HIDDEN);
        } else if (this._keep && this._mode === Mode.UNSTASHED) {
            this._listenBeside(true);
        } else {
            this._set(Mode.STASHED);
        }
    }

    // recents holds the touches (its grab): a touch that lands on the pill is the taskbar's, to its end
    takeTouch(event) {
        const type = event.type();
        if (type === Clutter.EventType.TOUCH_BEGIN) {
            const [x, y] = event.get_coords();
            if (!this._onPill(x, y))
                return false;
        } else if (!this._touch || this._touch.slot !== (event.get_event_sequence()?.get_slot() ?? 0)) {
            return false;
        }
        this._onEvent(event);
        return true;
    }

    // ---- the apps ----------------------------------------------------------------------------------------------

    // signals come in bursts (a window opening is created, focused and resized): one look once they are over
    _later(apps = false) {
        this._appsChanged ||= apps;
        if (this._laterId)
            return;
        this._laterId = GLib.idle_add(GLib.PRIORITY_DEFAULT_IDLE, () => {
            this._laterId = 0;
            if (this._appsChanged || this._recentIds().join() !== this._shownRecent?.join())
                this._rebuild();
            this._appsChanged = false;
            this.sync();
            return GLib.SOURCE_REMOVE;
        });
    }

    // the dock's apps, as the launcher keeps them
    _dockIds() {
        try {
            const [, bytes] = this._layoutFile.load_contents(null);
            const dock = JSON.parse(new TextDecoder().decode(bytes))?.dock;
            return Array.isArray(dock) ? dock.filter(id => typeof id === 'string') : [];
        } catch {
            return [];
        }
    }

    // the apps with a window open that are not in the dock, the one used last first
    _recentIds() {
        const dock = new Set(this._dockIds());
        const tracker = Shell.WindowTracker.get_default();
        const ids = [];
        for (const win of global.display.get_tab_list(Meta.TabList.NORMAL, null)) {
            if (!this._nav.isTask(win))
                continue;
            const id = tracker.get_window_app(win)?.get_id();
            if (id && !dock.has(id) && !ids.includes(id))
                ids.push(id);
        }
        return ids.slice(0, RECENT_MAX);
    }

    _rebuild() {
        for (const item of this._items)
            item.actor.destroy();
        this._items = [];
        this._content.destroy_all_children();
        const appSystem = Shell.AppSystem.get_default();
        const dock = this._dockIds().map(id => appSystem.lookup_app(id)).filter(app => app);
        const recentIds = this._recentIds();
        this._shownRecent = recentIds;
        const recent = recentIds.map(id => appSystem.lookup_app(id)).filter(app => app);

        const glyph = new AllAppsGlyph({width: dp(ALL_APPS), height: dp(ALL_APPS)});
        this._add({kind: 'apps', actor: glyph, x: ALL_APPS_X, size: ALL_APPS});
        this._divider(DIVIDER_X);
        let x = FIRST_APP_X - PITCH;
        for (const app of dock)
            this._addApp(app, (x += PITCH));
        if (recent.length) {
            const divider = (dock.length ? x : DIVIDER_X - GROUP_GAP) + GROUP_GAP;
            this._divider(divider);
            x = divider + GROUP_GAP - PITCH;
            for (const app of recent)
                this._addApp(app, (x += PITCH));
        }
        this._pillW = Math.max(x, ALL_APPS_X) + END;
        this._layout();
    }

    _add(item) {
        item.actor.set_pivot_point(0.5, 0.5);
        item.actor.set_position(dp(item.x - item.size / 2), dp((PILL_H - item.size) / 2));
        this._content.add_child(item.actor);
        this._items.push(item);
    }

    _addApp(app, x) {
        const path = this._nav.icons.get(app.get_id())?.path || null;
        const actor = new St.Icon({gicon: appGicon(app, path), icon_size: dp(ICON), style: 'padding: 0;'});
        this._add({kind: 'app', app, actor, x, size: ICON});
    }

    _divider(x) {
        this._content.add_child(new St.Widget({
            x: dp(x - DIVIDER_W / 2), y: dp((PILL_H - DIVIDER_H) / 2), width: dp(DIVIDER_W), height: dp(DIVIDER_H),
            style: `background-color: ${rgba(TabletTaskbar.divider)}; border-radius: ${dp(DIVIDER_W / 2)}px;`,
        }));
    }

    // ---- where it is -------------------------------------------------------------------------------------------

    _rects() {
        const m = Main.layoutManager.primaryMonitor;
        const w = m?.width ?? 0, h = m?.height ?? 0;
        const handle = [w / 2 - dp(HANDLE_W / 2), h - dp(HANDLE_ABOVE + HANDLE_H / 2),
            w / 2 + dp(HANDLE_W / 2), h - dp(HANDLE_ABOVE - HANDLE_H / 2)];
        const pill = [w / 2 - dp(this._pillW / 2), h - dp(PILL_BOTTOM + PILL_H),
            w / 2 + dp(this._pillW / 2), h - dp(PILL_BOTTOM)];
        return {m, handle, pill};
    }

    _layout() {
        if (this._destroyed)
            return;
        const {m, handle, pill} = this._rects();
        if (m) {
            this.actor.set_position(m.x, m.y);
            this.actor.set_size(m.width, m.height);
        }
        const [x0, y0, x1, y1] = lerpRect(handle, pill, this._p);
        // the pill's colours over the handle's in the first third of the way
        const solid = clamp(this._p * 3, 0, 1);
        for (const actor of [this._handle, this._pill]) {
            actor.set_position(x0, y0);
            actor.set_size(x1 - x0, y1 - y0);
        }
        this._handle.opacity = Math.round(255 * this._alpha * (1 - solid));
        this._sampleWhile(this._mode === Mode.STASHED && this._alpha > 0);
        this._pill.opacity = Math.round(255 * this._alpha * solid);
        this._pill.reactive = this._mode === Mode.UNSTASHED;
        this._content.set_position(pill[0], pill[1]);
        this._content.opacity = Math.round(255 * this._alpha * this._iconsAlpha);
        this.actor.visible = this._alpha > 0;
    }

    // ---- the handle's colour -----------------------------------------------------------------------------------

    _handleStyle() {
        const [l, d, t] = [TabletTaskbar.handleLight, TabletTaskbar.handleDark, this._handleDark];
        const c = [0, 1, 2].map(i => Math.round(lerp(l[i], d[i], t)));
        return `background-color: ${rgba(c, lerp(l[3], d[3], t))}; border-radius: ${dp(PILL_H)}px;`;
    }

    // reading the screen once a second while the handle shows
    _sampleWhile(on) {
        if (on === !!this._sampleId)
            return;
        if (!on) {
            GLib.source_remove(this._sampleId);
            this._sampleId = 0;
            return;
        }
        this._sampleId = GLib.timeout_add(GLib.PRIORITY_DEFAULT_IDLE, SAMPLE_MS, () => {
            this._sample();
            return GLib.SOURCE_CONTINUE;
        });
        this._sample();
    }

    // the app in front changed: a read once it has drawn
    _sampleSoon() {
        if (!this._sampleId || this._soonId)
            return;
        this._soonId = GLib.timeout_add(GLib.PRIORITY_DEFAULT_IDLE, 150, () => {
            this._soonId = 0;
            this._sample();
            return GLib.SOURCE_REMOVE;
        });
    }

    async _sample() {
        if (this._sampling || this._destroyed)
            return;
        // never during a swipe, its animation or the shade: a read draws its spot of the stage once more, a frame's
        // time taken from them
        const nav = this._nav;
        if (nav.busy || nav._recents || nav.shadeOpen())
            return;
        const {m, handle} = this._rects();
        if (!m)
            return;
        this._sampling = true;
        let luma = 0;
        try {
            this._shooter ??= new Shell.Screenshot();
            const y = Math.round(m.y + handle[1] - dp(SAMPLE_ABOVE));
            for (const x of [handle[0] + dp(8), (handle[0] + handle[2]) / 2, handle[2] - dp(8)]) {
                // eslint-disable-next-line no-await-in-loop
                const [color] = await this._shooter.pick_color(Math.round(m.x + x), y);
                luma += (0.2126 * color.red + 0.7152 * color.green + 0.0722 * color.blue) / 255 / 3;
            }
        } catch {
            return;
        } finally {
            this._sampling = false;
        }
        if (this._destroyed)
            return;
        const dark = luma > LUMA_LIGHT ? 1 : luma < LUMA_DARK ? 0 : this._handleTarget ?? 0;
        if (dark === this._handleTarget)
            return;
        this._handleTarget = dark;
        const from = this._handleDark;
        if (this._alpha === 0 || this._handle.opacity === 0) {
            this._handleDark = dark;
            this._handle.style = this._handleStyle();
            return;
        }
        this._run('handleColor', HANDLE_COLOR_MS, t => {
            this._handleDark = lerp(from, dark, t);
            this._handle.style = this._handleStyle();
        });
    }

    _onPill(x, y) {
        if (this._mode !== Mode.UNSTASHED || this._alpha < 1)
            return false;
        const [x0, y0, x1, y1] = this._rects().pill;
        const m = this._rects().m;
        return x - m.x >= x0 && x - m.x < x1 && y - m.y >= y0 && y - m.y < y1;
    }

    // ---- moving ------------------------------------------------------------------------------------------------

    _run(name, ms, fn, done) {
        this._motions.get(name)?.stop();
        let elapsed = 0;
        fn(0);
        this._layout();
        const anim = animate(this.actor, `taskbar-${name}`, dt => {
            elapsed += dt * 1000;
            const t = ms > 0 ? clamp(elapsed / ms, 0, 1) : 1;
            fn(t);
            this._layout();
            return t < 1;
        }, () => {
            this._motions.delete(name);
            done?.();
        });
        this._motions.set(name, anim);
    }

    _set(mode) {
        if (this._destroyed || mode === this._mode)
            return;
        const was = this._mode;
        this._mode = mode;
        this._listenBeside(mode === Mode.UNSTASHED && this._keep);
        if (mode === Mode.HIDDEN) {
            const alpha = this._alpha;
            this._run('alpha', FADE_MS * alpha, t => (this._alpha = alpha * (1 - t)), () => {
                this._motions.get('p')?.stop();
                this._motions.delete('p');
                this._p = 0;
                this._iconsAlpha = 0;
                this._layout();
            });
            return;
        }
        if (this._alpha < 1) {
            const alpha = this._alpha;
            this._run('alpha', FADE_MS * (1 - alpha), t => (this._alpha = lerp(alpha, 1, t)));
        }
        const p = this._p, icons = this._iconsAlpha;
        if (mode === Mode.UNSTASHED) {
            // from nothing (home, recents from home) the pill fades in as it is; from the handle it grows out of it
            if (was === Mode.HIDDEN && this._alpha === 0) {
                this._p = 1;
                this._iconsAlpha = 1;
                this._layout();
                return;
            }
            this._run('p', UNSTASH_MS * (1 - p), t => {
                const e = EMPHASIZED.get(t);
                this._p = lerp(p, 1, e);
                this._iconsAlpha = Math.max(icons, clamp((e - 0.5) * 2, 0, 1));
            });
        } else {
            this._run('icons', ICONS_OUT_MS * icons, t => (this._iconsAlpha = icons * (1 - t)));
            this._run('p', STASH_MS * p, t => (this._p = p * (1 - EMPHASIZED.get(t))));
        }
    }

    // A touch beside the pill over an app stashes it (and goes on to the app)
    _listenBeside(on) {
        if (on === !!this._besideId)
            return;
        if (!on) {
            global.stage.disconnect(this._besideId);
            this._besideId = 0;
            return;
        }
        this._besideId = global.stage.connect('captured-event', (_s, event) => {
            const type = event.type();
            if (type !== Clutter.EventType.TOUCH_BEGIN && type !== Clutter.EventType.BUTTON_PRESS)
                return Clutter.EVENT_PROPAGATE;
            const [x, y] = event.get_coords();
            if (!this._onPill(x, y) && !this._nav._g && !this._nav._recents) {
                this._keep = false;
                this._set(Mode.STASHED);
            }
            return Clutter.EVENT_PROPAGATE;
        });
    }

    // ---- touches on the pill -----------------------------------------------------------------------------------

    _itemAt(x, y) {
        const {m, pill} = this._rects();
        const px = (x - m.x - pill[0]) / dp(1), py = (y - m.y - pill[1]) / dp(1);
        if (py < 0 || py > PILL_H)
            return null;
        return this._items.find(item => Math.abs(px - item.x) <= (item.kind === 'app' ? PITCH : 2 * ALL_APPS_X) / 2) ??
            null;
    }

    _press(item, on) {
        if (!item)
            return;
        item.actor.ease({
            scale_x: on ? PRESSED_SCALE : 1, scale_y: on ? PRESSED_SCALE : 1, duration: 100,
            mode: Clutter.AnimationMode.EASE_OUT_QUAD,
        });
    }

    _onEvent(event) {
        const type = event.type();
        const [x, y] = event.get_coords();
        const slot = event.get_event_sequence()?.get_slot() ?? 0;
        switch (type) {
        case Clutter.EventType.TOUCH_BEGIN:
        case Clutter.EventType.BUTTON_PRESS:
            if (this._touch)
                return Clutter.EVENT_STOP;
            this._touch = {slot, x0: x, y0: y, item: this._itemAt(x, y)};
            this._press(this._touch.item, true);
            return Clutter.EVENT_STOP;
        case Clutter.EventType.TOUCH_UPDATE:
        case Clutter.EventType.MOTION:
            if (this._touch && this._touch.item && Math.hypot(x - this._touch.x0, y - this._touch.y0) > dp(TAP_SLOP)) {
                this._press(this._touch.item, false);
                this._touch.item = null;
            }
            return this._touch ? Clutter.EVENT_STOP : Clutter.EVENT_PROPAGATE;
        case Clutter.EventType.TOUCH_END:
        case Clutter.EventType.BUTTON_RELEASE: {
            const touch = this._touch;
            this._touch = null;
            if (!touch)
                return Clutter.EVENT_PROPAGATE;
            this._press(touch.item, false);
            if (touch.item)
                this._activate(touch.item);
            return Clutter.EVENT_STOP;
        }
        case Clutter.EventType.TOUCH_CANCEL:
            this._press(this._touch?.item, false);
            this._touch = null;
            return Clutter.EVENT_STOP;
        default:
            return Clutter.EVENT_PROPAGATE;
        }
    }

    _activate(item) {
        if (item.kind === 'apps') {
            this.hide();
            this._nav.openApps();
            return;
        }
        this._keep = false;
        this._nav.openApp(item.app);
    }

    destroy() {
        this._destroyed = true;
        this._listenBeside(false);
        this._sampleWhile(false);
        if (this._soonId)
            GLib.source_remove(this._soonId);
        this._soonId = 0;
        if (this._laterId)
            GLib.source_remove(this._laterId);
        this._laterId = 0;
        for (const anim of this._motions.values())
            anim.stop();
        this._motions.clear();
        this._monitor?.cancel();
        this._monitor = null;
        global.display.disconnectObject(this);
        global.window_manager.disconnectObject(this);
        Shell.AppSystem.get_default().disconnectObject(this);
        geometry.disconnectObject(this);
        Main.layoutManager.disconnectObject(this);
        this.actor.destroy();
    }
}
