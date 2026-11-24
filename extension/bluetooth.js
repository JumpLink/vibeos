// Bluetooth as a phone has it, on BlueZ: the system's pairing agent, trust for what was paired, and audio devices
// connected again by themselves.
//
//   Agent        BlueZ's default agent (org.bluez.Agent1, KeyboardDisplay) for as long as the session runs, so that a
//                car or a headset can pair from wherever the phone is: a PIN to type (cars still ask for 0000 or the
//                code they show), a passkey to type, a code to compare, a code to show, a yes or no. The questions
//                are the `ui`'s (pairdialog.js); nothing is ever answered without it. AuthorizeService: BlueZ asks
//                nothing for a trusted device; one just paired here is let in; any other, paired or not, is asked
//                about. While ours is the default agent the session bus has vibeos.BluetoothAgent, by which
//                Settings knows to leave the pairing to it rather than make its own agent the default.
//   Trust        a device that becomes bonded (paired and remembered) while the session runs is made Trusted, as
//                Android trusts every bonded device: it may connect its profiles without asking, and BlueZ reconnects
//                it. Devices paired before are left as they are.
//   AutoConnect  the paired, trusted audio devices (car kits and headsets: hands-free or headset, and speakers: an
//                A2DP sink) are connected when Bluetooth comes on, when the session starts with it on, and after a
//                connection drops that nobody here ended: a link lost or the other side gone (a car switched off,
//                its Bluetooth turned off and on). Retries back off, and stop for good after RETRY_S's last step,
//                until the next of those. A connection the phone ended (Settings' Disconnect) is not retried. BlueZ's
//                own policy retries a lost link too; the two meet harmlessly (AlreadyConnected, InProgress).
//
// Every D-Bus name, path and timing is in the constants below; `bus` is the system bus in the shell, a test's bus in
// the tests (tests/bt-agent.sh).
import Gio from 'gi://Gio';
import GLib from 'gi://GLib';

const BLUEZ = 'org.bluez';
export const AGENT_PATH = '/vibeos/Shell/BluetoothAgent';
export const AGENT_NAME = 'vibeos.BluetoothAgent';
const CAPABILITY = 'KeyboardDisplay';
const REJECTED = 'org.bluez.Error.Rejected';
const CANCELED = 'org.bluez.Error.Canceled';

// the remote services that make a device an audio device the phone connects to by itself: hands-free (car kits,
// headsets), headset (HSP), an A2DP sink (speakers, car stereos)
const AUDIO_UUIDS = new Set([
    '0000111e-0000-1000-8000-00805f9b34fb',
    '00001108-0000-1000-8000-00805f9b34fb',
    '00001131-0000-1000-8000-00805f9b34fb',
    '0000110b-0000-1000-8000-00805f9b34fb',
]);

// after Bluetooth comes on, and after the session starts, before the first connection attempt (the adapter's own
// setup, and the shell's start, go first)
const POWER_ON_DELAY_S = 2;
const START_DELAY_S = 8;
// a dropped connection: the waits before each attempt, then nothing until Bluetooth comes on again (about 15 minutes
// in all; each attempt pages the device for ~5 s, which the phone's Wi-Fi shares the radio with)
const RETRY_S = [3, 10, 20, 30, 60, 60, 120, 120, 180, 300];
// a Device1.Connect() that has not answered by then is taken as failed
const CONNECT_TIMEOUT_MS = 45000;
// Connected going false: the reason comes in Device1's Disconnected signal (BlueZ 5.82+); without one by then, the
// drop counts as one nobody here made
const REASON_WAIT_MS = 1500;
// a disconnect with these reasons is retried; Local (this phone ended it), Authentication and Suspend are not
const RETRY_REASONS = new Set(['org.bluez.Reason.Timeout', 'org.bluez.Reason.Remote', 'org.bluez.Reason.Unknown',
    'unknown']);
// the questions of a pairing, which can be started again from the phone when BlueZ gives up on one (pairAgain)
const PAIRING_KINDS = new Set(['pin', 'passkey', 'confirm', 'authorize', 'display']);
// a pairing started from the phone waits this long for its question to be answered
const PAIR_TIMEOUT_MS = 120000;

