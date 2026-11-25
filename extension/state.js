// What the status bar, the shade header and the quick settings tiles show, read from the system's services
// (NetworkManager, ModemManager, UPower, BlueZ, gsd's rfkill, GSettings), and the toggles the tiles flip.
// One instance; 'changed' is coalesced to at most once per frame.
import 'gi://GnomeBluetooth?version=3.0';

import Gio from 'gi://Gio';
import GLib from 'gi://GLib';
import GnomeBluetooth from 'gi://GnomeBluetooth';
import GObject from 'gi://GObject';
import GUdev from 'gi://GUdev';
import Meta from 'gi://Meta';
import NM from 'gi://NM';

import * as Main from 'resource:///org/gnome/shell/ui/main.js';
import {BroadbandModem} from 'resource:///org/gnome/shell/misc/modemManager.js';
import {getRfkillManager} from 'resource:///org/gnome/shell/ui/status/rfkill.js';

import {Brightness} from './brightness.js';
import {ChargeMeter} from './chargepower.js';
import {_, formatNumber, formatPercent} from './l10n.js';
import {PrivacySwitches} from './privacy.js';
import {setUserOrientationLock, userOrientationLock} from './rotation.js';

Gio._promisify(NM.Client, 'new_async');

const UPOWER_BUS = 'org.freedesktop.UPower';
const UPOWER_PATH = '/org/freedesktop/UPower';
const DISPLAY_DEVICE = '/org/freedesktop/UPower/devices/DisplayDevice';
const UPOWER_DEVICE = 'org.freedesktop.UPower.Device';
const PPD_BUS = 'net.hadess.PowerProfiles';
const PPD_PATH = '/net/hadess/PowerProfiles';
// The torch is the LED the kernel names for the job (devicename:colour:function, the function "torch" or "flash"),
// found as gmobile and Phosh find it, so any phone with a flash LED driver has one. The tile lights it at a quarter
// of its range: the S10e's stock torch default, 60 mA of its 240 (3 of leds-s2mpb02's 12), and never below what
// gmobile's hwdb says still shows (GM_TORCH_MIN_BRIGHTNESS).
const TORCH_FUNCTIONS = ['torch', 'flash'];
const TORCH_SHARE = 0.25;

// the torch LED as {name, level}, or null for a phone without one
function findTorch() {
    const leds = new GUdev.Client({subsystems: ['leds']}).query_by_subsystem('leds');
    for (const fn of TORCH_FUNCTIONS) {
        const led = leds.find(d => d.get_name().split(':').at(-1) === fn);
        if (!led)
            continue;
        const max = led.get_sysfs_attr_as_int('max_brightness');
        if (max <= 0)
            continue;
        const min = parseInt(led.get_property('GM_TORCH_MIN_BRIGHTNESS') ?? '1') || 1;
        return {name: led.get_name(), level: Math.min(max, Math.max(min, Math.round(max * TORCH_SHARE)))};
    }
    return null;
}

// UPower device states that mean "plugged in", until UPower's OnBattery is known: on a charger that gives about what
// the phone takes (the S10e's 500 mA with the screen on), the battery's state flips between charging and discharging
// every few seconds, and the bolt should be there as long as the charger is
const UP_CHARGING = 1;
const UP_FULLY_CHARGED = 4;
const UP_PENDING_CHARGE = 5;

// What the label says comes from what flows into the battery (chargepower.js). A charging pad is a supply of its own,
// of the kernel's type Wireless: no speed is worked out for it, it is "Wireless charging", as on One UI. UPower knows
// no Wireless type, and counts such a supply as line power by its online attribute, so the phone is plugged in (not
// OnBattery) on the pad.
// a plug-in brings a burst of uevents while the PD contract is made (500 mA first, the 9 V limit ~1 s later)
const CHARGE_SETTLE_MS = 400;

