// Magnetic notification swipes, 1:1 with SystemUI's MagneticNotificationRowManagerImpl (Android 16; the
// magnetic_notification_swipes flag is on in Lineage 23.2). The swiped row moves at half the finger and pulls its two
// neighbours on each side along, at 0.12 and 0.04 of it, rounding their corners as it goes. 72 dp out, the row lets go
// of them: it springs to the finger and follows it alone, the neighbours spring back; within 56 dp it is caught again.
// On release a row that let go is dismissed unless it is flung back the way it came; one still held is dismissed only
// by a fling (500 dp/s).
//
// A row is a listener (MagneticRowListener): setMagneticTranslation(t), triggerMagneticForce(t, spring, v),
// cancelMagneticAnimations(), cancelTranslationAnimations(), canBeDismissed, and setSwipeRoundness(top, bottom,
// animated) for the corners the swipe asks for (each corner the larger of that and its own).
import Gio from 'gi://Gio';
import GLib from 'gi://GLib';

const DP = 3;                                       // px per dp: rows count in screen px
const DETACH = 72 * DP;                             // MAGNETIC_DETACH_THRESHOLD_DP
const ATTACH = 56 * DP;                             // MAGNETIC_ATTACH_THRESHOLD_DP
const DISMISS_VELOCITY = 500 * DP;                  // px/s
const MULTIPLIERS = [0.04, 0.12, 0.5, 0.12, 0.04];  // MAGNETIC_TRANSLATION_MULTIPLIERS
const CENTRE = 2;
const ROUNDNESS = [0.5, 0.7, 0.9, 1.0, 0.9, 0.7, 0.5];
const REDUCTION = 0.65;                             // a row that cannot be dismissed moves less
const MAX_PRE_DETACH_ROUNDNESS = 0.8;
export const DETACH_SPRING = {stiffness: 800, dampingRatio: 0.95};
const SNAP_SPRING = {stiffness: 550, dampingRatio: 0.6};
const ATTACH_SPRING = {stiffness: 850, dampingRatio: 0.95};
const SWIPED = MULTIPLIERS[CENTRE];
const BUFFER = 10;                                  // DirectionEstimator's translations

const State = {IDLE: 0, TARGETS_SET: 1, PULLING: 2, DETACHED: 3};

// once 72 dp out: the sign of the last ten translations' average, the newest weighted most
class DirectionEstimator {
    constructor() {
        this._kernel = Array.from({length: BUFFER}, (_, i) => -0.45 * Math.cos(Math.PI * (i + 1) / BUFFER) + 0.55);
        this.reset();
    }

    reset() {
        this.direction = 0;
        this._buffer = new Array(BUFFER).fill(0);
        this._head = 0;
        this._accepting = true;
    }

    record(t) {
        if (!this._accepting)
            return;
        this._buffer[this._head] = t;
        this._head = (this._head + 1) % BUFFER;
    }

    halt() {
        this._accepting = false;
        let sum = 0;
        for (let k = 0; k < BUFFER; k++)
            sum += this._kernel[k] * this._buffer[(this._head + k) % BUFFER];
        this.direction = Math.sign(sum);
    }
}

// the threshold click (PRIMITIVE_CLICK): feedbackd's shortest tick, as for the back gesture. The pull's continuous
// low ticks are left out: feedbackd plays every effect at one strength
function thresholdHaptic() {
    Gio.DBus.session.call('org.sigxcpu.Feedback', '/org/sigxcpu/Feedback', 'org.sigxcpu.Feedback',
        'TriggerFeedback', new GLib.Variant('(ssa{sv}i)', ['de.yesman.GnomePhone', 'button-released', {}, -1]),
        null, Gio.DBusCallFlags.NONE, -1, null, null);
}

class MagneticSwipe {
    constructor() {
        this._state = State.IDLE;
        this._listeners = [];           // 5: the swiped row in the middle, its neighbours or null
        this._roundables = [];          // 7: the same and one more each side, rounded but not pulled
        this._direction = new DirectionEstimator();
    }

    get _swiped() {
        return this._listeners[CENTRE] ?? null;
    }

    // a swipe starts: @rows, the rows of the swiped one's section in order, take part
    setTargets(row, rows = [row]) {
        if (this._state !== State.IDLE)
            return;
        this._direction.reset();
        const i = rows.indexOf(row);
        const roundables = Array.from({length: ROUNDNESS.length}, (_, k) => rows[i + k - 3] ?? null);
        this._clearRoundness(roundables);
        this._roundables = roundables;
        const listeners = roundables.slice(1, -1);
        for (const l of listeners) {
            if (l && this._listeners.includes(l)) {
                l.cancelMagneticAnimations();
                if (l === this._swiped)
                    l.cancelTranslationAnimations();
            }
        }
        this._listeners = listeners;
        this._state = State.TARGETS_SET;
    }