const AGENT_XML = `<node>
  <interface name="org.bluez.Agent1">
    <method name="Release"/>
    <method name="RequestPinCode">
      <arg type="o" name="device" direction="in"/>
      <arg type="s" name="pincode" direction="out"/>
    </method>
    <method name="DisplayPinCode">
      <arg type="o" name="device" direction="in"/>
      <arg type="s" name="pincode" direction="in"/>
    </method>
    <method name="RequestPasskey">
      <arg type="o" name="device" direction="in"/>
      <arg type="u" name="passkey" direction="out"/>
    </method>
    <method name="DisplayPasskey">
      <arg type="o" name="device" direction="in"/>
      <arg type="u" name="passkey" direction="in"/>
      <arg type="q" name="entered" direction="in"/>
    </method>
    <method name="RequestConfirmation">
      <arg type="o" name="device" direction="in"/>
      <arg type="u" name="passkey" direction="in"/>
    </method>
    <method name="RequestAuthorization">
      <arg type="o" name="device" direction="in"/>
    </method>
    <method name="AuthorizeService">
      <arg type="o" name="device" direction="in"/>
      <arg type="s" name="uuid" direction="in"/>
    </method>
    <method name="Cancel"/>
  </interface>
</node>`;

function unpack(variant) {
    return variant?.deepUnpack?.() ?? null;
}

// BlueZ's objects, through its ObjectManager: adapters and devices with their properties, kept current by
// InterfacesAdded/Removed and PropertiesChanged. Emits to `listener`: device-added(path), device-changed(path, changed
// names), device-removed(path), adapter-changed(path, changed names), disconnected(path, reason), vanished(), and
// appeared() once a fresh GetManagedObjects is in.
class Bluez {
    constructor(bus, listener) {
        this._bus = bus;
        this._listener = listener;
        this.objects = new Map();         // path -> {interface: {property: value}}
        this._subs = [];
        this._cancel = new Gio.Cancellable();
        const sub = (iface, member, path, fn) => this._subs.push(bus.signal_subscribe(BLUEZ, iface, member, path,
            null, Gio.DBusSignalFlags.NONE, (_c, _s, p, _i, _m, params) => fn(p, params.deepUnpack())));
        sub('org.freedesktop.DBus.ObjectManager', 'InterfacesAdded', '/', (_p, [path, ifaces]) =>
            this._added(path, ifaces));
        sub('org.freedesktop.DBus.ObjectManager', 'InterfacesRemoved', '/', (_p, [path, names]) =>
            this._removed(path, names));
        sub('org.freedesktop.DBus.Properties', 'PropertiesChanged', null, (path, [iface, changed, invalidated]) =>
            this._changed(path, iface, changed, invalidated));
        sub('org.bluez.Device1', 'Disconnected', null, (path, [reason]) =>
            this._listener('disconnected', path, reason));
    }

    load() {
        this._bus.call(BLUEZ, '/', 'org.freedesktop.DBus.ObjectManager', 'GetManagedObjects', null,
            new GLib.VariantType('(a{oa{sa{sv}}})'), Gio.DBusCallFlags.NONE, -1, this._cancel, (bus, res) => {
                let objects;
                try {
                    [objects] = bus.call_finish(res).deepUnpack();
                } catch (e) {
                    if (!e.matches?.(Gio.IOErrorEnum, Gio.IOErrorEnum.CANCELLED))
                        console.warn(`vibeos: BlueZ's objects: ${e.message}`);
                    return;
                }
                this.objects.clear();
                for (const [path, ifaces] of Object.entries(objects))
                    this._added(path, ifaces, true);
                this._listener('appeared');
            });
    }

    clear() {
        this.objects.clear();
        this._listener('vanished');
    }

    _added(path, ifaces, quiet = false) {
        const obj = this.objects.get(path) ?? {};
        const isNew = !obj['org.bluez.Device1'];
        for (const [iface, props] of Object.entries(ifaces)) {
            obj[iface] = {};
            for (const [name, value] of Object.entries(props))
                obj[iface][name] = unpack(value);
        }
        this.objects.set(path, obj);
        if (!quiet && isNew && obj['org.bluez.Device1'])
            this._listener('device-added', path);
    }

