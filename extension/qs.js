// Quick Settings in the shade: the header (clock, date, carrier, system icons), the tile grid whose first two rows
// are the QQS, the brightness and volume sliders, the build row and the footer. The shade lays it out once per frame
// from its expansion f and the QS expansion q; tiles act on SystemState.
import Clutter from 'gi://Clutter';
import Gio from 'gi://Gio';
import GLib from 'gi://GLib';
import GnomeDesktop from 'gi://GnomeDesktop';
import GObject from 'gi://GObject';
import Gvc from 'gi://Gvc';
import Pango from 'gi://Pango';
import Shell from 'gi://Shell';
import St from 'gi://St';

import * as Main from 'resource:///org/gnome/shell/ui/main.js';
import * as SystemActions from 'resource:///org/gnome/shell/misc/systemActions.js';
import * as Volume from 'resource:///org/gnome/shell/ui/status/volume.js';

import {addEndMark} from './endmark.js';
import {Canvas, hasIcon, paintIcon, VectorIcon} from './icons.js';
import {animate, animateSpring, clamp, lerp, Spring} from './motion.js';
import {Color, cogl, font, mix, rgba, setSource, SurfaceEffect, TabletShade} from './palette.js';
import {openSettings} from './settings.js';
import {BAR_HEIGHT, inkBox, SYSTEM_SPAN, SystemIcons} from './statusbar.js';
import {px, screenPixels, SCALE, SCREEN_H, SCREEN_W, TABLET} from './units.js';

// --- geometry, screen px
export const QQS_HEIGHT = 644;          // the stack's top padding: 116 + 2 x 216 + 24 + 72
export const QS_SHIFT = 504;            // the QQS rows move 116 -> 620 as q goes 0 -> 1
const GRID_X = 48;
const GRID_TOP = 116;
const TILE_W = 228;
const TILE_H = 216;
const COL_PITCH = 252;
const ROW_GAP = 24;
const COLUMNS = 4;
const TABLET_COLUMNS = 3;
const QQS_ROWS = 2;
const ICON = 96;
const LARGE_ICON = 84;                  // a wide tile's icon, centred on LARGE_ICON_CX
const LARGE_ICON_CX = 108;
const LARGE_TEXT_X = 210;               // a wide tile's labels, past its icon
const TILE_RADIUS = 72;                 // on or off: an off tile is a rounded square too, not a pill
const CONTENT_FADE_FROM = 0.43;         // QS content fades in over q 0.43..1, the footer over 0.9..1
const FOOTER_FADE_FROM = 0.9;

const SLIDER_X = 54;
const SLIDER_END = 867;                 // then a gap and the auto-brightness button
const SLIDER_CY = 500;                  // QS position
const VOLUME_ROW = 180;                 // the volume row under it, and the tiles that far further down
const TRACK_H = 120;
const TRACK_RADIUS = 36;                // TRACK_INNER_RADIUS next to the thumb
const TRACK_INNER_RADIUS = 6;
const THUMB_W = 12;
const THUMB_H = 156;
const THUMB_GAP = 18;                   // each side of the thumb
const TRACK_ICON = 84;                  // TRACK_ICON_PAD from the segment's end
const TRACK_ICON_PAD = 18;
const AUTO_X = 897;
const AUTO_SIZE = 135;

const BUILD_INK_X = 49;
const BUILD_INK_TOP = 1612;
const FOOTER_CY = 2195.5;
const FOOTER_BUTTON = 120;              // circles
const POWER_X = 912;
const SETTINGS_X = 768;
const FOOTER_ICON = 60;

// header: ink positions of the QQS row (q = 0) and the QS layout (q = 1)
const CLOCK_QQS = [50, 24];
const CLOCK_QS = [53, 80];
const CLOCK_SCALE = 2.57;
const DATE_QQS = [198, 24];
const DATE_QS = [49, 189];
const ICONS_QQS = [912, 42];            // the cluster's right end and centre line
const ICONS_QS = [1031, 204];
const CARRIER_END = 1019;
const CARRIER_QQS_TOP = 24;
const CARRIER_QS_TOP = 110;
const HEADER_FADE_OUT = 0.14;           // over q the header fades out by here, moving a tenth of its way,
const HEADER_FADE_IN = 0.58;            // and fades back in from here at its QS place
const HEADER_DISPLACEMENT = 0.1066;
// a tablet's header: a 56 dp row, the date 8 dp past the clock
const TABLET_HEADER_H = 168;
const TABLET_DATE_GAP = 24;

// fonts (Google Sans Flex, rounded)
const TILE_LABEL_FONT = font(14, 600);
const TILE_SECONDARY_FONT = font(12, 500);
const CLOCK_FONT = font(14, 650);
const CLOCK_BIG_FONT = font(14 * CLOCK_SCALE, 650, {opsz: 14});
const DATE_FONT = font(14, 600);
const CARRIER_FONT = font(14, 550);
const BUILD_FONT = font(12, 500);

const CAP_HEIGHT = 0.73;                // cap height / em of the font (0.72-0.74)

// Tile colours: [background rgb, alpha], foreground [rgb, alpha]
const TILE_COLORS = {
    active: {bg: [Color.primary, 1], fg: [Color.onPrimary, 1]},
    inactive: {bg: SurfaceEffect[1], fg: [Color.onSurface, 1]},
    unavailable: {bg: [Color.surface, 0.18], fg: [Color.onSurfaceVariant, 0.38]},
};

// A tablet's tiles are pills with the icon and the labels side by side, on an opaque scrim. On is a rounded rectangle
// in the primary colour; off and unavailable are full pills.
const TABLET_TILE = [TabletShade.tile, 1];
const TABLET_DIM = [Color.onSurfaceVariant, 0.38];
const TABLET_TILE_COLORS = {
    active: {bg: [Color.primary, 1], fg: [Color.onPrimary, 1], radius: TILE_RADIUS},
    inactive: {bg: TABLET_TILE, fg: [Color.onSurface, 1], radius: TILE_H / 2},
    unavailable: {bg: [TabletShade.unavailable, 1], fg: TABLET_DIM, radius: TILE_H / 2},
};
// ... except Wi-Fi, Bluetooth and Do Not Disturb, whose icon sits on a target of its own: a rounded square in the
// primary colour when on, a circle when off, on a pill that stays dark
const TABLET_TARGET = 168;
const TABLET_TARGET_INSET = 24;
const TABLET_TARGET_RADIUS = 48;
const TABLET_DUAL_COLORS = {
    active: {bg: TABLET_TILE, fg: [Color.onSurface, 1], target: [Color.primary, 1], glyph: [Color.onPrimary, 1],
        radius: TILE_H / 2, targetRadius: TABLET_TARGET_RADIUS},
    inactive: {bg: TABLET_TILE, fg: [Color.onSurface, 1], target: [TabletShade.tileTarget, 1],
        glyph: [Color.onSurface, 1], radius: TILE_H / 2, targetRadius: TABLET_TARGET / 2},
    unavailable: {bg: [TabletShade.unavailable, 1], fg: TABLET_DIM, target: [TabletShade.unavailable, 1],
        glyph: TABLET_DIM, radius: TILE_H / 2, targetRadius: TABLET_TARGET / 2},
};
const DUAL_TARGET = new Set(['wifi', 'bluetooth', 'dnd']);
const TABLET_TEXT_END = 24;             // the labels end this far from the tile's end

