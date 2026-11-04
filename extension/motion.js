// Android's motion, run on Clutter's frame clock: the interpolators, FlingAnimationUtils, the AndroidX springs, the
// velocity tracker, and an animator that calls back once per frame.
import Clutter from 'gi://Clutter';

export const lerp = (a, b, t) => a + (b - a) * t;
export const clamp = (v, lo = 0, hi = 1) => Math.min(Math.max(v, lo), hi);

// cubic-bezier(x1, y1, x2, y2) as a function of x in [0, 1] (PathInterpolator)
export function bezier(x1, y1, x2, y2) {
    const cx = 3 * x1, bx = 3 * (x2 - x1) - cx, ax = 1 - cx - bx;
    const cy = 3 * y1, by = 3 * (y2 - y1) - cy, ay = 1 - cy - by;
    const sx = t => ((ax * t + bx) * t + cx) * t;
    const sy = t => ((ay * t + by) * t + cy) * t;
    const dx = t => (3 * ax * t + 2 * bx) * t + cx;
    return x => {
        if (x <= 0)
            return 0;
        if (x >= 1)
            return 1;
        // Newton, then bisection where the slope is too flat
        let t = x;
        for (let i = 0; i < 8; i++) {
            const e = sx(t) - x;
            if (Math.abs(e) < 1e-6)
                return sy(t);
            const d = dx(t);
            if (Math.abs(d) < 1e-6)
                break;
            t -= e / d;
        }
        let lo = 0, hi = 1;
        t = x;
        for (let i = 0; i < 40; i++) {
            const v = sx(t);
            if (Math.abs(v - x) < 1e-6)
                break;
            if (v < x)
                lo = t;
            else
                hi = t;
            t = (lo + hi) / 2;
        }
        return sy(t);
    };
}

// Interpolators.java
export const FAST_OUT_SLOW_IN = bezier(0.4, 0, 0.2, 1);
export const LINEAR_OUT_SLOW_IN = bezier(0, 0, 0.2, 1);
export const FAST_OUT_LINEAR_IN = bezier(0.4, 0, 1, 1);
export const TOUCH_RESPONSE = bezier(0.3, 0, 0.1, 1);
export const PANEL_CLOSE_ACCELERATED = bezier(0.3, 0, 0.5, 1);
export const EMPHASIZED_DECELERATE = bezier(0.05, 0.7, 0.1, 1);
export const EMPHASIZED_ACCELERATE = bezier(0.3, 0, 0.8, 0.15);
export const LINEAR = x => x;

// The fling threshold and the speed above which FAU treats a fling as fast, screen px/s
export const MIN_FLING_VELOCITY = 750;
const HIGH_VELOCITY = 3000;     // the dp/s constant FAU compares with px/s (a bug kept on purpose, spec §2.4)

// FlingAnimationUtils.apply: the duration (ms) and interpolator to move from @cur to @end at @velocity (px/s)
// maxLength in s; maxDistance in px (the cap scales with sqrt(diff / maxDistance))
export function fling(cur, end, velocity, {maxLength = 0.6, speedUpFactor = 0, maxDistance} = {}) {
    const diff = Math.abs(end - cur);
    if (diff === 0)
        return {duration: 0, interpolator: LINEAR};
    const v = Math.abs(velocity);
    const cap = maxLength * Math.sqrt(diff / (maxDistance ?? diff));
    const vf = speedUpFactor === 0 ? 1 : Math.min(v / HIGH_VELOCITY, 1);
    const x2 = lerp(0.35, 0.68, speedUpFactor);
    const sg = lerp(0.75, 1 / x2, vf);
    const sp = speedUpFactor * (1 - vf);
    let duration = v > 0 ? sg * diff / v : Infinity;
    let interpolator = bezier(sp, sp * sg, x2, 1);
    if (duration > cap) {
        if (v >= MIN_FLING_VELOCITY) {
            // keep the release velocity at the start, cross-fading into the curve
            const base = interpolator;
            const linear = cap * v / Math.max(diff, 1e-3);
            interpolator = x => {
                const w = LINEAR_OUT_SLOW_IN(x);
                return (1 - w) * (x * linear) + w * base(x);
            };
        } else {
            interpolator = FAST_OUT_SLOW_IN;
        }
        duration = cap;
    }
    return {duration: duration * 1000, interpolator};
}

