// Heads-up notifications, Android 16's (HeadsUpManagerImpl, AvalancheController and the stack's heads-up path, with
// physical_notification_movement, notification_appear_nonlinear and notification_avalanche_throttle_hun on, as they
// are on the reference phone: spec/aflags-lineage-23.2.txt). A notification that asks for a banner pops in under the
// status bar as its shade row, collapsed, on an opaque card that casts the 16 dp shadow of a pinned row. It stays 5 s
// from when it has slid in, or 1 s when another one waits, which then cycles in in its place. Tapped it opens, swiped
// sideways it is dismissed, pushed up it goes and its app peeks no more for a minute, and pulled down it opens the
// shade and moves along into the stack. GNOME's own banners stay blocked meanwhile: this takes their requests.
import Cairo from 'cairo';
import Clutter from 'gi://Clutter';
import GLib from 'gi://GLib';
import St from 'gi://St';

import * as Main from 'resource:///org/gnome/shell/ui/main.js';
import * as MessageTray from 'resource:///org/gnome/shell/ui/messageTray.js';

import {Canvas} from './icons.js';
import {Animator, bezier, clamp, FAST_OUT_SLOW_IN, lerp, MIN_FLING_VELOCITY, Spring, VelocityTracker} from './motion.js';
import {MIN_H, Row, ROW_W, SIDE} from './notifications.js';
import {Color} from './palette.js';
import {QQS_HEIGHT} from './qs.js';
import {contentAlpha} from './shade.js';
import {BAR_HEIGHT} from './statusbar.js';
import {px, SCALE, screenPixels} from './units.js';

// --- screen px
const SCREEN_W = 1080;
const SCREEN_H = 2280;
const PINNED_Y = BAR_HEIGHT + 24;       // under the status bar + heads_up_status_bar_padding, 8 dp
const ABOVE_SCREEN_Y = -96;             // heads_up_appear_y_above_screen: it comes from, and goes back to, 32 dp up
const CYCLING_PADDING = 24;             // heads_up_cycling_padding, 8 dp
const TOUCH_SLOP = 24;
const APPEAR_END = QQS_HEIGHT + 48 + MIN_H;     // the shade's height that shows the stack's first row whole

// --- motion
const REVEAL_MS = 400;                  // ANIMATION_DURATION_HEADS_UP_APPEAR, _DISAPPEAR and _CYCLING
const ALPHA_START = 0.55;               // ALPHA_APPEAR_START_FRACTION: the content shows from here to the end
const FAST_OUT_SLOW_IN_REVERSE = bezier(0.8, 0, 0.6, 1);
const Y_STIFFNESS = 380;                // PhysicsPropertyAnimator.createDefaultSpring moves it, not the 400 ms curve
const Y_DAMPING = 0.68;
const Y_THRESHOLD = 0.75;               // DynamicAnimation: 1 px of visible change x 0.75
const EXPAND_MS = 360;

// --- time, ms (SystemUI config.xml)
const DECAY_MS = 5000;                  // heads_up_notification_decay
const POST_DELAY_MS = 700;              // touch_acceptance_delay: the post time is when it has slid in, taps before it
                                        // are swallowed (shouldSwallowClick)
const MIN_SHOWN_MS = 500;               // heads_up_notification_minimum_time_with_throttling
const NEXT_SAME_MS = 1000;              // AvalancheController: one of the same urgency waiting cuts it to 1 s,
const NEXT_HIGHER_MS = 500;             // ... a more urgent one to 0.5 s
const SNOOZE_MS = 60000;                // heads_up_default_snooze_length_ms

const now = () => GLib.get_monotonic_time() / 1000;

// --- the shadow of a pinned row, z 16 dp under hwui's light at (540, 0, 600 dp), radius 800 dp. The ambient part
// (α 0.039) blurs over z / 2; the spot part (α 0.19) over 800 · 16 / (600 - 16) dp, 2.7 % larger than the card and
// 7 px lower, where the heads-up is pinned. Each is a Gaussian-blurred rounded rect, x0..x1 and its ends that far
// from the card's (screen px).
const SHADOW_PAD = 72;                  // how far out it reaches
const SHADOW_END = 96;                  // the part of either end that holds a 28 dp corner
const SHADOW_LAYERS = [
    {alpha: 0.039, sigma: 6, x0: 0, x1: ROW_W, top: 0, bottom: 0, radius: 84},
    {alpha: 0.19, sigma: 16, x0: -13.5, x1: ROW_W + 13.5, top: 3.8, bottom: 9.8, radius: 86},
];
const PROFILE_STOPS = 16;

