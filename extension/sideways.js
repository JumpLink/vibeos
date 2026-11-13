// The two system swipes while the panel is sideways: an app in landscape (a game, which rotation.js keeps sideways,
// or any app turned with the phone). navigation.js and back.js are laid out for the upright panel (their strips are
// at its bottom and its sides, 1080 x 2280 device px), and sideways neither could start: the screen was a trap,
// Quake's above all, which takes no Escape as a way out. Here the same two swipes, plainly:
//
//   up from the bottom edge (the gesture bar): home. The app goes to recents (minimized) and the launcher is
//     in front, which turns the panel upright again.
//   in from the left or the right edge (EDGE_W): back, which is Escape to the app (an unreadable app's back in
//     back.js too).
//
// An app whose desktop file says X-Phone-Leave=close is closed by either swipe instead. Quake: its Escape is its own
// menu, and hidden it would go on rendering and playing its sound.
//
// The window shrinks toward where it goes as the finger moves (from the bottom edge's middle for home, from the far
// edge for back); released past COMMIT_DP it goes, short of it it springs back. A finger resting on a strip for
// 250 ms without moving is the app's, as back.js gives it back (a game's buttons sit near the edges).
import Clutter from 'gi://Clutter';
import Gio from 'gi://Gio';
import GLib from 'gi://GLib';
import Shell from 'gi://Shell';

import * as Main from 'resource:///org/gnome/shell/ui/main.js';

import {NAV_GESTURE_H} from './task.js';

const EDGE_W = 30 * 3;                  // device px, the same edge strip as back.js
const SLOP = 8 * 3 * 0.75;              // touch slop × 0.75, as back.js
const LONG_PRESS_MS = 250;
const COMMIT_DP = 56;                   // released this far in: home / back
const MIN_SCALE = 0.8;                  // the window at COMMIT_DP and beyond
const SETTLE_MS = 200;
const CLOSE_GRACE_MS = 3000;            // an app asked to close that is still there: whole again

// one panel, wider than tall: ours to handle (docked, nothing is turned and the upright gestures stand)
export function sideways() {
    const monitors = Main.layoutManager.monitors;
    return monitors.length === 1 && monitors[0].width > monitors[0].height;
}

export class SidewaysGestures {
    constructor(nav) {
        this._nav = nav;
        this._g = null;
        this._longPressId = 0;
        this._closeIds = new Set();
        this._gesture = new Clutter.PanGesture({
            name: 'gnome-phone sideways',
            begin_threshold: SLOP / nav.scale,
            max_n_points: 1,
        });
        this._gesture.connectObject(
            'should-handle-sequence', (_g, event) => this._shouldHandle(event),
            'may-recognize', () => this._mayRecognize(),
            'recognize', () => this._begin(),
            'pan-update', () => this._move(),
            'end', () => this._end(false),
            'cancel', () => this._end(true),
            'notify::state', () => {
                const state = this._gesture.state;
                if ((state === Clutter.GestureState.CANCELLED || state === Clutter.GestureState.COMPLETED) &&
                    this._g && !this._g.started)
                    this._end(true);
            },
            this);
        global.stage.add_action_full('gnome-phone-sideways', Clutter.EventPhase.CAPTURE, this._gesture);
    }

    get busy() {
        return !!this._g;
    }

    _scale() {
        return this._nav.scale;
    }

    // [x, y] of the gesture's finger, device px
    _point() {
        const c = this._gesture.get_centroid_abs();
        return [c.x * this._scale(), c.y * this._scale()];
    }

    _shouldHandle(event) {
        if (event.type() !== Clutter.EventType.TOUCH_BEGIN || this._g || !sideways())
            return false;
        const nav = this._nav;
        if (Main.actionMode !== Shell.ActionMode.NORMAL || nav.busy || nav._back?.busy || nav.recentsShown ||
            nav.shadeOpen() || nav._keyguard?.showing)
            return false;
        const win = global.display.focus_window;
        if (!win || win.minimized || !nav.isTask(win))
            return false;
        const s = this._scale();
        const m = Main.layoutManager.monitors[0];
        const W = m.width * s, H = m.height * s;
        const [x, y] = event.get_coords().map(v => v * s);
        let kind;
        if (y >= H - NAV_GESTURE_H)
            kind = 'home';
        else if (x < EDGE_W || x > W - EDGE_W)
            kind = 'back';
        else
            return false;
        this._g = {kind, win, x0: x, y0: y, left: x < W / 2, W, H, started: false, past: false, p: 0};
        this._longPressId = GLib.timeout_add(GLib.PRIORITY_DEFAULT, LONG_PRESS_MS, () => {
            this._longPressId = 0;
            if (this._g && !this._g.started)
                this._gesture.cancel();
            return GLib.SOURCE_REMOVE;
        });
        return true;
    }

