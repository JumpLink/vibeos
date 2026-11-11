// What gesture navigation and recents share: an app drawn as its recents card (TaskView), the launcher's icon under
// a window flying home (FloatingIconView), DynamicAnimation's spring, Launcher3's curves, and the frame clock the
// animations run on.
//
// Every length here is in device pixels, the spec's own numbers from the S10e (1080 × 2280, 3 px/dp); the screen's
// size is the panel's (units.js). Actors are placed in stage units (÷ the monitor scale).
import Clutter from 'gi://Clutter';
import Cogl from 'gi://Cogl';
import GdkPixbuf from 'gi://GdkPixbuf';
import Gio from 'gi://Gio';
import St from 'gi://St';

import * as Main from 'resource:///org/gnome/shell/ui/main.js';

import {capture, capturing} from './capture.js';
import {CropEffect, PathInterpolator, cubic} from './launch.js';
import {Animator} from './motion.js';
import {overWindows} from './scanout.js';
import {SCREEN_H, SCREEN_W} from './units.js';

export {SCREEN_H, SCREEN_W};
export const NAV_GESTURE_H = 96;               // the bottom strip a swipe starts in, 32 dp
// the recents card: 684 × 1444 at [198, 296, 882, 1740] on the 1080 × 2280 reference; on another panel the panel's
// shape, as wide and as high up for its size
const TASK_REF_W = 684, TASK_REF_CY = 1018;
const taskW = TASK_REF_W * SCREEN_W / 1080, taskH = taskW * SCREEN_H / SCREEN_W, taskCY = TASK_REF_CY * SCREEN_H / 2280;
export const TASK = [(SCREEN_W - taskW) / 2, taskCY - taskH / 2, (SCREEN_W + taskW) / 2, taskCY + taskH / 2];
export const TASK_W = TASK[2] - TASK[0];
// where the screen scales about onto the card: [540, 807.27] on the reference
export const TASK_PIVOT = [TASK[0] / (1 - TASK_W / SCREEN_W), TASK[1] / (1 - TASK_W / SCREEN_W)];
export const TASK_FULL_SCALE = SCREEN_W / TASK_W;
export const TASK_CORNER_R = 84;               // 28 dp
export const FRAME_MS = 1000 / 60;

export const HOME_ICON_ALPHA_END = 0.9;        // floating icon alpha p/0.9
export const HOME_SHAPE_START = 0.9;           // the icon's background becomes its shape over p 0.9 … 1
export const HOME_ICON_PX = 168;               // the launcher's icon box, 56 dp
export const SPLASH_ICON_DP = 160;             // the launcher's icon files are drawn at this size

export const lerp = (a, b, t) => a + (b - a) * t;
export const clamp = (v, lo, hi) => Math.min(hi, Math.max(lo, v));
export const lerpRect = (a, b, t) => a.map((v, i) => lerp(v, b[i], t));
// a rect scaled by s about p
export const scaleRect = (r, s, [px, py]) => [px + (r[0] - px) * s, py + (r[1] - py) * s, px + (r[2] - px) * s,
    py + (r[3] - py) * s];

// Launcher3's interpolators (spec §0)
export const LINEAR = t => t;
export const DECELERATE = t => 1 - (1 - t) * (1 - t);
export const DECELERATE_3 = t => 1 - Math.pow(1 - t, 6);
export const ACCELERATE = t => t * t;
export const ACCELERATE_2 = t => t * t * t * t;
export const OVERSHOOT_1_2 = t => (t - 1) * (t - 1) * (2.2 * (t - 1) + 1.2) + 1;
export const SCROLL = t => 1 + Math.pow(t - 1, 5);
export const SCROLL_CUBIC = t => 1 + Math.pow(t - 1, 3);
export const ACCELERATE_DECELERATE = t => Math.cos((t + 1) * Math.PI) / 2 + 0.5;
const fn = p => t => p.get(t);
export const TOUCH_RESPONSE = fn(cubic(0.3, 0, 0.1, 1));
export const FAST_OUT_SLOW_IN = fn(cubic(0.4, 0, 0.2, 1));
export const SCALE_REVEAL = fn(new PathInterpolator(
    'M0,0 C0.045,0.0356 0.0975,0.2055 0.15,0.3952 C0.235,0.6855 0.235,1 1,1'));
export const clampToProgress = (f, a, b) => t => t < a ? 0 : t > b ? 1 : f((t - a) / (b - a));