    _removed(path, names) {
        const obj = this.objects.get(path);
        if (!obj)
            return;
        const wasDevice = !!obj['org.bluez.Device1'];
        for (const name of names)
            delete obj[name];
        if (!Object.keys(obj).length)
            this.objects.delete(path);
        if (wasDevice && !obj['org.bluez.Device1'])
            this._listener('device-removed', path);
    }

    _changed(path, iface, changed, invalidated) {
        const props = this.objects.get(path)?.[iface];
        if (!props)
            return;
        const names = [];
        for (const [name, value] of Object.entries(changed)) {
            const v = unpack(value);
            if (props[name] !== v) {
                props[name] = v;
                names.push(name);
            }
        }
        for (const name of invalidated)
            delete props[name];
        if (!names.length)
            return;
        if (iface === 'org.bluez.Device1')
            this._listener('device-changed', path, names);
        else if (iface === 'org.bluez.Adapter1')
            this._listener('adapter-changed', path, names);
    }

    device(path) {
        return this.objects.get(path)?.['org.bluez.Device1'] ?? null;
    }

    devices() {
        return [...this.objects.entries()].filter(([, o]) => o['org.bluez.Device1'])
            .map(([path, o]) => [path, o['org.bluez.Device1']]);
    }

    adapter(path) {
        return this.objects.get(path)?.['org.bluez.Adapter1'] ?? null;
    }

    call(path, iface, method, params, timeout = -1) {
        return new Promise((resolve, reject) => {
            this._bus.call(BLUEZ, path, iface, method, params, null, Gio.DBusCallFlags.NONE, timeout, this._cancel,
                (bus, res) => {
                    try {
                        resolve(bus.call_finish(res));
                    } catch (e) {
                        reject(e);
                    }
                });
        });
    }

    setProperty(path, iface, name, variant) {
        return this.call(path, 'org.freedesktop.DBus.Properties', 'Set', new GLib.Variant('(ssv)', [iface, name,
            variant]));
    }

    destroy() {
        this._cancel.cancel();
        for (const id of this._subs)
            this._bus.signal_unsubscribe(id);
        this._subs = [];
        this.objects.clear();
    }
}

// the name a device goes by in a question: its alias (its own name, or the one given it; BlueZ's alias of a device
// without a name is its address, as Android shows it then)
export function deviceName(props) {
    return props?.Alias || props?.Name || '';
}

function isAudio(props) {
    return (props?.UUIDs ?? []).some(u => AUDIO_UUIDS.has(u.toLowerCase()));
}

function autoConnectable(props) {
    return !!props && props.Paired && props.Trusted && !props.Blocked && isAudio(props);
}

// BlueZ's error name, or the message for anything else
function errorName(e) {
    return Gio.DBusError.get_remote_error(e) ?? e.message;
}

export class Bluetooth {
    // bus: where BlueZ is (the system bus); sessionBus: where vibeos.BluetoothAgent is owned (null: nowhere);
    // ui: the questions (pairdialog.js's PairingUI or a test's), ui.ask(request) -> {answer: Promise, close(reason),
    // update(entered)}; answer resolves to the reply (a string, a number, true) or null for no; close's reason is
    // 'canceled' when BlueZ gave up on the question (the other device's time ran out, or it stopped), else null.
    // A pairing's question has retry() -> false when the device is gone: the pairing started again from the phone.
    // timing: the waits, in ms, in place of the constants (the tests)
    constructor({bus = Gio.DBus.system, sessionBus = Gio.DBus.session, ui, autoConnect = true, timing = {}} = {}) {
        this._t = {
            start: START_DELAY_S * 1000, powerOn: POWER_ON_DELAY_S * 1000, retry: RETRY_S.map(s => s * 1000),
            reasonWait: REASON_WAIT_MS, connect: CONNECT_TIMEOUT_MS, ...timing,
        };
        this._bus = bus;
        this._sessionBus = sessionBus;
        this._ui = ui;
        this._autoConnectOn = autoConnect;
        this._registered = false;
        this._nameId = 0;
        this._request = null;           // {invocation, question, device}
        this._showing = null;           // a code shown (DisplayPinCode/DisplayPasskey): {device, question}
        this._answered = new Set();     // devices this agent said yes to, to trust once they are bonded
        this._retry = new Map();        // device path -> {attempt, timer}
        this._connecting = new Set();
        this._reasonWait = new Map();   // device path -> timeout id
        this._reasons = new Map();      // device path -> the reason of a disconnection not yet seen in Connected
        this._timers = new Set();

        this._impl = Gio.DBusExportedObject.wrapJSObject(AGENT_XML, this);
        this._impl.export(bus, AGENT_PATH);
        this._bluez = new Bluez(bus, (what, ...args) => this._event(what, ...args));
        this._watch = Gio.bus_watch_name_on_connection(bus, BLUEZ, Gio.BusNameWatcherFlags.NONE,
            () => this._appeared(), () => this._vanished());
    }