    // home: more up than across; back: more across (inward) than along
    _mayRecognize() {
        const g = this._g;
        if (!g)
            return false;
        const [x, y] = this._point();
        const dx = x - g.x0, dy = y - g.y0;
        if (g.kind === 'home')
            return -dy > Math.abs(dx);
        return Math.abs(dx) > Math.abs(dy) && (g.left ? dx > 0 : dx < 0);
    }

    _clearLongPress() {
        if (this._longPressId)
            GLib.source_remove(this._longPressId);
        this._longPressId = 0;
    }

    _distance() {
        const g = this._g;
        const [x, y] = this._point();
        if (g.kind === 'home')
            return Math.max(0, g.y0 - y);
        return Math.max(0, g.left ? x - g.x0 : g.x0 - x);
    }

    _begin() {
        const g = this._g;
        if (!g)
            return;
        this._clearLongPress();
        g.started = true;
        g.actor = g.win.get_compositor_private();
        if (g.actor) {
            g.actor.remove_all_transitions();
            // home: toward the bottom edge's middle; back: toward the far edge
            g.actor.set_pivot_point(g.kind === 'home' ? 0.5 : g.left ? 1 : 0, g.kind === 'home' ? 1 : 0.5);
        }
        this._move();
    }

    _move() {
        const g = this._g;
        if (!g?.started)
            return;
        const commit = COMMIT_DP * 3;
        const d = this._distance();
        const past = d >= commit;
        if (past !== g.past) {
            g.past = past;
            this._nav.haptic();
        }
        // linear to MIN_SCALE at the commit distance, a quarter as fast after it
        const p = d <= commit ? d / commit : 1 + (d - commit) / commit / 4;
        const scale = Math.max(0.5, 1 - (1 - MIN_SCALE) * p);
        g.actor?.set_scale(scale, scale);
    }

    _end(cancelled) {
        const g = this._g;
        this._clearLongPress();
        if (!g)
            return;
        this._g = null;
        if (!g.started)
            return;
        if (cancelled || !g.past || g.win.minimized || !g.win.get_compositor_private()) {
            this._whole(g.actor, true);
            return;
        }
        if (this._closes(g.win))
            this._close(g);
        else if (g.kind === 'home')
            this._home(g);
        else {
            this._whole(g.actor, true);
            this._nav.key(Clutter.KEY_Escape);
        }
    }

    // the window at rest again, eased or at once
    _whole(actor, eased) {
        if (!actor)
            return;
        actor.remove_all_transitions();
        if (eased) {
            actor.ease({
                scale_x: 1, scale_y: 1, opacity: 255,
                duration: SETTLE_MS,
                mode: Clutter.AnimationMode.EASE_OUT_QUAD,
            });
        } else {
            actor.set_scale(1, 1);
            actor.opacity = 255;
        }
    }

    // X-Phone-Leave=close in the window's app's desktop file, read now: the shell keeps the app info it read first,
    // and a key added since (as Quake's was, with Quake running) is not in it
    _closes(win) {
        const file = Shell.WindowTracker.get_default().get_window_app(win)?.get_app_info()?.get_filename();
        const info = file ? Gio.DesktopAppInfo.new_from_filename(file) : null;
        return info?.get_string('X-Phone-Leave') === 'close';
    }

    _toLauncher() {
        const nav = this._nav;
        nav.home.window?.activate(global.get_current_time());
        nav.launcherState('normal');
        nav.statusBarOverApp(null);
    }

    _home(g) {
        const actor = g.actor;
        if (actor)
            Main.wm.skipNextEffect(actor);
        g.win.minimize();
        this._whole(actor, false);
        this._toLauncher();
    }

    _close(g) {
        const {win, actor} = g;
        actor?.ease({
            opacity: 0,
            duration: SETTLE_MS,
            mode: Clutter.AnimationMode.EASE_OUT_QUAD,
        });
        win.delete(global.get_current_time());
        this._toLauncher();
        // an app that does not go: back as it was, in front
        const id = GLib.timeout_add(GLib.PRIORITY_DEFAULT, CLOSE_GRACE_MS, () => {
            this._closeIds.delete(id);
            const a = win.get_compositor_private();
            if (a) {
                this._whole(a, false);
                if (!win.minimized)
                    win.activate(global.get_current_time());
            }
            return GLib.SOURCE_REMOVE;
        });
        this._closeIds.add(id);
    }

    destroy() {
        this._clearLongPress();
        for (const id of this._closeIds)
            GLib.source_remove(id);
        this._closeIds.clear();
        if (this._g?.started)
            this._whole(this._g.actor, false);
        this._g = null;
        this._gesture.get_actor()?.remove_action(this._gesture);
        this._gesture.disconnectObject(this);
    }
}