let runs = 0;

// An Animator on the shell's slow-down factor (like the shell's own animations), saving its frames when capturing
export function animate(actor, name, onFrame, onDone) {
    const slow = St.Settings.get().slow_down_factor;
    const label = capturing() ? `${name}${++runs}` : null;
    return new Animator(actor, (dt, elapsed) => {
        const more = onFrame(dt / slow, elapsed / slow);
        if (label)
            capture(label, elapsed * 1000 / slow);
        return more;
    }, onDone);
}

// ---- DynamicAnimation's spring: exact closed-form steps, and its own end test --------------------------------------

export class Spring {
    constructor([k, zeta], x, v, target, minVisible = 1, max = Infinity) {
        Object.assign(this, {k, zeta, x, v, target, minVisible, max, done: false});
    }

    animateTo(target) {
        if (target !== this.target)
            this.done = false;
        this.target = target;
    }

    // dt in s, v in units/s
    step(dt) {
        if (this.done)
            return this.x;
        const w = Math.sqrt(this.k), z = this.zeta, x0 = this.x - this.target, v0 = this.v;
        let x, v;
        if (z > 1) {
            const gp = -z * w + w * Math.sqrt(z * z - 1), gm = -z * w - w * Math.sqrt(z * z - 1);
            const B = (gm * x0 - v0) / (gm - gp), A = x0 - B;
            x = A * Math.exp(gm * dt) + B * Math.exp(gp * dt);
            v = A * gm * Math.exp(gm * dt) + B * gp * Math.exp(gp * dt);
        } else if (z === 1) {
            const A = x0, B = v0 + w * x0, e = Math.exp(-w * dt);
            x = (A + B * dt) * e;
            v = (A + B * dt) * e * -w + B * e;
        } else {
            const wd = w * Math.sqrt(1 - z * z), d = (z * w * x0 + v0) / wd, e = Math.exp(-z * w * dt);
            x = e * (x0 * Math.cos(wd * dt) + d * Math.sin(wd * dt));
            v = -w * z * x + e * (-wd * x0 * Math.sin(wd * dt) + wd * d * Math.cos(wd * dt));
        }
        this.x = Math.min(x + this.target, this.max);
        this.v = v;
        const vt = 0.75 * this.minVisible;
        if (Math.abs(this.v) < vt * 62.5 && Math.abs(this.x - this.target) < vt) {
            this.x = this.target;
            this.v = 0;
            this.done = true;
        }
        return this.x;
    }

    // dt in s, in sub-steps of at most 4 ms for the stiff springs over a long frame
    advance(dt) {
        for (let left = Math.min(dt, 0.1); left > 0 && !this.done; left -= 0.004)
            this.step(Math.min(left, 0.004));
        return this.x;
    }
}

// SpringAnimationBuilder's curve: the spring from 1 to 0 at rest, sampled over its own settle time (threshold
// multiplier 0.65), so it can be stretched over any duration. Returns f(t) for t in [0, 1], 1 → 0.
export function springCurve([k, zeta], minVisible) {
    const w = Math.sqrt(k), wd = w * Math.sqrt(1 - zeta * zeta), a = zeta * w;
    const x = tau => Math.exp(-a * tau) * (Math.cos(wd * tau) + a / wd * Math.sin(wd * tau));
    const v = tau => -Math.exp(-a * tau) * (w * w / wd) * Math.sin(wd * tau);
    const vt = 0.65 * minVisible;
    let end = 0;
    for (let tau = 0; tau < 10; tau += 0.001) {
        if (Math.abs(x(tau)) < vt && Math.abs(v(tau)) < vt * 62.5) {
            end = tau;
            break;
        }
    }
    return t => t >= 1 ? 0 : x(t * end);
}

// ---- what moves ---------------------------------------------------------------------------------------------------

const black = () => new Cogl.Color({red: 0, green: 0, blue: 0, alpha: 255});

// A view's offscreen buffer is what it costs: a new crop effect allocates a screen-sized one on its first paint,
// 15-50 ms of the shell's frame on the S10e, at the start of every back gesture and for every recents card. So
// the roots, each with its effect and buffer, are kept for the next views.
//
// The effect sits on `inner`, which never moves; the view moves and scales `root`, its parent. Clutter redraws an
// effect's buffer when the actor it is on is redrawn, and moving an actor redraws it, but moving its parent does
// not: so a card that only slides (a recents scroll, a fling) is drawn from the buffer it already has, and the
// window is drawn into it again only when the window itself changes. With the effect on the moving actor, every
// frame drew each card's window into a screen-sized buffer first: three of those a frame took the POCO's 120 Hz
// recents scroll to 13 % missed frames, against 1 % now (2026-11-10, the same scripted swipes).
const VIEWS_KEPT = 3;
const keptViews = [];