// FlingAnimationUtils.applyDismissing (FAU:294-339): the notification dismiss fling
export function flingDismissing(cur, end, velocity, rowWidth, {maxLength = 0.4} = {}) {
    const diff = Math.abs(end - cur);
    const v = Math.abs(velocity);
    const cap = maxLength * Math.sqrt(diff / rowWidth);
    const y2 = lerp(0.4, 0.5, clamp((v - 750) / (9000 - 750)));
    const interpolator = bezier(0, 0, 0.5, y2);
    let duration = v > 0 ? (y2 / 0.5) * diff / v : cap;
    duration = Math.min(duration, cap);
    return {duration: duration * 1000, interpolator};
}

// OverScroller's fling (SplineOverScroller): how far and how long a scroll flung at @velocity (px/s) runs, and its
// position curve; @density is px per dp
const INFLEXION = 0.35;
const DECELERATION_RATE = Math.log(0.78) / Math.log(0.9);
const SPLINE_SAMPLES = 100;
const SPLINE_POSITION = (() => {
    const p1 = 0.5 * INFLEXION, p2 = 1 - 1 * (1 - INFLEXION);
    const table = [];
    let xMin = 0;
    for (let i = 0; i < SPLINE_SAMPLES; i++) {
        const alpha = i / SPLINE_SAMPLES;
        let xMax = 1, x, coef;
        for (;;) {
            x = xMin + (xMax - xMin) / 2;
            coef = 3 * x * (1 - x);
            const tx = coef * ((1 - x) * p1 + x * p2) + x * x * x;
            if (Math.abs(tx - alpha) < 1e-5)
                break;
            if (tx > alpha)
                xMax = x;
            else
                xMin = x;
        }
        table.push(coef * ((1 - x) * 0.5 + x) + x * x * x);
    }
    table.push(1);
    return table;
})();

export function splineFling(velocity, density = 3) {
    const physicalCoeff = 9.80665 * 39.37 * density * 160 * 0.84;
    const friction = 0.015 * physicalCoeff;             // ViewConfiguration.getScrollFriction()
    const v = Math.abs(velocity);
    if (v === 0)
        return {distance: 0, duration: 0, position: () => 1};
    const l = Math.log(INFLEXION * v / friction);
    const distance = Math.sign(velocity) * friction * Math.exp(DECELERATION_RATE / (DECELERATION_RATE - 1) * l);
    const duration = 1000 * Math.exp(l / (DECELERATION_RATE - 1));
    const position = t => {
        if (t >= 1)
            return 1;
        const i = Math.floor(SPLINE_SAMPLES * t);
        const t0 = i / SPLINE_SAMPLES;
        const d0 = SPLINE_POSITION[i], d1 = SPLINE_POSITION[i + 1];
        return d0 + (t - t0) * (d1 - d0) * SPLINE_SAMPLES;
    };
    return {distance, duration, position};
}

// AndroidX SpringForce: F = -k x - 2 zeta sqrt(k) v, mass 1. Steps exactly (closed form) over any dt.
export class Spring {
    constructor({stiffness = 1500, dampingRatio = 1, value = 0, velocity = 0, target = 0, threshold = 0.001} = {}) {
        this.stiffness = stiffness;
        this.dampingRatio = dampingRatio;
        this.value = value;
        this.velocity = velocity;
        this.target = target;
        this.threshold = threshold;
    }

    get done() {
        return Math.abs(this.value - this.target) < this.threshold &&
            Math.abs(this.velocity) < this.threshold * 10;
    }

    step(dt) {
        const k = this.stiffness, z = this.dampingRatio, w = Math.sqrt(k);
        const x0 = this.value - this.target, v0 = this.velocity;
        let x, v;
        if (z > 1) {
            const s = w * Math.sqrt(z * z - 1);
            const r1 = -z * w + s, r2 = -z * w - s;
            const c2 = (r1 * x0 - v0) / (r1 - r2), c1 = x0 - c2;
            x = c1 * Math.exp(r1 * dt) + c2 * Math.exp(r2 * dt);
            v = c1 * r1 * Math.exp(r1 * dt) + c2 * r2 * Math.exp(r2 * dt);
        } else if (z === 1) {
            const c1 = x0, c2 = v0 + w * x0;
            const e = Math.exp(-w * dt);
            x = (c1 + c2 * dt) * e;
            v = (c2 - w * (c1 + c2 * dt)) * e;
        } else {
            const wd = w * Math.sqrt(1 - z * z);
            const c1 = x0, c2 = (v0 + z * w * x0) / wd;
            const e = Math.exp(-z * w * dt);
            const cos = Math.cos(wd * dt), sin = Math.sin(wd * dt);
            x = e * (c1 * cos + c2 * sin);
            v = e * ((c2 * wd - z * w * c1) * cos - (c1 * wd + z * w * c2) * sin);
        }
        this.value = this.target + x;
        this.velocity = v;
        if (this.done) {
            this.value = this.target;
            this.velocity = 0;
        }
        return this.value;
    }
}