    // ---- BlueZ coming and going -------------------------------------------------------------------------------

    _appeared() {
        this._bluez.load();
        this._register();
    }

    _vanished() {
        this._registered = false;
        this._unownName();
        this._endRequest(null);
        this._closeShown();
        for (const path of [...this._retry.keys()])
            this._stopRetry(path);
        this._bluez.clear();
    }

    async _register() {
        try {
            await this._bluez.call('/org/bluez', 'org.bluez.AgentManager1', 'RegisterAgent',
                new GLib.Variant('(os)', [AGENT_PATH, CAPABILITY]));
        } catch (e) {
            // AlreadyExists: still ours from before (the shell's connection is the same)
            if (errorName(e) !== 'org.bluez.Error.AlreadyExists') {
                console.warn(`vibeos: Bluetooth agent not registered: ${e.message}`);
                return;
            }
        }
        try {
            await this._bluez.call('/org/bluez', 'org.bluez.AgentManager1', 'RequestDefaultAgent',
                new GLib.Variant('(o)', [AGENT_PATH]));
        } catch (e) {
            console.warn(`vibeos: Bluetooth agent not the default: ${e.message}`);
            return;
        }
        this._registered = true;
        this._ownName();
    }

    _ownName() {
        if (this._nameId || !this._sessionBus)
            return;
        this._nameId = Gio.bus_own_name_on_connection(this._sessionBus, AGENT_NAME, Gio.BusNameOwnerFlags.NONE,
            null, null);
    }

    _unownName() {
        if (this._nameId)
            Gio.bus_unown_name(this._nameId);
        this._nameId = 0;
    }

    get registered() {
        return this._registered;
    }

    // ---- the agent ------------------------------------------------------------------------------------------

    _props(devicePath) {
        return this._bluez.device(devicePath) ?? {};
    }

    // a question for the ui, answered through `reply` (the answer, or null for no)
    _ask(invocation, question, reply) {
        if (this._request) {
            // BlueZ asks one thing at a time; a second is a different BlueZ's
            invocation.return_dbus_error(REJECTED, 'Busy');
            return;
        }
        const device = question.device;
        let handle;
        try {
            handle = this._ui.ask(question);
        } catch (e) {
            logError(e, 'vibeos: Bluetooth pairing question');
            invocation.return_dbus_error(REJECTED, 'No dialog');
            return;
        }
        const request = {invocation, handle, device};
        this._request = request;
        handle.answer.then(answer => {
            if (this._request !== request)
                return;
            this._request = null;
            if (answer === null || answer === undefined || answer === false) {
                invocation.return_dbus_error(REJECTED, 'Rejected by the user');
                return;
            }
            this._answered.add(device);
            invocation.return_value(reply(answer));
        }).catch(e => {
            logError(e, 'vibeos: Bluetooth pairing answer');
            if (this._request === request) {
                this._request = null;
                invocation.return_dbus_error(REJECTED, 'No answer');
            }
        });
    }

    // the request in progress ends without an answer: BlueZ cancelled it (@reason 'canceled'), or went
    _endRequest(error, reason = null) {
        const request = this._request;
        this._request = null;
        if (!request)
            return;
        request.handle.close(reason);
        if (error)
            request.invocation.return_dbus_error(error, 'Canceled');
    }

    _question(kind, devicePath, extra = {}) {
        const props = this._props(devicePath);
        const question = {kind, device: devicePath, name: deviceName(props), icon: props.Icon ?? '', ...extra};
        if (PAIRING_KINDS.has(kind))
            question.retry = () => this.pairAgain(devicePath);
        return question;
    }