const MOBILE_ICONS = ['ic_signal_cellular_0_4_bar', 'ic_signal_cellular_1_4_bar', 'ic_signal_cellular_2_4_bar',
    'ic_signal_cellular_3_4_bar', 'ic_signal_cellular_4_4_bar'];


// The tiles of this phone, in the reference's default order (no power-profiles-daemon: battery saver shows unavailable)
// state: 'active' | 'inactive' | 'unavailable'
const TILES = [
    {
        id: 'wifi', label: 'Wi-Fi', panel: 'wifi',
        icon: s => !s.wifi.enabled ? 'ic_signal_wifi_off'
            : s.wifi.connected ? `ic_wifi_${[0, 1, 2, 2, 3][s.wifi.level] ?? 0}` : 'ic_wifi_0',
        state: s => !s.wifi.available ? 'unavailable' : s.wifi.enabled ? 'active' : 'inactive',
        toggle: s => s.setWifiEnabled(!s.wifi.enabled),
    },
    {
        id: 'bluetooth', label: 'Bluetooth', panel: 'bluetooth',
        icon: s => !s.bluetooth.powered ? 'qs_bluetooth_icon_off'
            : s.bluetooth.connected ? 'qs_bluetooth_icon_on' : 'qs_bluetooth_icon_disconnected',
        state: s => !s.bluetooth.available ? 'unavailable' : s.bluetooth.powered ? 'active' : 'inactive',
        toggle: s => s.setBluetooth(!s.bluetooth.powered),
    },
    {
        id: 'flashlight', label: 'Flashlight',
        icon: s => s.torch ? 'qs_flashlight_icon_on' : 'qs_flashlight_icon_off',
        state: s => s.torch === null ? 'unavailable' : s.torch ? 'active' : 'inactive',
        toggle: s => s.setTorch(!s.torch),
    },
    {
        id: 'dnd', label: 'Do Not Disturb', panel: 'notifications',
        icon: () => 'ic_qs_dnd_on',                 // the filled glyph in both states, as on the device
        state: s => s.dnd ? 'active' : 'inactive',
        toggle: s => {
            s.dnd = !s.dnd;
        },
    },
    {
        id: 'airplane', label: 'Airplane mode', panel: 'network',
        icon: s => s.airplane ? 'qs_airplane_icon_on' : 'qs_airplane_icon_off',
        state: s => s.airplane ? 'active' : 'inactive',
        toggle: s => s.setAirplane(!s.airplane),
    },
    {
        id: 'mobile', label: 'Mobile data', panel: 'wwan', wide: true,
        icon: s => !s.mobile.present ? 'ic_qs_no_sim'
            : s.mobile.dataEnabled ? MOBILE_ICONS[s.mobile.level] ?? MOBILE_ICONS[0] : 'ic_signal_mobile_data_off',
        state: s => !s.mobile.present || s.airplane ? 'unavailable' : s.mobile.dataEnabled ? 'active' : 'inactive',
        secondary: s => !s.mobile.present ? 'No SIM' : s.mobile.dataEnabled ? s.mobile.operator || 'On' : 'Off',
        toggle: s => s.setMobileData(!s.mobile.dataEnabled),
    },
    {
        id: 'rotation', label: 'Auto-rotate', panel: 'display',
        icon: s => s.autoRotate ? 'qs_auto_rotate_icon_on' : 'qs_auto_rotate_icon_off',
        state: s => s.autoRotate ? 'active' : 'inactive',
        toggle: s => {
            s.autoRotate = !s.autoRotate;
        },
    },
    {
        id: 'location', label: 'Location', panel: 'location',
        icon: s => s.location ? 'qs_location_icon_on' : 'qs_location_icon_off',
        state: s => s.location ? 'active' : 'inactive',
        toggle: s => {
            s.location = !s.location;
        },
    },
    {
        id: 'dark', label: 'Dark theme', panel: 'display',
        icon: s => s.darkMode ? 'qs_light_dark_theme_icon_on' : 'qs_light_dark_theme_icon_off',
        state: s => s.darkMode ? 'active' : 'inactive',
        toggle: s => {
            s.darkMode = !s.darkMode;
        },
    },
    {
        id: 'saver', label: 'Battery Saver', panel: 'power',
        icon: s => s.powerSave ? 'qs_battery_saver_icon_on' : 'qs_battery_saver_icon_off',
        state: s => s.powerSave === null ? 'unavailable' : s.powerSave ? 'active' : 'inactive',
        toggle: s => s.setPowerSave(!s.powerSave),
    },
    {
        id: 'nightlight', label: 'Night Light', panel: 'night-light',
        icon: s => s.nightLight ? 'qs_nightlight_icon_on' : 'qs_nightlight_icon_off',
        state: s => s.nightLight ? 'active' : 'inactive',
        toggle: s => {
            s.nightLight = !s.nightLight;
        },
    },
];

function colorStyle([c, a]) {
    return rgba(c.map(Math.round), a);
}

// interpolate two [rgb, alpha] colours
function mixColor([ca, aa], [cb, ab], t) {
    return [mix(ca, cb, t), lerp(aa, ab, t)];
}

// Position a Clutter.Text so its first line's cap height is centred on @capCenterY, text box start at @x (logical)
function placeCapCentered(text, x, capCenterY, sizePx) {
    const baseline = text.get_layout().get_baseline() / 1024 / text.get_resource_scale();
    text.set_position(x, capCenterY + CAP_HEIGHT * sizePx / 2 - baseline);
}

// --- tiles