// what the lock screen and the AOD say while the battery charges: the percentage, then AOSP's "Charging slowly" and
// "Charging", One UI's "Fast charging", "Super fast charging" and "Wireless charging", and AOSP's "Not charging" (the
// battery's STATUS says the charger holds it: too warm, or a charge limit), as one sentence a language can order its
// own way; then, while the battery takes some, how much (the owner asked, 11-08). A full battery is "Charged", which
// the callers say in its place.
export function chargingText(level, speed, amps = null) {
    const format = {
        /* TRANSLATORS: the lock screen's charging line; %s is the battery's level ("75%") */
        slow: _('%s • Charging slowly'),
        /* TRANSLATORS: the lock screen's charging line; %s is the battery's level ("75%") */
        fast: _('%s • Fast charging'),
        /* TRANSLATORS: the lock screen's charging line; %s is the battery's level ("75%") */
        superfast: _('%s • Super fast charging'),
        /* TRANSLATORS: the lock screen's charging line; %s is the battery's level ("75%") */
        wireless: _('%s • Wireless charging'),
        /* TRANSLATORS: the lock screen's charging line, for a charger that is plugged in but holds the battery
           where it is (too warm, or a charge limit); %s is the battery's level ("75%") */
        idle: _('%s • Not charging'),
    }[speed] ?? /* TRANSLATORS: the lock screen's charging line; %s is the battery's level ("75%") */ _('%s • Charging');
    const text = format.replace('%s', formatPercent(level));
    /* TRANSLATORS: a current the battery charges at; %s is the number of amperes ("1.5") */
    return amps >= 0.05 ? `${text} • ${_('%s A').replace('%s', formatNumber(amps, 1))}` : text;
}

// the icon's 5 Wi-Fi levels (0-4) from NetworkManager's 0..100 strength
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

// The network type Android shows left of the signal while mobile data is connected (MobileMappings: 5G, LTE, H+…),
// from ModemManager's access technologies, the highest one set (MMModemAccessTechnology's bits)
const DATA_TYPES = [
    [1 << 15, '5G'],                    // 5GNR
    [1 << 14, 'LTE'],
    [1 << 9, 'H+'],                     // HSPA+
    [1 << 6 | 1 << 7 | 1 << 8, 'H'],    // HSDPA, HSUPA, HSPA
    [1 << 5, '3G'],                     // UMTS
    [1 << 4, 'E'],                      // EDGE
    [1 << 3 | 1 << 1, 'G'],             // GPRS, GSM
];
const MM_MODEM_STATE_CONNECTED = 11;    // MMModemState: a packet data bearer is connected

