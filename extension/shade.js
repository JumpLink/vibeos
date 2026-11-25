// The notification shade. Pulled from the status bar it tracks the finger over its layout's travel (shadelayout.js; a
// phone's 2190 px); on release it flings open (motion.js's fling) with an overshoot that springs back, or flings closed.
// Dragged down once open, the QS expands; dragged up, the shade closes.
//
// A phone's: behind it the screen blurs, dims and zooms out (depth controller) and the status bar fades; in front, the
// first two rows of tiles and the notification panel, whose top (stackY) comes down from the status bar and is also
// the edge the quick settings are cut at, so the tiles are uncovered as it passes them. The header and the
// notifications fade in at the end. The second pull takes the panel down off the screen as it fades.
// A tablet's: an opaque scrim comes down as a sheet, the quick settings and the panel with it (_applyTablet).
import Clutter from 'gi://Clutter';
import GLib from 'gi://GLib';
import GObject from 'gi://GObject';
import Meta from 'gi://Meta';
import Shell from 'gi://Shell';
import St from 'gi://St';

import * as Main from 'resource:///org/gnome/shell/ui/main.js';

import {animate, Animator, clamp, FAST_OUT_SLOW_IN, fling, lerp, MIN_FLING_VELOCITY, Spring,
    VelocityTracker} from './motion.js';
import {NotificationStack} from './notifications.js';
import {Color, rgba, SHADE_PANEL, TabletShade} from './palette.js';
import {clipAbove, QSPanel} from './qs.js';
import {wholeFrames} from './redraws.js';
import {shadeLayout} from './shadelayout.js';
import {BAR_HEIGHT} from './statusbar.js';
import {geometry, HEIGHT, px, SCALE, SCREEN_H} from './units.js';

// --- the shade's geometry and motion, screen px
let H = 2190;                           // the height the finger tracks over (the layout's travel)
let QS_TRAVEL = 1636;                   // the second pull's way, q 0 -> 1 (the layout's qsTravel)
const OVERSHOOT = 48;                   // at 4500 px/s and above; 20 % of it at rest
const OVERSHOOT_FULL_VELOCITY = 4500;
const SPRING_BACK_MS = 400;
const OPEN_NO_VELOCITY_MS = 350;
const QS_OPPOSITE_MS = 350;
const TOUCH_SLOP = 24;
const CUTOFF = 0.5;
const STACK_START_FRACTION = 0.5;
const BLUR_MIN = 1;
const BLUR_MAX = 102;                   // the blur radius with the shade open
const BLUR_INTERACTION = 0.8;           // the rest is the spring
const BLUR_SPRING_STIFFNESS = 200;
const BLUR_VELOCITY_SCALE = 100;
const BLUR_VELOCITY_MAX = 3000;
const ZOOM_LAUNCHER = 0.05;
const ZOOM_APP = 0.025;
const MIRROR_MS = 150;                  // the shade fading away around the brightness slider
const TABLET_FADE_FROM = 0.15;          // a tablet's quick settings and panel fade in over this part of the pull...
const TABLET_FADE_TO = 0.7;             // ... as they come down

// the content's alpha over the expansion f: 0 below f 0.417, 0.164 at 0.5, 0.837 at 0.7
export function contentAlpha(f) {
    return ease(clamp((f - 0.3) / 0.7));
}

// ... and the notifications scrim's curve, which also drives the blur: 1 from f 0.5
export function notificationScrimAlpha(f) {
    return ease(clamp(f / 0.5));
}

function ease(m) {
    const x = 1.2 * m - 0.2;
    return x <= 0 ? 0 : 1 - 0.5 * (1 - Math.cos(Math.PI * (1 - x) ** 2));
}

const blurOfRatio = r => r === 0 ? 0 : lerp(BLUR_MIN, BLUR_MAX, r);
const ratioOfBlur = b => b === 0 ? 0 : clamp((b - BLUR_MIN) / (BLUR_MAX - BLUR_MIN));