const Tile = GObject.registerClass(
class Tile extends St.Button {
    // @tablet: a tablet's pill tile, its width set by the layout (setWidthPx)
    constructor(spec, state, onActivated, tablet = false) {
        const w = tablet || spec.wide ? 2 * TILE_W + ROW_GAP : TILE_W;
        super({
            reactive: true,
            can_focus: true,
            button_mask: St.ButtonMask.PRIMARY,
            width: px(w),
            height: px(TILE_H),
        });
        this._spec = spec;
        this._state = state;
        this._tablet = tablet;
        this._w = w;
        this.wide = tablet || !!spec.wide;
        this.accessible_name = spec.label;
        this._palette = !tablet ? TILE_COLORS : DUAL_TARGET.has(spec.id) ? TABLET_DUAL_COLORS : TABLET_TILE_COLORS;

        this._content = new St.Widget({x_expand: true, y_expand: true});
        this.set_child(this._content);

        if (this._palette === TABLET_DUAL_COLORS) {
            this._target = new St.Widget({width: px(TABLET_TARGET), height: px(TABLET_TARGET)});
            this._content.add_child(this._target);
        }
        this._icon = new VectorIcon(spec.icon(state), px(this.wide ? LARGE_ICON : ICON), Color.onSurface);
        this._content.add_child(this._icon);
        if (this.wide) {
            const ellipsize = tablet ? Pango.EllipsizeMode.END : Pango.EllipsizeMode.NONE;
            this._label = new Clutter.Text({font_name: TILE_LABEL_FONT, text: spec.label, ellipsize});
            this._secondary = new Clutter.Text({font_name: TILE_SECONDARY_FONT, ellipsize});
            this._content.add_child(this._label);
            this._content.add_child(this._secondary);
        }

        // the press highlight
        this._highlight = new St.Widget({
            style: `background-color: ${rgba(Color.onSurface, 0.15)};`,
            opacity: 0,
        });
        this._content.add_child(this._highlight);
        this.connect('notify::pressed', () => {
            this._highlight.opacity = this.pressed ? 255 : 0;
        });

        // A two-target tile (a tablet's Wi-Fi, Bluetooth, Do Not Disturb): its icon's square toggles it, the rest of
        // it opens its details, the setting's page, as a long press does
        this._onTarget = true;
        if (this._target && spec.panel) {
            this.connect('captured-event', (_a, event) => {
                const type = event.type();
                if (type === Clutter.EventType.TOUCH_BEGIN || type === Clutter.EventType.BUTTON_PRESS) {
                    const [ok, x] = this.transform_stage_point(...event.get_coords());
                    this._onTarget = !ok || x < px(2 * TABLET_TARGET_INSET + TABLET_TARGET);
                }
                return Clutter.EVENT_PROPAGATE;
            });
        }
        this.connect('clicked', () => {
            if (!this._onTarget) {
                openSettings(spec.panel);
                onActivated?.(this, true);
                return;
            }
            spec.toggle(state);
            onActivated?.(this);
        });
        if (spec.panel && Clutter.LongPressGesture) {
            const longPress = new Clutter.LongPressGesture({required_button: Clutter.BUTTON_PRIMARY});
            longPress.connect('recognize', () => {
                openSettings(spec.panel);
                onActivated?.(this, true);
            });
            this.add_action(longPress);
        }

        // the colours spring between inactive and active; the corner stays
        this._mode = spec.state(state);
        this._from = this._palette[this._mode];
        this._to = this._palette[this._mode];
        this._colorT = 1;
        state.connectObject('changed', () => this._sync(), this);
        this._heightPx = TILE_H;
        this._applyStyle();
        this._sync();
    }

    _sync() {
        const spec = this._spec, state = this._state;
        this._icon.name = spec.icon(state);
        if (this.wide)
            this._secondary.text = spec.secondary?.(state) ?? '';

        const mode = spec.state(state);
        this.reactive = mode !== 'unavailable' || !!spec.panel;
        if (mode === this._mode) {
            this._layoutContent();
            return;
        }
        // restart the colour spring from where it is now
        const current = this._colors();
        this._from = current;
        this._to = this._palette[mode];
        this._mode = mode;
        this._colorT = 0;
        this._spring?.stop();
        const spring = new Spring({stiffness: 1500, dampingRatio: 1, value: 0, target: 1});
        this._spring = animateSpring(this, spring, v => {
            this._colorT = v;
            this._applyStyle();
        }, () => {
            this._spring = null;
        });
        this._applyStyle();
        this._layoutContent();
    }

    _colors() {
        const t = clamp(this._colorT, 0, 1), a = this._from, b = this._to;
        const colors = {bg: mixColor(a.bg, b.bg, t), fg: mixColor(a.fg, b.fg, t)};
        if (this._tablet) {
            colors.glyph = mixColor(a.glyph ?? a.fg, b.glyph ?? b.fg, t);
            colors.radius = lerp(a.radius, b.radius, t);
            if (a.target) {
                colors.target = mixColor(a.target, b.target, t);
                colors.targetRadius = lerp(a.targetRadius, b.targetRadius, t);
            }
        }
        return colors;
    }

    _applyStyle() {
        const {bg, fg, glyph = fg, radius = TILE_RADIUS, target, targetRadius} = this._colors();
        const r = px(radius).toFixed(2);
        this.style = `background-color: ${colorStyle(bg)}; border-radius: ${r}px;`;
        this._highlight.style = `background-color: ${rgba(Color.onSurface, 0.15)}; border-radius: ${r}px;`;
        if (this._target) {
            this._target.style = `background-color: ${colorStyle(target)}; ` +
                `border-radius: ${px(targetRadius).toFixed(2)}px;`;
        }
        this._icon.setColor(glyph[0], glyph[1]);
        if (this.wide) {
            this._label.color = cogl(fg[0], fg[1]);
            this._secondary.color = cogl(fg[0], fg[1]);
        }
    }

    // a tablet tile's width (screen px), from the layout
    setWidthPx(w) {
        if (Math.abs(w - this._w) < 0.01)
            return;
        this._w = w;
        this.width = px(w);
        this._layoutContent();
    }

    // the squished height (screen px); the content stays centred and unscaled
    setHeightPx(h) {
        if (Math.abs(h - this._heightPx) < 0.01)
            return;
        this._heightPx = h;
        this.height = px(h);
        this._layoutContent();
    }

    _layoutContent() {
        const h = this._heightPx;
        const w = this._w;
        this._highlight.set_size(px(w), px(h));
        const cy = h / 2;
        if (!this.wide) {
            this._icon.set_position(px((w - ICON) / 2), px(cy - ICON / 2));
            return;
        }
        this._icon.set_position(px(LARGE_ICON_CX - LARGE_ICON / 2), px(cy - LARGE_ICON / 2));
        this._target?.set_position(px(TABLET_TARGET_INSET), px(cy - TABLET_TARGET / 2));
        if (this._tablet) {
            const textW = px(Math.max(w - LARGE_TEXT_X - TABLET_TEXT_END, 0));
            this._label.width = textW;
            this._secondary.width = textW;
        }
        const labelSize = 14 * 3, secondarySize = 12 * 3;
        if (this._secondary.text) {
            // the label and the secondary line, centred together
            placeCapCentered(this._label, px(LARGE_TEXT_X), px(cy - 24), px(labelSize));
            placeCapCentered(this._secondary, px(LARGE_TEXT_X), px(cy + 30), px(secondarySize));
        } else {
            placeCapCentered(this._label, px(LARGE_TEXT_X), px(cy), px(labelSize));
        }
        this._secondary.visible = !!this._secondary.text;
    }

    destroy() {
        this._spring?.stop();
        super.destroy();
    }
});

// --- the brightness and volume rows