// the share of a Gaussian of @sigma that lies beyond @d: the alpha of a blurred edge @d px outside it
function beyond(d, sigma) {
    const x = d / (sigma * Math.SQRT2);
    const t = 1 / (1 + 0.3275911 * Math.abs(x));   // Abramowitz & Stegun 7.1.26
    const erf = 1 - ((((1.061405429 * t - 1.453152027) * t + 1.421413741) * t - 0.284496736) * t + 0.254829592) *
        t * Math.exp(-x * x);
    return 0.5 * (1 - Math.sign(x) * erf);
}

// a gradient whose stops go from @d0 to @d1 px outside the edge
function addProfile(pattern, layer, d0, d1) {
    for (let i = 0; i <= PROFILE_STOPS; i++) {
        const d = lerp(d0, d1, i / PROFILE_STOPS);
        pattern.addColorStopRGBA(i / PROFILE_STOPS, 0, 0, 0, layer.alpha * beyond(d, layer.sigma));
    }
}

function fillClipped(cr, pattern, x0, y0, x1, y1) {
    cr.save();
    cr.rectangle(x0, y0, x1 - x0, y1 - y0);
    cr.clip();
    cr.setSource(pattern);
    cr.paint();
    cr.restore();
}

// The sides: from the card's left and right edges out, from y0 to y1
function paintSides(cr, layer, y0, y1) {
    const reach = 3 * layer.sigma;
    for (const [edge, out] of [[layer.x0, -1], [layer.x1, 1]]) {
        const g = new Cairo.LinearGradient(edge - out * reach, 0, edge + out * reach, 0);
        addProfile(g, layer, -reach, reach);
        const [a, b] = out < 0 ? [-SHADOW_PAD, edge + reach] : [edge - reach, ROW_W + SHADOW_PAD];
        fillClipped(cr, g, a, y0, b, y1);
    }
}

// One end of the shadow, the card's edge at y 0 and outside it up (y < 0): the edge, its corners, the sides below
function paintEnd(cr, edgeOffset) {
    for (const layer of SHADOW_LAYERS) {
        const reach = 3 * layer.sigma, r = layer.radius;
        const edge = edgeOffset(layer), cy = edge + r;
        for (const cx of [layer.x0 + r, layer.x1 - r]) {
            const g = new Cairo.RadialGradient(cx, cy, Math.max(r - reach, 0), cx, cy, r + reach);
            addProfile(g, layer, Math.max(r - reach, 0) - r, reach);
            const [a, b] = cx < ROW_W / 2 ? [-SHADOW_PAD, cx] : [cx, ROW_W + SHADOW_PAD];
            fillClipped(cr, g, a, -SHADOW_PAD, b, cy);
        }
        const g = new Cairo.LinearGradient(0, edge - reach, 0, edge + reach);
        addProfile(g, layer, reach, -reach);
        fillClipped(cr, g, layer.x0 + r, -SHADOW_PAD, layer.x1 - r, cy);
        paintSides(cr, layer, cy, SHADOW_END);
    }
}

function clear(cr) {
    cr.setOperator(Cairo.Operator.CLEAR);
    cr.paint();
    cr.setOperator(Cairo.Operator.OVER);
}

// Three pictures drawn once each: the top end, a 1 px slice of the sides that stretches, the bottom end
class Shadow {
    constructor() {
        const w = px(ROW_W + 2 * SHADOW_PAD), endH = px(SHADOW_PAD + SHADOW_END);
        this.actor = new Clutter.Actor({reactive: false});
        this._top = new Canvas({width: w, height: endH, y: px(-SHADOW_PAD)});
        this._top.draw = cr => {
            clear(cr);
            screenPixels(cr);
            cr.translate(SHADOW_PAD, SHADOW_PAD);
            paintEnd(cr, layer => layer.top);
        };
        this._middle = new Canvas({width: w, height: 1, y: px(SHADOW_END)});
        this._middle.draw = cr => {
            clear(cr);
            screenPixels(cr);
            cr.translate(SHADOW_PAD, 0);
            for (const layer of SHADOW_LAYERS)
                paintSides(cr, layer, 0, SCALE);
        };
        this._bottom = new Canvas({width: w, height: endH});
        this._bottom.draw = cr => {
            clear(cr);
            screenPixels(cr);
            cr.translate(SHADOW_PAD, SHADOW_END);
            cr.scale(1, -1);
            paintEnd(cr, layer => -layer.bottom);
        };
        for (const part of [this._top, this._middle, this._bottom])
            this.actor.add_child(part);
    }