    // A pairing BlueZ gave up on (a car's PIN asked while the phone was locked: 30 s), started again from the phone:
    // BlueZ asks this agent the question again, and the device is connected once paired, as Settings' Pair does.
    // False: BlueZ has forgotten the device (an unpaired one goes soon after its connection), nothing started.
    pairAgain(path) {
        const props = this._bluez.device(path);
        if (!props)
            return false;
        if (props.Paired)
            return true;
        this._bluez.call(path, 'org.bluez.Device1', 'Pair', null, PAIR_TIMEOUT_MS)
            .then(() => this._bluez.call(path, 'org.bluez.Device1', 'Connect', null, this._t.connect))
            .catch(e => console.log(`vibeos: Bluetooth: pairing again did not finish: ${errorName(e)}`));
        return true;
    }

    ReleaseAsync(_params, invocation) {
        this._registered = false;
        this._unownName();
        this._endRequest(CANCELED);
        this._closeShown();
        invocation.return_value(null);
    }

    RequestPinCodeAsync([device], invocation) {
        this._ask(invocation, this._question('pin', device), pin => new GLib.Variant('(s)', [String(pin)]));
    }

    RequestPasskeyAsync([device], invocation) {
        this._ask(invocation, this._question('passkey', device), key => new GLib.Variant('(u)', [Number(key)]));
    }

    RequestConfirmationAsync([device, passkey], invocation) {
        this._ask(invocation, this._question('confirm', device, {code: String(passkey).padStart(6, '0')}),
            () => null);
    }

    RequestAuthorizationAsync([device], invocation) {
        this._ask(invocation, this._question('authorize', device), () => null);
    }

    // BlueZ asks this only for a device that is not trusted (it lets a trusted one in itself). One this agent has
    // just paired, its Trusted not yet set, is let in: that is the car connecting hands-free and audio right after
    // the pairing. Anything else is asked about.
    AuthorizeServiceAsync([device, uuid], invocation) {
        this._fresh(device).then(props => {
            if (props.Paired && !props.Blocked && (props.Trusted || this._answered.has(device))) {
                invocation.return_value(null);
                return;
            }
            this._ask(invocation, this._question('service', device, {uuid: uuid.toLowerCase()}), () => null);
        });
    }

    // a device's properties as BlueZ has them now (the cache's PropertiesChanged lags its agent calls)
    async _fresh(device) {
        try {
            const reply = await this._bluez.call(device, 'org.freedesktop.DBus.Properties', 'GetAll',
                new GLib.Variant('(s)', ['org.bluez.Device1']));
            const [props] = reply.deepUnpack();
            return Object.fromEntries(Object.entries(props).map(([k, v]) => [k, unpack(v)]));
        } catch {
            return this._props(device);
        }
    }

    // a code to show: nothing is answered (BlueZ goes on at once), the code stays up until the pairing ends
    DisplayPinCodeAsync([device, pin], invocation) {
        invocation.return_value(null);
        this._show(this._question('display', device, {code: pin}));
    }

    DisplayPasskeyAsync([device, passkey, entered], invocation) {
        invocation.return_value(null);
        const code = String(passkey).padStart(6, '0');
        if (this._showing?.device === device && this._showing.question.code === code) {
            this._showing.handle.update?.(entered);
            return;
        }
        this._show(this._question('display', device, {code, entered}));
    }

    CancelAsync(_params, invocation) {
        this._endRequest(CANCELED, 'canceled');
        this._closeShown('canceled');
        invocation.return_value(null);
    }

    _show(question) {
        this._closeShown();
        let handle;
        try {
            handle = this._ui.ask(question);
        } catch (e) {
            logError(e, 'vibeos: Bluetooth pairing code');
            return;
        }
        const shown = {device: question.device, question, handle};
        this._showing = shown;
        this._answered.add(question.device);
        handle.answer.then(answer => {
            if (this._showing !== shown)
                return;
            this._showing = null;
            // Cancel pressed under a code: the pairing stops
            if (answer === null || answer === undefined || answer === false) {
                this._bluez.call(question.device, 'org.bluez.Device1', 'CancelPairing', null)
                    .catch(e => console.log(`vibeos: Bluetooth pairing not cancelled: ${errorName(e)}`));
            }
        }).catch(() => {});
    }