export const Shade = GObject.registerClass({
    Signals: {'expansion-changed': {}},
}, class Shade extends St.Widget {
    constructor(state, statusBar) {
        const monitor = Main.layoutManager.primaryMonitor;
        super({
            name: 'vibeosShade',
            reactive: true,
            visible: false,
            x: monitor.x,
            y: monitor.y,
            width: monitor.width,
            height: monitor.height,
        });
        this._statusBar = statusBar;

        // behind: the blurred screen, then the shade panel tint (α 0.723 · f); drawn in whole frames while it shows
        // (redraws.js)
        this._blurLayer = new St.Widget({width: this.width, height: this.height});
        this._blur = new Shell.BlurEffect({mode: Shell.BlurMode.BACKGROUND, radius: 0, brightness: 1});
        this._blur.enabled = false;
        this._blurLayer.add_effect(this._blur);
        this.add_child(this._blurLayer);
        this._tint = new St.Widget({
            width: this.width,
            height: this.height,
            style: `background-color: ${rgba(SHADE_PANEL[0].map(Math.round), SHADE_PANEL[1])};`,
            opacity: 0,
        });
        this.add_child(this._tint);

        this._qs = new QSPanel(state, this);
        this.add_child(this._qs);

        // the notification panel: from stackY down, rounded at the top (shaped and coloured by the layout)
        this._scrim = new St.Widget({width: this.width, height: px(SCREEN_H), opacity: 0});
        this.add_child(this._scrim);

        this._stack = new NotificationStack(this);
        this.add_child(this._stack);

        // a turned screen: the shade and its full-screen layers cover it as it is now
        geometry.connectObject('changed', () => this._fit(), this);
        this._layout = null;            // shadelayout.js's

        // expansion: f (0..1), the overshoot in px, the QS expansion q (0..1)
        this._f = 0;
        this._over = 0;
        this._q = 0;
        this._mirror = 0;
        this._stackY = 0;
        this._tracking = false;
        this._dragToQs = false;         // the drag is a two-finger pull
        this._qsAfterOpen = false;      // ... let go before the quick settings: they open after the shade
        this._velocity = new VelocityTracker();

        // the blur's spring (the part of the blur BLUR_INTERACTION leaves it) and its bookkeeping
        this._blurSpring = new Spring({stiffness: BLUR_SPRING_STIFFNESS, dampingRatio: 1, threshold: 0.5});
        this._isBlurred = false;
        this._isClosed = true;
        this._isOpen = false;
        this._prevDepthF = 0;
        this._prevDepthTime = -1;

        // pull from the status bar
        this._pull = new Clutter.PanGesture({
            pan_axis: Clutter.PanAxis.Y,
            max_n_points: 1,
            begin_threshold: px(TOUCH_SLOP),
        });
        // not while locked: an app's window over the lock screen (whenlocked.js) shows the status bar, not the shade
        this._pull.connect('may-recognize', () => !Main.sessionMode.isLocked);
        this._pull.connect('recognize', g => this._beginDrag(g, false));
        this._pull.connect('pan-update', g => this._updateDrag(g));
        this._pull.connect('end', () => this._endDrag(false));
        this._pull.connect('cancel', () => this._endDrag(true));
        statusBar.add_action(this._pull);
        // a phone's: two fingers pulled from the status bar open the shade straight into all the quick settings
        this._pullQs = new Clutter.PanGesture({
            pan_axis: Clutter.PanAxis.Y,
            min_n_points: 2,
            max_n_points: 2,
            begin_threshold: px(TOUCH_SLOP),
        });
        this._pullQs.connect('may-recognize', () => !Main.sessionMode.isLocked && this._layout.phone &&
            QS_TRAVEL > 0);
        this._pullQs.connect('recognize', g => this._beginDrag(g, false, null, true));
        this._pullQs.connect('pan-update', g => this._updateDrag(g));
        this._pullQs.connect('end', () => this._endDrag(false));
        this._pullQs.connect('cancel', () => this._endDrag(true));
        statusBar.add_action(this._pullQs);
        // at shutdown the status bar may go first
        statusBar.connectObject(
            'destroy', () => {
                this._statusBar = null;
            },
            'content-changed', () => this._barChanged(),
            this);

        // drags on the open shade: down expands the QS, up collapses QS and then the shade
        this._pan = new Clutter.PanGesture({
            pan_axis: Clutter.PanAxis.Y,
            max_n_points: 1,
            begin_threshold: px(TOUCH_SLOP),
        });
        this._pan.connect('recognize', g => this._beginDrag(g, true));
        this._pan.connect('pan-update', g => this._updateDrag(g));
        this._pan.connect('end', () => this._endDrag(false));
        this._pan.connect('cancel', () => this._endDrag(true));
        this.add_action(this._pan);

        // a tap on nothing under the stack closes it
        this._tap = new Clutter.ClickGesture();
        this._tap.connect('recognize', g => {
            const point = g.get_point_coords(0);
            const target = global.stage.get_actor_at_pos(Clutter.PickMode.REACTIVE,
                this.x + point.x, this.y + point.y);
            // on the scrim anywhere under the header
            if (target === this && point.y * SCALE > this._layout.header.height)
                this.close();
        });
        this.add_action(this._tap);

        this.connect('destroy', () => this._onDestroy());
        this._fitLayout();
    }

    get expansion() {
        return this._f;
    }

    get isOpen() {
        return this._f > 0;
    }

    // how far it is pulled, screen px
    get pulledHeight() {
        return this._f * H;
    }

    // where the stack's first row is, screen px, and whether it shows @notification expanded
    get notificationsTop() {
        return this._stackY + this._layout.rows.top;
    }

    rowExpanded(notification) {
        return this._stack.rowExpanded(notification);
    }

    // A pull from a heads-up notification: the shade starts as far open as the notification reaches down
    // (@startHeight), and the drag goes on from there
    beginPull(gesture, startHeight) {
        this._beginDrag(gesture, false, startHeight);
    }

    updatePull(gesture) {
        this._updateDrag(gesture);
    }

    endPull(cancelled) {
        this._endDrag(cancelled);
    }

    // --- dragging

    // the shade's position along one axis: f · H, then the QS's q · QS_TRAVEL (px)
    get _position() {
        return this._f * H + this._q * QS_TRAVEL;
    }

    _setPosition(p, maxP) {
        p = clamp(p, 0, maxP);
        this._f = Math.min(p, H) / H;
        this._q = QS_TRAVEL > 0 ? Math.max(p - H, 0) / QS_TRAVEL : 0;
        this._queueApply();
    }

    // @toQs: a two-finger pull, which runs through the first pull and the second in one way of the finger
    _beginDrag(gesture, open, startP = null, toQs = false) {
        this._qsAfterOpen = false;
        // on the open shade a drag on a long stack scrolls it first (NPVC: canCollapsePanelOnTouch, QS expands
        // only from the stack's top)
        if (open && this._f === 1 && this._q === 0 && !this._anim) {
            const begin = gesture.get_begin_centroid_abs().y;
            const up = gesture.get_centroid_abs().y < begin;
            const x = this._layout ? (gesture.get_begin_centroid_abs().x - this.x) * SCALE : null;
            if (this._stack.canScroll(begin * SCALE, up, x)) {
                this._scrolling = true;
                this._dragStartY = gesture.get_centroid_abs().y;
                this._velocity.clear();
                this._trackVelocity(gesture);
                this._stack.beginScroll();
                return;
            }
        }
        this._stack.stopScroll();
        this._stopAnimation();
        this._tracking = true;
        this._dragOpen = open;
        this._dragToQs = toQs;
        this._dragStartP = startP ?? this._position;
        this._dragStartY = gesture.get_centroid_abs().y;
        this._velocity.clear();
        this._trackVelocity(gesture);
        if (!open) {
            this.visible = true;
            this._queueApply();
        }
    }

    _trackVelocity(gesture) {
        const event = gesture.get_point_event(0);
        const time = event ? event.get_time() : GLib.get_monotonic_time() / 1000;
        this._velocity.add(time, 0, gesture.get_centroid_abs().y * SCALE);
    }

    _updateDrag(gesture) {
        if (this._scrolling) {
            this._trackVelocity(gesture);
            this._stack.dragScroll((gesture.get_centroid_abs().y - this._dragStartY) * SCALE);
            return;
        }
        if (!this._tracking)
            return;
        this._trackVelocity(gesture);
        const dy = (gesture.get_centroid_abs().y - this._dragStartY) * SCALE;
        // a pull from the status bar stops at the QQS; on the open shade the drag reaches the full QS, as does a
        // two-finger pull over the same way as a one-finger one
        if (this._dragToQs)
            this._setPosition(this._dragStartP + dy * (H + QS_TRAVEL) / H, H + QS_TRAVEL);
        else
            this._setPosition(this._dragStartP + dy, this._dragOpen ? H + QS_TRAVEL : H);
    }

    _endDrag(cancelled) {
        if (this._scrolling) {
            this._scrolling = false;
            if (!cancelled)
                this._stack.flingScroll(this._velocity.velocityY);
            return;
        }
        if (!this._tracking)
            return;
        this._tracking = false;
        const v = cancelled ? 0 : this._velocity.velocityY;
        if (this._q > 0) {
            const expand = Math.abs(v) >= MIN_FLING_VELOCITY ? v > 0 : this._q > CUTOFF;
            this._flingQs(expand, v);
        } else {
            const expand = Math.abs(v) >= MIN_FLING_VELOCITY ? v > 0 : this._f > CUTOFF;
            // a two-finger pull let go short of the quick settings opens them once the shade is open
            this._qsAfterOpen = expand && this._dragToQs;
            this._flingPanel(expand, v);
        }
    }

    // --- animations

    _stopAnimation() {
        this._anim?.stop();
        this._anim = null;
    }

    // NPVC.flingToHeight: open to H with an overshoot that depends on the velocity, or close
    _flingPanel(expand, v) {
        this._stopAnimation();
        const cur = this._f * H;
        const over0 = this._over;
        if (expand) {
            const addOverscroll = over0 === 0 && v >= 0;
            const amount = addOverscroll ? lerp(0.2, 1, clamp(v / OVERSHOOT_FULL_VELOCITY)) : 0;
            const springBack = addOverscroll || over0 !== 0;
            this._grab();
            if (cur === H && over0 === 0 && !addOverscroll) {
                this._settled();
                return;
            }
            let {duration, interpolator} = fling(cur, H + amount * OVERSHOOT, v,
                {maxLength: 0.4, speedUpFactor: 0, maxDistance: SCREEN_H});
            if (v === 0)
                duration = OPEN_NO_VELOCITY_MS;
            this._anim = animate(this, {
                from: cur, to: H, duration, interpolator,
                onUpdate: (h, fraction) => {
                    if (amount > 0)
                        this._over = lerp(over0, amount * OVERSHOOT, FAST_OUT_SLOW_IN(fraction));
                    this._f = h / H;
                    this._queueApply();
                },
                onDone: () => {
                    this._anim = null;
                    if (springBack)
                        this._springBack();
                    else
                        this._settled();
                },
            });
        } else {
            const {duration, interpolator} = fling(cur, 0, v,
                {maxLength: 0.6, speedUpFactor: 0.6, maxDistance: SCREEN_H});
            this._anim = animate(this, {
                from: cur, to: 0, duration, interpolator,
                onUpdate: (h, fraction) => {
                    if (over0 !== 0)
                        this._over = lerp(over0, 0, FAST_OUT_SLOW_IN(fraction));
                    this._f = h / H;
                    this._queueApply();
                },
                onDone: () => {
                    this._anim = null;
                    this._closed();
                },
            });
        }
    }

    // the overshoot back to 0 over 400 ms
    _springBack() {
        const over0 = this._over;
        this._anim = animate(this, {
            from: over0, to: 0, duration: SPRING_BACK_MS, interpolator: FAST_OUT_SLOW_IN,
            onUpdate: v => {
                this._over = v;
                this._queueApply();
            },
            onDone: () => {
                this._anim = null;
                this._settled();
            },
        });
    }

    // QS open or shut on the release: a fling capped at 0.4 s; against the velocity, 350 ms
    _flingQs(expand, v) {
        this._stopAnimation();
        const cur = this._q * QS_TRAVEL, target = expand ? QS_TRAVEL : 0;
        const opposite = (v > 0 && !expand) || (v < 0 && expand);
        if (opposite)
            v = 0;
        let {duration, interpolator} = fling(cur, target, v, {maxLength: 0.4});
        if (opposite)
            duration = QS_OPPOSITE_MS;
        this._anim = animate(this, {
            from: cur, to: target, duration, interpolator,
            onUpdate: p => {
                this._f = 1;
                this._q = p / QS_TRAVEL;
                this._queueApply();
            },
            onDone: () => {
                this._anim = null;
                this._settled();
            },
        });
    }

    // open from code (no velocity: 350 ms)
    open() {
        if (this._tracking)
            return;
        this.visible = true;
        this._flingPanel(true, 0);
    }

    // close from code: the QS and the shade together, the closing curve at no velocity
    close() {
        if (this._tracking || this._f === 0)
            return;
        this._stopAnimation();
        const p0 = this._position, over0 = this._over;
        const {duration, interpolator} = fling(p0, 0, 0, {maxLength: 0.6, speedUpFactor: 0.6, maxDistance: SCREEN_H});
        this._anim = animate(this, {
            from: p0, to: 0, duration, interpolator,
            onUpdate: (p, fraction) => {
                if (over0 !== 0)
                    this._over = lerp(over0, 0, FAST_OUT_SLOW_IN(fraction));
                this._setPosition(p, H + QS_TRAVEL);
            },
            onDone: () => {
                this._anim = null;
                this._closed();
            },
        });
    }

    toggle() {
        if (this._f > 0)
            this.close();
        else
            this.open();
    }

    _settled() {
        if (this._qsAfterOpen) {
            this._qsAfterOpen = false;
            this._flingQs(true, 0);
            return;
        }
        this._queueApply();
    }

    // the stack's "No notifications" came or went: the panel goes or comes with it
    stackEmptyChanged() {
        this._queueApply();
    }

    // a tablet's panel ends under the notifications: its height as the stack lays them out now (screen px)
    stackPanelChanged(height) {
        if (this._layout && !this._layout.phone)
            this._scrim.height = px(height);
    }

    _closed() {
        this._qsAfterOpen = false;
        this._f = 0;
        this._q = 0;
        this._over = 0;
        this._ungrab();
        this._queueApply();
    }

    // --- modal while open: keys come here (Escape closes), the screen below takes no input

    _grab() {
        if (this._grabbed)
            return;
        this._grabbed = Main.pushModal(this, {actionMode: Shell.ActionMode.POPUP});
        this.grab_key_focus();
    }

    _ungrab() {
        if (!this._grabbed)
            return;
        Main.popModal(this._grabbed);
        this._grabbed = null;
    }

    // St gives the actor with key focus the :focus pseudo class, and that restyles every actor inside it: each icon
    // in the shade drew its picture again as the grab was taken, and again as it was given back. Nothing here is
    // styled by focus.
    vfunc_key_focus_in() {
    }

    vfunc_key_focus_out() {
    }

    vfunc_key_press_event(event) {
        const key = event.get_key_symbol();
        if (key === Clutter.KEY_Escape || key === Clutter.KEY_Back) {
            this.close();
            return Clutter.EVENT_STOP;
        }
        return Clutter.EVENT_PROPAGATE;
    }

    // --- the brightness mirror: while the slider is dragged, everything but the slider fades and the blur goes

    setBrightnessDragging(dragging) {
        this._mirrorAnim?.stop();
        const from = this._mirror, to = dragging ? 1 : 0;
        this._mirrorAnim = animate(this, {
            from, to, duration: MIRROR_MS * Math.abs(to - from), interpolator: FAST_OUT_SLOW_IN,
            onUpdate: v => {
                this._mirror = v;
                this._queueApply();
            },
            onDone: () => {
                this._mirrorAnim = null;
            },
        });
    }

    onTileActivated(tile, longPress) {
        if (longPress)
            this.close();
    }

    // --- per frame

    _queueApply() {
        if (this._applyId)
            return;
        this._applyId = global.compositor.get_laters().add(Meta.LaterType.BEFORE_REDRAW, () => {
            this._applyId = 0;
            this._apply();
            return GLib.SOURCE_REMOVE;
        });
    }

    _apply() {
        if (this._layout.phone)
            this._applyPhone();
        else
            this._applyTablet();
    }

    // A phone's: the panel's top comes down from the status bar's bottom to the layout's, then the second pull takes it
    // down off the screen. The quick settings are cut at it, its cards fade in with the header at the end of the pull
    // (contentAlpha), and the panel itself shows from the start, with the blur and the dim behind.
    _applyPhone() {
        const f = this._f, q = this._q, over = this._over, m = this._mirror, layout = this._layout;
        this.visible = f > 0 || this._tracking;
        this._updateDepth(f);

        const {panel, header} = layout;
        const top = lerp(header.height, panel.top + over, f) + q * (HEIGHT - panel.top);
        this._stackY = top;
        const content = contentAlpha(f);

        this._tint.opacity = Math.round(255 * f * (1 - m));
        this._qs.layoutFor({q, contentAlpha: content, mirror: m});
        this._qs.set_clip(0, 0, this.width, Math.max(px(top), 0));
        // the status bar fades as the screen behind blurs; the header takes its line
        if (this._statusBar) {
            this._statusBar.opacity = Math.round(255 * (1 - notificationScrimAlpha(f) * (1 - m)));
            this._statusBar.setLineHidden(content > 0);
        }

        const away = 1 - clamp(q);
        this._scrim.set_position(px(panel.x), px(top));
        this._scrim.opacity = Math.round(255 * f * (1 - m) * away * (1 - this._stack.emptyAlpha));
        this._scrim.visible = this._scrim.opacity > 0;
        this._stack.layoutFor({stackY: top, ratio: lerp(STACK_START_FRACTION, 1, f), alpha: content * (1 - m) * away,
            clipTop: header.height});

        this._applyBlur();
        this.emit('expansion-changed');
    }

    // the spring part of the blur
    _updateDepth(f) {
        const now = GLib.get_monotonic_time() / 1e6;
        const dt = this._prevDepthTime < 0 ? 1 : clamp(now - this._prevDepthTime, 0.00001, 1);
        const diff = f - this._prevDepthF;
        if (diff === 0 && this._prevTracking === this._tracking) {
            this._prevDepthTime = now;
            return;
        }
        const direction = Math.sign(diff);
        const velocity = clamp(BLUR_VELOCITY_SCALE * diff / dt, -BLUR_VELOCITY_MAX, BLUR_VELOCITY_MAX);
        if (f > 0) {
            if (this._isClosed) {
                this._animateBlur(true, velocity);
                this._isClosed = false;
            }
            if (this._tracking && !this._isBlurred)
                this._animateBlur(true, 0);
            if (!this._tracking && direction < 0 && this._isBlurred)
                this._animateBlur(false, velocity);
            if (f === 1) {
                if (!this._isOpen) {
                    this._isOpen = true;
                    if (!this._isBlurred)
                        this._animateBlur(true, velocity);
                }
            } else {
                this._isOpen = false;
            }
        } else if (!this._isClosed) {
            this._isClosed = true;
            if (this._isBlurred)
                this._animateBlur(false, velocity);
        }
        this._prevDepthF = f;
        this._prevDepthTime = now;
        this._prevTracking = this._tracking;
    }

    _animateBlur(blur, velocity) {
        this._isBlurred = blur;
        this._blurSpring.target = blur ? BLUR_MAX : 0;
        this._blurSpring.velocity = velocity;
        if (this._blurAnim)
            return;
        this._blurAnim = new Animator(this, dt => {
            let left = Math.min(dt, 0.1);
            while (left > 0) {
                const h = Math.min(left, 0.004);
                this._blurSpring.step(h);
                left -= h;
            }
            this._queueApply();
            return !this._blurSpring.done;
        }, () => {
            this._blurAnim = null;
        });
    }

    _applyBlur() {
        const f = this._f;
        const animationRadius = clamp(this._blurSpring.value, BLUR_MIN, BLUR_MAX);
        const expansionRadius = blurOfRatio(notificationScrimAlpha(f));
        let radius = expansionRadius * BLUR_INTERACTION + animationRadius * (1 - BLUR_INTERACTION);
        radius = Math.max(radius, blurOfRatio(notificationScrimAlpha(this._q) * f));
        radius *= 1 - this._mirror;
        const blur = f > 0 ? Math.trunc(radius) : 0;

        this._blur.enabled = blur > 0;
        wholeFrames(this, blur > 0);
        if (blur > 0 && this._blur.radius !== blur)
            this._blur.radius = blur;

        // the screen behind zooms out with the blur: 5 % over the launcher, 2.5 % over an app
        const zoom = 1 - ratioOfBlur(blur) * (this._statusBar?.overApp ? ZOOM_APP : ZOOM_LAUNCHER);
        const group = global.window_group;
        if (zoom === 1) {
            group.set_scale(1, 1);
            group.set_translation(0, 0, 0);
        } else {
            // about the panel's centre: a stock shell's window group has no size of its own, so a relative pivot
            // would be its top left corner
            const m = Main.layoutManager.primaryMonitor;
            group.set_pivot_point(0, 0);
            group.set_scale(zoom, zoom);
            group.set_translation((m.x + m.width / 2) * (1 - zoom), (m.y + m.height / 2) * (1 - zoom), 0);
        }
    }

    // A tablet's shade: the screen behind neither blurs nor zooms. An opaque scrim comes down from the top as a sheet
    // whose edge follows the pull. The quick settings and the notification panel come down with the finger, from a
    // whole pull above their places, out from under the header, fading in; a fast fling takes them a little past, they
    // spring back, and closing they go back up the same way. Upright the second pull takes the panel on down off the
    // screen as it fades. The panel ends under its notifications' footer, or is a short one around "No notifications",
    // and grows to the screen's bottom at most (the stack's panelHeight), translucent over the scrim as a phone's is.
    _applyTablet() {
        const f = this._f, q = this._q, m = this._mirror, layout = this._layout;
        this.visible = f > 0 || this._tracking;
        const content = FAST_OUT_SLOW_IN(clamp((f - TABLET_FADE_FROM) / (TABLET_FADE_TO - TABLET_FADE_FROM)));
        const slide = (f - 1) * H + this._over;

        // The scrim is a sheet that comes down from the top as the phone's panel does, not the whole screen going
        // grey at once: the shade shows only above its edge, which follows the pull (and a fling's overshoot) from
        // the header's bottom (the clock and the icons are there from the start) to the screen's
        const {panel, header} = layout;
        const edge = Math.max(f * this.height + px(this._over), px(header.height));
        this.set_clip(0, 0, this.width, Math.min(edge, this.height));
        const scrim = notificationScrimAlpha(f);
        this._tint.opacity = Math.round(255 * (1 - m));
        this._qs.layoutFor({q, contentAlpha: content, mirror: m, slide});
        // the header draws the status bar's clock and icons at their places while the shade shows, the bar's own
        // hidden under them; the rest of the bar fades as the scrim comes
        if (this._statusBar) {
            this._statusBar.opacity = Math.round(255 * (1 - scrim * (1 - m)));
            this._statusBar.setLineHidden(this.visible);
        }

        const top = panel.top + slide + q * (HEIGHT - panel.top);
        this._stackY = top;
        const alpha = content * (1 - m) * (1 - q);
        this._scrim.set_position(px(panel.x), px(top));
        this._scrim.opacity = Math.round(255 * alpha);
        this._scrim.visible = this._scrim.opacity > 0;
        clipAbove(this._scrim, px(header.height));
        this._stack.layoutFor({stackY: top, ratio: lerp(STACK_START_FRACTION, 1, f), alpha,
            endShift: panel.roundBottom ? slide : 0, clipTop: header.height});
        this.emit('expansion-changed');
    }

    _fit() {
        const monitor = Main.layoutManager.primaryMonitor;
        this.set_position(monitor.x, monitor.y);
        this.set_size(monitor.width, monitor.height);
        this._blurLayer.set_size(monitor.width, monitor.height);
        this._tint.set_size(monitor.width, monitor.height);
        this._scrim.width = monitor.width;
        this._fitLayout();
    }

    // the layout for the screen as it is turned now: a turn to landscape leaves no second pull to be in
    _fitLayout() {
        const layout = this._layout = shadeLayout(this._statusBar?.barHeight ?? BAR_HEIGHT);
        H = layout.travel;
        QS_TRAVEL = layout.qsTravel;
        if (QS_TRAVEL === 0)
            this._q = 0;
        const {panel} = layout;
        const r = px(panel.radius).toFixed(2);
        if (layout.phone) {
            // translucent over the blurred screen: the dim, and a half opaque panel
            this._tint.style = `background-color: ${rgba(SHADE_PANEL[0].map(Math.round), SHADE_PANEL[1])};`;
            this._scrim.style = `background-color: ${rgba(Color.notificationScrim, 0.5)}; ` +
                `border-radius: ${r}px ${r}px 0 0;`;
        } else {
            // the panel as long as its notifications (stackPanelChanged), every corner round, translucent as a
            // phone's
            this._tint.style = `background-color: ${rgba(TabletShade.scrim)};`;
            this._scrim.style = `background-color: ${rgba(Color.notificationScrim, 0.5)}; border-radius: ${r}px;`;
            this._qs.remove_clip();
        }
        // a phone's panel that runs off the bottom of the screen is a pull longer, so its end never shows on the way
        // down; a tablet's has the stack's height (setFrame below)
        this._scrim.set_size(px(panel.width), px(panel.height + (panel.roundBottom ? 0 : H)));
        this._qs.setLayout(layout);
        this._syncBarLine();
        this._stack.setFrame(layout.rows);
        this._queueApply();
    }

    // the status bar laid out again (a turn, the clock's minute): a phone's header is the bar's height, and the header
    // draws the clock and the icons where the bar has them
    _barChanged() {
        if (this._layout?.phone && this._layout.header.height !== this._statusBar.barHeight)
            this._fitLayout();
        else
            this._syncBarLine();
    }

    _syncBarLine() {
        if (this._layout && this._statusBar)
            this._qs.setBarLine(this._statusBar.contentLine);
    }
    _onDestroy() {
        geometry.disconnectObject(this);
        wholeFrames(this, false);
        this._stopAnimation();
        this._blurAnim?.stop();
        this._mirrorAnim?.stop();
        if (this._applyId)
            global.compositor.get_laters().remove(this._applyId);
        this._ungrab();
        this._statusBar?.remove_action(this._pull);
        this._statusBar?.remove_action(this._pullQs);
        if (this._statusBar) {
            this._statusBar.opacity = 255;
            this._statusBar.setLineHidden(false);
        }
        global.window_group.set_scale(1, 1);
        global.window_group.set_translation(0, 0, 0);
    }
});