// A row of the shade's quick settings: a track whose thumb sets a level, the level's icon on the track, and a round
// button at the row's end that turns into a rounded square while its option is on. Brightness's button is adaptive
// brightness (a LineageOS addition); volume's, below it, mutes the speaker. @spec:
//   get(): the level, 0..1, or null while there is none; set(v): sets it from the thumb
//   icon(v): the track's icon at level v
//   button: {name, icons: [off, on], on(): whether the option is on, toggle()}
//   dragging(d): the thumb taken (true) and let go (false), if the shade does something about it
// sync() takes up the level and the option when they change.
const LevelSlider = GObject.registerClass(
class LevelSlider extends St.Widget {
    constructor(spec) {
        super({width: px(SCREEN_W), height: px(THUMB_H), reactive: false});
        this._spec = spec;
        // the track's ends and the auto button's size, screen px of the row; a tablet's layout moves them
        this._x0 = SLIDER_X;
        this._x1 = SLIDER_END;
        this._autoSize = AUTO_SIZE;
        this._autoOff = SurfaceEffect[1];

        this._track = new Canvas({
            x: px(SLIDER_X), y: 0, width: px(SLIDER_END - SLIDER_X), height: px(THUMB_H), reactive: true,
        });
        this._track.draw = cr => this._paintTrack(cr);
        this.add_child(this._track);

        this._value = spec.get() ?? 0.5;
        this._dragging = false;

        // press or drag anywhere on the track: with no threshold it takes the touch at once, so the shade's own
        // gestures give way (the Compose slider jumps to the press too)
        const pan = new Clutter.PanGesture({begin_threshold: 0, max_n_points: 1});
        pan.connect('recognize', g => this._dragTo(g));
        pan.connect('pan-update', g => this._dragTo(g));
        pan.connect('end', () => this._endDrag());
        pan.connect('cancel', () => this._endDrag());
        this._track.add_action(pan);

        // the button: a circle when off, a rounded square when on
        this._auto = new St.Button({
            x: px(AUTO_X), y: px((THUMB_H - AUTO_SIZE) / 2), width: px(AUTO_SIZE), height: px(AUTO_SIZE),
            reactive: true, can_focus: true, accessible_name: spec.button.name,
        });
        this._autoIcon = new VectorIcon(spec.button.icons[0], px(72), Color.onSurface);
        this._auto.set_child(this._autoIcon);
        this._auto.connect('clicked', () => spec.button.toggle());
        this.add_child(this._auto);
        this._autoOn = !!spec.button.on();
        this._autoT = this._autoOn ? 1 : 0;
        this._styleAuto();

        // the track icon cross-fades between the two segments (appear 100 ms after 33, disappear 50 ms)
        this._iconOnActive = false;
        this._activeIconAlpha = 0;
        this._inactiveIconAlpha = 1;
    }

    get dragging() {
        return this._dragging;
    }

    // a tablet's row: the track from @x0 to @x1, then the auto button of @autoSize at the row's end @end, the parts
    // that are off in the colour of an off tile (screen px)
    setSpan(x0, x1, end, autoSize, offColor) {
        this._x0 = x0;
        this._x1 = x1;
        this._autoSize = autoSize;
        this._autoOff = offColor;
        this.set_size(px(end), px(THUMB_H));
        this._track.set_position(px(x0), 0);
        this._track.set_size(px(x1 - x0), px(THUMB_H));
        this._auto.set_position(px(end - autoSize), px((THUMB_H - autoSize) / 2));
        this._auto.set_size(px(autoSize), px(autoSize));
        this._styleAuto();
        this._track.queue_repaint();
    }

    sync() {
        if (!this._dragging) {
            const v = this._spec.get();
            if (v !== null && Math.abs(v - this._value) > 1e-4) {
                this._value = v;
                this._track.queue_repaint();
            }
        }
        const on = !!this._spec.button.on();
        if (on !== this._autoOn) {
            this._autoOn = on;
            this._autoSpring?.stop();
            const from = this._autoT;
            const spring = new Spring({stiffness: 1500, dampingRatio: 1, value: from, target: this._autoOn ? 1 : 0});
            this._autoSpring = animateSpring(this, spring, v => {
                this._autoT = v;
                this._styleAuto();
            }, () => {
                this._autoSpring = null;
            });
        }
    }

    _styleAuto() {
        const t = clamp(this._autoT, 0, 1);
        const bg = mixColor(this._autoOff, [Color.primary, 1], t);
        const fg = mix(Color.onSurface, Color.onPrimary, t);
        const radius = lerp(this._autoSize / 2, TRACK_RADIUS, t);
        this._auto.style = `background-color: ${colorStyle(bg)}; border-radius: ${px(radius).toFixed(2)}px;`;
        this._autoIcon.setColor(fg);
        this._autoIcon.name = this._spec.button.icons[this._autoOn ? 1 : 0];
    }

    _dragTo(gesture) {
        const point = gesture.get_point_coords(0);      // in the track's own coordinates
        const w = this._x1 - this._x0;
        const v = clamp((point.x * SCALE - THUMB_W / 2) / (w - THUMB_W));
        if (!this._dragging) {
            this._dragging = true;
            this._spec.dragging?.(true);
        }
        this._value = v;
        this._spec.set(v);
        this._track.queue_repaint();
    }

    _endDrag() {
        if (!this._dragging)
            return;
        this._dragging = false;
        this._spec.dragging?.(false);
        // what the level settled on, if it is not where the finger left it (the volume's steps)
        this.sync();
    }

    _setIconSide(onActive) {
        if (onActive === this._iconOnActive)
            return;
        this._iconOnActive = onActive;
        this._iconAnim?.stop();
        const a0 = this._activeIconAlpha, i0 = this._inactiveIconAlpha;
        // appear: 100 ms after 33 ms; disappear: 50 ms
        this._iconAnim = animate(this, {
            from: 0, to: 1, duration: 133, interpolator: x => x,
            onUpdate: (v, f, t) => {
                const ms = t * 133;
                const appear = clamp((ms - 33) / 100), disappear = clamp(ms / 50);
                this._activeIconAlpha = onActive ? lerp(a0, 1, appear) : lerp(a0, 0, disappear);
                this._inactiveIconAlpha = onActive ? lerp(i0, 0, disappear) : lerp(i0, 1, appear);
                this._track.queue_repaint();
            },
            onDone: () => {
                this._iconAnim = null;
            },
        });
    }

    _paintTrack(cr) {
        screenPixels(cr);
        const w = this._x1 - this._x0;
        const c = THUMB_W / 2 + this._value * (w - THUMB_W);
        const top = (THUMB_H - TRACK_H) / 2;
        const activeEnd = c - THUMB_W / 2 - THUMB_GAP;
        const inactiveStart = c + THUMB_W / 2 + THUMB_GAP;

        const segment = (x0, x1, r0, r1, color, alpha) => {
            if (x1 - x0 <= 0)
                return;
            const h = TRACK_H;
            const left = Math.min(r0, (x1 - x0) / 2), right = Math.min(r1, (x1 - x0) / 2);
            cr.newPath();
            cr.arc(x0 + left, top + left, left, Math.PI, 1.5 * Math.PI);
            cr.arc(x1 - right, top + right, right, 1.5 * Math.PI, 2 * Math.PI);
            cr.arc(x1 - right, top + h - right, right, 0, 0.5 * Math.PI);
            cr.arc(x0 + left, top + h - left, left, 0.5 * Math.PI, Math.PI);
            cr.closePath();
            setSource(cr, color, alpha);
            cr.fill();
        };
        segment(0, activeEnd, TRACK_RADIUS, TRACK_INNER_RADIUS, Color.primary, 1);
        segment(inactiveStart, w, TRACK_INNER_RADIUS, TRACK_RADIUS, ...this._autoOff);

        // the thumb: a narrow upright bar
        cr.newPath();
        const tr = THUMB_W / 2;
        cr.arc(c, tr, tr, Math.PI, 2 * Math.PI);
        cr.arc(c, THUMB_H - tr, tr, 0, Math.PI);
        cr.closePath();
        setSource(cr, Color.primary);
        cr.fill();

        // the icon at the end of the inactive segment while it fits there, else at the end of the active one
        const icon = this._spec.icon(this._value);
        const fitsInactive = TRACK_ICON < w - inactiveStart - 2 * TRACK_ICON_PAD;
        const fitsActive = TRACK_ICON < activeEnd - 2 * TRACK_ICON_PAD;
        if (fitsInactive)
            this._setIconSide(false);
        else if (fitsActive)
            this._setIconSide(true);
        const iy = top + (TRACK_H - TRACK_ICON) / 2;
        if (fitsInactive || this._inactiveIconAlpha > 0) {
            paintIcon(cr, icon, w - TRACK_ICON_PAD - TRACK_ICON, iy, TRACK_ICON, Color.onSurface,
                this._inactiveIconAlpha);
        }
        if (fitsActive && this._activeIconAlpha > 0) {
            paintIcon(cr, icon, activeEnd - TRACK_ICON_PAD - TRACK_ICON, iy, TRACK_ICON, Color.onPrimary,
                this._activeIconAlpha);
        }
    }

    destroy() {
        this._autoSpring?.stop();
        this._iconAnim?.stop();
        super.destroy();
    }
});

