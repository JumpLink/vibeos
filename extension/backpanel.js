// The back arrow: SystemUI's BackPanel and BackPanelController (Android 16, navigationbar/gestural), ported line for
// line. A pill on the edge the swipe came from, its every dimension on a spring of its own; the state machine moves
// the springs' resting positions (ENTRY → ACTIVE → COMMITTED, or back to INACTIVE, FLUNG, CANCELLED) and the finger
// stretches them between those and the fully stretched shape. Lengths are the S10e's device pixels (3 px per dp).
//
// One quirk of the source is kept as it is: deactivationTriggerThreshold's getter negates its field, so the arrow
// lets go of ACTIVE only after the finger has come back 32 dp toward the edge (which is what the recordings show),
// and the range "reactivation..deactivation" is empty.
import Clutter from 'gi://Clutter';
import Cogl from 'gi://Cogl';
import GLib from 'gi://GLib';
import GObject from 'gi://GObject';

import * as Main from 'resource:///org/gnome/shell/ui/main.js';

import {cubic} from './launch.js';
import {Animator, VelocityTracker} from './motion.js';
import {SCREEN_H, SCREEN_W, Spring, clamp, lerp} from './task.js';
import {SCALE} from './units.js';

const DP = 3;
const curve = (...p) => {
    const c = cubic(...p);
    return t => c.get(t);
};

// the panel's window, 105 × 268 dp, centred 64 dp above the finger and no higher than 64 dp
export const PANEL_W = 105 * DP, PANEL_H = 268 * DP;
const FINGER_OFFSET = 64 * DP, MIN_ARROW_Y = 64 * DP;
const ARROW_THICKNESS = 4 * DP;
// measured on the S10e (dark theme): materialColorSecondaryContainer and OnSecondaryContainer
const BACKGROUND = [38, 62, 66], ARROW = [168, 195, 198];

// ViewConfiguration: edge slop 12 dp, touch slop 8 dp, minimum fling 50 dp/s
const EDGE_SLOP = 12 * DP, TOUCH_SLOP = 8 * DP, MIN_FLING_DISTANCE = TOUCH_SLOP * 3, MIN_FLING_V = 50 * DP;
const STATIC_THRESHOLD = 16 * DP, REACTIVATION = 32 * DP, DEACTIVATION = -32 * DP;
const FULLY_STRETCHED = Math.min(SCREEN_W, 412 * DP);

const MIN_DURATION_ACTIVE_BEFORE_INACTIVE = 300;
const MIN_DURATION_ACTIVE_AFTER_INACTIVE = 130;
const MIN_DURATION_CANCELLED = 200;
const MIN_DURATION_COMMITTED = 80;
const MIN_DURATION_COMMITTED_AFTER_FLING = 120;
const MIN_DURATION_INACTIVE_BEFORE_FLUNG = 50;
const MIN_DURATION_INACTIVE_BEFORE_ACTIVE = 160;
const MIN_DURATION_ENTRY_BEFORE_ACTIVE = 10;
const MAX_DURATION_ENTRY_BEFORE_ACTIVE = 100;
const MIN_DURATION_FLING = 160;
const MIN_DURATION_ENTRY_TO_ACTIVE_AS_FLING = 100;
const MIN_DURATION_INACTIVE_TO_ACTIVE_AS_FLING = 400;
const POP_ON_FLING_DELAY = 60;
const POP_ON_FLING_V = 2, POP_ON_COMMITTED_V = 3;
const POP_ON_ENTRY_TO_ACTIVE_V = 4.5, POP_ON_INACTIVE_TO_ACTIVE_V = 4.7, POP_ON_INACTIVE_V = -1.5;
const FAILSAFE_MS = 350;

// DynamicAnimation's minimum visible changes
const PIXELS = 1, ROTATION = 0.1, ALPHA = 1 / 256, SCALE_CHANGE = 1 / 500;

export const State = {
    GONE: 'gone', ENTRY: 'entry', ACTIVE: 'active', INACTIVE: 'inactive', FLUNG: 'flung', COMMITTED: 'committed',
    CANCELLED: 'cancelled',
};

// ---- EdgePanelParams ------------------------------------------------------------------------------------------------

const entryWidth = curve(0.19, 1.27, 0.71, 0.86);
const entryWidthTowardsEdge = curve(1, -3, 1, 1.2);
const activeWidth = curve(0.7, -0.24, 0.48, 1.21);
const arrowAngle = entryWidth;
const horizontalTranslation = curve(0.2, 1, 1, 1);
const verticalTranslation = curve(0.5, 1.15, 0.41, 0.94);
const farCorner = curve(0.03, 0.19, 0.14, 1.09);
const edgeCorner = curve(0, 1.11, 0.85, 0.84);
const heightCurve = curve(1, 0.05, 0.9, -0.29);

// Two values and a threshold with hysteresis around it (the arrow's alpha springs and its alpha in and out); the
// source shares one instance between the states and never resets it, so neither does this
class Step {
    constructor(threshold, factor, post, pre) {
        Object.assign(this, {threshold, factor, post, pre});
        this.lowerFactor = 2 - factor;
        this.crossedOnce = false;
        this.start = {value: pre, isNew: false};
        this.previous = this.start;
    }