    _closeShown(reason = null) {
        const shown = this._showing;
        this._showing = null;
        shown?.handle.close(reason);
    }

    // ---- BlueZ's events -------------------------------------------------------------------------------------

    _event(what, path, arg) {
        try {
            switch (what) {
            case 'appeared':
                this._sessionStart();
                break;
            case 'device-changed':
                this._deviceChanged(path, arg);
                break;
            case 'device-removed':
                this._stopRetry(path);
                this._answered.delete(path);
                if (this._request?.device === path)
                    this._endRequest(CANCELED);
                if (this._showing?.device === path)
                    this._closeShown();
                break;
            case 'adapter-changed':
                if (arg.includes('Powered'))
                    this._poweredChanged(path);
                break;
            case 'disconnected':
                this._disconnectReason(path, arg);
                break;
            }
        } catch (e) {
            logError(e, `vibeos: Bluetooth ${what}`);
        }
    }

    _deviceChanged(path, names) {
        const props = this._bluez.device(path);
        if (!props)
            return;
        // bonded now: trusted, as every bonded device is on a phone. Only a bonding seen to happen: devices paired
        // before keep what they had. (A BlueZ without Bonded: Paired.)
        const bondedNow = 'Bonded' in props ? names.includes('Bonded') && props.Bonded
            : names.includes('Paired') && props.Paired;
        if (bondedNow) {
            // a bonding has the user's yes behind it (a question answered here, or Pair tapped in Settings): its
            // services connect unasked until Trusted is in
            this._answered.add(path);
            if (this._showing?.device === path)
                this._closeShown();
            if (!props.Trusted && !props.Blocked) {
                this._bluez.setProperty(path, 'org.bluez.Device1', 'Trusted', new GLib.Variant('b', true))
                    .catch(e => console.warn(`vibeos: Bluetooth: a paired device not trusted: ${e.message}`));
            }
        }
        if (names.includes('Paired') && !props.Paired) {
            this._answered.delete(path);
            this._stopRetry(path);
        }
        if (names.includes('Connected')) {
            if (props.Connected) {
                this._stopRetry(path);
                this._cancelReasonWait(path);
                this._reasons.delete(path);
            } else {
                this._dropped(path);
            }
        }
        if (names.includes('Trusted') && !props.Trusted)
            this._stopRetry(path);
    }

    // ---- connecting the audio devices -----------------------------------------------------------------------

    _poweredAdapters() {
        return [...this._bluez.objects.entries()].filter(([, o]) => o['org.bluez.Adapter1']?.Powered)
            .map(([path]) => path);
    }

    _candidates(adapterPath = null) {
        return this._bluez.devices().filter(([path, props]) => autoConnectable(props) && !props.Connected &&
            (!adapterPath || props.Adapter === adapterPath || path.startsWith(`${adapterPath}/`)))
            .map(([path]) => path);
    }

    _sessionStart() {
        if (!this._autoConnectOn)
            return;
        for (const adapter of this._poweredAdapters())
            this._later(this._t.start, () => this._connectAll(adapter));
    }

    _poweredChanged(adapterPath) {
        if (!this._autoConnectOn)
            return;
        if (this._bluez.adapter(adapterPath)?.Powered) {
            this._later(this._t.powerOn, () => this._connectAll(adapterPath));
        } else {
            for (const path of [...this._retry.keys()]) {
                if (path.startsWith(`${adapterPath}/`))
                    this._stopRetry(path);
            }
        }
    }

    // the adapter's audio devices, one after another (a page at a time: two at once would share the radio)
    async _connectAll(adapterPath) {
        if (!this._bluez.adapter(adapterPath)?.Powered)
            return;
        for (const path of this._candidates(adapterPath)) {
            if (!this._bluez.adapter(adapterPath)?.Powered)
                return;
            // eslint-disable-next-line no-await-in-loop
            await this._connect(path);
        }
    }

    async _connect(path) {
        const props = this._bluez.device(path);
        if (!autoConnectable(props) || props.Connected || this._connecting.has(path) || this._request)
            return false;
        this._connecting.add(path);
        try {
            await this._bluez.call(path, 'org.bluez.Device1', 'Connect', null, this._t.connect);
            return true;
        } catch (e) {
            const name = errorName(e);
            if (name === 'org.bluez.Error.AlreadyConnected' || name === 'org.bluez.Error.InProgress')
                return true;
            console.log(`vibeos: Bluetooth: an audio device did not connect: ${name}`);
            return false;
        } finally {
            this._connecting.delete(path);
        }
    }