// brightness: the backlight through SystemState; dragging it fades the shade around it (the brightness mirror)
function brightnessSlider(state, shade) {
    const slider = new LevelSlider({
        get: () => state.brightness,
        set: v => {
            state.brightness = v;
        },
        icon: v => v <= 0.2 ? 'ic_brightness_low' : v >= 0.8 ? 'ic_brightness_full' : 'ic_brightness_medium',
        button: {
            name: 'Adaptive brightness',
            icons: ['ic_qs_brightness_auto_off', 'ic_qs_brightness_auto_on'],
            on: () => state.autoBrightness,
            toggle: () => {
                state.autoBrightness = !state.autoBrightness;
            },
        },
        dragging: d => shade.setBrightnessDragging?.(d),
    });
    state.connectObject('changed', () => slider.sync(), slider);
    return slider;
}

// volume: the default output's, as the volume keys set it (volume.js); the button mutes it, and a level above none
// unmutes it again. Below the brightness row, its look (owner, 11-21).
function volumeSlider() {
    const control = Volume.getMixerControl();
    let stream = null;
    const slider = new LevelSlider({
        get: () => {
            if (!stream)
                return null;
            return stream.is_muted ? 0 : clamp(stream.volume / control.get_vol_max_norm());
        },
        set: v => {
            if (!stream)
                return;
            if (v > 0 && stream.is_muted)
                stream.change_is_muted(false);
            stream.volume = Math.round(control.get_vol_max_norm() * v);
            stream.push_volume();
        },
        icon: v => v <= 0 ? 'ic_volume_media_mute' : 'ic_volume_media',
        button: {
            name: 'Mute',
            icons: ['ic_volume_media', 'ic_volume_media_mute'],
            on: () => !!stream?.is_muted,
            toggle: () => stream?.change_is_muted(!stream.is_muted),
        },
    });
    const syncStream = () => {
        const s = control.get_state() === Gvc.MixerControlState.READY ? control.get_default_sink() : null;
        if (s !== stream) {
            stream?.disconnectObject(slider);
            stream = s;
            stream?.connectObject('notify::volume', () => slider.sync(), 'notify::is-muted', () => slider.sync(),
                slider);
        }
        slider.sync();
    };
    control.connectObject('default-sink-changed', syncStream, 'state-changed', syncStream, slider);
    slider.connect('destroy', () => stream?.disconnectObject(slider));
    syncStream();
    return slider;
}

// --- footer: settings and power, and on a tablet the active apps' pill

const PILL_FONT = font(14, 500);
const PILL_ICON = 60;                   // the info glyph and the chevron
const PILL_ICON_X = 73.5;               // the info glyph's centre from the pill's start
const PILL_TEXT_X = 138;
const PILL_CHEVRON_END = 73.5;          // the chevron's centre from the pill's end
const PILL_GAP = 24;                    // the pill's end before the settings button