    // around the card at (x, y), @h tall; a card shorter than its two ends squeezes the whole shadow
    place(x, y, h, alpha) {
        const minH = 2 * SHADOW_END;
        const layoutH = Math.max(h, minH);
        this.actor.set_position(px(x - SHADOW_PAD), px(y));
        this.actor.scale_y = h >= minH ? 1 : h / minH;
        this._middle.scale_y = (layoutH - minH) / SCALE;
        this._bottom.y = px(layoutH - SHADOW_END);
        this.actor.opacity = Math.round(255 * alpha);
    }

    destroy() {
        this.actor.destroy();
    }
}

// --- one heads-up on screen

class Entry {
    constructor(headsUp, notification, {cycling = false} = {}) {
        this._headsUp = headsUp;
        this.notification = notification;
        this.critical = notification.urgency === MessageTray.Urgency.CRITICAL;
        this.removed = false;           // the notification is gone: nothing may touch it
        this.expanded = false;
        this.swiping = false;
        this.tracking = false;          // the shade took it along
        this.shownAt = now();
        this.postTime = this.shownAt + POST_DELAY_MS;
        this.earliestRemoval = this.shownAt + MIN_SHOWN_MS;

        this.row = new Row(headsUp, notification, {card: [Color.surfaceContainer, 1], glyph: Color.surfaceContainer});
        this.row.setBaseRoundness(1, 1);
        this.row.setHeadsUp(true);
        this.shadow = new Shadow();
        headsUp.layer.add_child(this.shadow.actor);
        headsUp.layer.add_child(this.row);
        // swiped sideways, the card takes its shadow along
        this.row.connect('notify::translation-x', () => {
            if (this.shadow)
                this.shadow.actor.translation_x = this.row.translation_x;
        });

        // laid out: the expansion p, the reveal f (0 .. 1, linear in time), cut from the top or the bottom
        this.p = 0;
        this.f = 0;
        this._revealTo = 1;
        this._clipTop = false;
        this._interpolator = FAST_OUT_SLOW_IN;
        this._alpha = 1;
        this._dragY = 0;
        // CYCLING_IN starts 8 dp above its place, HEADS_UP_APPEAR above the screen
        this.y = cycling ? PINNED_Y - CYCLING_PADDING : ABOVE_SCREEN_Y;
        this._moveTo(PINNED_Y);

        // dragged up or down (HeadsUpTouchHelper): down pulls the shade, up pushes it away
        this._velocity = new VelocityTracker();
        const pan = new Clutter.PanGesture({
            pan_axis: Clutter.PanAxis.Y,
            max_n_points: 1,
            begin_threshold: px(TOUCH_SLOP),
        });
        pan.connect('recognize', g => this._beginDrag(g));
        pan.connect('pan-update', g => this._updateDrag(g));
        pan.connect('end', () => this._endDrag(false));
        pan.connect('cancel', () => this._endDrag(true));
        this.row.add_action(pan);

        this._kick();
    }

    get height() {
        return this.row.heightFor(this.p);
    }

    get busy() {
        return this.removed || this.swiping || this.tracking || !!this._leaving || !!this._drag;
    }

    // the notification changed: its row again
    update() {
        if (this.removed)
            return;
        this.row.update();
        this._apply();
    }

    forget() {
        this.removed = true;
        this.row.forget();
    }

    // --- motion, one frame at a time while anything moves

    _kick() {
        if (this._anim?.running)
            return;
        this._anim = new Animator(this.row, dt => this._frame(dt), () => {
            this._anim = null;
        });
        this._apply();
    }

    // A new target keeps the spring's speed. It may overshoot once: when it swings back, the bounce goes (critically
    // damped), and a spring that heads away again is put at its end (PhysicsPropertyAnimator's double overshoot)
    _moveTo(target) {
        if (this._ySpring?.target === target)
            return;
        this._ySpring = new Spring({
            stiffness: Y_STIFFNESS, dampingRatio: Y_DAMPING, value: this.y, velocity: this._ySpring?.velocity ?? 0,
            target, threshold: Y_THRESHOLD,
        });
        this._yStart = this.y - target;
        this._kick();
    }

