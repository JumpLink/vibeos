// A scrolling list's physics, as libspongy has them for the apps (libspongy/spongy.c): dragged or flung past an end it
// does not stop but stretches like a sponge, and springs back. The shell cannot link libspongy, so its stretch edge
// and the list that drives two of them are here in JavaScript, with the same constants and curves. Distances are
// screen px, times milliseconds.
import {splineFling} from './motion.js';

const NATURAL_FREQUENCY = 24.657;
const DAMPING_RATIO = 0.98;
const ABSORB_VELOCITY_ADJUST = 13;
const LINEAR_STRETCH_INTENSITY = 0.016;
const EXP_STRETCH_INTENSITY = 0.016;
const SCROLL_DIST_AFFECTED_BY_EXP = 0.33;
const LINEAR_VELOCITY_TAKE_OVER = 200;
const LINEAR_DISTANCE_TAKE_OVER = 8;
const VELOCITY_THRESHOLD = 0.01;
const VALUE_THRESHOLD = 0.001;
const NON_ZERO_EPSILON = 0.00004;
const INTERPOLATION_STRENGTH = 0.7;
const FLING_DESTRETCH_FACTOR = 4;
const PULL_EPS = 1e-6;
const DENSITY = 3;
const MIN_FLING = 50 * DENSITY;
const MAX_FLING = 8000 * DENSITY;

const IDLE = 0, PULL = 1, RECEDE = 3;
const sgn = x => (x > 0) - (x < 0);

// A stretch at one end: how far it is pulled (0..1 of the view), and the spring that takes it back
class Edge {
    constructor() {
        this.size = 0;
        this.finish();
        this.pullDistance = 0;
        this.t = 0;
    }

    finish() {
        this.state = IDLE;
        this.distance = 0;
        this.velocity = 0;
    }

    get finished() {
        return this.state === IDLE;
    }

    // delta in units of the size
    pull(delta, now) {
        if (this.state !== PULL)
            this.pullDistance = this.distance;      // catches a receding stretch where it is
        this.state = PULL;
        this.t = now;
        this.pullDistance = Math.min(this.pullDistance + delta, 1);
        this.distance = Math.max(0, this.pullDistance);
        this.velocity = 0;
        if (this.distance === 0)
            this.state = IDLE;
    }

    // returns what it took
    pullDistanceBy(delta, now) {
        const fin = Math.max(0, delta + this.distance), d = fin - this.distance;
        if (d === 0 && this.distance === 0)
            return 0;
        this.pull(d, now);
        return d;
    }

    release(now) {
        this.pullDistance = 0;
        if (this.state !== PULL)
            return;
        this.state = RECEDE;
        this.velocity = 0;
        this.t = now;
    }

    absorb(v, now) {
        this.state = RECEDE;
        this.velocity = Math.trunc(v) * ABSORB_VELOCITY_ADJUST;
        this.t = now;
    }

    _spring(now) {
        const dt = (now - this.t) / 1000;
        if (dt < 0.001)
            return;
        this.t = now;
        const H = this.size, d = this.distance;
        let v = this.velocity;
        if (H <= 0) {
            this.finish();
            return;
        }
        // the last few pixels at a straight 200 px/s, so it lands instead of creeping
        if (Math.abs(v) <= LINEAR_VELOCITY_TAKE_OVER && Math.abs(d * H) < LINEAR_DISTANCE_TAKE_OVER &&
            sgn(v) === -sgn(d)) {
            v = sgn(v) * LINEAR_VELOCITY_TAKE_OVER;
            const td = d + v * dt / H;
            if (sgn(td) !== sgn(d)) {
                this.distance = 0;
                this.velocity = 0;
            } else {
                this.distance = td;
                this.velocity = v;
            }
            return;
        }
        const wn = NATURAL_FREQUENCY, z = DAMPING_RATIO, wd = wn * Math.sqrt(1 - z * z);
        const x0 = d * H, s = (z * wn * x0 + v) / wd, E = Math.exp(-z * wn * dt);
        const x = E * (x0 * Math.cos(wd * dt) + s * Math.sin(wd * dt));
        const vv = x * -wn * z + E * (-wd * x0 * Math.sin(wd * dt) + wd * s * Math.cos(wd * dt));
        this.distance = x / H;
        this.velocity = vv;
        if (this.distance > 1) {
            this.distance = 1;
            this.velocity = 0;
        }
        const xp = this.distance * H;
        if (xp < 0 || (Math.abs(this.velocity) < VELOCITY_THRESHOLD && xp < VALUE_THRESHOLD)) {
            this.distance = 0;
            this.velocity = 0;
        }
    }