    // the finger moved the row to @t (px from where it rests); false: not the row being swiped, nothing done
    setTranslation(row, t) {
        if (this._state === State.IDLE || row !== this._swiped)
            return false;
        const dismissible = row.canBeDismissed;
        switch (this._state) {
        case State.TARGETS_SET:
            this._direction.record(t);
            this._pull(t, dismissible);
            this._state = State.PULLING;
            break;
        case State.PULLING:
            this._direction.record(t);
            this._updateRoundness(t, false);
            if (dismissible && Math.abs(t) >= DETACH) {
                this._direction.halt();
                this._snapNeighboursBack(null);
                this._detach(row, t);
                this._state = State.DETACHED;
            } else {
                this._pull(t, dismissible);
            }
            break;
        case State.DETACHED:
            this._direction.record(t);
            if (Math.abs(t) <= ATTACH) {
                this._updateRoundness(t, true);
                this._attach(t);
                this._state = State.PULLING;
            } else {
                row.setMagneticTranslation(t);
            }
            break;
        }
        return true;
    }

    _pull(t, dismissible) {
        this._listeners.forEach((l, i) => {
            if (!l)
                return;
            const reduce = !dismissible || !l.canBeDismissed ? REDUCTION : 1;
            l.setMagneticTranslation(MULTIPLIERS[i] * t * reduce);
        });
    }

    _updateRoundness(t, animated) {
        const r = Math.min(Math.max(Math.abs(SWIPED * t) / DETACH, 0), MAX_PRE_DETACH_ROUNDNESS);
        const last = ROUNDNESS.length - 1;
        this._applyRoundness(ROUNDNESS.map((m, i) => [i === 0 ? 0 : m * r, i === last ? 0 : m * r]), animated);
    }

    // NotificationRoundnessManager.setRoundnessForAffectedViews: the first one there is rounds its bottom only
    _applyRoundness(set, animated) {
        let first = true;
        this._roundables.forEach((row, i) => {
            if (!row)
                return;
            const [top, bottom] = set[i];
            row.setSwipeRoundness(first ? 0 : top, bottom, animated);
            first = false;
        });
    }

    _detach(row, t) {
        const v = row.swipeVelocity ?? 0;
        row.cancelMagneticAnimations();
        row.triggerMagneticForce(t, DETACH_SPRING, this._direction.direction * Math.abs(v));
        // the swiped row fully round, the rows next to it on the side that faces it
        const n = ROUNDNESS.length, c = (n - 1) / 2;
        this._applyRoundness(Array.from({length: n}, (_, i) =>
            i === c - 1 ? [0, 1] : i === c ? [1, 1] : i === c + 1 ? [1, 0] : [0, 0]), true);
        thresholdHaptic();
    }

    _attach(t) {
        const v = this._swiped?.swipeVelocity ?? 0;
        thresholdHaptic();
        this._listeners.forEach((l, i) => {
            if (!l)
                return;
            l.cancelMagneticAnimations();
            l.triggerMagneticForce(MULTIPLIERS[i] * t, ATTACH_SPRING,
                i === CENTRE ? -this._direction.direction * Math.abs(v) : 0);
        });
        this._direction.reset();
    }

    _snapNeighboursBack(v) {
        this._listeners.forEach((l, i) => {
            if (!l || i === CENTRE)
                return;
            l.cancelMagneticAnimations();
            l.triggerMagneticForce(0, SNAP_SPRING, v === null ? 0 : v * MULTIPLIERS[i]);
        });
    }

    // the finger is up: the row goes (@dismissing) or snaps back, at @v px/s
    end(row, dismissing, v = null) {
        this._direction.reset();
        if (row === this._swiped) {
            switch (this._state) {
            case State.TARGETS_SET:
                if (dismissing)
                    thresholdHaptic();
                break;
            case State.PULLING:
                if (dismissing)
                    thresholdHaptic();
                this._snapNeighboursBack(v);
                break;
            case State.DETACHED:
                row.cancelMagneticAnimations();
                break;
            }
            this._state = State.IDLE;
        } else if (this._listeners.includes(row)) {
            // a neighbour going away (Clear all mid-swipe): its own animation takes over
            row.cancelMagneticAnimations();
        }
    }

    // isMagneticRowSwipedDismissible: a row that let go goes unless it was flung back against the way it went
    isDismissible(row, v) {
        if (row !== this._swiped)
            return false;
        const fast = Math.abs(v) >= DISMISS_VELOCITY;
        if (this._state === State.DETACHED)
            return !(fast && this._direction.direction !== Math.sign(v));
        return fast;
    }

    // which way the row let go: 1 right, -1 left, 0 it did not
    detachDirection(row) {
        return row === this._swiped ? this._direction.direction : 0;
    }

    // the corners back as they were (animated, as Roundable.requestRoundnessReset)
    resetRoundness() {
        this._clearRoundness([]);
        this._roundables = [];
    }

    _clearRoundness(keep) {
        for (const row of this._roundables) {
            if (row && !keep.includes(row))
                row.setSwipeRoundness(0, 0, true);
        }
    }

    // everything dropped: the shade went away mid-swipe, say
    reset() {
        this._direction.reset();
        for (const l of this._listeners) {
            l?.cancelMagneticAnimations();
            l?.cancelTranslationAnimations();
        }
        this._state = State.IDLE;
        this._listeners = [];
        this.resetRoundness();
    }
}

// one swipe at a time, as SystemUI's singleton: the shade's rows and the heads-up share it
export const magnetic = new MagneticSwipe();