    get(progress) {
        const upper = progress > this.threshold * this.factor;
        const lower = progress > this.threshold * this.lowerFactor;
        let v;
        if (upper && !this.crossedOnce) {
            this.crossedOnce = true;
            v = {value: this.post, isNew: true};
        } else if (lower) {
            v = {value: this.previous.value, isNew: false};
        } else if (this.crossedOnce) {
            this.crossedOnce = false;
            v = {value: this.pre, isNew: true};
        } else {
            v = this.start;
        }
        this.previous = v;
        return v;
    }
}

const alphaSpring = new Step(0.165, 1.05, [180, 0.9], [2000, 0.6]);
const alphaInterpolator = new Step(0.165, 1.05, 1, 0);

const activeCommittedArrowSpring = [1500, 0.29];
const flungCommitted = [10000, 1];

const P = {};
P.entry = {
    hTrans: 4 * DP, scale: 0.98, pivotX: 51 * DP,
    hTransSpring: [800, 0.76], vTransSpring: [30000, 1], scaleSpring: [120, 0.8],
    arrow: {length: 8.6 * DP, height: 5 * DP, alpha: 0, lengthSpring: [600, 0.4], heightSpring: [600, 0.4],
        alphaSpring, alphaInterpolator},
    bg: {alpha: 1, width: 0, height: 48 * DP, edge: 6 * DP, far: 6 * DP,
        widthSpring: [450, 0.65], heightSpring: [1500, 0.45], farSpring: [300, 0.5], edgeSpring: [150, 0.5]},
};
P.active = {
    hTrans: 14 * DP, scale: 1, pivotX: 48 * DP,
    hTransSpring: [1000, 0.8], scaleSpring: [325, 0.55],
    arrow: {length: 6.4 * DP, height: 7.2 * DP, alpha: 1, lengthSpring: activeCommittedArrowSpring,
        heightSpring: activeCommittedArrowSpring, alphaSpring, alphaInterpolator},
    bg: {alpha: 1, width: 48 * DP, height: 48 * DP, edge: 24 * DP, far: 24 * DP,
        widthSpring: [850, 0.75], heightSpring: [10000, 1], edgeSpring: [2600, 0.855], farSpring: [1200, 0.3]},
};
P.preThreshold = {
    hTrans: 4 * DP, scale: 0.98, pivotX: 51 * DP,
    scaleSpring: [120, 0.8], hTransSpring: [6000, 1],
    arrow: {length: 8 * DP, height: 5.6 * DP, alpha: 1, lengthSpring: [100, 0.6], heightSpring: [100, 0.6],
        alphaSpring, alphaInterpolator},
    bg: {alpha: 1, width: 51 * DP, height: 46 * DP, edge: 16 * DP, far: 20 * DP,
        widthSpring: [650, 1], heightSpring: [1500, 0.45], farSpring: [300, 1], edgeSpring: [250, 0.5]},
};
P.committed = {
    ...P.active, hTrans: null, pivotX: null, scale: 0.86, scaleSpring: [5700, 1],
    arrow: {...P.active.arrow, length: null, height: null},
    bg: {...P.active.bg, alpha: 0, width: null, widthSpring: flungCommitted, heightSpring: flungCommitted,
        edgeSpring: flungCommitted, farSpring: flungCommitted, alphaSpring: [1400, 1]},
};
P.flung = {
    ...P.committed,
    arrow: {...P.committed.arrow, lengthSpring: [850, 0.46], heightSpring: [850, 0.46], length: P.active.arrow.length,
        height: P.active.arrow.height},
};
P.cancelled = {...P.entry, bg: {...P.entry.bg, width: 0, alpha: 0, alphaSpring: [450, 1]}};
P.stretched = {
    hTrans: 18 * DP, scale: 1,
    arrow: {length: 5.6 * DP, height: 8 * DP, alpha: 1},
    bg: {alpha: 1, width: 60 * DP, height: 48 * DP, edge: 24 * DP, far: 24 * DP},
};

// ---- BackPanel.AnimatedFloat: a value at rest somewhere, on a SpringAnimation ------------------------------------------

class AnimatedFloat {
    constructor(panel, {minVisible = PIXELS, min = -Infinity, max = Infinity} = {}) {
        this._panel = panel;
        this._min = min;
        this._max = max;
        this.resting = 0;
        // SpringForce's defaults until a state sets its own: STIFFNESS_MEDIUM, DAMPING_RATIO_MEDIUM_BOUNCY
        this._spring = new Spring([1500, 0.5], 0, 0, 0, minVisible);
        this._spring.done = true;
        this._ends = [];
    }

    get pos() {
        return this._spring.x;
    }

    get running() {
        return !this._spring.done;
    }

    // the setter cancels what is running (the velocity stays, as DynamicAnimation's does)
    set spring([k, zeta]) {
        this.cancel();
        this._spring.k = k;
        this._spring.zeta = zeta;
    }

    snapTo(x) {
        this.cancel();
        this.resting = x;
        this._spring.target = x;
        this._spring.x = x;
        this._panel.queueRepaint();
    }

    snapToResting() {
        this.snapTo(this.resting);
    }

