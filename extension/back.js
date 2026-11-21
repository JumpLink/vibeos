// Predictive back: a swipe in from either edge. An arrow
// follows the finger (backpanel.js); what back does was asked of the app when the finger came down (a11y.js):
//
//   an app with somewhere to go back to (a Back button showing): the arrow alone, and on release the app goes back
//     itself (the button is clicked; libadwaita slides its own page away)
//   an app at its root: the window shrinks toward the far edge to 0.75 under the
//     finger (BACK_GESTURE of the progress spring), its corners rounding to 120 px, over the launcher at 0.85 behind a
//     black 0.8 scrim blurred 90 → 10 px; on release it flies into its icon from rest, the scrim fading in 233 ms
//   a dialog, or an app the probe cannot read (Waydroid's: Android takes Escape as BACK): Escape
//   recents: as its own Escape; the launcher's drawer: closed
//   the keyboard up, over any of these: the keyboard goes down, and that is all (Android's back with the IME shown)
//
// The gesture: a strip EDGE_W wide on either side above the gesture bar; the swipe starts when it is more
// across than along past SLOP, and gives the touch back to the app after 250 ms without that.
import Clutter from 'gi://Clutter';
import GLib from 'gi://GLib';
import Meta from 'gi://Meta';
import Shell from 'gi://Shell';

import * as Main from 'resource:///org/gnome/shell/ui/main.js';

import {BackProbe} from './a11y.js';
import {BackPanelController} from './backpanel.js';
import {dismissKeyboard} from './keyboard/keyboard.js';
import {cubic, openerOf} from './launch.js';
import {Animator} from './motion.js';
import {overWindows} from './scanout.js';
import {NAV_GESTURE_H, SCREEN_H, SCREEN_W, Spring, TaskView, clamp, lerp} from './task.js';
import {geometry} from './units.js';
import {sideways} from './sideways.js';

const EDGE_W = 30 * 3;                  // the edge strip a back swipe starts in
const SLOP = 8 * 3 * 0.75;              // touch slop × 0.75
const LONG_PRESS_MS = 250;              // min(back_timeout, the long press timeout)
let MAX_DISTANCE = SCREEN_W;            // progress = dx / the display's width, linear
geometry.connect('changed', () => {
    MAX_DISTANCE = SCREEN_W;
});
const PROGRESS_SPRING = [1500, 1];      // the progress follows the finger on a medium, unbouncy spring
const PROBE_WAIT_MS = 600;              // released before the app answered: how long to wait for it
// The window first follows the finger 150 ms after it came down (the launcher is started first, and the
// progress until then never reaches it), its spring starting from 0 then; a fling released sooner flies from the
// whole window. The setup is done at once, unseen, so that its first frames are not the window's first moves.
const HOME_START_MS = 150;
// The walk takes ~200 ms with the shell idle, three times that under a gesture's frames: the focused app is asked
// ahead, when it gets the focus and once a touch in it has had time to change its page, and the answer is used at
// once while the touch's own probe checks it
const PREFETCH_MS = 700;

// the window going back to the home screen
const MIN_WINDOW_SCALE = 0.75;
const WINDOW_MARGIN = 10 * 3;
const END_CORNER_R = 40 * 3;
const SCRIM_ALPHA = 0.8;                // dark theme
const MAX_BLUR = 30 * 3;                // the launcher's blur at the start of the gesture
const MIN_BLUR_PRE_COMMIT = 10;

const BACK_GESTURE = (() => {
    const c = cubic(0.1, 0.1, 0, 1);
    return t => c.get(t);
})();
const DECELERATE = t => 1 - (1 - t) * (1 - t);

// back into the file a window was opened from (launch.js openerOf): it lands on the file's box in FILE_MS, emphasized
// decelerate, its corners those of the file's thumbnail, and fades out over the last part of the way
const FILE_MS = 400;
const FILE_FADE_FROM_MS = 220;
const FILE_CORNER_R = 12 * 3;
const FILE_CURVE = cubic(0.05, 0.7, 0.1, 1);
const CLOSE_WAIT_MS = 1500;             // a window still there by then refused to close (unsaved changes): it is back

