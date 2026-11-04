// What the status bar, the shade header and the quick settings tiles show, read from the system's services
// (NetworkManager, ModemManager, UPower, BlueZ, gsd's rfkill, GSettings), and the toggles the tiles flip.
// One instance; 'changed' is coalesced to at most once per frame.
import 'gi://GnomeBluetooth?version=3.0';

import Gio from 'gi://Gio';
import GLib from 'gi://GLib';
import GnomeBluetooth from 'gi://GnomeBluetooth';
import GObject from 'gi://GObject';
import Meta from 'gi://Meta';
import NM from 'gi://NM';

import * as Main from 'resource:///org/gnome/shell/ui/main.js';
import {BroadbandModem} from 'resource:///org/gnome/shell/misc/modemManager.js';
import {getRfkillManager} from 'resource:///org/gnome/shell/ui/status/rfkill.js';

Gio._promisify(NM.Client, 'new_async');

const UPOWER_BUS = 'org.freedesktop.UPower';
const UPOWER_PATH = '/org/freedesktop/UPower';
const DISPLAY_DEVICE = '/org/freedesktop/UPower/devices/DisplayDevice';
const UPOWER_DEVICE = 'org.freedesktop.UPower.Device';
const PPD_BUS = 'net.hadess.PowerProfiles';
const PPD_PATH = '/net/hadess/PowerProfiles';

// UPower device states that mean "plugged in", until UPower's OnBattery is known: on a charger that gives about what
// the phone takes (the S10e's 500 mA with the screen on), the battery's state flips between charging and discharging
// every few seconds, and Android's bolt is there as long as the charger is (BatteryControllerImpl.mPluggedIn)
const UP_CHARGING = 1;
const UP_FULLY_CHARGED = 4;
const UP_PENDING_CHARGE = 5;

// Android's 5 Wi-Fi levels from NetworkManager's 0..100 strength (WifiManager.calculateSignalLevel, roughly)
function wifiLevel(strength) {
    if (strength >= 80)
        return 4;
    if (strength >= 55)
        return 3;
    if (strength >= 30)
        return 2;
    if (strength >= 5)
        return 1;
    return 0;
}

// The 4-bar mobile level from ModemManager's 0..100 quality
function mobileLevel(quality) {
    if (quality >= 70)
        return 4;
    if (quality >= 50)
        return 3;
    if (quality >= 30)
        return 2;
    if (quality >= 10)
        return 1;
    return 0;
}