    stretchTo(amount, v0 = null, spring = null) {
        if (v0 !== null) {
            this.cancel();
            this._spring.v = v0;
        }
        if (spring)
            this.spring = spring;
        this._animateTo(this.resting + amount);
    }

    stretchBy(final, amount) {
        this._animateTo(this.resting + amount * ((final ?? 0) - this.resting));
    }

    updateResting(pos, animated = true) {
        if (pos === null || pos === undefined)
            return;
        this.resting = pos;
        if (animated)
            this._animateTo(pos);
        else
            this.snapTo(pos);
    }

    _animateTo(x) {
        this._spring.target = x;
        if (this._spring.done) {
            this._spring.done = false;
            this._panel.run();
        }
    }

    // an end listener, told once with whether the animation was cancelled
    onEnd(fn) {
        this._ends.push(fn);
    }

    _end(cancelled) {
        const ends = this._ends;
        this._ends = [];
        for (const fn of ends)
            fn(cancelled);
    }

    cancel() {
        if (this._spring.done)
            return;
        this._spring.done = true;
        this._end(true);
    }

    step(dt) {
        if (this._spring.done)
            return;
        this._spring.advance(dt);
        this._spring.x = clamp(this._spring.x, this._min, this._max);
        if (this._spring.done)
            this._end(false);
    }
}

// ---- BackPanel: the drawing --------------------------------------------------------------------------------------

// BackPanel.onDraw on the GPU: drawn with Cairo, the panel's texture went up again every frame and the stall halved
// the frame rate. The pill and the chevron are distances in the panel's own pixels, under its translation and its
// scale about the pivot; the right edge's panel is the left's mirrored.
const rgb = c => c.map(v => (v / 255).toFixed(4)).join(', ');

const PANEL_DECLARATIONS = `
uniform vec4 bounds;   // the effect's texture in actor coordinates: x, y, width, height
uniform float mirror;  // 1: the right edge's panel
uniform vec4 xf;       // horizontal translation, vertical translation, scale, the scale's pivot x
uniform vec4 pill;     // width, half height, edge-side corner radius, far-side corner radius (fitted)
uniform vec4 arrow;    // length, height, background alpha, arrow alpha
`;

const PANEL_CODE = `
vec2 p = (bounds.xy + cogl_tex_coord_in[0].xy * bounds.zw) * ${SCALE.toFixed(4)};
if (mirror > 0.5)
    p.x = ${PANEL_W.toFixed(1)} - p.x;
vec2 q = p - vec2(xf.x, ${(PANEL_H / 2).toFixed(1)} + xf.y);
q = vec2((q.x - xf.w) / xf.z + xf.w, q.y / xf.z);
float aa = 1.0 / xf.z;

// the pill: edge-side corners on the screen's side, far-side corners away from it
vec2 c = vec2(pill.x * 0.5, 0.0);
float r = q.x < c.x ? pill.z : pill.w;
vec2 e = abs(q - c) - vec2(pill.x * 0.5, pill.y) + vec2(r);
float d = length(max(e, 0.0)) + min(max(e.x, e.y), 0.0) - r;
float bg = pill.x > 0.0 && pill.y > 0.0 ? clamp(0.5 - d / aa, 0.0, 1.0) * arrow.z : 0.0;

// the chevron (0, -dy) -> (dx, 0) -> (0, dy), centred in the pill and pointing away from the edge: one arm, folded
// about the axis; square caps, a mitred tip (bevelled past Skia's miter limit of 4)
float dx = arrow.x, dy = arrow.y, len = length(vec2(dx, dy)), hw = ${(ARROW_THICKNESS / 2).toFixed(1)};
float fg = 0.0;
if (len > 0.0) {
    vec2 v = vec2(q.x - (pill.x - dx) * 0.5, abs(q.y)) - vec2(dx, 0.0);
    vec2 u = vec2(-dx, dy) / len;
    float sd = max(abs(v.x * u.y - v.y * u.x) - hw, dot(v, u) - len - hw);
    if (len > 4.0 * dy)
        sd = max(sd, v.x - hw * dy / len);
    fg = clamp(0.5 - sd / aa, 0.0, 1.0) * min(arrow.w, arrow.z);
}
cogl_color_out = vec4(vec3(${rgb(ARROW)}) * fg + vec3(${rgb(BACKGROUND)}) * bg * (1.0 - fg), fg + bg * (1.0 - fg));
`;

const PanelEffect = GObject.registerClass(
class PanelEffect extends Clutter.ShaderEffect {
    vfunc_get_static_snippet() {
        return Cogl.Snippet.new(Cogl.SnippetHook.FRAGMENT, PANEL_DECLARATIONS, PANEL_CODE);
    }

    // where the offscreen texture lies in actor coordinates: the paint box on the stage (the actor at x, y), enlarged
    // the way _clutter_actor_box_enlarge_for_effects does, in whole stage units
    setBounds(x, y, w, h) {
        const x2 = Math.ceil(x + w + 0.75), y2 = Math.ceil(y + h + 0.75);
        const rw = Math.round(w), rh = Math.round(h);
        this.set_uniform_float('bounds', 4, [x2 - rw - 3 - x, y2 - rh - 3 - y, rw + 3, rh + 3]);
    }
});