// ---- back to home: the window under the finger, then into its icon ------------------------------------------------
// A window a file was opened in (a photo from Files) goes back to the app it came from instead: that app's window is
// under it all along, with no scrim, and on release the window lands on the file and closes (Android finishing the
// viewer back to the activity that started it).

class BackToHome {
    constructor(nav, win, left) {
        this._nav = nav;
        this._win = win;
        this._left = left;
        this._opener = openerOf(win);
        this._scale = nav.scale;
        const m = Main.layoutManager.primaryMonitor;
        this.layer = new Clutter.Actor({name: 'vibeosBack', x: m.x, y: m.y, width: m.width, height: m.height});
        Main.layoutManager.uiGroup.insert_child_above(this.layer, global.window_group);
        overWindows(this.layer);
        // The scrim over the launcher, a colour layer with a background blur, is the launcher's to draw: the
        // shell's own blur reads the framebuffer at logical coordinates on a scaled monitor, and rebuilds its buffers
        // whenever the radius changes (17 fps here). The back state starts it at 0.8 over 90 px.
        this._task = new TaskView(win, this._scale, this.layer);
        this._task.place([0, 0, SCREEN_W, SCREEN_H], 0, 1);
        this._lastBlur = MAX_BLUR;
        this.p = 0;
        this.rect = [0, 0, SCREEN_W, SCREEN_H];
        this.r = 0;

        if (this._opener) {
            // the app it came from right under it, whatever was opened over it since
            this._opener.from.raise();
            return;
        }
        const app = Shell.WindowTracker.get_default().get_window_app(win);
        // the workspace at 0.85 at once, the depth of an app in front with its blur held, the app's icon hidden
        nav.launcherState('back', 0, app?.get_id() ?? '');
        nav.statusBarOverApp(false);
    }

    // p: BACK_GESTURE of the progress; dy: the finger's travel along the edge
    progress(p, dy) {
        this.p = p;
        const w = SCREEN_W * lerp(1, MIN_WINDOW_SCALE, p), h = SCREEN_H / SCREEN_W * w;
        // the window follows the finger up or down, never past the margin
        const ratio = DECELERATE(Math.min(SCREEN_H / 2, Math.abs(dy)) / (SCREEN_H / 2));
        const yOff = (dy < 0 ? -1 : 1) * ratio * Math.max(0, (SCREEN_H - h) / 2 - WINDOW_MARGIN);
        const top = (SCREEN_H - h) / 2 + yOff;
        const left = this._left ? SCREEN_W - p * WINDOW_MARGIN - w : p * WINDOW_MARGIN;
        this.rect = [left, top, left + w, top + h];
        this.r = lerp(0, END_CORNER_R, p);
        this._task.place(this.rect, this.r, 1);
        if (this._opener)
            return;
        const blur = Math.round(lerp(MAX_BLUR, MIN_BLUR_PRE_COMMIT, p));
        if (blur !== this._lastBlur)
            this._nav.launcher('Scrim', new GLib.Variant('(dd)', [SCRIM_ALPHA, blur]));
        this._lastBlur = blur;
    }

    // released: into the icon from rest, the launcher's reveal without its fade, and its scrim and blur gone in 233 ms
    // (the launcher's, from the flight's start)
    commit(done) {
        if (this._opener) {
            this._intoFile(done);
            return;
        }
        this._nav.flight(this._win, this._task, this.rect, this.r, {x: 0, y: 0}, {state: 'back-reveal', cropTop: true},
            () => {
                this.destroy();
                done();
            });
    }