export const SystemState = GObject.registerClass({
    Signals: {'changed': {}},
}, class SystemState extends GObject.Object {
    constructor() {
        super();
        this.wifi = {available: false, enabled: false, connected: false, level: 0, name: ''};
        this.mobile = {present: false, level: 0, operator: '', tech: '', dataConnected: false, dataEnabled: false};
        this.battery = {present: false, level: 0, plugged: false};
        this.powerSave = null;          // null: no power-profiles-daemon
        this.bluetooth = {available: false, powered: false, connected: false, name: ''};
        this.airplane = false;

        this._cancellable = new Gio.Cancellable();
        this._changedId = 0;

        this._notifications = new Gio.Settings({schema_id: 'org.gnome.desktop.notifications'});
        this._interface = new Gio.Settings({schema_id: 'org.gnome.desktop.interface'});
        this._location = new Gio.Settings({schema_id: 'org.gnome.system.location'});
        this._touchscreen = new Gio.Settings({schema_id: 'org.gnome.settings-daemon.peripherals.touchscreen'});
        this._power = new Gio.Settings({schema_id: 'org.gnome.settings-daemon.plugins.power'});
        this._color = new Gio.Settings({schema_id: 'org.gnome.settings-daemon.plugins.color'});
        for (const settings of [this._notifications, this._interface, this._location, this._touchscreen,
            this._power, this._color])
            settings.connectObject('changed', () => this._changed(), this);
        Main.brightnessManager.globalScale?.connectObject('notify::value', () => this._changed(), this);

        this._initNetwork();
        this._initBattery();
        this._initBluetooth();
        this._initRfkill();
    }

    // coalesce bursts of property changes into one 'changed' before the next frame
    _changed() {
        if (this._changedId)
            return;
        const laters = global.compositor.get_laters();
        this._changedId = laters.add(Meta.LaterType.BEFORE_REDRAW, () => {
            this._changedId = 0;
            this.emit('changed');
            return GLib.SOURCE_REMOVE;
        });
    }

    // --- NetworkManager, ModemManager

    async _initNetwork() {
        try {
            this._client = await NM.Client.new_async(this._cancellable);
        } catch (e) {
            if (!e.matches(Gio.IOErrorEnum, Gio.IOErrorEnum.CANCELLED))
                console.warn(`gnome-phone: no NetworkManager: ${e.message}`);
            return;
        }
        this._client.connectObject(
            'notify::wireless-enabled', () => this._syncNetwork(),
            'notify::wwan-enabled', () => this._syncNetwork(),
            'notify::primary-connection', () => this._syncNetwork(),
            'notify::active-connections', () => this._syncNetwork(),
            'device-added', () => this._syncDevices(),
            'device-removed', () => this._syncDevices(),
            this);
        this._syncDevices();
    }

    _syncDevices() {
        this._wifiDevice?.disconnectObject(this);
        this._wifiDevice = this._client.get_devices().find(d => d instanceof NM.DeviceWifi) ?? null;
        this._wifiDevice?.connectObject(
            'notify::active-access-point', () => this._syncAccessPoint(),
            'state-changed', () => this._syncNetwork(),
            this);
        this._syncAccessPoint();

        const modem = this._client.get_devices().find(d => d instanceof NM.DeviceModem) ?? null;
        if (modem !== this._modemDevice) {
            this._modemDevice?.disconnectObject(this);
            this._modem?.disconnectObject(this);
            this._modemDevice = modem;
            this._modem = null;
            if (modem) {
                modem.connectObject('state-changed', () => this._syncNetwork(), this);
                try {
                    this._modem = new BroadbandModem(modem.udi, modem.current_capabilities);
                    this._modem.connectObject(
                        'notify::signal-quality', () => this._syncNetwork(),
                        'notify::operator-name', () => this._syncNetwork(),
                        'notify::access-technology', () => this._syncNetwork(),
                        this);
                } catch (e) {
                    console.warn(`gnome-phone: no modem: ${e.message}`);
                }
            }
        }
        this._syncNetwork();
    }

    _syncAccessPoint() {
        this._accessPoint?.disconnectObject(this);
        this._accessPoint = this._wifiDevice?.active_access_point ?? null;
        this._accessPoint?.connectObject('notify::strength', () => this._syncNetwork(), this);
        this._syncNetwork();
    }

    _syncNetwork() {
        const client = this._client;
        const wifiState = this._wifiDevice?.state ?? NM.DeviceState.UNAVAILABLE;
        const ap = this._accessPoint;
        let name = '';
        if (ap?.ssid)
            name = NM.utils_ssid_to_utf8(ap.ssid.get_data());
        this.wifi = {
            available: !!this._wifiDevice,
            enabled: client.wireless_enabled,
            connected: wifiState === NM.DeviceState.ACTIVATED && !!ap,
            level: ap ? wifiLevel(ap.strength) : 0,
            name,
        };

        const modemState = this._modemDevice?.state ?? NM.DeviceState.UNAVAILABLE;
        const tech = this._modem?.accessTechnology ?? '';
        this.mobile = {
            present: !!this._modemDevice,
            level: mobileLevel(this._modem?.signal_quality ?? 0),
            operator: this._modem?.operator_name ?? '',
            tech: tech === 'unknown' || tech === 'other' ? '' : tech,
            dataConnected: modemState === NM.DeviceState.ACTIVATED,
            dataEnabled: client.wwan_enabled && modemState === NM.DeviceState.ACTIVATED,
        };
        this._changed();
    }

    setWifiEnabled(enabled) {
        this._client?.dbus_set_property(NM.DBUS_PATH, NM.DBUS_INTERFACE, 'WirelessEnabled',
            new GLib.Variant('b', enabled), -1, null, null);
    }

    // Mobile data: (de)activate the modem's connection
    setMobileData(enabled) {
        const device = this._modemDevice;
        if (!device)
            return;
        if (!enabled) {
            device.disconnect_async(null, null);
            return;
        }
        if (!this._client.wwan_enabled) {
            this._client.dbus_set_property(NM.DBUS_PATH, NM.DBUS_INTERFACE, 'WwanEnabled',
                new GLib.Variant('b', true), -1, null, null);
        }
        const connection = device.get_available_connections()[0] ?? null;
        this._client.activate_connection_async(connection, device, null, null, null);
    }

    // --- UPower, power-profiles-daemon

    _initBattery() {
        Gio.DBusProxy.new(Gio.DBus.system, Gio.DBusProxyFlags.NONE, null,
            UPOWER_BUS, DISPLAY_DEVICE, UPOWER_DEVICE, this._cancellable, (o, res) => {
                try {
                    this._upower = Gio.DBusProxy.new_finish(res);
                } catch (e) {
                    return;
                }
                this._upower.connectObject('g-properties-changed', () => this._syncBattery(), this);
                this._syncBattery();
            });
        Gio.DBusProxy.new(Gio.DBus.system, Gio.DBusProxyFlags.NONE, null,
            UPOWER_BUS, UPOWER_PATH, UPOWER_BUS, this._cancellable, (o, res) => {
                try {
                    this._upowerDaemon = Gio.DBusProxy.new_finish(res);
                } catch (e) {
                    return;
                }
                this._upowerDaemon.connectObject('g-properties-changed', () => this._syncBattery(), this);
                this._syncBattery();
            });
        Gio.DBusProxy.new(Gio.DBus.system, Gio.DBusProxyFlags.NONE, null,
            PPD_BUS, PPD_PATH, PPD_BUS, this._cancellable, (o, res) => {
                try {
                    this._ppd = Gio.DBusProxy.new_finish(res);
                } catch (e) {
                    return;
                }
                this._ppd.connectObject('g-properties-changed', () => this._syncPowerSave(), this);
                this._syncPowerSave();
            });
    }

    _syncBattery() {
        if (!this._upower)
            return;
        const prop = name => this._upower.get_cached_property(name)?.unpack();
        const onBattery = this._upowerDaemon?.get_cached_property('OnBattery')?.unpack();
        this.battery = {
            present: !!prop('IsPresent'),
            level: Math.round(prop('Percentage') ?? 0),
            plugged: onBattery !== undefined ? !onBattery
                : [UP_CHARGING, UP_FULLY_CHARGED, UP_PENDING_CHARGE].includes(prop('State') ?? 0),
        };
        this._changed();
    }

    _syncPowerSave() {
        const profile = this._ppd.get_cached_property('ActiveProfile')?.unpack();
        this.powerSave = profile === undefined ? null : profile === 'power-saver';
        this._changed();
    }

    setPowerSave(enabled) {
        if (this.powerSave === null)
            return;
        this._ppd.call('org.freedesktop.DBus.Properties.Set',
            new GLib.Variant('(ssv)', [PPD_BUS, 'ActiveProfile',
                new GLib.Variant('s', enabled ? 'power-saver' : 'balanced')]),
            Gio.DBusCallFlags.NONE, -1, null, null);
    }

    // --- Bluetooth, rfkill

    _initBluetooth() {
        this._bt = new GnomeBluetooth.Client();
        this._bt.connectObject(
            'notify::default-adapter-powered', () => this._syncBluetooth(),
            'notify::default-adapter-state', () => this._syncBluetooth(),
            'notify::default-adapter', () => this._syncBluetooth(),
            'device-added', (c, device) => this._watchDevice(device),
            'device-removed', () => this._syncBluetooth(),
            this);
        const devices = this._bt.get_devices();
        for (let i = 0; i < devices.get_n_items(); i++)
            this._watchDevice(devices.get_item(i));
        this._syncBluetooth();
    }

    _watchDevice(device) {
        device.connectObject('notify::connected', () => this._syncBluetooth(), this);
        this._syncBluetooth();
    }

    _syncBluetooth() {
        const devices = this._bt.get_devices();
        let connected = null;
        for (let i = 0; i < devices.get_n_items(); i++) {
            const device = devices.get_item(i);
            if (device.connected) {
                connected = device;
                break;
            }
        }
        this.bluetooth = {
            available: this._bt.default_adapter_state !== GnomeBluetooth.AdapterState.ABSENT,
            powered: this._bt.default_adapter_powered,
            connected: !!connected,
            name: connected?.alias ?? '',
        };
        this._changed();
    }

    setBluetooth(enabled) {
        this._rfkill?._proxy.call('org.freedesktop.DBus.Properties.Set',
            new GLib.Variant('(ssv)', ['org.gnome.SettingsDaemon.Rfkill', 'BluetoothAirplaneMode',
                new GLib.Variant('b', !enabled)]),
            Gio.DBusCallFlags.NONE, -1, null, null);
        if (enabled && !this._bt.default_adapter_powered)
            this._bt.default_adapter_powered = true;
    }

    _initRfkill() {
        this._rfkill = getRfkillManager();
        const sync = () => {
            this.airplane = !!this._rfkill.airplane_mode;
            this._changed();
        };
        this._rfkill.connectObject('notify::airplane-mode', sync, this);
        sync();
    }

    setAirplane(enabled) {
        this._rfkill.airplane_mode = enabled;
    }

    // --- GSettings

    get dnd() {
        return !this._notifications.get_boolean('show-banners');
    }

    set dnd(v) {
        this._notifications.set_boolean('show-banners', !v);
    }

    get darkMode() {
        return this._interface.get_string('color-scheme') === 'prefer-dark';
    }

    set darkMode(v) {
        this._interface.set_string('color-scheme', v ? 'prefer-dark' : 'default');
    }

    get location() {
        return this._location.get_boolean('enabled');
    }

    set location(v) {
        this._location.set_boolean('enabled', v);
    }

    // Android's auto-rotate is the inverse of GNOME's orientation lock
    get autoRotate() {
        return !this._touchscreen.get_boolean('orientation-lock');
    }

    set autoRotate(v) {
        this._touchscreen.set_boolean('orientation-lock', !v);
    }

    get nightLight() {
        return this._color.get_boolean('night-light-enabled');
    }

    set nightLight(v) {
        this._color.set_boolean('night-light-enabled', v);
    }

    get autoBrightness() {
        return this._power.get_boolean('ambient-enabled');
    }

    set autoBrightness(v) {
        this._power.set_boolean('ambient-enabled', v);
    }

    // 0..1, the shell's brightness manager (backlight via gsd-power)
    get brightness() {
        return Main.brightnessManager.globalScale?.value ?? null;
    }

    set brightness(v) {
        const scale = Main.brightnessManager.globalScale;
        if (scale)
            scale.value = v;
    }

    destroy() {
        this._cancellable.cancel();
        if (this._changedId)
            global.compositor.get_laters().remove(this._changedId);
        Main.brightnessManager.globalScale?.disconnectObject(this);
        for (const obj of [this._notifications, this._interface, this._location, this._touchscreen, this._power,
            this._color, this._client, this._wifiDevice, this._accessPoint, this._modemDevice, this._modem, this._upower,
            this._upowerDaemon, this._ppd, this._bt, this._rfkill])
            obj?.disconnectObject(this);
        const devices = this._bt.get_devices();
        for (let i = 0; i < devices.get_n_items(); i++)
            devices.get_item(i).disconnectObject(this);
        this._bt = null;
        this._client = null;
    }
});