class BackPanel {
    constructor() {
        // the actor paints a plain rectangle into the effect's texture; the effect draws the panel over it
        this.actor = new Clutter.Actor({name: 'gnomePhoneBackPanel', width: PANEL_W / SCALE, height: PANEL_H / SCALE,
            visible: false, reactive: false, background_color: new Cogl.Color({red: 0, green: 0, blue: 0, alpha: 255})});
        this._effect = new PanelEffect();
        this.actor.add_effect(this._effect);
        Main.layoutManager.uiGroup.add_child(this.actor);
        this.isLeft = true;
        this._anim = null;

        const f = opts => new AnimatedFloat(this, opts);
        this.arrowLength = f();
        this.arrowHeight = f({minVisible: ROTATION});
        this.backgroundWidth = f({min: 0});
        this.backgroundHeight = f({min: 0});
        this.backgroundEdgeCornerRadius = f();
        this.backgroundFarCornerRadius = f();
        this.scale = f({minVisible: SCALE_CHANGE, min: 0});
        this.scalePivotX = f({min: 0});
        this.horizontalTranslation = f();
        this.arrowAlpha = f({minVisible: ALPHA, min: 0, max: 1});
        this.backgroundAlpha = f({minVisible: ALPHA, min: 0, max: 1});
        this.verticalTranslation = f();
        this._all = [this.arrowLength, this.arrowHeight, this.backgroundWidth, this.backgroundEdgeCornerRadius,
            this.backgroundFarCornerRadius, this.scalePivotX, this.scale, this.horizontalTranslation, this.arrowAlpha,
            this.backgroundAlpha];
    }

    get visible() {
        return this.actor.visible;
    }

    set visible(v) {
        this.actor.visible = v;
    }

    // the panel's window: on the left or right edge, its top at y (device px)
    place(y) {
        const m = Main.layoutManager.primaryMonitor;
        const ax = m.x + (this.isLeft ? 0 : SCREEN_W - PANEL_W) / SCALE, ay = m.y + y / SCALE;
        this.actor.set_position(ax, ay);
        this._effect.setBounds(ax, ay, PANEL_W / SCALE, PANEL_H / SCALE);
        Main.layoutManager.uiGroup.set_child_above_sibling(this.actor, null);
    }

    queueRepaint() {
        this._draw();
    }

    // the springs step on the stage's frame clock while any of them runs (the stage: the panel may be hidden)
    run() {
        if (this._anim?.running)
            return;
        const springs = [...this._all, this.verticalTranslation];
        this._anim = new Animator(global.stage, dt => {
            for (const a of springs)
                a.step(dt);
            this.queueRepaint();
            return springs.some(a => a.running);
        });
    }

    cancelAnimations() {
        for (const a of this._all)
            a.cancel();
    }

    setStretch(hTrans, arrow, arrowAlpha, bgAlpha, bgWidth, bgHeight, edge, far, dims) {
        this.horizontalTranslation.stretchBy(dims.hTrans, hTrans);
        this.arrowLength.stretchBy(dims.arrow.length, arrow);
        this.arrowHeight.stretchBy(dims.arrow.height, arrow);
        this.arrowAlpha.stretchBy(dims.arrow.alpha, arrowAlpha);
        this.backgroundAlpha.stretchBy(dims.bg.alpha, bgAlpha);
        this.backgroundWidth.stretchBy(dims.bg.width, bgWidth);
        this.backgroundHeight.stretchBy(dims.bg.height, bgHeight);
        this.backgroundEdgeCornerRadius.stretchBy(dims.bg.edge, edge);
        this.backgroundFarCornerRadius.stretchBy(dims.bg.far, far);
    }

    popOffEdge(v) {
        this.scale.stretchTo(0, v * -0.8);
        this.horizontalTranslation.stretchTo(0, v * 200);
    }

    popScale(v) {
        this.scalePivotX.snapTo(this.backgroundWidth.pos / 2);
        this.scale.stretchTo(0, v);
    }

    popArrowAlpha(v, spring = null) {
        this.arrowAlpha.stretchTo(0, v, spring);
    }

    resetStretch() {
        this.backgroundAlpha.snapTo(1);
        this.verticalTranslation.snapTo(0);
        this.scale.snapTo(1);
        for (const a of [this.horizontalTranslation, this.arrowLength, this.arrowHeight, this.arrowAlpha,
            this.backgroundWidth, this.backgroundHeight, this.backgroundEdgeCornerRadius,
            this.backgroundFarCornerRadius])
            a.snapToResting();
    }

    setRestingDimens(d, animate = true) {
        this.horizontalTranslation.updateResting(d.hTrans);
        this.scale.updateResting(d.scale);
        this.backgroundAlpha.updateResting(d.bg.alpha);
        this.arrowAlpha.updateResting(d.arrow.alpha, animate);
        this.arrowLength.updateResting(d.arrow.length, animate);
        this.arrowHeight.updateResting(d.arrow.height, animate);
        this.scalePivotX.updateResting(d.pivotX, animate);
        this.backgroundWidth.updateResting(d.bg.width, animate);
        this.backgroundHeight.updateResting(d.bg.height, animate);
        this.backgroundEdgeCornerRadius.updateResting(d.bg.edge, animate);
        this.backgroundFarCornerRadius.updateResting(d.bg.far, animate);
    }