    _stepY(dt) {
        const spring = this._ySpring;
        let left = Math.min(dt, 0.1);
        while (left > 0 && !spring.done) {
            const h = Math.min(left, 0.004);
            spring.step(h);
            left -= h;
            const offset = spring.value - spring.target;
            const backwards = Math.sign(spring.velocity) === Math.sign(this._yStart);
            const overshooting = offset !== 0 && Math.sign(offset) !== Math.sign(this._yStart);
            if (overshooting && backwards && spring.dampingRatio !== 1) {
                spring.dampingRatio = 1;
            } else if (!overshooting && spring.velocity !== 0 && (spring.dampingRatio === 1 || backwards)) {
                spring.value = spring.target;
                spring.velocity = 0;
            }
        }
        this.y = spring.done ? spring.target : spring.value;
        if (spring.done)
            this._ySpring = null;
    }

    _frame(dt) {
        if (this._ySpring)
            this._stepY(dt);
        if (this.f !== this._revealTo) {
            const step = dt * 1000 / REVEAL_MS;
            this.f = this._revealTo > this.f ? Math.min(this.f + step, this._revealTo)
                : Math.max(this.f - step, this._revealTo);
        }
        if (this._expand) {
            const e = this._expand;
            e.t = Math.min(e.t + dt * 1000 / EXPAND_MS, 1);
            this.p = lerp(e.from, e.to, FAST_OUT_SLOW_IN(e.t));
            if (e.t === 1)
                this._expand = null;
        }
        this._apply();
        if (this.f === this._revealTo && this._onRevealed) {
            const done = this._onRevealed;
            this._onRevealed = null;
            done();
        }
        return !!this._ySpring || this.f !== this._revealTo || !!this._expand;
    }

    // The outline's share of the height and the content's alpha follow the reveal through its interpolator
    // (updateAppearRect, updateAppearAnimationAlpha); the card is cut at its bottom, or at its top as it cycles out
    _apply() {
        if (!this.row)
            return;
        const h = this.height;
        const shown = h * this._interpolator(this.f);
        const [top, bottom] = this._clipTop ? [h - shown, h] : [0, shown];
        const y = this.y + this._dragY;
        this.row.set_position(px(SIDE), px(y));
        this.row.setExpansion(this.p);
        this.row.setVisibleRange(top, bottom);
        this.row.setContentAlpha(this._interpolator(clamp((this.f - ALPHA_START) / (1 - ALPHA_START))));
        // one the stack has taken over is gone, and takes no taps from it
        const visible = this._alpha > 0;
        this.row.visible = this.shadow.actor.visible = visible;
        this.row.opacity = Math.round(255 * this._alpha);
        this.shadow.place(SIDE, y + top, bottom - top, this._alpha);
    }

    expandTo(p) {
        this._expand = {from: this.p, to: p, t: 0};
        this._kick();
    }

    // Away: up off the screen, cut from the bottom (HEADS_UP_DISAPPEAR); or, cycling out for one @nextHeight tall,
    // down by what that one is taller and 8 dp, cut from the top (HEADS_UP_CYCLING_OUT)
    leave({cycling = false, nextHeight = 0} = {}, onDone = null) {
        this._leaving = true;
        this._expand = null;
        this.y += this._dragY;
        this._dragY = 0;
        this._interpolator = FAST_OUT_SLOW_IN_REVERSE;
        this._clipTop = cycling;
        this._revealTo = 0;
        this._onRevealed = () => {
            this.destroy();
            onDone?.();
        };
        this._moveTo(cycling ? PINNED_Y + Math.max(nextHeight - this.height, 0) + CYCLING_PADDING : ABOVE_SCREEN_Y);
        this._kick();
    }

    // --- the shade opening takes it along (StackScrollAlgorithm: the tracked heads-up row): from where it is to the
    // stack's first row, and as that row is laid out, as the shade's height goes from its bottom to that row's end
    // (the appear fraction); there it fades as the stack shows

    track(shade) {
        this.tracking = true;
        this._ySpring = null;
        this._expand = null;
        this.y += this._dragY;
        this._dragY = 0;
        this._track = {y: this.y, p: this.p, start: this.y + this.height,
            toP: shade.rowExpanded(this.notification) ? 1 : 0};
    }