    // One frame: the stretch amount to draw with (signed: negative at the far end) and whether another frame is wanted
    frame(now, far) {
        if (this.state === RECEDE)
            this._spring(now);
        let a = 0;
        if (this.distance !== 0)
            a = damp(Math.min(1, this.distance)) * (far ? -1 : 1);
        let last = false;
        if (this.state === RECEDE && this.distance === 0 && this.velocity === 0) {
            this.state = IDLE;
            last = true;                // one more frame, to draw it unstretched
        }
        return {amount: Math.abs(a) <= NON_ZERO_EPSILON ? 0 : a, animating: this.state !== IDLE || last};
    }
}

// a pull's distance, damped
function damp(x) {
    const o = Math.abs(x), s = x > 0 ? 1 : -1;
    return s * (LINEAR_STRETCH_INTENSITY * o +
        EXP_STRETCH_INTENSITY * (1 - Math.exp(-o * (Math.E / SCROLL_DIST_AFFECTED_BY_EXP))));
}

// Where a pixel at @p (along the axis, 0..size) of the stretched picture samples its source (the stretch shader's
// mapping)
export function stretchMap(p, o, dim) {
    if (dim <= 0)
        return p;
    const stretched = dim / (1 + Math.abs(o));
    if (o > 0) {
        const off = dim - p;
        const v = 1 + (off / dim - 1) * INTERPOLATION_STRENGTH;
        return stretched - off / (1 + o * v);
    }
    if (o < 0) {
        const off = p;
        const v = 1 + (off / dim - 1) * INTERPOLATION_STRENGTH;
        return dim - (stretched - off / (1 - o * v));
    }
    return p;
}

// The inverse: where the source pixel at @src is drawn
export function stretchUnmap(src, o, dim) {
    if (o === 0 || dim <= 0)
        return src;
    let lo = -dim, hi = 2 * dim;
    for (let i = 0; i < 40; i++) {
        const mid = (lo + hi) / 2;
        if (stretchMap(mid, o, dim) < src)
            lo = mid;
        else
            hi = mid;
    }
    return (lo + hi) / 2;
}

// A list along one axis, offset 0 at its top and maxOffset at its end: a drag that goes on past an end stretches it, a
// fling that reaches an end hands its speed to the stretch, a drag or fling back undoes the stretch before it scrolls.
// The caller has taken the touch slop already: down() is a drag under way.
export class SpongyList {
    constructor() {
        this.offset = 0;
        this.maxOffset = 0;
        this.viewHeight = 0;
        this.top = new Edge();
        this.bottom = new Edge();
        this.fling = null;
        this.down_ = false;
        this.lastY = 0;
    }

    get stretched() {
        return !this.top.finished || !this.bottom.finished;
    }

    // the view's height and how far the content can scroll; what the offset passed is cut off
    setRange(viewHeight, maxOffset) {
        this.viewHeight = viewHeight;
        this.maxOffset = Math.max(0, maxOffset);
        this.offset = Math.min(this.offset, this.maxOffset);
        this.top.size = this.bottom.size = viewHeight;
    }

    get _canUp() {
        return this.offset > 0;
    }

    get _canDown() {
        return this.offset < this.maxOffset;
    }

    // scrolls by dy, returns what was not taken: how far past an end it would have gone
    _scrollBy(dy) {
        const to = this.offset + dy;
        if (to < 0) {
            this.offset = 0;
            return to;
        }
        if (to > this.maxOffset) {
            this.offset = this.maxOffset;
            return to - this.maxOffset;
        }
        this.offset = to;
        return 0;
    }

    // a touch on the list: it stops a fling and catches a receding stretch where it is
    begin(y, offset, now) {
        this.offset = Math.min(Math.max(offset, 0), this.maxOffset);
        this.fling = null;
        this.down_ = true;
        this.lastY = y;
        if (this.top.distance !== 0 && !this._canUp)
            this.top.pullDistanceBy(0, now);
        if (this.bottom.distance !== 0 && !this._canDown)
            this.bottom.pullDistanceBy(0, now);
    }

    // a drag while stretched goes to the stretch first, and only what is left of it scrolls; what it took, px
    _releaseGlow(dy, now) {
        const H = this.viewHeight;
        let consumed = 0;
        if (H <= 0)
            return 0;
        if (this.top.distance !== 0) {
            if (this._canUp) {
                this.top.release(now);
            } else {
                const want = -dy / H, got = this.top.pullDistanceBy(want, now);
                consumed = Math.abs(got - want) < PULL_EPS ? dy : -got * H;
                if (this.top.distance === 0)
                    this.top.release(now);
            }
        } else if (this.bottom.distance !== 0) {
            if (this._canDown) {
                this.bottom.release(now);
            } else {
                const want = dy / H, got = this.bottom.pullDistanceBy(want, now);
                consumed = Math.abs(got - want) < PULL_EPS ? dy : got * H;
                if (this.bottom.distance === 0)
                    this.bottom.release(now);
            }
        }
        return consumed;
    }