    setSprings(s) {
        const set = (a, spring) => {
            if (spring)
                a.spring = spring;
        };
        set(this.arrowLength, s.arrowLength);
        set(this.arrowHeight, s.arrowHeight);
        set(this.arrowAlpha, s.arrowAlpha);
        set(this.backgroundAlpha, s.backgroundAlpha);
        set(this.backgroundFarCornerRadius, s.far);
        set(this.backgroundEdgeCornerRadius, s.edge);
        set(this.scale, s.scale);
        set(this.backgroundWidth, s.backgroundWidth);
        set(this.backgroundHeight, s.backgroundHeight);
        set(this.horizontalTranslation, s.horizontalTranslation);
        set(this.verticalTranslation, s.verticalTranslation);
    }

    // BackPanel.onDraw: the springs' values to the shader
    _draw() {
        const edge = this.backgroundEdgeCornerRadius.pos, far = this.backgroundFarCornerRadius.pos;
        const half = this.backgroundHeight.pos / 2, w = this.backgroundWidth.pos;
        // the radii fitted as Skia's RRect does when they overlap
        const k = w > 0 && half > 0 ? Math.min(1, w / (edge + far), 2 * half / (2 * edge), 2 * half / (2 * far)) : 0;
        const fx = this._effect;
        fx.set_uniform_float('mirror', 1, [this.isLeft ? 0 : 1]);
        fx.set_uniform_float('xf', 4, [this.horizontalTranslation.pos, this.verticalTranslation.pos,
            Math.max(this.scale.pos, 1e-3), this.scalePivotX.pos]);
        fx.set_uniform_float('pill', 4, [Math.max(0, w), Math.max(0, half), Math.max(0, edge * k), Math.max(0, far * k)]);
        fx.set_uniform_float('arrow', 4, [this.arrowLength.pos, this.arrowHeight.pos,
            clamp(this.backgroundAlpha.pos, 0, 1), clamp(this.arrowAlpha.pos, 0, 1)]);
        fx.queue_repaint();
    }

    destroy() {
        this._anim?.stop();
        this.actor.destroy();
    }
}

// ---- BackPanelController: the state machine --------------------------------------------------------------------

const now = () => GLib.get_monotonic_time() / 1000;
const smoothStep = (a, b, x) => {
    const t = clamp((x - a) / (b - a), 0, 1);
    return t * t * (3 - 2 * t);
};

// Fed the touch from the edge (device px, ms) by the back gesture; tells it back through
// {setTriggerBack(b), triggerBack(), cancelBack(), haptic()}
export class BackPanelController {
    constructor(callbacks) {
        this._cb = callbacks;
        this._view = new BackPanel();
        this.state = State.GONE;
        this._previous = State.GONE;
        // none of these is reset between gestures, as in the source
        this._xOnActive = 0;
        this._previousX = 0;
        this._totalActive = 0;
        this._totalInactive = 0;
        this._deltaStartX = 0;
        this._pastThresholdTime = 0;
        this._entryToActiveDelay = 0;
        this._previousPreThresholdWidth = entryWidth;
        this._velocity = new VelocityTracker();
        this._lastT = 0;
        this._startX = this._startY = 0;
        this._entryTime = this._inactiveTime = 0;
        this._passedDragSlop = false;
        this._posted = new Map();
        this._failsafeId = 0;

        this._committedL = {tag: 'committed', fn: () => this._update(State.COMMITTED)};
        this._goneL = {tag: 'gone', fn: () => {
            this._cancelFailsafe();
            this._update(State.GONE);
        }};
        this._alphaGoneL = {tag: 'alphaGone', fn: () => {
            this._updateResting();
            if (!this._addEnd(this._view.backgroundAlpha, this._goneL))
                this._scheduleFailsafe();
        }};
        this._update(State.GONE, true);
        this._updateResting();
    }

    set isLeft(v) {
        this._view.isLeft = v;
    }

    get isLeft() {
        return this._view.isLeft;
    }

    // ---- mainHandler -----------------------------------------------------------------------------------------------

    _post(tag, ms, fn) {
        const id = GLib.timeout_add(GLib.PRIORITY_DEFAULT, Math.max(0, Math.round(ms)), () => {
            this._posted.get(tag)?.delete(id);
            fn();
            return GLib.SOURCE_REMOVE;
        });
        if (!this._posted.has(tag))
            this._posted.set(tag, new Set());
        this._posted.get(tag).add(id);
    }

    _removePosted(tag) {
        for (const id of this._posted.get(tag) ?? [])
            GLib.source_remove(id);
        this._posted.delete(tag);
    }

    // DelayedOnAnimationEndListener on a value: posted when its animation ends (not when it is cancelled), or run
    // now when nothing runs
    _addEnd(value, listener) {
        if (!value.running) {
            listener.fn();
            return false;
        }
        value.onEnd(cancelled => {
            if (!cancelled)
                this._post(listener.tag, 0, listener.fn);
        });
        return true;
    }

    _scheduleFailsafe() {
        this._cancelFailsafe();
        this._failsafeId = GLib.timeout_add(GLib.PRIORITY_DEFAULT, FAILSAFE_MS, () => {
            this._failsafeId = 0;
            this._update(State.GONE, true);
            return GLib.SOURCE_REMOVE;
        });
    }