// "N apps are active", where Android counts the apps with a foreground service: here the apps running with no window
// of theirs open, an app's own name on the session bus with no window (the Phone and Messaging apps' daemons, a
// player playing on) and the apps the desktop portal's background monitor reports. None, no pill. A tap opens the apps'
// settings.
const ActiveAppsPill = GObject.registerClass(
class ActiveAppsPill extends St.Button {
    constructor(onTap) {
        super({
            reactive: true, can_focus: true, visible: false, height: px(FOOTER_BUTTON),
            style: `background-color: ${colorStyle(TABLET_TILE)}; border-radius: ${px(FOOTER_BUTTON / 2)}px;`,
        });
        this._box = new St.Widget({x_expand: true, y_expand: true});
        this.set_child(this._box);
        this._info = new VectorIcon('ic_qs_footer_info', px(PILL_ICON), Color.onSurface);
        this._text = new Clutter.Text({font_name: PILL_FONT, color: cogl(Color.onSurface),
            ellipsize: Pango.EllipsizeMode.END});
        this._chevron = new VectorIcon('ic_chevron_end', px(PILL_ICON), Color.onSurface);
        for (const child of [this._info, this._text, this._chevron])
            this._box.add_child(child);
        this.connect('clicked', () => onTap());
        this.count = 0;
        this._names = [];               // the well-known names on the session bus
        this._portalIds = [];
        this._proxy = null;
        const bus = Gio.DBus.session;
        this._busSub = bus.signal_subscribe('org.freedesktop.DBus', 'org.freedesktop.DBus', 'NameOwnerChanged',
            '/org/freedesktop/DBus', null, Gio.DBusSignalFlags.NONE, (_c, _s, _p, _i, _n, params) => {
                if (!params.get_child_value(0).get_string()[0].startsWith(':'))
                    this._listLater();
            });
        Shell.AppSystem.get_default().connectObject('app-state-changed', () => this._count(), this);
        this._listLater();
        Gio.DBusProxy.new_for_bus(Gio.BusType.SESSION, Gio.DBusProxyFlags.DO_NOT_AUTO_START, null,
            'org.freedesktop.background.Monitor', '/org/freedesktop/background/monitor',
            'org.freedesktop.background.Monitor', null, (_o, res) => {
                try {
                    this._proxy = Gio.DBusProxy.new_for_bus_finish(res);
                } catch {
                    return;
                }
                if (this._destroyed) {
                    this._proxy = null;
                    return;
                }
                this._proxy.connectObject('g-properties-changed', () => this._read(), this);
                this._read();
            });
        this.connect('destroy', () => {
            this._destroyed = true;
            this._proxy?.disconnectObject(this);
            this._proxy = null;
            Gio.DBus.session.signal_unsubscribe(this._busSub);
            Shell.AppSystem.get_default().disconnectObject(this);
            if (this._listId)
                GLib.source_remove(this._listId);
            this._listId = 0;
        });
    }

    // names come and go in bursts: the list once they settle
    _listLater() {
        if (this._listId)
            return;
        this._listId = GLib.timeout_add(GLib.PRIORITY_DEFAULT_IDLE, 500, () => {
            this._listId = 0;
            Gio.DBus.session.call('org.freedesktop.DBus', '/org/freedesktop/DBus', 'org.freedesktop.DBus',
                'ListNames', null, new GLib.VariantType('(as)'), Gio.DBusCallFlags.NONE, -1, null, (bus, res) => {
                    try {
                        [this._names] = bus.call_finish(res).deepUnpack();
                    } catch {
                        return;
                    }
                    if (!this._destroyed)
                        this._count();
                });
            return GLib.SOURCE_REMOVE;
        });
    }

    _read() {
        const apps = this._proxy?.get_cached_property('BackgroundApps')?.deepUnpack() ?? [];
        this._portalIds = apps.map(app => app.app_id?.deepUnpack?.() ?? app.app_id).filter(id => id);
        this._count();
    }

    _count() {
        const appSystem = Shell.AppSystem.get_default();
        const ids = new Set();
        for (const name of this._names) {
            if (name.startsWith(':') || name.startsWith('vibeos.Shell') || name.startsWith('vibeos.Launcher'))
                continue;
            const app = appSystem.lookup_app(`${name}.desktop`);
            if (app && app.get_n_windows() === 0)
                ids.add(app.get_id());
        }
        for (const id of this._portalIds)
            ids.add(id.endsWith('.desktop') ? id : `${id}.desktop`);
        if (ids.size !== this.count)
            this.setCount(ids.size);
    }

    setCount(n) {
        this.count = n;
        this._text.text = n === 1 ? '1 app is active' : `${n} apps are active`;
        this.visible = n > 0;
        this.setWidthPx(this._w ?? 0);
    }

    // its width (screen px), from the column's start to the settings button
    setWidthPx(w) {
        this._w = w;
        this.width = px(w);
        const cy = FOOTER_BUTTON / 2;
        this._info.set_position(px(PILL_ICON_X - PILL_ICON / 2), px(cy - PILL_ICON / 2));
        this._chevron.set_position(px(w - PILL_CHEVRON_END - PILL_ICON / 2), px(cy - PILL_ICON / 2));
        this._text.width = px(Math.max(0, w - PILL_TEXT_X - PILL_CHEVRON_END - PILL_ICON / 2 - 24));
        placeCapCentered(this._text, px(PILL_TEXT_X), px(cy), px(14 * 3));
    }
});

const FooterButton = GObject.registerClass(
class FooterButton extends St.Button {
    constructor(icon, bg, fg, x, name) {
        super({
            x: px(x), y: 0, width: px(FOOTER_BUTTON), height: px(FOOTER_BUTTON),
            reactive: true, can_focus: true, accessible_name: name,
            style: `background-color: ${colorStyle(bg)}; border-radius: ${px(FOOTER_BUTTON / 2)}px;`,
        });
        this.set_child(new VectorIcon(icon, px(FOOTER_ICON), fg));
    }
});

// --- header