    // into the file: the app it came from takes the focus at once, the window closes once it is out of sight
    _intoFile(done) {
        const {from, box} = this._opener;
        const m = Main.layoutManager.primaryMonitor;
        const sc = this._scale;
        const end = [(box.x - m.x) * sc, (box.y - m.y) * sc, (box.x - m.x + box.w) * sc, (box.y - m.y + box.h) * sc];
        const start = this.rect, r0 = this.r;
        const win = this._win;
        from.activate(global.get_current_time());
        this._fly = new Animator(global.stage, (dt, elapsed) => {
            const ms = elapsed * 1000;
            const p = FILE_CURVE.get(clamp(ms / FILE_MS, 0, 1));
            const rect = start.map((v, i) => lerp(v, end[i], p));
            const alpha = 1 - clamp((ms - FILE_FADE_FROM_MS) / (FILE_MS - FILE_FADE_FROM_MS), 0, 1);
            this._task.place(rect, lerp(r0, FILE_CORNER_R, p), alpha);
            return ms < FILE_MS;
        }, () => {
            this._fly = null;
            const actor = win.get_compositor_private();
            this.destroy();
            if (actor) {
                // hidden until it is gone, rather than whole again for the moment the app takes to close it
                actor.opacity = 0;
                Main.wm.skipNextEffect(actor);
                win.delete(global.get_current_time());
                const id = GLib.timeout_add(GLib.PRIORITY_DEFAULT, CLOSE_WAIT_MS, () => {
                    const still = win.get_compositor_private();
                    if (still) {
                        still.opacity = 255;
                        win.activate(global.get_current_time());
                    }
                    return GLib.SOURCE_REMOVE;
                });
                actor.connect('destroy', () => GLib.source_remove(id));
            }
            done();
        });
    }

    // the progress came back to 0: the window is whole again, the launcher back to rest (or it back over the app it
    // came from)
    cancel() {
        if (this._opener) {
            this._win.raise();
            this.destroy();
            return;
        }
        this._nav.launcherState('normal');
        this._nav.statusBarOverApp(null);
        this.destroy();
    }

    destroy() {
        if (this._destroyed)
            return;
        this._destroyed = true;
        this._fly?.stop();
        this._task.destroy();
        this.layer.destroy();
    }
}

// ---- the gesture --------------------------------------------------------------------------------------------------

export class EdgeBack {
    constructor(nav) {
        this._nav = nav;
        this._scale = nav.scale;
        this._probe = new BackProbe();
        this._panel = new BackPanelController({
            setTriggerBack: b => this._setTriggerBack(b),
            triggerBack: () => this._invoke(),
            cancelBack: () => this._cancel(),
            haptic: () => nav.haptic(),
        });
        this._g = null;
        this._answer = null;                // {win, result}: what back meant in the focused app when last asked
        this._epoch = 0;                    // bumped when a back changes the app: older answers are stale
        this._prefetchId = 0;
        this._longPressId = 0;
        this._spring = new Spring(PROGRESS_SPRING, 0, 0, 0, 1);
        this._spring.done = true;
        this._anim = null;

        this._gesture = new Clutter.PanGesture({
            name: 'vibeos back',
            begin_threshold: SLOP / this._scale,
            max_n_points: 1,
        });
        this._gesture.connectObject(
            'should-handle-sequence', (_g, event) => this._shouldHandle(event),
            'may-recognize', () => this._mayRecognize(),
            'recognize', () => this._begin(),
            'pan-update', () => this._move(),
            'end', () => this._end(false),
            'cancel', () => this._end(true),
            // a sequence dropped before the gesture recognized emits neither end nor cancel
            'notify::state', () => {
                const state = this._gesture.state;
                if ((state === Clutter.GestureState.CANCELLED || state === Clutter.GestureState.COMPLETED) &&
                    this._g && !this._g.started)
                    this._end(true);
            },
            this);
        global.stage.add_action_full('vibeos-back', Clutter.EventPhase.CAPTURE, this._gesture);
        global.display.connectObject('notify::focus-window', () => this._prefetch(), this);
    }

    // watch the touches of actor, which holds a grab, or of the stage when null
    watch(actor) {
        actor ??= global.stage;
        const current = this._gesture.get_actor();
        if (current === actor)
            return;
        current?.remove_action(this._gesture);
        actor.add_action_full('vibeos-back', Clutter.EventPhase.CAPTURE, this._gesture);
    }

    // ask the focused app what back means there, a moment from now
    _prefetch(ms = PREFETCH_MS) {
        if (this._prefetchId)
            GLib.source_remove(this._prefetchId);
        this._prefetchId = GLib.timeout_add(GLib.PRIORITY_DEFAULT, ms, () => {
            this._prefetchId = 0;
            const target = this._g ? null : this._target();
            if (target?.kind === 'probe')
                this._ask(target.win);
            return GLib.SOURCE_REMOVE;
        });
    }