    _cancelFailsafe() {
        if (this._failsafeId)
            GLib.source_remove(this._failsafeId);
        this._failsafeId = 0;
    }

    _cancelAllPending() {
        this._cancelFailsafe();
        this._view.cancelAnimations();
        for (const tag of ['committed', 'gone', 'alphaGone'])
            this._removePosted(tag);
    }

    // ---- the touch -------------------------------------------------------------------------------------------------

    _track(t, x, y) {
        this._velocity.add(t, x, y);
        this._lastT = t;
    }

    onDown(x, y, t) {
        this._velocity.clear();
        this._track(t, x, y);
        this._cancelAllPending();
        this._startX = x;
        this._startY = y;
        this._update(State.GONE);
        this._updateYStart(y);
        this._passedDragSlop = false;
        this._view.resetStretch();
    }

    onMove(x, y, t) {
        this._track(t, x, y);
        if (this._dragSlopExceeded(x))
            this._handleMove(x, y);
    }

    onUp(x, _y, t) {
        // VelocityTracker on ACTION_UP: nothing new, unless the finger had stopped
        if (t - this._lastT > 40)
            this._velocity.clear();
        switch (this.state) {
        case State.ENTRY:
            if (this._flungAway(x) || this._previousX > STATIC_THRESHOLD)
                this._update(State.FLUNG);
            else
                this._update(State.CANCELLED);
            break;
        case State.INACTIVE:
            if (this._flungAway(x)) {
                this._cb.setTriggerBack(true);
                this._post(null, MIN_DURATION_INACTIVE_BEFORE_FLUNG, () => this._update(State.FLUNG));
            } else {
                this._update(State.CANCELLED);
            }
            break;
        case State.ACTIVE:
            if (this._previous === State.ENTRY && now() - this._entryTime < MIN_DURATION_ENTRY_TO_ACTIVE_AS_FLING)
                this._update(State.FLUNG);
            else if (this._previous === State.INACTIVE &&
                now() - this._inactiveTime < MIN_DURATION_INACTIVE_TO_ACTIVE_AS_FLING)
                this._post(null, MIN_DURATION_ACTIVE_AFTER_INACTIVE, () => this._update(State.COMMITTED));
            else
                this._update(State.COMMITTED);
            break;
        default:
            this._update(State.CANCELLED);
        }
    }

    // something else took the touch: gone at once
    onCancel() {
        this._update(State.GONE);
    }

    _dragSlopExceeded(x) {
        if (this._passedDragSlop)
            return true;
        if (Math.abs(x - this._startX) > EDGE_SLOP) {
            this._update(State.ENTRY);
            this._passedDragSlop = true;
        }
        return this._passedDragSlop;
    }

    _flungAway(endX) {
        const distance = this.isLeft ? endX - this._deltaStartX : this._deltaStartX - endX;
        const vx = this._velocity.velocityX;
        return distance > MIN_FLING_DISTANCE && (this.isLeft ? vx : -vx) > MIN_FLING_V;
    }

    _pastThresholdToActive(past, delay) {
        const first = this._pastThresholdTime === 0;
        if (!past) {
            this._pastThresholdTime = 0;
            return false;
        }
        if (first) {
            this._pastThresholdTime = now();
            this._entryToActiveDelay = delay();
        }
        return now() - this._pastThresholdTime > this._entryToActiveDelay;
    }

    _updateStateOnMove(yT, xT) {
        const withinY = xT * 2 >= yT, pastStatic = xT > STATIC_THRESHOLD;
        switch (this.state) {
        case State.ENTRY: {
            // the faster the finger, the sooner: 10 … 100 ms
            const delay = () => lerp(MIN_DURATION_ENTRY_BEFORE_ACTIVE, MAX_DURATION_ENTRY_BEFORE_ACTIVE,
                1 - smoothStep(0.5, 1, Math.abs(this._velocity.velocityX / 1000)));
            if (this._pastThresholdToActive(pastStatic, delay))
                this._update(State.ACTIVE);
            break;
        }
        case State.INACTIVE:
            if (this._pastThresholdToActive(pastStatic && this._totalInactive >= REACTIVATION && withinY,
                () => MIN_DURATION_INACTIVE_BEFORE_ACTIVE))
                this._update(State.ACTIVE);
            break;
        case State.ACTIVE:
            if ((!withinY || this._totalActive <= DEACTIVATION) &&
                now() - this._entryTime > MIN_DURATION_ACTIVE_BEFORE_INACTIVE)
                this._update(State.INACTIVE);
            break;
        }
    }