    // the finger at y: moving up (y shrinking) scrolls toward the end
    move(y, now) {
        if (!this.down_)
            return;
        let dy = this.lastY - y;
        this.lastY = y;
        dy -= this._releaseGlow(dy, now);
        const rest = this._scrollBy(dy);
        const H = this.viewHeight;
        // what the list could not take stretches the edge
        if (H > 0) {
            if (rest < 0)
                this.top.pullDistanceBy(-rest / H, now);
            else if (rest > 0)
                this.bottom.pullDistanceBy(rest / H, now);
        }
        // a scroll away from a stretched edge lets the stretch go
        if (!this.top.finished && dy > 0)
            this.top.release(now);
        if (!this.bottom.finished && dy < 0)
            this.bottom.release(now);
    }

    _shouldAbsorb(edge, velocity) {
        if (velocity > 0)
            return true;
        return Math.abs(splineFling(-velocity, DENSITY).distance) < edge.distance * this.viewHeight;
    }

    // the finger lets go at @velocity px/s (down positive)
    end(velocityY, now) {
        if (!this.down_)
            return;
        this.down_ = false;
        let velocity = -velocityY;              // toward the end positive
        velocity = Math.max(-MAX_FLING, Math.min(velocity, MAX_FLING));
        if (Math.abs(velocity) < MIN_FLING)
            velocity = 0;
        if (velocity !== 0) {
            // flinging while stretched moves the stretch, not the list
            let absorbed = false;
            if (this.top.distance !== 0) {
                if (this._shouldAbsorb(this.top, -velocity)) {
                    this.top.absorb(-velocity, now);
                    absorbed = true;
                }
            } else if (this.bottom.distance !== 0) {
                if (this._shouldAbsorb(this.bottom, velocity)) {
                    this.bottom.absorb(velocity, now);
                    absorbed = true;
                }
            }
            if (!absorbed)
                this._startFling(velocity, now);
        }
        this.top.release(now);
        this.bottom.release(now);
    }

    cancel(now) {
        this.down_ = false;
        this.top.release(now);
        this.bottom.release(now);
    }

    _startFling(velocity, now) {
        const {distance, duration, position, speed} = splineFling(velocity, DENSITY);
        this.fling = duration > 0 ? {start: now, distance, duration, position, speed, last: 0} : null;
    }

    // a fling out of a stretch first undoes it, four times as fast as it scrolls
    _consumeInStretch(unconsumed, now) {
        const size = this.viewHeight;
        if (size <= 0)
            return unconsumed;
        if (unconsumed > 0 && this.top.distance !== 0) {
            const delta = -unconsumed * FLING_DESTRETCH_FACTOR / size, got = this.top.pullDistanceBy(delta, now);
            if (Math.abs(got - delta) < PULL_EPS)
                return 0;
            this.top.finish();
            return unconsumed + size / FLING_DESTRETCH_FACTOR * got;
        }
        if (unconsumed < 0 && this.bottom.distance !== 0) {
            const delta = unconsumed * FLING_DESTRETCH_FACTOR / size, got = this.bottom.pullDistanceBy(delta, now);
            if (Math.abs(got - delta) < PULL_EPS)
                return 0;
            this.bottom.finish();
            return unconsumed - size / FLING_DESTRETCH_FACTOR * got;
        }
        return unconsumed;
    }

    // One frame at @now: the fling and the springs advance. Returns {top, bottom, animating}: the stretch amounts to
    // draw with (the top one anchored at the top, positive; the bottom one at the bottom, negative)
    frame(now) {
        let more = false;
        const f = this.fling;
        if (f) {
            const t = Math.min(Math.max((now - f.start) / f.duration, 0), 1);
            const pos = f.distance * f.position(t);
            let dy = pos - f.last;
            f.last = pos;
            dy = this._consumeInStretch(dy, now);
            const rest = this._scrollBy(dy);
            if (t >= 1 || rest !== 0) {
                if (rest !== 0) {
                    // the end reached with speed left: the stretch takes it
                    const v = Math.abs(f.speed(t));
                    if (rest < 0 && this.top.finished)
                        this.top.absorb(v, now);
                    else if (rest > 0 && this.bottom.finished)
                        this.bottom.absorb(v, now);
                }
                this.fling = null;
            } else {
                more = true;
            }
        }
        const a = this.top.frame(now, false), b = this.bottom.frame(now, true);
        return {top: a.amount, bottom: b.amount, animating: more || a.animating || b.animating || this.down_};
    }
}