function takeView() {
    const m = Main.layoutManager.primaryMonitor;
    const kept = keptViews.pop();
    if (kept) {
        kept.root.set({x: m.x, y: m.y, width: m.width, height: m.height, visible: true, scale_x: 1, scale_y: 1,
            translation_x: 0, translation_y: 0});
        kept.inner.background_color = black();
        return kept;
    }
    const view = {
        root: new Clutter.Actor({x: m.x, y: m.y, width: m.width, height: m.height}),
        inner: new Clutter.Actor({width: m.width, height: m.height, background_color: black()}),
        effect: new CropEffect(),
        gone: false,
    };
    view.root.add_child(view.inner);
    view.inner.add_effect(view.effect);
    view.root.connect('destroy', () => (view.gone = true));
    overWindows(view.root);
    return view;
}

function keepView(view) {
    if (view.gone)
        return;
    view.root.get_parent()?.remove_child(view.root);
    if (keptViews.length < VIEWS_KEPT)
        keptViews.push(view);
    else
        view.root.destroy();
}

let warming = null;

// Two views (a flight home takes the window's and its icon's) painted once ahead of time, under everything (the
// launcher covers them), so that the first gesture of a session does not pay for their buffers either: 76 ms of
// the frame the first time
export function warmViews() {
    if (keptViews.length || warming)
        return;
    const views = [takeView(), takeView()];
    for (const view of views)
        Main.layoutManager.uiGroup.insert_child_below(view.root, null);
    const id = global.stage.connect('after-paint', () => {
        global.stage.disconnect(id);
        warming = null;
        views.forEach(keepView);
    });
    warming = {views, id};
}

// the kept views' buffers go with the extension
export function dropKeptViews() {
    if (warming) {
        global.stage.disconnect(warming.id);
        for (const view of warming.views)
            view.root.destroy();
        warming = null;
    }
    for (const view of keptViews.splice(0))
        view.root.destroy();
}

// The app as its recents card shows it: the whole screen (the status bar's band black, as the bar is over an app)
// with a clone of the window in it, through the crop shader for the corners. The real window is hidden while the
// clone stands in. parent: the actor the card is drawn in (the recents layer).
export class TaskView {
    constructor(win, scale, parent) {
        this.win = win;
        this.actor = win.get_compositor_private();
        this._scale = scale;
        const m = Main.layoutManager.primaryMonitor;
        this._view = takeView();
        this.root = this._view.root;
        this._clone = new Clutter.Clone({source: this.actor, x: this.actor.x - m.x, y: this.actor.y - m.y});
        this._view.inner.add_child(this._clone);
        this._effect = this._view.effect;
        this._effect.setAlpha(1);
        this._effect.setHole(0, 0, 0, 0, 0);
        parent.add_child(this.root);
        this.actor.opacity = 0;
        // the window closed under the card: nothing left to show or restore
        this.actor.connectObject('destroy', () => {
            this.actor = null;
            this._clone.source = null;
        }, this);
    }

    // The window's content scaled uniformly to the rect's width (device px), cropped about its middle to the
    // rect's height (or from its top); r the corners on screen (px). Off screen, the card is not drawn at all.
    place(rect, r, alpha = 1, cropTop = false) {
        const sc = this._scale;
        this.rect = rect;
        const visible = alpha > 0 && rect[2] > 0 && rect[0] < SCREEN_W && rect[3] > 0 && rect[1] < SCREEN_H;
        this.root.visible = visible;
        if (!visible)
            return;
        const s = (rect[2] - rect[0]) / SCREEN_W;
        const cropH = (rect[3] - rect[1]) / s;
        const top = cropTop ? 0 : (SCREEN_H - cropH) / 2;
        this.root.set_pivot_point(0, 0);
        this.root.scale_x = this.root.scale_y = s;
        this.root.translation_x = rect[0] / sc - this.root.x;
        this.root.translation_y = (rect[1] - top * s) / sc - this.root.y;
        this._effect.setGeometry(this._view.inner, [0, top / sc, SCREEN_W / sc, (top + cropH) / sc],
            r / (s * sc), 1 / (s * sc));
        this._effect.setAlpha(alpha);
    }