    _handleMove(x, y) {
        const yOffset = y - this._startY, yT = Math.abs(yOffset);
        const xT = Math.max(0, this.isLeft ? x - this._startX : this._startX - x);
        const xDelta = xT - this._previousX;
        this._previousX = xT;
        if (Math.abs(xDelta) > 0) {
            // the range reactivation..deactivation is empty: only the direction counts
            if (Math.sign(xDelta) === Math.sign(this._totalActive)) {
                this._totalActive += xDelta;
            } else {
                this._totalActive = xDelta;
                this._deltaStartX = x;
            }
            this._totalInactive = Math.max(this._totalInactive + xDelta, -TOUCH_SLOP);
        }
        this._updateStateOnMove(yT, xT);

        let progress = null;
        if (this.state === State.ACTIVE) {
            progress = clamp((xT - this._xOnActive) / FULLY_STRETCHED, 0, 1);
            this._view.setStretch(horizontalTranslation(progress), arrowAngle(progress), 1, 1, activeWidth(progress),
                1, 1, 1, P.stretched);
        } else if (this.state === State.ENTRY) {
            progress = clamp(xT / STATIC_THRESHOLD, 0, 1);
            this._view.setStretch(0, arrowAngle(progress), P.entry.arrow.alphaInterpolator.get(progress).value, 1,
                entryWidth(progress), heightCurve(progress), edgeCorner(progress), farCorner(progress),
                P.preThreshold);
        } else if (this.state === State.INACTIVE) {
            progress = clamp(this._totalInactive / REACTIVATION, 0, 1);
            this._view.setStretch(0, arrowAngle(progress),
                P.preThreshold.arrow.alphaInterpolator.get(progress).value, 1, this._preThresholdWidth(progress),
                heightCurve(progress), edgeCorner(progress), farCorner(progress), P.preThreshold);
        }
        this._setArrowStrokeAlpha(progress);
        this._setVerticalTranslation(yOffset);
    }

    _preThresholdWidth(progress) {
        let interpolator = this._previousPreThresholdWidth;
        if (this._totalInactive > TOUCH_SLOP)
            interpolator = this._totalInactive > 0 ? entryWidth : entryWidthTowardsEdge;
        this._previousPreThresholdWidth = interpolator;
        return Math.max(0, interpolator(progress));
    }

    _setArrowStrokeAlpha(progress) {
        let p = null;
        if (this.state === State.ENTRY || this.state === State.INACTIVE)
            p = progress;
        else if (this.state === State.ACTIVE || this.state === State.FLUNG || this.state === State.COMMITTED)
            p = 1;
        else
            p = 0;
        const dims = this.state === State.ENTRY ? P.entry : this.state === State.ACTIVE ? P.active : P.preThreshold;
        if (p !== null) {
            const v = dims.arrow.alphaSpring.get(p);
            if (v.isNew)
                this._view.popArrowAlpha(0, v.value);
        }
    }

    // a rubber band 15 times the finger's travel, through verticalTranslationInterpolator
    _setVerticalTranslation(yOffset) {
        const max = (PANEL_H - P.entry.bg.height) / 2;
        const p = clamp(Math.abs(yOffset) / (max * 15), 0, 1);
        this._view.verticalTranslation.stretchTo(verticalTranslation(p) * max * Math.sign(yOffset));
    }

    _updateYStart(touchY) {
        const y = Math.max(touchY - FINGER_OFFSET, MIN_ARROW_Y) - PANEL_H / 2;
        this._view.place(clamp(Math.round(y), 0, SCREEN_H));
    }

    // ---- the springs' resting positions by state -----------------------------------------------------------------