    _ask(win) {
        const epoch = this._epoch;
        return this._probe.probe(win.get_pid()).then(result => {
            if (result.kind !== 'unknown' && epoch === this._epoch)
                this._answer = {win, result};
            return result;
        });
    }

    // the app is going back: what back means there is not known until it is asked again
    _forget() {
        this._answer = null;
        this._epoch++;
    }

    _point() {
        const c = this._gesture.get_centroid_abs();
        const t = Clutter.get_current_event_time() || GLib.get_monotonic_time() / 1000;
        return [c.x * this._scale, c.y * this._scale, t];
    }

    // what back means for what is in front now: {kind, win} or null
    _target() {
        const nav = this._nav;
        const focus = global.display.focus_window;
        const launcher = !focus || nav.home.isHome(focus);
        if (Main.keyboard.visible)
            return {kind: 'keyboard', launcher};
        if (nav.recentsShown)
            return {kind: 'recents'};
        if (nav.bouncerShown)
            return {kind: 'keyguard'};
        if (launcher)
            return {kind: nav.drawerOpen ? 'drawer' : 'home-screen'};
        if (focus.get_transient_for() || focus.get_window_type() !== Meta.WindowType.NORMAL)
            return {kind: 'key', win: focus};
        if (focus.get_wm_class()?.startsWith('waydroid.') || !nav.isTask(focus))
            return {kind: 'key', win: focus};
        return {kind: 'probe', win: focus};
    }

    // a touch that may start a back: on an edge strip, with nothing else under way
    _mayStart(event) {
        if (this._g || this._postCommit)
            return false;
        // sideways the strips are sideways.js's
        if (sideways())
            return false;
        const nav = this._nav;
        if (nav.busy || nav.shadeOpen())
            return false;
        if (!nav.recentsShown && !nav.bouncerShown && Main.actionMode !== Shell.ActionMode.NORMAL)
            return false;
        const [x, y] = event.get_coords().map(v => v * this._scale);
        return y < SCREEN_H - NAV_GESTURE_H && (x < EDGE_W || x > SCREEN_W - EDGE_W);
    }

    _shouldHandle(event) {
        if (event.type() !== Clutter.EventType.TOUCH_BEGIN)
            return false;
        // any touch may change what back means in the app: ask again once it has
        this._prefetch();
        if (!this._mayStart(event)) {
            // The app's touch, and a tap on a row opens a page under it: what the app said before no longer holds. A
            // back right after waits for its answer rather than act on the old one, which went home from a page with
            // somewhere to go back to, or started the window toward home and snapped it back (the screen blinked)
            this._forget();
            return false;
        }
        const [x, y] = event.get_coords().map(v => v * this._scale);
        const target = this._target();
        const t = event.get_time();
        const g = this._g = {x0: x, y0: y, left: x < EDGE_W, target, mode: null, probe: null, started: false,
            trigger: false, initX: x, thresholdX: x, x, y, home: null, homeTimer: 0, homeHeld: false, released: false,
            downAt: GLib.get_monotonic_time() / 1000};
        if (target.kind === 'probe') {
            // what the app said last, at once; its answer now, when it comes, wins
            if (this._answer?.win === target.win)
                this._decide(g, this._answer.result);
            g.probe = this._ask(target.win).then(result => {
                if (!g.performed && !g.cancelled && (result.kind !== 'unknown' || !g.mode))
                    this._decide(g, result);
            });
        } else {
            g.mode = target.kind;
        }
        this._panel.isLeft = this._g.left;
        this._panel.onDown(x, y, t);
        // a finger resting on the edge is the app's
        this._longPressId = GLib.timeout_add(GLib.PRIORITY_DEFAULT, LONG_PRESS_MS, () => {
            this._longPressId = 0;
            if (this._g && !this._g.started)
                this._gesture.cancel();
            return GLib.SOURCE_REMOVE;
        });
        return true;
    }