    follow(shade) {
        const t = this._track;
        const a = clamp((shade.pulledHeight - t.start) / Math.max(APPEAR_END - t.start, MIN_H));
        this.y = lerp(t.y, shade.notificationsTop, a);
        this.p = lerp(t.p, t.toP, a);
        this.row.setChevron(this.p > 0.5);
        this._alpha = Math.min(this._alpha, 1 - contentAlpha(shade.expansion));
        this._apply();
    }

    // --- drags

    _trackVelocity(gesture) {
        const event = gesture.get_point_event(0);
        const time = event ? event.get_time() : now();
        this._velocity.add(time, 0, gesture.get_centroid_abs().y * SCALE);
    }

    _beginDrag(gesture) {
        if (this.busy)
            return;
        const down = gesture.get_centroid_abs().y > gesture.get_begin_centroid_abs().y;
        this._headsUp.holdTimeout(true);
        if (down) {
            // the shade opens from the heads-up's bottom
            this._drag = 'shade';
            this._headsUp.shade.beginPull(gesture, this.y + this.height);
            return;
        }
        this._drag = 'up';
        this._dragStartY = gesture.get_centroid_abs().y;
        this._velocity.clear();
        this._trackVelocity(gesture);
    }

    _updateDrag(gesture) {
        if (this._drag === 'shade') {
            this._headsUp.shade.updatePull(gesture);
        } else if (this._drag === 'up') {
            this._trackVelocity(gesture);
            this._dragY = Math.min((gesture.get_centroid_abs().y - this._dragStartY) * SCALE, 0);
            this._apply();
        }
    }

    _endDrag(cancelled) {
        const drag = this._drag;
        this._drag = null;
        if (drag === 'shade') {
            this._headsUp.shade.endPull(cancelled);
        } else if (drag === 'up') {
            const v = cancelled ? 0 : this._velocity.velocityY;
            if (!cancelled && (this._dragY < -TOUCH_SLOP || v < -MIN_FLING_VELOCITY)) {
                this._headsUp.pushedAway(this);
                return;
            }
            this.y += this._dragY;
            this._dragY = 0;
            this._moveTo(PINNED_Y);
        }
        this._headsUp.holdTimeout(false);
    }

    destroy() {
        this._anim?.stop();
        this._anim = null;
        this.shadow?.destroy();
        this.shadow = null;
        this.row?.destroy();
        this.row = null;
    }
}

// --- the heads-up manager

export class HeadsUp {
    constructor(shade) {
        this.shade = shade;
        this.layer = new St.Widget({
            name: 'gnomePhoneHeadsUp',
            visible: false,
            width: px(SCREEN_W),
            height: px(SCREEN_H),
        });
        this._queue = [];               // waiting, the most urgent first
        this._entry = null;             // the one pinned
        this._tracked = null;           // ... the one the shade took along
        this._leaving = new Set();
        this._snoozed = new Map();      // an app's key -> until when (ms) none of its notifications peek
        this._timeoutId = 0;

        const tray = Main.messageTray;
        this._bannerWasBlocked = tray.bannerBlocked;
        tray.bannerBlocked = true;
        tray.connectObject(
            'source-added', (_tray, source) => this._addSource(source),
            'source-removed', (_tray, source) => source.disconnectObject(this),
            this);
        for (const source of tray.getSources())
            this._addSource(source);
        shade.connectObject(
            'notify::visible', () => this._shadeVisibleChanged(),
            'expansion-changed', () => this._tracked?.follow(shade),
            this);
        // the lock screen (it locks as the screen goes off, so the AOD too): nothing peeks, and what was up goes
        Main.sessionMode.connectObject('updated', () => {
            if (Main.sessionMode.isLocked)
                this._releaseAll();
        }, this);
    }

    _addSource(source) {
        source.connectObject(
            'notification-request-banner', (_s, n) => this._onRequestBanner(n),
            'notification-removed', (_s, n) => this._onRemoved(n),
            this);
    }

