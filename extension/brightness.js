// The shell's brightness, three changes (2026-11-09).
//
// Saving: the shell keeps each monitor's brightness under its EDID vendor, product and serial, and a DSI panel has no
// EDID. The three are null, a GVariant string cannot be, and every save threw ("Argument string may not be null" from
// BrightnessManager._setState): nothing was ever kept, and each start took the backlight as it found it. They are
// saved as '' and read back as null.
//
// The slider: the shell sets the backlight in proportion to it, and the S10e panel's light is in proportion to its
// level (AOR only), so every level comfortable indoors sat in the slider's first fifth. The backlight now follows the
// slider to the power GAMMA: the owner's night level, 14 of 255, at a fifth of the slider (their ask), the rest of it
// in steps up to the same full brightness as before.
//
// Auto: gsd-power's target is two thirds at whatever light there is when it starts or the slider moves, then follows
// the light's ratio to that, and the shell adds the slider to it as a +-0.5 bias: auto sat bright in a dark room, and
// the slider at its start did not darken it. Now auto moves the slider itself, as on Android: to a level per decade of
// lux, plus however far the owner last dragged it from there. gsd-power's targets only say when auto is on (the
// setting, the screen on, its sensor claimed), and the shell stays in its manual mode.
import Gio from 'gi://Gio';
import GLib from 'gi://GLib';

import * as Main from 'resource:///org/gnome/shell/ui/main.js';

const GAMMA = 1.9;              // a fifth of the slider is 14 of 255 on the S10e (the owner's night level, 11-09)
const LUX_ZERO = 0.105;         // the slider at 0 lux; at 2 lux, the S10e's reading in a dark room, a fifth
const LUX_DECADE = 0.2;         // the slider per tenfold of lux: full at 30000 lux, daylight
const LUX_HYSTERESIS = 0.02;    // a quarter more or less light is not followed
const EASE_TICK_MS = 50;
const EASE_UP_S = 1;            // time constants: brighter quickly, darker slowly, as on Android
const EASE_DOWN_S = 3;
const EASE_DONE = 0.002;

const SENSOR_BUS = 'net.hadess.SensorProxy';
const SENSOR_PATH = '/net/hadess/SensorProxy';
const ID_KEYS = ['_vendor', '_product', '_serial', '_colorMode', '_connector'];

export class Brightness {
    constructor() {
        this._manager = Main.brightnessManager;
        this._cancellable = new Gio.Cancellable();
        this._fixSaving();
        this._patchScales();
        this._initAuto();
        this._manager.connectObject('changed', () => {
            this._patchScales();
            this._connectScale();
        }, this);
    }

    // --- saving

    _fixSaving() {
        const getState = this._manager._getState;
        this._manager._getState = function () {
            const states = getState.call(this);
            for (const {id} of states) {
                for (const key of ID_KEYS) {
                    if (id[key] === '')
                        id[key] = null;
                }
            }
            return states;
        };
        this._manager._setState = function (values) {
            global.set_persistent_state('brightnesses', new GLib.Variant('a(sssssd)',
                [...values].map(({id, brightness}) => [...id.toTuple().map(s => s ?? ''), brightness])));
        };
    }

    // --- the slider's curve

    // MonitorBrightnessScale is not exported: its prototype is reached through a scale, once there is one
    _patchScales() {
        const [first] = this._manager._monitorScales.values();
        if (!first || this._scaleProto)
            return;
        const proto = this._scaleProto = Object.getPrototypeOf(first);
        this._scaleMethods = {
            _setRelativeBrightness: proto._setRelativeBrightness,
            _getRelativeBrightness: proto._getRelativeBrightness,
        };
        proto._setRelativeBrightness = function (backlight, brightness) {
            const {brightnessMin: min, brightnessMax: max} = backlight;
            backlight.brightness = Math.round(min + (max - min) * Math.clamp(brightness, 0, 1) ** GAMMA);
        };
        proto._getRelativeBrightness = function (backlight) {
            const {brightness, brightnessMin: min, brightnessMax: max} = backlight;
            return Math.clamp((brightness - min) / (max - min), 0, 1) ** (1 / GAMMA);
        };
        // the scales took their values before: what was saved (the shell's own lookup missed it, see above), or the
        // backlight as it is, on the curve
        for (const scale of this._manager._monitorScales.values()) {
            const saved = this._manager._getSavedBrightnesses([scale.monitor]).get(scale.monitor);
            if (saved >= 0) {
                scale.value = saved;
            } else {
                scale._currentBacklightBrightness = -1;
                scale.syncWithBacklight();
            }
        }
    }

    _unpatchScales() {
        if (!this._scaleProto)
            return;
        Object.assign(this._scaleProto, this._scaleMethods);
        this._scaleProto = null;
        // the backlight stays where it is, the scales go back to the shell's straight line
        for (const scale of this._manager._monitorScales.values()) {
            scale._currentBacklightBrightness = -1;
            scale.syncWithBacklight();
        }
    }

    // --- auto