    // more across than along
    _mayRecognize() {
        const [x, y] = this._point();
        const g = this._g;
        return !!g && Math.abs(x - g.x0) > Math.abs(y - g.y0);
    }

    _clearLongPress() {
        if (this._longPressId)
            GLib.source_remove(this._longPressId);
        this._longPressId = 0;
    }

    _begin() {
        const g = this._g;
        if (!g)
            return;
        this._clearLongPress();
        g.started = true;
        // the finger is back's now: the launcher under it lets go (Android pilfers it), so the drawer's list
        // neither scrolls nor opens the app it started on
        if (g.target.kind === 'drawer' || g.target.kind === 'home-screen' || g.target.launcher)
            this._nav.launcher('Cancel', null);
        this._move();
    }

    // the app answered (or answered before: then this may change the gesture's mind)
    _decide(g, result) {
        g.result = result;
        const mode = result.kind === 'none' ? 'home' : result.kind === 'button' ? 'button' : 'key';
        if (mode !== 'home') {
            // not back to home after all: the window is whole again, the arrow alone goes on
            this._clearHomeTimer(g);
            g.home?.cancel();
            g.home = null;
        }
        g.mode = mode;
        if (mode === 'home' && g.started && !g.released)
            this._startHome(g);
        g.whenDecided?.();
    }

    _startHome(g) {
        if (g.home || !g.target.win || g.target.win.minimized)
            return;
        g.home = new BackToHome(this._nav, g.target.win, g.left);
        const wait = g.downAt + HOME_START_MS - GLib.get_monotonic_time() / 1000;
        if (wait <= 0) {
            // late (the app answered after the window was due to follow): from whole on the spring, as from the
            // timer below, rather than a jump to where the finger already is
            Object.assign(this._spring, {x: 0, v: 0, target: 0, done: true});
            this._progressTo(this._rawProgress());
            return;
        }
        // whole until then, and then from 0
        g.homeHeld = true;
        g.homeTimer = GLib.timeout_add(GLib.PRIORITY_DEFAULT, Math.ceil(wait), () => {
            g.homeTimer = 0;
            g.homeHeld = false;
            if (this._g === g && !g.released) {
                Object.assign(this._spring, {x: 0, v: 0, target: 0, done: true});
                this._progressTo(this._rawProgress());
            }
            return GLib.SOURCE_REMOVE;
        });
    }

    _clearHomeTimer(g) {
        if (g.homeTimer)
            GLib.source_remove(g.homeTimer);
        g.homeTimer = 0;
        g.homeHeld = false;
    }

    _move() {
        const g = this._g;
        if (!g)
            return;
        const [x, y, t] = this._point();
        // a back cancelled and swiped out again starts counting from where it turned
        if ((g.left && x < g.thresholdX) || (!g.left && x > g.thresholdX)) {
            g.thresholdX = x;
            if ((g.left && g.thresholdX < g.initX) || (!g.left && g.thresholdX > g.initX))
                g.initX = g.thresholdX;
        }
        g.x = x;
        g.y = y;
        this._panel.onMove(x, y, t);
        if (g.mode === 'home')
            this._startHome(g);
        this._progressTo(this._rawProgress());
    }

    _rawProgress() {
        const g = this._g;
        const start = g.trigger ? g.initX : g.thresholdX;
        const d = g.left ? g.x - start : start - g.x;
        return clamp(Math.max(0, d) / MAX_DISTANCE, 0, 1);
    }

    _setTriggerBack(trigger) {
        const g = this._g;
        if (!g)
            return;
        if (g.trigger !== trigger && !trigger)
            g.thresholdX = g.x;
        g.trigger = trigger;
    }

    // the progress ×100 on its spring, the window following every frame
    _progressTo(raw, done = null) {
        this._spring.animateTo(raw * 100);
        this._springDone = done;
        if (this._spring.done) {
            this._frame(0);
            done?.();
            return;
        }
        this._anim ??= new Animator(global.stage, dt => this._frame(dt), () => (this._anim = null));
    }

    _frame(dt) {
        this._spring.advance(dt);
        const g = this._g;
        if (g?.home && !g.homeHeld)
            g.home.progress(BACK_GESTURE(this._spring.x / 100), g.y - g.y0);
        if (!this._spring.done)
            return true;
        const done = this._springDone;
        this._springDone = null;
        done?.();
        return false;
    }