    destroy() {
        this._clone.destroy();
        keepView(this._view);
        if (this.actor) {
            this.actor.disconnectObject(this);
            this.actor.opacity = 255;
        }
    }
}

// The plate an icon file sits on: its edge colour, as Launcher3 fills a window's footprint with the adaptive
// icon's background
const plates = new Map();

function plateColour(path) {
    if (plates.has(path))
        return plates.get(path);
    let colour = null;
    try {
        const pb = GdkPixbuf.Pixbuf.new_from_file(path);
        const n = pb.get_n_channels(), stride = pb.get_rowstride(), px = pb.get_pixels();
        const w = pb.get_width(), h = pb.get_height();
        const sum = [0, 0, 0];
        let count = 0;
        for (const [fx, fy] of [[0.5, 0.1], [0.5, 0.9], [0.1, 0.5], [0.9, 0.5]]) {
            const i = Math.round(fy * (h - 1)) * stride + Math.round(fx * (w - 1)) * n;
            if (n === 4 && px[i + 3] < 200)
                continue;
            for (let c = 0; c < 3; c++)
                sum[c] += px[i + c];
            count++;
        }
        if (count)
            colour = sum.map(v => Math.round(v / count));
    } catch (e) {
        logError(e, `gnome-phone: no plate colour for ${path}`);
    }
    plates.set(path, colour);
    return colour;
}

// An app's icon: the launcher's own picture of it (its icon theme, at 160 dp) when it sent one, else the app's
export function appGicon(app, path) {
    return path ? new Gio.FileIcon({file: Gio.File.new_for_path(path)}) : app?.get_icon() ?? null;
}

// FloatingIconView: the launcher's icon under the flying window, filling its footprint, then becoming the icon
export class FloatingIcon {
    constructor(app, path, scale, below) {
        this._scale = scale;
        this._colour = path ? plateColour(path) : null;
        // a kept screen-sized view, the footprint drawn through its crop: an actor resized every frame would have
        // its effect allocate a new buffer every frame (a 53 ms paint in the flight home)
        this._view = takeView();
        this.root = this._view.root;
        this._size = Math.round(SPLASH_ICON_DP * 3 / scale);
        this._icon = new St.Icon({gicon: appGicon(app, path), icon_size: this._size, style: 'padding: 0;'});
        this._view.inner.add_child(this._icon);
        this._effect = this._view.effect;
        this._effect.setHole(0, 0, 0, 0, 0);
        below.get_parent().insert_child_below(this.root, below);
    }

    place(rect, r, p) {
        const sc = this._scale;
        // the footprint in the root's own coordinates (the root covers the monitor)
        const [x0, y0, x1, y1] = rect.map((v, i) => v / sc - (i % 2 ? this.root.y : this.root.x));
        const w = x1 - x0, h = y1 - y0;
        const plate = 1 - clamp((p - HOME_SHAPE_START) / (1 - HOME_SHAPE_START), 0, 1);
        const [red, green, blue] = this._colour ?? [0, 0, 0];
        this._view.inner.background_color = new Cogl.Color({red, green, blue,
            alpha: this._colour ? Math.round(255 * plate) : 0});
        // the art grows with the footprint's short side, never below the icon's own size
        const size = Math.max(HOME_ICON_PX / sc, Math.min(w, h));
        this._icon.set_pivot_point(0, 0);
        this._icon.scale_x = this._icon.scale_y = size / this._size;
        this._icon.set_position(x0 + (w - size) / 2, y0 + (h - size) / 2);
        this._effect.setGeometry(this._view.inner, [x0, y0, x1, y1], r / sc, 1 / sc);
        this._effect.setAlpha(clamp(p / HOME_ICON_ALPHA_END, 0, 1));
    }

    // out of the layer it flew in, which goes when the flight ends, into the shell's UI just above the windows,
    // where it stays put
    lift() {
        const [x, y] = this.root.get_transformed_position();
        this.root.get_parent()?.remove_child(this.root);
        Main.layoutManager.uiGroup.insert_child_above(this.root, global.window_group);
        this.root.set_position(x, y);
    }

    destroy() {
        this._icon.destroy();
        keepView(this._view);
    }
}