    // MessageTray's rules for a banner, and SystemUI's for a peek: not over the shade, nor locked, nor snoozed
    _onRequestBanner(notification) {
        if (notification.acknowledged || notification.urgency === MessageTray.Urgency.LOW)
            return;
        const critical = notification.urgency === MessageTray.Urgency.CRITICAL;
        if (!notification.source?.policy.showBanners && !critical)
            return;
        if (!this._canPeek() || (this._isSnoozed(notification) && !critical))
            return;
        if (this._entry?.notification === notification) {
            // alerting again: shown from now (updateEntry with updatePostTime)
            this._entry.update();
            this._entry.postTime = Math.max(this._entry.postTime, now());
            this._alert(notification);
            this._reschedule();
            return;
        }
        if (this._queue.includes(notification))
            return;
        this._queue.push(notification);
        this._queue.sort((a, b) => b.urgency - a.urgency);
        if (this._entry)
            this._reschedule();
        else
            this._showNext(false);
    }

    _canPeek() {
        return !this.shade.visible && !Main.sessionMode.isLocked &&
            Main.sessionMode.hasNotifications;
    }

    _snoozeKey(notification) {
        const source = notification.source;
        return source?.app?.get_id() ?? source?.title ?? '';
    }

    _isSnoozed(notification) {
        const until = this._snoozed.get(this._snoozeKey(notification));
        return until !== undefined && until > now();
    }

    // what showing a banner does to the notification (MessageTray._updateShowingNotification)
    _alert(notification) {
        notification.acknowledged = true;
        notification.playSound();
    }

    _showNext(cycling) {
        const notification = this._queue.shift();
        if (!notification)
            return null;
        const entry = new Entry(this, notification, {cycling});
        this._entry = entry;
        notification.connectObject(
            'notify', (_n, pspec) => {
                if (pspec.name !== 'acknowledged')
                    this._queueUpdate(entry);
            },
            'action-added', () => this._queueUpdate(entry),
            'action-removed', () => this._queueUpdate(entry),
            this);
        this._alert(notification);
        this._sync();
        this._reschedule();
        return entry;
    }

    _queueUpdate(entry) {
        if (entry.updateId)
            return;
        entry.updateId = GLib.idle_add(GLib.PRIORITY_DEFAULT, () => {
            entry.updateId = 0;
            if (entry.row)
                entry.update();
            return GLib.SOURCE_REMOVE;
        });
    }

    // --- how long it stays: the decay from its post time, cut short for one waiting; never under the minimum. An
    // expanded one, a critical one, one being touched stays; one whose notification has gone goes at the minimum

    _reschedule() {
        this._clearTimeout();
        const e = this._entry;
        if (!e || (!e.removed && (e.expanded || e.critical || this._held)))
            return;
        let duration = DECAY_MS;
        const next = this._queue[0];
        if (next?.urgency > e.notification.urgency)
            duration = next.urgency === MessageTray.Urgency.CRITICAL ? 0 : NEXT_HIGHER_MS;
        else if (next?.urgency === e.notification.urgency)
            duration = NEXT_SAME_MS;
        let at = duration === 0 ? now() : Math.max(e.postTime + duration, e.earliestRemoval);
        if (e.removed)
            at = e.earliestRemoval;
        this._timeoutId = GLib.timeout_add(GLib.PRIORITY_DEFAULT, Math.max(Math.round(at - now()), 0), () => {
            this._timeoutId = 0;
            this._unpin();
            return GLib.SOURCE_REMOVE;
        });
    }

    _clearTimeout() {
        if (this._timeoutId)
            GLib.source_remove(this._timeoutId);
        this._timeoutId = 0;
    }

    holdTimeout(held) {
        this._held = held;
        if (held)
            this._clearTimeout();
        else
            this._reschedule();
    }

    // The pinned one goes: the next one cycles in as it cycles out; or it goes up off the screen, or (swiped out)
    // it has gone already
    _unpin({dismissed = false} = {}) {
        const e = this._entry;
        if (!e)
            return;
        this._entry = null;
        this._held = false;
        this._clearTimeout();
        if (!e.removed)
            e.notification.disconnectObject(this);
        const next = this._canPeek() ? this._showNext(!dismissed) : null;
        if (dismissed) {
            e.destroy();
        } else {
            this._leaving.add(e);
            e.leave({cycling: !!next, nextHeight: next?.height ?? 0}, () => this._left(e));
        }
        this._sync();
    }

    // a transient notification is done with once its banner has been (MessageTray._hideNotificationCompleted)
    _left(e) {
        this._leaving.delete(e);
        if (!e.removed) {
            e.notification.disconnectObject(this);
            if (e.notification.isTransient)
                e.notification.destroy(MessageTray.NotificationDestroyedReason.EXPIRED);
        }
        this._sync();
    }