// VelocityTracker, the LSQ2 strategy: a quadratic least-squares fit of the last 100 ms of positions; px/s
export class VelocityTracker {
    constructor() {
        this._samples = [];
    }

    clear() {
        this._samples = [];
    }

    // t in ms
    add(t, x, y) {
        const last = this._samples.at(-1);
        // a pause of 40 ms means the pointer stopped (VelocityTracker::ASSUME_POINTER_STOPPED_TIME)
        if (last && t - last.t > 40)
            this._samples = [];
        this._samples.push({t, x, y});
        while (this._samples.length > 20 || this._samples[0].t < t - 100)
            this._samples.shift();
    }

    _fit(key) {
        const s = this._samples;
        if (s.length < 2)
            return 0;
        const t0 = s.at(-1).t;
        // fit p(t) = a + b t + c t^2, t in s relative to the newest sample; velocity = b
        let n = 0, st = 0, st2 = 0, st3 = 0, st4 = 0, sp = 0, stp = 0, st2p = 0;
        for (const p of s) {
            const t = (p.t - t0) / 1000, v = p[key];
            n++;
            st += t;
            st2 += t * t;
            st3 += t * t * t;
            st4 += t * t * t * t;
            sp += v;
            stp += t * v;
            st2p += t * t * v;
        }
        if (s.length === 2) {
            const dt = (s[1].t - s[0].t) / 1000;
            return dt > 0 ? (s[1][key] - s[0][key]) / dt : 0;
        }
        // solve the 3x3 normal equations (Cramer)
        const m = [[n, st, st2], [st, st2, st3], [st2, st3, st4]];
        const r = [sp, stp, st2p];
        const det = a => a[0][0] * (a[1][1] * a[2][2] - a[1][2] * a[2][1]) -
            a[0][1] * (a[1][0] * a[2][2] - a[1][2] * a[2][0]) +
            a[0][2] * (a[1][0] * a[2][1] - a[1][1] * a[2][0]);
        const d = det(m);
        if (Math.abs(d) < 1e-12) {
            const dt = (s.at(-1).t - s[0].t) / 1000;
            return dt > 0 ? (s.at(-1)[key] - s[0][key]) / dt : 0;
        }
        const mb = m.map((row, i) => [row[0], r[i], row[2]]);
        return det(mb) / d;
    }

    get velocityX() {
        return this._fit('x');
    }

    get velocityY() {
        return this._fit('y');
    }
}

// Calls onFrame(dtSeconds, elapsedSeconds) once per frame of @actor's frame clock until stop() or onFrame returns
// false; onDone runs after the last frame (not after stop())
export class Animator {
    constructor(actor, onFrame, onDone = null) {
        this._onFrame = onFrame;
        this._onDone = onDone;
        this._last = 0;
        this._timeline = new Clutter.Timeline({actor, duration: 1000, repeat_count: -1});
        this._elapsed = 0;
        this._timeline.connect('new-frame', () => {
            const delta = this._timeline.get_delta() / 1000;
            this._elapsed += delta;
            if (this._onFrame(delta, this._elapsed) === false) {
                this.stop();
                this._onDone?.();
            }
        });
        this._timeline.start();
    }

    stop() {
        if (!this._timeline)
            return;
        this._timeline.stop();
        this._timeline.run_dispose();
        this._timeline = null;
    }

    get running() {
        return !!this._timeline;
    }
}

// A timed animation of a value from @from to @to over @duration ms with @interpolator; onUpdate(value, fraction)
export function animate(actor, {from, to, duration, interpolator = FAST_OUT_SLOW_IN, delay = 0, onUpdate, onDone}) {
    if (duration <= 0) {
        onUpdate(to, 1);
        onDone?.();
        return null;
    }
    return new Animator(actor, (dt, elapsed) => {
        const t = clamp((elapsed * 1000 - delay) / duration);
        const fraction = interpolator(t);
        onUpdate(lerp(from, to, fraction), fraction, t);
        return t < 1;
    }, onDone);
}

// A spring animation of a value; onUpdate(value, velocity)
export function animateSpring(actor, spring, onUpdate, onDone) {
    return new Animator(actor, dt => {
        // integrate in sub-steps of at most 4 ms for the stiff springs
        let left = Math.min(dt, 0.1);
        while (left > 0) {
            const h = Math.min(left, 0.004);
            spring.step(h);
            left -= h;
        }
        onUpdate(spring.value, spring.velocity);
        return !spring.done;
    }, onDone);
}