const ShadeHeader = GObject.registerClass(
class ShadeHeader extends St.Widget {
    constructor(state) {
        super({width: px(SCREEN_W), height: px(260)});
        this._state = state;

        this._clock = new Clutter.Text({font_name: CLOCK_FONT, color: cogl(Color.white)});
        this._bigClock = new Clutter.Text({font_name: CLOCK_BIG_FONT, color: cogl(Color.white), visible: false});
        this._date = new Clutter.Text({font_name: DATE_FONT, color: cogl(Color.white)});
        this._carrier = new Clutter.Text({font_name: CARRIER_FONT, color: cogl(Color.white), opacity: 0});
        // a tablet's icons are the status bar's, on a canvas of its size: a drawing area of a fractional size
        // stretches its whole-pixel texture over it, and two sizes would draw the same icons a fraction apart
        this._icons = new SystemIcons(state, TABLET
            ? {end: ICONS_QQS[0], centerY: ICONS_QQS[1], height: BAR_HEIGHT, span: SYSTEM_SPAN}
            : {end: ICONS_QQS[0], centerY: ICONS_QQS[1], height: 116});
        this._tabletHeader = null;
        this._barLine = null;
        for (const child of [this._icons, this._clock, this._bigClock, this._date, this._carrier])
            this.add_child(child);

        this._wallClock = new GnomeDesktop.WallClock({time_only: true});
        this._wallClock.connectObject('notify::clock', () => this._syncTime(), this);
        state.connectObject('changed', () => this._syncCarrier(), this);
        this._q = 0;
        this._syncTime();
        this._syncCarrier();
    }

    _syncTime() {
        const now = GLib.DateTime.new_now_local();
        const time = now.format('%H:%M');
        this._bigClock.text = time;
        // a tablet's clock is the status bar's (setBarLine)
        if (!this._barLine)
            this._clock.text = time;
        this._date.text = now.format('%a, %b %-d');
        this._clockInk = inkBox(this._clock);
        this._bigInk = inkBox(this._bigClock);
        this._dateInk = inkBox(this._date);
        this.setProgress(this._q);
    }

    _syncCarrier() {
        const {mobile} = this._state;
        const text = mobile.present ? mobile.operator || '' : 'No SIM';
        if (this._carrier.text !== text) {
            this._carrier.text = text;
            this.setProgress(this._q);
        }
    }

    // A tablet's header row spans the screen (@header: width, screen px). Its clock and icons are the status bar's
    // (setBarLine), and the date follows the clock
    setTablet(header) {
        this._tabletHeader = header;
        this.set_size(px(header.width), px(TABLET_HEADER_H));
        this.opacity = 255;
        this.setProgress(this._q);
    }

    // A tablet's clock and icons are drawn where the status bar has its own, in its clock's text and font, so they
    // stay in place all the while the shade opens over the bar and closes (@line: StatusBar.contentLine)
    setBarLine(line) {
        this._barLine = line;
        this._clock.font_name = line.clockFont;
        this._clock.text = line.clockText;
        this._clockInk = inkBox(this._clock);
        if (this._icons.end !== line.systemEnd)
            this._icons.setEnd(line.systemEnd);
        this._icons.setCenterY(line.centerY);
        this.setProgress(this._q);
    }

    // the date fades in and out with the shade's content; the clock and the icons are there all the while
    setDateAlpha(alpha) {
        this._date.opacity = Math.round(255 * alpha);
    }

    _layoutTablet() {
        const line = this._barLine;
        this._bigClock.visible = false;
        this._carrier.visible = false;
        this._clock.visible = !!line;
        this._icons.visible = !!line;
        this._date.visible = !!line;
        if (!line)
            return;
        // the clock's ink where the status bar's is
        const ink = this._clockInk;
        const top = px(line.clockInkTop);
        this._clock.set_position(px(line.clockInkX) - ink.x, top - ink.y);
        // the date on the clock's baseline, past its end
        const baseline = top + ink.height;
        const dateBaseline = this._date.get_layout().get_baseline() / 1024 / this._date.get_resource_scale();
        this._date.set_position(px(line.clockInkX + TABLET_DATE_GAP) + ink.width - this._dateInk.x,
            baseline - dateBaseline);
        this._icons.set_position(this._icons.x, 0);
        this._icons.opacity = 255;
    }

    // q: the QS expansion; the header moves linearly between its keyframes (HEADER_*)
    setProgress(q) {
        this._q = q;
        if (this._tabletHeader) {
            this._layoutTablet();
            return;
        }
        // the clock grows x2.57 from its ink corner; a crisp small copy at rest
        const [x0, y0] = CLOCK_QQS, [x1, y1] = CLOCK_QS;
        const x = lerp(x0, x1, q), y = lerp(y0, y1, q);
        const scale = lerp(1, CLOCK_SCALE, q);
        this._clock.visible = q <= 0;
        this._bigClock.visible = q > 0;
        if (q <= 0) {
            this._clock.set_position(px(x) - this._clockInk.x, px(y) - this._clockInk.y);
        } else {
            const k = scale / CLOCK_SCALE;
            this._bigClock.set_scale(k, k);
            this._bigClock.set_position(px(x) - this._bigInk.x * k, px(y) - this._bigInk.y * k);
        }

        // date and icons: fade out by 14 %, back in from 58 %, displaced 0.1066 on the way out
        // the spring overshoots q past 1 (and under 0): alpha is kept to 0..1, or opacity 256 throws mid-layout
        const keyed = ([sx, sy], [ex, ey]) => {
            if (q <= HEADER_FADE_OUT) {
                const t = q / HEADER_FADE_OUT;
                return {x: sx, y: sy + t * HEADER_DISPLACEMENT * (ey - sy), alpha: Math.min(1, 1 - t)};
            }
            if (q < HEADER_FADE_IN)
                return {x: ex, y: lerp(sy, ey, q), alpha: 0};
            return {x: ex, y: lerp(sy, ey, q), alpha: Math.min(1, (q - HEADER_FADE_IN) / (1 - HEADER_FADE_IN))};
        };
        const date = keyed(DATE_QQS, DATE_QS);
        this._date.set_position(px(date.x) - this._dateInk.x, px(date.y) - this._dateInk.y);
        this._date.opacity = Math.round(255 * date.alpha);
        const icons = keyed(ICONS_QQS, ICONS_QS);
        this._icons.set_position(px(icons.x - ICONS_QQS[0]), px(icons.y - ICONS_QQS[1]));
        this._icons.opacity = Math.round(255 * icons.alpha);

        // the carrier only fades in, from 58 %
        const ink = inkBox(this._carrier);
        const ctop = lerp(CARRIER_QQS_TOP, CARRIER_QS_TOP, q);
        this._carrier.set_position(px(CARRIER_END) - ink.x - ink.width, px(ctop) - ink.y);
        this._carrier.opacity = Math.round(255 * clamp((q - HEADER_FADE_IN) / (1 - HEADER_FADE_IN)));
    }
});

// --- the panel

// @actor (a child of the panel) cut where it reaches above @y (the panel's px); whole again below it
export function clipAbove(actor, y) {
    const top = y - actor.y;
    if (top > 0)
        actor.set_clip(0, top, actor.width, Math.max(actor.height - top, 0));
    else if (actor.has_clip)
        actor.remove_clip();
}