    // Gone from the tray (it is disposed next): out of the queue, or, pinned, away once it has been up long enough
    // (canRemoveImmediately)
    _onRemoved(notification) {
        const i = this._queue.indexOf(notification);
        if (i !== -1)
            this._queue.splice(i, 1);
        for (const e of [this._entry, this._tracked, ...this._leaving]) {
            if (e?.notification === notification && !e.removed) {
                notification.disconnectObject(this);
                e.forget();
            }
        }
        this._reschedule();
    }

    // all of them, at once, with no animation (HeadsUpManager.releaseAllImmediately): the lock screen covers them,
    // and one left pinned under it would still be there, its time run out, after the unlock
    _releaseAll() {
        this._queue = [];
        this._clearTimeout();
        this._held = false;
        for (const e of [this._entry, this._tracked, ...this._leaving]) {
            if (!e)
                continue;
            if (e.updateId)
                GLib.source_remove(e.updateId);
            e.updateId = 0;
            this._leaving.delete(e);
            e.destroy();
            this._left(e);
        }
        this._entry = this._tracked = null;
        this._sync();
    }

    _sync() {
        this.layer.visible = !!this._entry || !!this._tracked || this._leaving.size > 0;
    }

    // --- the row's taps and swipes (it takes these from the stack it is in)

    _pinned(row) {
        const e = this._entry;
        return e?.row === row && !e.busy ? e : null;
    }

    activate(row) {
        const e = this._pinned(row);
        if (!e || now() < e.postTime)
            return;
        const n = e.notification;
        this._unpin();
        n.activate();
    }

    activateAction(row, action) {
        const e = this._pinned(row);
        if (!e)
            return;
        this._unpin();
        action.activate();
    }

    // expanded it stays; collapsed, it goes no sooner than the minimum from now (HeadsUpEntry.setExpanded)
    toggleExpanded(row) {
        const e = this._pinned(row);
        if (!e)
            return;
        e.expanded = !e.expanded;
        row.setChevron(e.expanded);
        e.expandTo(e.expanded ? 1 : 0);
        if (!e.expanded)
            e.earliestRemoval = now() + MIN_SHOWN_MS;
        this._reschedule();
    }

    onSwipeBegin(row) {
        if (this._entry?.row !== row)
            return;
        this._entry.swiping = true;
        this.holdTimeout(true);
    }

    onSwipeEnd(row) {
        if (this._entry?.row !== row)
            return;
        this._entry.swiping = false;
        this.holdTimeout(false);
    }

    onDismissed(row) {
        const e = this._entry;
        if (e?.row !== row)
            return;
        const n = e.removed ? null : e.notification;
        this._unpin({dismissed: true});
        n?.destroy(MessageTray.NotificationDestroyedReason.DISMISSED);
    }

    // pushed up: away, and its app peeks no more for a while (the panel collapsing snoozes it)
    pushedAway(e) {
        if (e !== this._entry)
            return;
        this._snoozed.set(this._snoozeKey(e.notification), now() + SNOOZE_MS);
        this._unpin();
    }

    // --- the shade: what is pinned goes along into it; what waits is in it already. Shut again, it is over

    _shadeVisibleChanged() {
        if (this.shade.visible) {
            this._queue = [];
            const e = this._entry;
            if (!e)
                return;
            this._entry = null;
            this._held = false;
            this._clearTimeout();
            e.track(this.shade);
            this._tracked = e;
            this._tracked.follow(this.shade);
        } else if (this._tracked) {
            const e = this._tracked;
            this._tracked = null;
            this._leaving.add(e);
            e.leave({}, () => this._left(e));
        }
        this._sync();
    }

    destroy() {
        this._clearTimeout();
        Main.messageTray.disconnectObject(this);
        for (const source of Main.messageTray.getSources())
            source.disconnectObject(this);
        this.shade.disconnectObject(this);
        Main.sessionMode.disconnectObject(this);
        for (const e of [this._entry, this._tracked, ...this._leaving]) {
            if (!e)
                continue;
            if (e.updateId)
                GLib.source_remove(e.updateId);
            if (!e.removed)
                e.notification.disconnectObject(this);
            e.destroy();
        }
        this._entry = this._tracked = null;
        this._leaving.clear();
        Main.messageTray.bannerBlocked = this._bannerWasBlocked;
        this.layer.destroy();
    }
}