    _end(cancelled) {
        this._clearLongPress();
        const g = this._g;
        if (!g)
            return;
        if (!g.started) {
            // never a back: the touch is the app's, and what it said before no longer holds (as any touch of its)
            this._panel.onCancel();
            this._g = null;
            this._forget();
            return;
        }
        g.released = true;
        if (cancelled) {
            this._panel.onCancel();
            this._cancel();
            return;
        }
        const [x, y, t] = this._point();
        this._panel.onUp(x, y, t);
    }

    // the panel committed: go back
    _invoke() {
        const g = this._g;
        if (!g || g.invoked)
            return;
        g.invoked = true;
        this._clearHomeTimer(g);
        // the progress spring is reset: no more progress
        this._anim?.stop();
        this._anim = null;
        Object.assign(this._spring, {x: 0, v: 0, target: 0, done: true});
        if (!g.mode) {
            // the app has not answered yet: give it a moment
            const id = GLib.timeout_add(GLib.PRIORITY_DEFAULT, PROBE_WAIT_MS, () => {
                g.whenDecided = null;
                g.mode ??= 'key';
                this._perform(g);
                return GLib.SOURCE_REMOVE;
            });
            g.whenDecided = () => {
                GLib.source_remove(id);
                g.whenDecided = null;
                this._perform(g);
            };
            this._g = null;
            return;
        }
        this._g = null;
        this._perform(g);
    }

    _perform(g) {
        const nav = this._nav;
        g.performed = true;
        switch (g.mode) {
        case 'home':
            if (!g.home && g.target.win && !g.target.win.minimized)
                g.home = new BackToHome(nav, g.target.win, g.left);
            if (g.home) {
                this._postCommit = g.home;
                g.home.commit(() => (this._postCommit = null));
            }
            break;
        case 'button':
            this._forget();
            this._probe.activate(g.result).then(ok => {
                if (!ok)
                    nav.key(Clutter.KEY_Left, [Clutter.KEY_Alt_L]);
                this._prefetch();
            });
            break;
        case 'recents':
            nav.recentsBack();
            break;
        case 'keyguard':
            nav.keyguardBack();
            break;
        case 'drawer':
            nav.launcher('Back', null);
            break;
        case 'keyboard':
            // (the gesture had the touch: the key it may have begun on is let go, and the close is not held for a lift
            // the stage never sees)
            dismissKeyboard();
            break;
        case 'home-screen':
            break;
        default:
            this._forget();
            nav.key(Clutter.KEY_Escape);
            this._prefetch();
        }
    }

    // the panel cancelled: the progress springs back to 0, then the window is the app's again
    _cancel() {
        const g = this._g;
        if (!g)
            return;
        this._g = null;
        g.cancelled = true;
        // still held whole: nothing to spring back from
        if (g.homeHeld)
            Object.assign(this._spring, {x: 0, v: 0, target: 0, done: true});
        this._clearHomeTimer(g);
        // what is still running belongs to this gesture alone
        const home = g.home;
        if (!home) {
            this._anim?.stop();
            this._anim = null;
            Object.assign(this._spring, {x: 0, v: 0, target: 0, done: true});
            return;
        }
        this._g = {...g, released: true, cancelling: true};
        this._progressTo(0, () => {
            home.cancel();
            if (this._g?.cancelling)
                this._g = null;
        });
    }

    // a back is under way (or its window still settling)
    get busy() {
        return !!(this._g || this._postCommit);
    }

    destroy() {
        this._clearLongPress();
        if (this._prefetchId)
            GLib.source_remove(this._prefetchId);
        global.display.disconnectObject(this);
        this._anim?.stop();
        if (this._g)
            this._clearHomeTimer(this._g);
        this._g?.home?.destroy();
        this._postCommit?.destroy();
        this._g = null;
        this._gesture.get_actor()?.remove_action(this._gesture);
        this._gesture.disconnectObject(this);
        this._panel.destroy();
        this._probe.destroy();
    }
}