export const QSPanel = GObject.registerClass(
class QSPanel extends St.Widget {
    constructor(state, shade) {
        super({width: px(SCREEN_W), height: px(SCREEN_H), clip_to_allocation: false});
        this._state = state;
        this._shade = shade;

        this._header = new ShadeHeader(state);
        this.add_child(this._header);

        this._brightness = brightnessSlider(state, shade);
        this.add_child(this._brightness);
        this._volume = volumeSlider();
        this.add_child(this._volume);

        // tiles in reading order; a wide tile that does not fit the row wraps
        this._tiles = [];
        let col = 0, row = 0;
        for (const spec of TILES) {
            const span = spec.wide ? 2 : 1;
            if (col + span > COLUMNS) {
                col = 0;
                row++;
            }
            const tile = new Tile(spec, state, (t, longPress) => shade.onTileActivated?.(t, longPress), TABLET);
            tile._row = row;
            tile._col = col;
            this._tiles.push(tile);
            this.add_child(tile);
            col += span;
            if (col >= COLUMNS) {
                col = 0;
                row++;
            }
        }

        // the build row: the OS's name where Android shows its build number (tap copies it, as Android does). Not
        // selectable: a Clutter.Text is by default, and a swipe closing the shade from this row painted the selection
        // over it, a pale box.
        const build = GLib.get_os_info('PRETTY_NAME') ?? 'GNOME';
        this._build = new Clutter.Text({font_name: BUILD_FONT, color: cogl(Color.onSurface), text: build,
            reactive: true, selectable: false});
        const copy = new Clutter.ClickGesture();
        copy.connect('recognize', () => {
            St.Clipboard.get_default().set_text(St.ClipboardType.CLIPBOARD, build);
        });
        this._build.add_action(copy);
        this.add_child(this._build);

        this._footer = new St.Widget({width: px(SCREEN_W), height: px(FOOTER_BUTTON)});
        const settings = new FooterButton('ic_qs_footer_settings', SurfaceEffect[1], Color.onSurface, SETTINGS_X,
            'Settings');
        settings.connect('clicked', () => {
            shade.close?.();
            openSettings();
        });
        const power = new FooterButton('ic_qs_footer_power', [Color.primary, 1], Color.onPrimary, POWER_X,
            'Power menu');
        power.connect('clicked', () => {
            shade.close?.();
            SystemActions.getDefault().activatePowerOff();
        });
        this._footer.add_child(settings);
        this._footer.add_child(power);
        this._settingsButton = settings;
        this._powerButton = power;
        if (TABLET) {
            this._activeApps = new ActiveAppsPill(() => {
                shade.close?.();
                openSettings('applications');
            });
            this._footer.add_child(this._activeApps);
        }
        this.add_child(this._footer);
        addEndMark(this);

        this._tablet = null;
        this._f = -1;
        this._q = -1;
        this._squish = -1;
        this.layoutFor({f: 1, q: 0, squish: 1, contentAlpha: 1});
    }

    get brightnessDragging() {
        return this._brightness.dragging;
    }

    // a tablet's: where the status bar has its clock and icons (StatusBar.contentLine)
    setBarLine(line) {
        this._header.setBarLine(line);
    }

    // a tablet's layout (shadelayout.js) for the screen as it is turned now
    setTablet(layout) {
        this._tablet = layout;
        const {qs} = layout;
        this.set_size(px(layout.width), px(layout.height));
        this._header.setTablet(layout.header);
        const tileW = (qs.width - (TABLET_COLUMNS - 1) * ROW_GAP) / TABLET_COLUMNS;
        this._tiles.forEach((tile, i) => {
            tile._row = Math.floor(i / TABLET_COLUMNS);
            tile._col = i % TABLET_COLUMNS;
            tile.setWidthPx(tileW);
        });
        this._tilePitch = tileW + ROW_GAP;
        this._brightness.setSpan(qs.left, qs.trackEnd, qs.left + qs.width, qs.autoSize, TABLET_TILE);
        this._volume.setSpan(qs.left, qs.trackEnd, qs.left + qs.width, qs.autoSize, TABLET_TILE);
        this._footer.set_size(px(layout.width), px(FOOTER_BUTTON));
        this._settingsButton.x = px(qs.settingsCX - FOOTER_BUTTON / 2);
        this._powerButton.x = px(qs.powerCX - FOOTER_BUTTON / 2);
        if (this._activeApps) {
            this._activeApps.x = px(qs.left);
            this._activeApps.setWidthPx(qs.settingsCX - FOOTER_BUTTON / 2 - PILL_GAP - qs.left);
        }
        this._settingsButton.style = `background-color: ${colorStyle(TABLET_TILE)}; ` +
            `border-radius: ${px(FOOTER_BUTTON / 2)}px;`;
        this._build.visible = false;
        this._q = -1;
    }

    // @squish: the tiles' height factor while the shade opens (0.1..1); @contentAlpha: the header's;
    // @mirror: all but the brightness slider fade while it is dragged; @slide: a tablet's quick settings on their way
    // down, how far above their places they are (screen px, 0 or less)
    layoutFor({q, squish, contentAlpha, mirror = 0, slide = 0}) {
        if (this._tablet) {
            this._layoutTablet(q, contentAlpha, 1 - mirror, slide);
            return;
        }
        const keep = 1 - mirror;
        const shift = (q - 1) * QS_SHIFT;           // everything below the header moves with q
        const content = clamp((q - CONTENT_FADE_FROM) / (1 - CONTENT_FADE_FROM));
        const footer = clamp((q - FOOTER_FADE_FROM) / (1 - FOOTER_FADE_FROM));

        if (q !== this._q)
            this._header.setProgress(q);
        this._header.opacity = Math.round(255 * contentAlpha * keep);

        const tileH = TILE_H * (0.1 + 0.9 * squish);
        for (const tile of this._tiles) {
            const qqs = tile._row < QQS_ROWS;
            // squished rows close up from the top; rows below the QQS follow the QS layout
            const qqsY = GRID_TOP + tile._row * (tileH + ROW_GAP);
            const qsY = GRID_TOP + QS_SHIFT + VOLUME_ROW + tile._row * (TILE_H + ROW_GAP);
            const y = qqs ? lerp(qqsY, qsY, q) : qsY + shift;
            tile.set_position(px(GRID_X + tile._col * COL_PITCH), px(y));
            tile.setHeightPx(qqs ? lerp(tileH, TILE_H, q) : TILE_H);
            const alpha = (qqs ? 1 : content) * keep;
            tile.opacity = Math.round(255 * alpha);
            tile.visible = alpha > 0;
        }

        this._brightness.set_position(0, px(SLIDER_CY - THUMB_H / 2 + shift));
        this._brightness.opacity = Math.round(255 * content);
        this._brightness.visible = content > 0;
        // the volume row fades with the rest while the brightness is dragged
        this._volume.set_position(0, px(SLIDER_CY + VOLUME_ROW - THUMB_H / 2 + shift));
        this._volume.opacity = Math.round(255 * content * keep);
        this._volume.visible = content * keep > 0;

        const ink = this._buildInk ??= inkBox(this._build);
        this._build.set_position(px(BUILD_INK_X) - ink.x, px(BUILD_INK_TOP + shift) - ink.y);
        this._build.opacity = Math.round(255 * content * keep);
        this._build.visible = content > 0;

        this._footer.set_position(0, px(FOOTER_CY - FOOTER_BUTTON / 2 + shift));
        this._footer.opacity = Math.round(255 * footer * keep);
        this._footer.visible = footer > 0;

        this._q = q;
        this._squish = squish;
    }

    // A tablet's quick settings come down with the shade, fading in, rather than squish; the header stays and they
    // come out from under it. In landscape they are all there at once. Upright the first two rows come first; the
    // second pull moves them down to make room for the brightness row, and the other rows and the footer fade in as
    // on a phone.
    _layoutTablet(q, contentAlpha, keep, slide) {
        const {qs, landscape, header} = this._tablet;
        const under = px(header.height);
        if (landscape)
            q = 1;
        if (q !== this._q)
            this._header.setProgress(q);
        this._header.setDateAlpha(contentAlpha * keep);

        const content = clamp((q - CONTENT_FADE_FROM) / (1 - CONTENT_FADE_FROM));
        const footer = landscape ? 1 : clamp((q - FOOTER_FADE_FROM) / (1 - FOOTER_FADE_FROM));
        const shift = (q - 1) * (qs.gridTop - qs.qqsTop);
        for (const tile of this._tiles) {
            const qqs = tile._row < qs.qqsRows;
            const qsY = qs.gridTop + tile._row * (TILE_H + ROW_GAP);
            const y = qqs ? lerp(qs.qqsTop + tile._row * (TILE_H + ROW_GAP), qsY, q) : qsY + shift;
            tile.set_position(px(qs.left + tile._col * this._tilePitch), px(y + slide));
            const alpha = (qqs ? 1 : content) * contentAlpha * keep;
            tile.opacity = Math.round(255 * alpha);
            tile.visible = alpha > 0;
            clipAbove(tile, under);
        }

        this._brightness.set_position(0, px(qs.brightnessCY - THUMB_H / 2 + shift + slide));
        this._brightness.opacity = Math.round(255 * content * contentAlpha);
        this._brightness.visible = content * contentAlpha > 0;
        clipAbove(this._brightness, under);

        this._volume.set_position(0, px(qs.volumeCY - THUMB_H / 2 + shift + slide));
        this._volume.opacity = Math.round(255 * content * contentAlpha * keep);
        this._volume.visible = content * contentAlpha * keep > 0;
        clipAbove(this._volume, under);

        this._footer.set_position(0, px(qs.footerCY - FOOTER_BUTTON / 2 + slide));
        this._footer.opacity = Math.round(255 * footer * contentAlpha * keep);
        this._footer.visible = footer * contentAlpha > 0;
        clipAbove(this._footer, under);
        this._q = q;
    }
});

export {hasIcon};