    // Connected went false. BlueZ sends the reason (Device1.Disconnected, 5.82+) just before; without one by
    // REASON_WAIT_MS, the drop counts as one nobody here made.
    _dropped(path) {
        const reason = this._reasons.get(path);
        this._reasons.delete(path);
        if (reason) {
            this._disconnected(path, reason);
            return;
        }
        if (this._reasonWait.has(path))
            return;
        const id = GLib.timeout_add(GLib.PRIORITY_DEFAULT, this._t.reasonWait, () => {
            this._reasonWait.delete(path);
            this._timers.delete(id);
            const late = this._reasons.get(path);
            this._reasons.delete(path);
            this._disconnected(path, late ?? 'unknown');
            return GLib.SOURCE_REMOVE;
        });
        this._reasonWait.set(path, id);
        this._timers.add(id);
    }

    _cancelReasonWait(path) {
        const id = this._reasonWait.get(path);
        if (id) {
            GLib.source_remove(id);
            this._timers.delete(id);
        }
        this._reasonWait.delete(path);
    }

    // Device1.Disconnected: kept for the Connected=false that follows it
    _disconnectReason(path, reason) {
        this._reasons.set(path, reason);
        if (this._reasonWait.has(path)) {
            this._cancelReasonWait(path);
            this._reasons.delete(path);
            this._disconnected(path, reason);
        }
    }

    _disconnected(path, reason) {
        if (!this._autoConnectOn || !RETRY_REASONS.has(reason))
            return;
        const props = this._bluez.device(path);
        if (!autoConnectable(props) || props.Connected || this._retry.has(path))
            return;
        const adapter = props.Adapter ?? path.replace(/\/dev_[^/]*$/, '');
        if (!this._bluez.adapter(adapter)?.Powered)
            return;
        this._retry.set(path, {attempt: 0, timer: 0});
        this._scheduleRetry(path);
    }

    _scheduleRetry(path) {
        const retry = this._retry.get(path);
        if (!retry)
            return;
        if (retry.attempt >= this._t.retry.length) {
            this._retry.delete(path);
            return;
        }
        retry.timer = this._later(this._t.retry[retry.attempt], async () => {
            retry.timer = 0;
            retry.attempt++;
            if (this._retry.get(path) !== retry)
                return;
            const ok = await this._connect(path);
            if (this._retry.get(path) !== retry)
                return;
            if (ok || this._bluez.device(path)?.Connected)
                this._retry.delete(path);
            else
                this._scheduleRetry(path);
        });
    }

    _stopRetry(path) {
        const retry = this._retry.get(path);
        if (retry?.timer) {
            GLib.source_remove(retry.timer);
            this._timers.delete(retry.timer);
        }
        this._retry.delete(path);
    }

    _later(ms, fn) {
        const id = GLib.timeout_add(GLib.PRIORITY_DEFAULT, ms, () => {
            this._timers.delete(id);
            try {
                const r = fn();
                r?.catch?.(e => logError(e, 'vibeos: Bluetooth'));
            } catch (e) {
                logError(e, 'vibeos: Bluetooth');
            }
            return GLib.SOURCE_REMOVE;
        });
        this._timers.add(id);
        return id;
    }

    destroy() {
        for (const id of this._timers)
            GLib.source_remove(id);
        this._timers.clear();
        this._retry.clear();
        this._reasonWait.clear();
        this._reasons.clear();
        this._endRequest(CANCELED);
        this._closeShown();
        if (this._watch)
            Gio.bus_unwatch_name(this._watch);
        this._watch = 0;
        if (this._registered) {
            this._bus.call(BLUEZ, '/org/bluez', 'org.bluez.AgentManager1', 'UnregisterAgent',
                new GLib.Variant('(o)', [AGENT_PATH]), null, Gio.DBusCallFlags.NONE, -1, null, null);
        }
        this._registered = false;
        this._unownName();
        this._bluez.destroy();
        this._impl.unexport();
    }
}