    _initAuto() {
        this._active = false;
        this._gsdTarget = this._manager._abTarget;
        this._luxGoal = -1;          // the slider for the light, -1 until a reading
        this._offset = 0;            // the owner's drag from it
        this._target = -1;           // where the slider is easing to
        this._setting = false;
        this._easeId = 0;

        // a new choice of auto starts it from its curve
        this._power = new Gio.Settings({schema_id: 'org.gnome.settings-daemon.plugins.power'});
        this._power.connectObject('changed::ambient-enabled', () => (this._offset = 0), this);

        const target = Object.getOwnPropertyDescriptor(Object.getPrototypeOf(this._manager), 'autoBrightnessTarget');
        this._setShellTarget = v => {
            if (this._manager._abTarget !== v)
                target.set.call(this._manager, v);
        };
        // gsd-power's D-Bus call lands here (ShellDBus.SetAutoBrightnessTarget)
        Object.defineProperty(this._manager, 'autoBrightnessTarget', {
            configurable: true,
            get: () => this._gsdTarget,
            set: v => {
                this._gsdTarget = v;
                this._update();
            },
        });
        this._connectScale();

        // gsd-power claims the light sensor while auto is on; iio-sensor-proxy's property changes reach every listener
        Gio.DBusProxy.new_for_bus(Gio.BusType.SYSTEM, Gio.DBusProxyFlags.DO_NOT_AUTO_START, null, SENSOR_BUS,
            SENSOR_PATH, SENSOR_BUS, this._cancellable, (o, res) => {
                try {
                    this._sensor = Gio.DBusProxy.new_for_bus_finish(res);
                } catch (e) {
                    if (!e.matches(Gio.IOErrorEnum, Gio.IOErrorEnum.CANCELLED))
                        console.warn(`gnome-phone: brightness: ${e.message}`);
                    return;
                }
                this._sensor.connectObject(
                    'g-properties-changed', (proxy, changed) => {
                        const lux = changed.lookup_value('LightLevel', null);
                        if (lux)
                            this._onLux(lux.unpack());
                        if (changed.lookup_value('LightLevelUnit', null))
                            this._update();
                    },
                    'notify::g-name-owner', () => this._update(), this);
                this._update();
            });
    }

    _connectScale() {
        const scale = this._manager.globalScale;
        if (scale === this._scale)
            return;
        this._scale?.disconnectObject(this);
        this._scale = scale;
        scale?.connectObject('notify::value', () => this._onScale(), this);
    }

    // lux from the sensor, else the shell's own auto brightness on gsd-power's target
    get _luxSensor() {
        return this._sensor?.g_name_owner !== null && this._sensor?.get_cached_property('LightLevelUnit')
            ?.unpack() === 'lux';
    }

    _update() {
        const usable = this._luxSensor;
        this._setShellTarget(usable ? -1 : this._gsdTarget);
        const active = usable && this._gsdTarget >= 0;
        if (active === this._active)
            return;
        this._active = active;
        this._luxGoal = -1;
        this._stopEase();
        if (active) {
            const lux = this._sensor.get_cached_property('LightLevel')?.unpack();
            if (lux !== undefined)
                this._onLux(lux);
        }
    }

    _onLux(lux) {
        if (!this._active)
            return;
        const goal = Math.clamp(LUX_ZERO + LUX_DECADE * Math.log10(1 + Math.max(lux, 0)), 0, 1);
        if (this._luxGoal >= 0 && Math.abs(goal - this._luxGoal) < LUX_HYSTERESIS)
            return;
        // the screen coming on, or auto: at once; the light changing: eased
        const first = this._luxGoal < 0;
        this._luxGoal = goal;
        this._easeTo(Math.clamp(goal + this._offset, 0, 1), first);
    }

    // the owner moved the slider (or something set the backlight): auto keeps that distance from its curve
    _onScale() {
        if (this._setting || !this._active || this._luxGoal < 0)
            return;
        this._stopEase();
        this._offset = this._scale.value - this._luxGoal;
    }

    _setScale(v) {
        this._setting = true;
        this._scale.value = v;
        this._setting = false;
    }

    _easeTo(target, now) {
        this._target = target;
        if (!this._scale)
            return;
        if (now) {
            this._stopEase();
            this._setScale(target);
            return;
        }
        if (this._easeId)
            return;
        let last = GLib.get_monotonic_time();
        this._easeId = GLib.timeout_add(GLib.PRIORITY_DEFAULT, EASE_TICK_MS, () => {
            const time = GLib.get_monotonic_time();
            const value = this._scale?.value;
            if (value === undefined) {
                this._easeId = 0;
                return GLib.SOURCE_REMOVE;
            }
            const tau = this._target > value ? EASE_UP_S : EASE_DOWN_S;
            let next = value + (this._target - value) * (1 - Math.exp(-(time - last) / 1e6 / tau));
            last = time;
            if (Math.abs(this._target - next) < EASE_DONE)
                next = this._target;
            this._setScale(next);
            if (next !== this._target)
                return GLib.SOURCE_CONTINUE;
            this._easeId = 0;
            return GLib.SOURCE_REMOVE;
        });
    }

    _stopEase() {
        if (this._easeId)
            GLib.source_remove(this._easeId);
        this._easeId = 0;
    }

    destroy() {
        this._cancellable.cancel();
        this._stopEase();
        this._manager.disconnectObject(this);
        this._scale?.disconnectObject(this);
        this._sensor?.disconnectObject(this);
        this._power.disconnectObject(this);
        delete this._manager.autoBrightnessTarget;
        // the shell's own auto brightness again, from gsd-power's last target
        this._manager.autoBrightnessTarget = this._gsdTarget;
        this._unpatchScales();
        delete this._manager._getState;
        delete this._manager._setState;
    }
}