export function dataType(technologies) {
    return DATA_TYPES.find(([bits]) => technologies & bits)?.[1] ?? '';
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
        this.mobile = {present: false, level: 0, operator: '', tech: '', dataConnected: false, dataEnabled: false,
            dataType: ''};
        this.battery = {present: false, level: 0, plugged: false, speed: null, amps: null,
            icon: 'battery-missing-symbolic'};
        this.powerSave = null;          // null: no power-profiles-daemon
        this.bluetooth = {available: false, powered: false, connected: false, name: ''};
        this.airplane = false;
        this.torch = null;              // null: no torch LED
        this._torch = findTorch();
        // Settings' Microphone and Camera switches, for every app (privacy.js): true while one is off
        this._privacy = new PrivacySwitches(() => this._changed());

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
        // no brightness pop-up, as on Android: the shade's slider is the only one. The shell's manager shows its OSD
        // for every change it is asked for, the slider's included (BrightnessManager._sync)
        Main.brightnessManager._showOSD = () => {};
        // saving that works without an EDID, a slider with room at the dark end, auto from the lux
        this._brightness = new Brightness();

        this._initNetwork();
        this._initBattery();
        this._initBluetooth();
        this._initRfkill();
        this._syncTorch();
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
                console.warn(`vibeos: no NetworkManager: ${e.message}`);
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
            'notify::state', () => this._syncNetwork(),
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
                        'notify::operator-name', () => this._readOperatorNames(),
                        'notify::access-technology', () => this._syncNetwork(),
                        this);
                } catch (e) {
                    console.warn(`vibeos: no modem: ${e.message}`);
                }
            }
            this._watchModem(modem?.udi);
            this._readOperatorNames();
        }
        this._syncNetwork();
    }

    // ModemManager's own view of the modem, for what the shell's BroadbandModem keeps to itself: the access
    // technologies as they are (it sums them up as 4G, 3G…) and the modem's state, connected while a bearer is. Its
    // properties are pushed as they change: nothing is polled.
    _watchModem(path) {
        this._mmModem?.disconnectObject(this);
        this._mmModem = null;
        if (!path?.startsWith('/org/freedesktop/ModemManager1/'))
            return;
        Gio.DBusProxy.new(Gio.DBus.system, Gio.DBusProxyFlags.DO_NOT_AUTO_START, null, 'org.freedesktop.ModemManager1',
            path, 'org.freedesktop.ModemManager1.Modem', this._cancellable, (o, res) => {
                let proxy;
                try {
                    proxy = Gio.DBusProxy.new_finish(res);
                } catch (e) {
                    if (!e.matches(Gio.IOErrorEnum, Gio.IOErrorEnum.CANCELLED))
                        console.warn(`vibeos: no ModemManager modem: ${e.message}`);
                    return;
                }
                if (path !== this._modemDevice?.udi)
                    return;     // another modem by now
                this._mmModem = proxy;
                proxy.connectObject('g-properties-changed', () => this._syncNetwork(), this);
                this._syncNetwork();
            });
    }

    // The mobile network's name, as Android has it. A network that sends no name of its own leaves ModemManager
    // with its code ("26203"), and the shell's BroadbandModem then picks a name from mobile-broadband-provider-info,
    // which lists several operators under one code: it said "sipgate" for a LycaMobile SIM. So, read from
    // ModemManager directly: the SIM's own name for its operator (its SPN, what Android shows at home), and the
    // network's name while roaming, when it has a real one.
    async _readOperatorNames() {
        const path = this._modemDevice?.udi;
        if (!path?.startsWith('/org/freedesktop/ModemManager1/')) {
            this._simOperator = this._networkOperator = '';
            return;
        }
        const get = async (object, iface, prop) => {
            try {
                const reply = await Gio.DBus.system.call('org.freedesktop.ModemManager1', object,
                    'org.freedesktop.DBus.Properties', 'Get', new GLib.Variant('(ss)', [iface, prop]),
                    null, Gio.DBusCallFlags.NONE, 2000, this._cancellable);
                return reply.deepUnpack()[0].deepUnpack();
            } catch {
                return null;
            }
        };
        const sim = await get(path, 'org.freedesktop.ModemManager1.Modem', 'Sim');
        const spn = sim && sim !== '/' ? await get(sim, 'org.freedesktop.ModemManager1.Sim', 'OperatorName') : null;
        const network = await get(path, 'org.freedesktop.ModemManager1.Modem.Modem3gpp', 'OperatorName');
        const registration = await get(path, 'org.freedesktop.ModemManager1.Modem.Modem3gpp', 'RegistrationState');
        if (path !== this._modemDevice?.udi)
            return;             // another modem by now
        this._simOperator = spn ?? '';
        // a name, not the code; MMModem3gppRegistrationState 5 is roaming
        this._networkOperator = network && !/^\d+$/.test(network) ? network : '';
        this._roaming = registration === 5;
        this._syncNetwork();
    }

    // home: the SIM's name, else the network's; roaming: the network's first. The shell's guess only when neither
    // has one.
    _operatorName() {
        const sim = this._simOperator ?? '', network = this._networkOperator ?? '';
        const name = this._roaming ? network || sim : sim || network;
        return name || this._modem?.operator_name || '';
    }

    _syncAccessPoint() {
        this._accessPoint?.disconnectObject(this);
        this._accessPoint = this._wifiDevice?.active_access_point ?? null;
        this._accessPoint?.connectObject('notify::strength', () => this._syncNetwork(), this);
        this._syncNetwork();
    }

    // Whether the phone's data goes over the modem, as Android shows the network type only while mobile data is the
    // default network: with Wi-Fi connected, NetworkManager keeps the modem's bearer up beside it, on a route of a
    // higher metric, and the data goes over Wi-Fi
    _dataOverModem() {
        const primary = this._client.primary_connection;
        if (!primary || !this._modemDevice)
            return false;
        const type = primary.get_connection_type();
        if (type === 'gsm' || type === 'cdma' || primary.get_devices().includes(this._modemDevice))
            return true;
        // a VPN goes over another connection: the modem's, while no connection but the modem's is up beside it
        if (!primary.vpn && type !== 'wireguard')
            return false;
        return this._client.get_active_connections().every(c => c === primary ||
            c.state !== NM.ActiveConnectionState.ACTIVATED || c.get_connection_type() === 'loopback' ||
            c.get_devices().includes(this._modemDevice));
    }

    _syncNetwork() {
        const client = this._client;
        const wifiState = this._wifiDevice?.state ?? NM.DeviceState.UNAVAILABLE;
        // the access point the device has now, whichever change came first: one taken from an earlier notification
        // can be gone, or not there yet
        if ((this._wifiDevice?.active_access_point ?? null) !== this._accessPoint) {
            this._syncAccessPoint();
            return;
        }
        const ap = this._accessPoint;
        let name = '';
        if (ap?.ssid)
            name = NM.utils_ssid_to_utf8(ap.ssid.get_data());
        this.wifi = {
            available: !!this._wifiDevice,
            enabled: client.wireless_enabled,
            // connected is the device's state alone: an access point not known yet only leaves the fan empty
            connected: wifiState === NM.DeviceState.ACTIVATED,
            level: ap ? wifiLevel(ap.strength) : 0,
            name,
        };

        const modemState = this._modemDevice?.state ?? NM.DeviceState.UNAVAILABLE;
        const tech = this._modem?.accessTechnology ?? '';
        const mm = this._mmModem;
        const bearerUp = mm?.get_cached_property('State')?.unpack() === MM_MODEM_STATE_CONNECTED;
        const overModem = bearerUp && this._dataOverModem();
        this.mobile = {
            present: !!this._modemDevice,
            level: mobileLevel(this._modem?.signal_quality ?? 0),
            operator: this._operatorName(),
            tech: tech === 'unknown' || tech === 'other' ? '' : tech,
            dataConnected: modemState === NM.DeviceState.ACTIVATED,
            dataEnabled: client.wwan_enabled && modemState === NM.DeviceState.ACTIVATED,
            dataType: overModem ? dataType(mm.get_cached_property('AccessTechnologies')?.unpack() ?? 0) : '',
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
        // UPower says nothing when only the charger's contract changes: the kernel's power_supply uevents do. The meter
        // reads the supplies again for them, and every few seconds while a charger is in (never while none is)
        this._charge = new ChargeMeter({onChange: () => this._syncBattery()});
        this._chargeSpeedId = 0;
        this._udev = new GUdev.Client({subsystems: ['power_supply']});
        this._udev.connectObject('uevent', () => {
            if (this._chargeSpeedId)
                return;
            this._chargeSpeedId = GLib.timeout_add(GLib.PRIORITY_DEFAULT, CHARGE_SETTLE_MS, () => {
                this._chargeSpeedId = 0;
                this._charge.refresh();
                return GLib.SOURCE_REMOVE;
            });
        }, this);
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
        // power-profiles-daemon is ordered after multi-user.target, so it can come up long after the shell (48 s
        // into a Poco boot). Starting it while making the proxy then times out after 25 s and leaves Battery Saver
        // unavailable for the whole session. So the proxy is made at once and follows the name instead: GDBusProxy
        // loads the properties whenever the daemon appears, notifies g-name-owner after that, and clears them when
        // it goes.
        Gio.DBusProxy.new(Gio.DBus.system, Gio.DBusProxyFlags.DO_NOT_AUTO_START_AT_CONSTRUCTION, null,
            PPD_BUS, PPD_PATH, PPD_BUS, this._cancellable, (o, res) => {
                try {
                    this._ppd = Gio.DBusProxy.new_finish(res);
                } catch (e) {
                    return;
                }
                this._ppd.connectObject(
                    'g-properties-changed', () => this._syncPowerSave(),
                    'notify::g-name-owner', () => this._syncPowerSave(), this);
                this._syncPowerSave();
                // not running yet: ask the bus to start it, without waiting for the answer (a daemon that is not
                // started at boot still comes up; one that is already on its way is picked up by the name)
                if (!this._ppd.g_name_owner) {
                    Gio.DBus.system.call('org.freedesktop.DBus', '/org/freedesktop/DBus', 'org.freedesktop.DBus',
                        'StartServiceByName', new GLib.Variant('(su)', [PPD_BUS, 0]), null, Gio.DBusCallFlags.NONE,
                        -1, this._cancellable, (bus, r) => {
                            try {
                                bus.call_finish(r);
                            } catch (e) {
                                // a timeout or no such service: the tile stays unavailable until the name appears
                            }
                        });
                }
            });
    }

    _syncBattery() {
        if (!this._upower)
            return;
        const prop = name => this._upower.get_cached_property(name)?.unpack();
        const onBattery = this._upowerDaemon?.get_cached_property('OnBattery')?.unpack();
        const plugged = onBattery !== undefined ? !onBattery
            : [UP_CHARGING, UP_FULLY_CHARGED, UP_PENDING_CHARGE].includes(prop('State') ?? 0);
        this._charge.setPlugged(plugged);
        this.battery = {
            present: !!prop('IsPresent'),
            level: Math.round(prop('Percentage') ?? 0),
            plugged,
            speed: plugged ? this._charge.reading.speed : null,
            amps: plugged ? this._charge.reading.amps : null,
            icon: prop('IconName') || 'battery-missing-symbolic',
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

    // --- the torch

    // what the LED is at, read back from sysfs: the tile shows that, not what was asked for
    _syncTorch() {
        if (!this._torch)
            return;
        Gio.File.new_for_path(`/sys/class/leds/${this._torch.name}/brightness`).load_contents_async(this._cancellable,
            (file, res) => {
                try {
                    const [, bytes] = file.load_contents_finish(res);
                    this.torch = parseInt(new TextDecoder().decode(bytes)) > 0;
                } catch (e) {
                    if (e.matches(Gio.IOErrorEnum, Gio.IOErrorEnum.CANCELLED))
                        return;
                    this.torch = null;
                }
                this._changed();
            });
    }

    // logind lets the active session set its seat's LEDs, so no udev rule is needed
    setTorch(on) {
        if (this.torch === null)
            return;
        this.torch = on;
        this._changed();
        Gio.DBus.system.call('org.freedesktop.login1', '/org/freedesktop/login1/session/auto',
            'org.freedesktop.login1.Session', 'SetBrightness',
            new GLib.Variant('(ssu)', ['leds', this._torch.name, on ? this._torch.level : 0]),
            null, Gio.DBusCallFlags.NONE, -1, this._cancellable, (bus, res) => {
                try {
                    bus.call_finish(res);
                } catch (e) {
                    if (e.matches(Gio.IOErrorEnum, Gio.IOErrorEnum.CANCELLED))
                        return;
                    console.warn(`vibeos: torch: ${e.message}`);
                }
                this._syncTorch();
            });
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

    // Android's auto-rotate is the inverse of GNOME's orientation lock, as the user set it (rotation.js may hold the
    // lock for the lock screen and home)
    get autoRotate() {
        return !userOrientationLock(this._touchscreen);
    }

    set autoRotate(v) {
        setUserOrientationLock(this._touchscreen, !v);
        this._changed();
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

    get micOff() {
        return this._privacy.micOff;
    }

    get cameraOff() {
        return this._privacy.cameraOff;
    }

    destroy() {
        this._privacy.destroy();
        this._cancellable.cancel();
        if (this._changedId)
            global.compositor.get_laters().remove(this._changedId);
        if (this._chargeSpeedId)
            GLib.source_remove(this._chargeSpeedId);
        this._charge.destroy();
        this._udev?.disconnectObject(this);
        this._udev = null;
        Main.brightnessManager.globalScale?.disconnectObject(this);
        delete Main.brightnessManager._showOSD;
        this._brightness.destroy();
        for (const obj of [this._notifications, this._interface, this._location, this._touchscreen, this._power,
            this._color, this._client, this._wifiDevice, this._accessPoint, this._modemDevice, this._modem, this._mmModem,
            this._upower, this._upowerDaemon, this._ppd, this._bt, this._rfkill])
            obj?.disconnectObject(this);
        const devices = this._bt.get_devices();
        for (let i = 0; i < devices.get_n_items(); i++)
            devices.get_item(i).disconnectObject(this);
        this._bt = null;
        this._client = null;
    }
});