    _updateResting() {
        const v = this._view, st = this.state;
        switch (st) {
        case State.GONE:
        case State.ENTRY:
            v.setSprings({arrowLength: P.entry.arrow.lengthSpring, arrowHeight: P.entry.arrow.heightSpring,
                scale: P.entry.scaleSpring, verticalTranslation: P.entry.vTransSpring,
                horizontalTranslation: P.entry.hTransSpring, backgroundAlpha: P.entry.bg.alphaSpring,
                backgroundWidth: P.entry.bg.widthSpring, backgroundHeight: P.entry.bg.heightSpring,
                edge: P.entry.bg.edgeSpring, far: P.entry.bg.farSpring});
            break;
        case State.INACTIVE:
            v.setSprings({arrowLength: P.preThreshold.arrow.lengthSpring,
                arrowHeight: P.preThreshold.arrow.heightSpring,
                horizontalTranslation: P.preThreshold.hTransSpring, scale: P.preThreshold.scaleSpring,
                backgroundWidth: P.preThreshold.bg.widthSpring, backgroundHeight: P.preThreshold.bg.heightSpring,
                edge: P.preThreshold.bg.edgeSpring, far: P.preThreshold.bg.farSpring});
            break;
        case State.ACTIVE:
            v.setSprings({arrowLength: P.active.arrow.lengthSpring, arrowHeight: P.active.arrow.heightSpring,
                scale: P.active.scaleSpring, horizontalTranslation: P.active.hTransSpring,
                backgroundWidth: P.active.bg.widthSpring, backgroundHeight: P.active.bg.heightSpring,
                edge: P.active.bg.edgeSpring, far: P.active.bg.farSpring});
            break;
        case State.FLUNG:
            v.setSprings({arrowLength: P.flung.arrow.lengthSpring, arrowHeight: P.flung.arrow.heightSpring,
                backgroundWidth: P.flung.bg.widthSpring, backgroundHeight: P.flung.bg.heightSpring,
                edge: P.flung.bg.edgeSpring, far: P.flung.bg.farSpring});
            break;
        case State.COMMITTED:
            v.setSprings({arrowLength: P.committed.arrow.lengthSpring, arrowHeight: P.committed.arrow.heightSpring,
                scale: P.committed.scaleSpring, backgroundAlpha: P.committed.bg.alphaSpring,
                backgroundWidth: P.committed.bg.widthSpring, backgroundHeight: P.committed.bg.heightSpring,
                edge: P.committed.bg.edgeSpring, far: P.committed.bg.farSpring});
            break;
        case State.CANCELLED:
            v.setSprings({backgroundAlpha: P.cancelled.bg.alphaSpring});
            break;
        }
        const pick = (map, fallback = null) => map[st] ?? fallback;
        v.setRestingDimens({
            scale: pick({[State.ACTIVE]: P.active.scale, [State.FLUNG]: P.active.scale,
                [State.COMMITTED]: P.committed.scale}, P.preThreshold.scale),
            pivotX: pick({[State.GONE]: P.preThreshold.pivotX, [State.ENTRY]: P.preThreshold.pivotX,
                [State.INACTIVE]: P.preThreshold.pivotX, [State.CANCELLED]: P.preThreshold.pivotX,
                [State.ACTIVE]: P.active.pivotX}),
            hTrans: pick({[State.GONE]: -P.active.bg.width, [State.ENTRY]: P.entry.hTrans,
                [State.INACTIVE]: P.entry.hTrans, [State.FLUNG]: P.active.hTrans, [State.ACTIVE]: P.active.hTrans,
                [State.CANCELLED]: P.cancelled.hTrans}),
            arrow: pick({[State.GONE]: P.entry.arrow, [State.ENTRY]: P.entry.arrow, [State.INACTIVE]: P.entry.arrow,
                [State.ACTIVE]: P.active.arrow, [State.FLUNG]: P.flung.arrow, [State.COMMITTED]: P.committed.arrow,
                [State.CANCELLED]: P.cancelled.arrow}),
            bg: pick({[State.GONE]: P.entry.bg, [State.ENTRY]: P.entry.bg, [State.INACTIVE]: P.entry.bg,
                [State.ACTIVE]: P.active.bg, [State.FLUNG]: P.active.bg, [State.COMMITTED]: P.committed.bg,
                [State.CANCELLED]: P.cancelled.bg}),
        }, !(st === State.FLUNG || st === State.COMMITTED));
    }

    _playWithBackgroundWidth(listener, delay = 0) {
        if (delay > 0) {
            this._post(null, delay, () => this._playWithBackgroundWidth(listener, 0));
            return;
        }
        this._updateResting();
        if (!this._addEnd(this._view.backgroundWidth, listener))
            this._scheduleFailsafe();
    }

    _update(state, force = false) {
        if (!force && this.state === state)
            return;
        this._previous = this.state;
        this.state = state;
        const prev = this._previous, v = this._view;

        switch (state) {
        case State.CANCELLED:
            this._cb.cancelBack();
            break;
        case State.FLUNG:
        case State.COMMITTED:
            if (prev !== State.FLUNG)
                this._cb.triggerBack();
            break;
        case State.ENTRY:
        case State.INACTIVE:
            this._cb.setTriggerBack(false);
            break;
        case State.ACTIVE:
            this._cb.setTriggerBack(true);
            break;
        }

        switch (state) {
        case State.GONE:
            this._updateResting();
            v.visible = false;
            break;
        case State.ENTRY:
            v.visible = true;
            this._updateResting();
            this._entryTime = now();
            break;
        case State.ACTIVE:
            this._xOnActive = this._previousX;
            this._updateResting();
            this._cb.haptic();
            v.popOffEdge(prev === State.INACTIVE ? POP_ON_INACTIVE_TO_ACTIVE_V : POP_ON_ENTRY_TO_ACTIVE_V);
            break;
        case State.INACTIVE:
            this._inactiveTime = now();
            this._totalInactive = DEACTIVATION;
            v.popOffEdge(POP_ON_INACTIVE_V);
            this._cb.haptic();
            this._updateResting();
            break;
        case State.FLUNG:
            if (prev !== State.ACTIVE)
                this._cb.haptic();
            this._post(null, POP_ON_FLING_DELAY, () => v.popScale(POP_ON_FLING_V));
            this._post(this._committedL.tag, MIN_DURATION_FLING, this._committedL.fn);
            this._updateResting();
            break;
        case State.COMMITTED:
            if (prev === State.FLUNG) {
                this._updateResting();
                this._post(this._goneL.tag, MIN_DURATION_COMMITTED_AFTER_FLING, this._goneL.fn);
            } else {
                v.popScale(POP_ON_COMMITTED_V);
                this._post(this._alphaGoneL.tag, MIN_DURATION_COMMITTED, this._alphaGoneL.fn);
            }
            break;
        case State.CANCELLED: {
            this._playWithBackgroundWidth(this._goneL, Math.max(0, MIN_DURATION_CANCELLED - (now() - this._entryTime)));
            v.popArrowAlpha(0, P.cancelled.arrow.alphaSpring.get(0).value);
            break;
        }
        }
    }

    destroy() {
        this._cancelFailsafe();
        for (const tag of [...this._posted.keys()])
            this._removePosted(tag);
        this._view.destroy();
    }
}
