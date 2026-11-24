// bt-agent.js (run by bt-agent.sh): bluetooth.js against a BlueZ stand-in on the test's own bus. The stand-in owns
// org.bluez on a connection of its own and has an adapter and four devices; the questions go to a stand-in for the
// dialogs (pairdialog.js), answered here. Nothing reaches a real BlueZ, adapter or device.
import Gio from 'gi://Gio';
import GLib from 'gi://GLib';
import System from 'system';

import {AGENT_NAME, AGENT_PATH, Bluetooth} from './bluetooth.js';

const HFP = '0000111e-0000-1000-8000-00805f9b34fb';
const A2DP_SINK = '0000110b-0000-1000-8000-00805f9b34fb';
const HID = '00001124-0000-1000-8000-00805f9b34fb';
const ADAPTER = '/org/bluez/hci0';
const TIMING = {start: 100, powerOn: 100, retry: [100, 100, 100, 100], reasonWait: 200, connect: 2000};

const bluezBus = Gio.DBusConnection.new_for_address_sync(GLib.getenv('DBUS_SESSION_BUS_ADDRESS'),
    Gio.DBusConnectionFlags.AUTHENTICATION_CLIENT | Gio.DBusConnectionFlags.MESSAGE_BUS_CONNECTION, null, null);

function sleep(ms) {
    return new Promise(resolve => GLib.timeout_add(GLib.PRIORITY_DEFAULT, ms, () => {
        resolve();
        return GLib.SOURCE_REMOVE;
    }));
}

async function waitFor(cond, what, ms = 3000) {
    const end = GLib.get_monotonic_time() + ms * 1000;
    while (!cond()) {
        if (GLib.get_monotonic_time() > end)
            throw new Error(`timed out waiting for: ${what}`);
        // eslint-disable-next-line no-await-in-loop
        await sleep(10);
    }
}

function check(ok, what) {
    if (!ok)
        throw new Error(`failed: ${what}`);
    print(`ok - ${what}`);
}

// ---- the BlueZ stand-in ---------------------------------------------------------------------------------------

const TYPES = {
    Alias: 's', Name: 's', Icon: 's', Adapter: 'o', UUIDs: 'as', Paired: 'b', Bonded: 'b', Trusted: 'b',
    Blocked: 'b', Connected: 'b', Powered: 'b',
};

const DEVICE_XML = `<node><interface name="org.bluez.Device1">
  <method name="Connect"/>
  <method name="Pair"/>
  <method name="CancelPairing"/>
  <property name="Alias" type="s" access="read"/>
  <property name="Name" type="s" access="read"/>
  <property name="Icon" type="s" access="read"/>
  <property name="Adapter" type="o" access="read"/>
  <property name="UUIDs" type="as" access="read"/>
  <property name="Paired" type="b" access="read"/>
  <property name="Bonded" type="b" access="read"/>
  <property name="Trusted" type="b" access="readwrite"/>
  <property name="Blocked" type="b" access="read"/>
  <property name="Connected" type="b" access="read"/>
  <signal name="Disconnected"><arg type="s"/><arg type="s"/></signal>
</interface></node>`;

const ADAPTER_XML = `<node><interface name="org.bluez.Adapter1">
  <property name="Powered" type="b" access="readwrite"/>
</interface></node>`;

const MANAGER_XML = `<node><interface name="org.bluez.AgentManager1">
  <method name="RegisterAgent"><arg type="o" direction="in"/><arg type="s" direction="in"/></method>
  <method name="UnregisterAgent"><arg type="o" direction="in"/></method>
  <method name="RequestDefaultAgent"><arg type="o" direction="in"/></method>
</interface></node>`;

const OM_XML = `<node><interface name="org.freedesktop.DBus.ObjectManager">
  <method name="GetManagedObjects"><arg type="a{oa{sa{sv}}}" direction="out"/></method>
  <signal name="InterfacesAdded"><arg type="o"/><arg type="a{sa{sv}}"/></signal>
  <signal name="InterfacesRemoved"><arg type="o"/><arg type="as"/></signal>
</interface></node>`;

class Props {
    constructor(xml, path, iface, props) {
        this.path = path;
        this.iface = iface;
        this.values = {...props};
        for (const name of Object.keys(props)) {
            Object.defineProperty(this, name, {
                get: () => this.values[name],
                set: v => this.written(name, v),
            });
        }
        this.impl = Gio.DBusExportedObject.wrapJSObject(xml, this);
        this.impl.export(bluezBus, path);
    }

    // a property the agent wrote (Properties.Set)
    written(name, value) {
        this.set(name, value);
    }

    // a change BlueZ makes, sent as PropertiesChanged at once, as BlueZ sends each (GJS holds them for an idle, where
    // a change and its undoing made before it went out as none: the agent never saw the car connect, nor drop)
    set(name, value) {
        this.values[name] = value;
        this.impl.emit_property_changed(name, new GLib.Variant(TYPES[name], value));
        this.impl.flush();
    }

    variants() {
        return Object.fromEntries(Object.entries(this.values).map(([k, v]) => [k, new GLib.Variant(TYPES[k], v)]));
    }
}

class Device extends Props {
    constructor(address, props) {
        super(DEVICE_XML, `${ADAPTER}/dev_${address}`, 'org.bluez.Device1', {
            Adapter: ADAPTER, Icon: 'audio-card', Name: props.Alias, Paired: false, Bonded: false, Trusted: false,
            Blocked: false, Connected: false, ...props,
        });
        this.connects = 0;
        this.connectResults = [];
        this.cancelPairings = 0;
        this.pairs = 0;
        this.trustWrites = [];
    }

    written(name, value) {
        if (name === 'Trusted')
            this.trustWrites.push(value);
        super.written(name, value);
    }

    ConnectAsync(_params, invocation) {
        this.connects++;
        const result = this.connectResults.shift() ?? 'ok';
        if (result !== 'ok') {
            invocation.return_dbus_error(result, 'Host is down');
            return;
        }
        invocation.return_value(null);
        this.set('Connected', true);
    }

    // paired from the phone (the agent's question answered at once)
    PairAsync(_params, invocation) {
        this.pairs++;
        invocation.return_value(null);
        this.set('Paired', true);
        this.set('Bonded', true);
    }

    CancelPairingAsync(_params, invocation) {
        this.cancelPairings++;
        invocation.return_value(null);
    }

    // a disconnection as BlueZ 5.88 tells it: the reason first, then Connected (device_remove_connection)
    drop(reason) {
        if (reason)
            this.impl.emit_signal('Disconnected', new GLib.Variant('(ss)', [reason, 'mock']));
        this.set('Connected', false);
    }
}

class Manager {
    constructor() {
        this.agent = null;
        this.defaultAgent = null;
        this.registrations = 0;
        this.unregistrations = 0;
        this.impl = Gio.DBusExportedObject.wrapJSObject(MANAGER_XML, this);
        this.impl.export(bluezBus, '/org/bluez');
    }

    RegisterAgentAsync([path, capability], invocation) {
        if (this.agent) {
            invocation.return_dbus_error('org.bluez.Error.AlreadyExists', 'Already Exists');
            return;
        }
        this.agent = {owner: invocation.get_sender(), path, capability};
        this.registrations++;
        invocation.return_value(null);
    }

    UnregisterAgentAsync([path], invocation) {
        if (this.agent?.path === path)
            this.agent = null;
        this.unregistrations++;
        invocation.return_value(null);
    }

    RequestDefaultAgentAsync([path], invocation) {
        this.defaultAgent = path;
        invocation.return_value(null);
    }
}

const adapter = new Props(ADAPTER_XML, ADAPTER, 'org.bluez.Adapter1', {Powered: true});
const car = new Device('00_00_00_00_00_01', {Alias: 'Car', UUIDs: [HFP, A2DP_SINK], Paired: true, Bonded: true,
    Trusted: true});
const headset = new Device('00_00_00_00_00_02', {Alias: 'Headset', UUIDs: [HFP], Paired: true, Bonded: true});
const keyboard = new Device('00_00_00_00_00_03', {Alias: 'Keyboard', UUIDs: [HID], Paired: true, Bonded: true,
    Trusted: true});
const newCar = new Device('00_00_00_00_00_04', {Alias: 'New car', UUIDs: [HFP, A2DP_SINK]});
const speaker = new Device('00_00_00_00_00_05', {Alias: 'Speaker', UUIDs: [A2DP_SINK]});
const stereo = new Device('00_00_00_00_00_06', {Alias: 'Stereo', UUIDs: [HFP, A2DP_SINK]});
const devices = [car, headset, keyboard, newCar, speaker, stereo];
const manager = new Manager();

const objectManager = {
    GetManagedObjectsAsync(_params, invocation) {
        const objects = {[ADAPTER]: {'org.bluez.Adapter1': adapter.variants()}};
        for (const d of devices)
            objects[d.path] = {'org.bluez.Device1': d.variants()};
        invocation.return_value(new GLib.Variant('(a{oa{sa{sv}}})', [objects]));
    },
};
Gio.DBusExportedObject.wrapJSObject(OM_XML, objectManager).export(bluezBus, '/');

let bluezName = 0;
function ownBluez() {
    return new Promise(resolve => {
        bluezName = Gio.bus_own_name_on_connection(bluezBus, 'org.bluez', Gio.BusNameOwnerFlags.NONE,
            () => resolve(), null);
    });
}

function unownBluez() {
    Gio.bus_unown_name(bluezName);
    bluezName = 0;
    manager.agent = null;
}

// a call BlueZ makes to its agent: {value} or {error: its D-Bus name}
function agentCall(method, signature, args, replyType = null) {
    return new Promise(resolve => {
        bluezBus.call(manager.agent.owner, manager.agent.path, 'org.bluez.Agent1', method,
            signature ? new GLib.Variant(signature, args) : null, replyType ? new GLib.VariantType(replyType) : null,
            Gio.DBusCallFlags.NONE, 5000, null, (bus, res) => {
                try {
                    resolve({value: bus.call_finish(res).deepUnpack()});
                } catch (e) {
                    resolve({error: Gio.DBusError.get_remote_error(e) ?? e.message});
                }
            });
    });
}

function nameOwned(name) {
    const [owned] = bluezBus.call_sync('org.freedesktop.DBus', '/org/freedesktop/DBus', 'org.freedesktop.DBus',
        'NameHasOwner', new GLib.Variant('(s)', [name]), null, Gio.DBusCallFlags.NONE, -1, null).deepUnpack();
    return owned;
}

// ---- the dialogs' stand-in ------------------------------------------------------------------------------------

const ui = {
    questions: [],
    ask(question) {
        let resolve;
        const answer = new Promise(r => (resolve = r));
        const handle = {
            question, answer, closed: false, entered: null,
            reply: value => resolve(value),
            close: reason => {
                handle.closed = true;
                handle.reason = reason ?? null;
                resolve(null);
            },
            update: entered => (handle.entered = entered),
        };
        this.questions.push(handle);
        return handle;
    },
    get last() {
        return this.questions[this.questions.length - 1];
    },
};

// ---- the tests ------------------------------------------------------------------------------------------------

async function main() {
    await ownBluez();
    const bt = new Bluetooth({bus: Gio.DBus.session, sessionBus: Gio.DBus.session, ui, timing: TIMING});

    // registered, the default, and Settings can tell
    await waitFor(() => manager.agent && manager.defaultAgent && nameOwned(AGENT_NAME), 'the agent registered');
    check(manager.agent.path === AGENT_PATH && manager.agent.capability === 'KeyboardDisplay',
        'registered as KeyboardDisplay at its path');
    check(manager.defaultAgent === AGENT_PATH, 'made the default agent');
    check(nameOwned(AGENT_NAME), `${AGENT_NAME} owned while it is the default`);

    // the session's start: the paired, trusted audio device is connected; the untrusted one and the keyboard not
    await waitFor(() => car.Connected, 'the car connected at the start');
    await sleep(300);
    check(car.connects === 1, 'the trusted car connected once at the start');
    check(headset.connects === 0 && keyboard.connects === 0, 'an untrusted headset and a keyboard not connected');
    check(headset.trustWrites.length === 0, 'a device paired before is not made trusted');

    // a PIN typed
    let reply = agentCall('RequestPinCode', '(o)', [newCar.path], '(s)');
    await waitFor(() => ui.questions.length === 1, 'the PIN question');
    check(ui.last.question.kind === 'pin' && ui.last.question.name === 'New car', 'the PIN asked for, by name');
    ui.last.reply('0000');
    let r = await reply;
    check(r.value?.[0] === '0000', 'the PIN typed goes to BlueZ');

    // ... and refused
    reply = agentCall('RequestPinCode', '(o)', [newCar.path], '(s)');
    await waitFor(() => ui.questions.length === 2, 'the second PIN question');
    ui.last.reply(null);
    r = await reply;
    check(r.error === 'org.bluez.Error.Rejected', 'Cancel answers Rejected');

    // BlueZ cancels a question (the other side gave up, or 60 s passed): the dialog goes
    reply = agentCall('RequestPasskey', '(o)', [newCar.path], '(u)');
    await waitFor(() => ui.questions.length === 3, 'the passkey question');
    check(ui.last.question.kind === 'passkey', 'a passkey asked for');
    const cancelled = ui.last;
    const busy = await agentCall('RequestConfirmation', '(ou)', [speaker.path, 1], null);
    check(busy.error === 'org.bluez.Error.Rejected' && ui.questions.length === 3,
        'a second question while one is open is refused, not stacked');
    await agentCall('Cancel', null, null);
    r = await reply;
    check(cancelled.closed && r.error === 'org.bluez.Error.Canceled', 'Cancel closes the dialog');
    check(cancelled.reason === 'canceled' && typeof cancelled.question.retry === 'function',
        'the dialog told BlueZ gave up, and the pairing can be started again');

    // a code to compare
    reply = agentCall('RequestConfirmation', '(ou)', [newCar.path, 1234], null);
    await waitFor(() => ui.questions.length === 4, 'the confirmation');
    check(ui.last.question.kind === 'confirm' && ui.last.question.code === '001234', 'the code shown with 6 digits');
    ui.last.reply(true);
    r = await reply;
    check(r.value !== undefined && !r.error, 'Pair confirms');

    // a service of a device not paired: asked, and refused
    reply = agentCall('AuthorizeService', '(os)', [speaker.path, HFP.toUpperCase()], null);
    await waitFor(() => ui.questions.length === 5, 'the service question');
    check(ui.last.question.kind === 'service' && ui.last.question.uuid === HFP, 'an unpaired device\'s service asked');
    check(!ui.last.question.retry, 'a service question is no pairing to start again');
    ui.last.reply(null);
    r = await reply;
    check(r.error === 'org.bluez.Error.Rejected', 'Don\'t allow refuses it');

    // the car just paired here connects its services before it is trusted: let in, nothing asked
    newCar.set('Paired', true);
    r = await agentCall('AuthorizeService', '(os)', [newCar.path, HFP], null);
    check(!r.error && ui.questions.length === 5, 'a device just paired here connects its services unasked');
    // ... and the headset paired before, not trusted: asked
    reply = agentCall('AuthorizeService', '(os)', [headset.path, HFP], null);
    await waitFor(() => ui.questions.length === 6, 'the headset\'s service question');
    ui.last.reply(true);
    r = await reply;
    check(!r.error, 'a paired, untrusted device asked about and allowed');

    // bonded: trusted
    newCar.set('Bonded', true);
    await waitFor(() => newCar.Trusted, 'the new car trusted');
    check(newCar.trustWrites.length === 1 && newCar.trustWrites[0] === true, 'a device bonded here is made trusted');

    // a passkey shown; the keys typed on the other side update it; Cancel stops the pairing
    r = await agentCall('DisplayPasskey', '(ouq)', [speaker.path, 42, 0], null);
    check(!r.error, 'DisplayPasskey answered at once');
    await waitFor(() => ui.questions.length === 7, 'the passkey shown');
    const shown = ui.last;
    check(shown.question.kind === 'display' && shown.question.code === '000042', 'the passkey shown with 6 digits');
    await agentCall('DisplayPasskey', '(ouq)', [speaker.path, 42, 3], null);
    await waitFor(() => shown.entered === 3, 'the keys typed shown');
    check(ui.questions.length === 7, 'the same dialog updated, not a second');
    shown.reply(null);
    await waitFor(() => speaker.cancelPairings === 1, 'the pairing cancelled');
    check(true, 'Cancel under a shown code cancels the pairing');

    // a PIN shown goes once the device is bonded, which makes it trusted
    await agentCall('DisplayPinCode', '(os)', [speaker.path, '123456'], null);
    await waitFor(() => ui.questions.length === 8, 'the PIN shown');
    const pinShown = ui.last;
    speaker.set('Paired', true);
    speaker.set('Bonded', true);
    await waitFor(() => pinShown.closed && speaker.Trusted, 'the shown PIN closed and the speaker trusted');
    check(speaker.cancelPairings === 1, 'a shown code closed by the bonding cancels nothing');

    // the car drops (out of range, switched off): connected again, through failures
    car.connectResults = ['org.bluez.Error.Failed', 'org.bluez.Error.Failed'];
    car.drop('org.bluez.Reason.Remote');
    await waitFor(() => car.Connected, 'the car back', 5000);
    await sleep(500);
    check(car.connects === 4, 'a dropped car retried until it connects (2 failures, then connected)');

    // ... the phone itself disconnects it: left alone
    car.drop('org.bluez.Reason.Local');
    await sleep(600);
    check(car.connects === 4 && !car.Connected, 'a disconnection made here is not retried');

    // Bluetooth turned on: the car connected
    adapter.set('Powered', false);
    await sleep(150);
    adapter.set('Powered', true);
    await waitFor(() => car.Connected, 'the car connected after Bluetooth came on');
    check(car.connects === 5, 'Bluetooth on connects the car');

    // a drop with no reason (a BlueZ before 5.82) counts as one nobody here made
    car.drop(null);
    await waitFor(() => car.Connected, 'the car back after a drop without a reason');
    check(car.connects === 6, 'a drop without a reason is retried');

    // retries stop with Bluetooth off
    car.connectResults = Array(10).fill('org.bluez.Error.Failed');
    car.drop('org.bluez.Reason.Timeout');
    await waitFor(() => car.connects === 7, 'the first retry');
    adapter.set('Powered', false);
    await sleep(600);
    check(car.connects === 7, 'retries stop when Bluetooth goes off');

    // ... and give up after the last step
    car.connectResults = [];
    adapter.set('Powered', true);
    await waitFor(() => car.Connected, 'the car on again');
    const before = car.connects;
    car.connectResults = Array(10).fill('org.bluez.Error.Failed');
    car.drop('org.bluez.Reason.Remote');
    await sleep(1200);
    check(car.connects === before + TIMING.retry.length, 'retries give up after their last step');

    // a pairing BlueZ gave up on, started again from the phone (the notification's tap): Pair, then Connect; a device
    // BlueZ has forgotten starts nothing
    check(!bt.pairAgain(`${ADAPTER}/dev_00_00_00_00_00_09`), 'a device BlueZ forgot is not paired again');
    check(bt.pairAgain(stereo.path), 'a pairing started again from the phone');
    await waitFor(() => stereo.pairs === 1 && stereo.Connected && stereo.Trusted, 'paired again, connected, trusted');
    check(stereo.connects === 1, 'the device paired again is connected once');

    // BlueZ restarts: registered again
    unownBluez();
    await waitFor(() => !nameOwned(AGENT_NAME), `${AGENT_NAME} released while BlueZ is gone`);
    check(!bt.registered, 'not registered while BlueZ is gone');
    await ownBluez();
    await waitFor(() => manager.registrations === 2 && nameOwned(AGENT_NAME), 'registered again');
    check(manager.defaultAgent === AGENT_PATH, 'the default agent again after BlueZ restarted');

    // turned off: unregistered, the name gone
    bt.destroy();
    await waitFor(() => manager.unregistrations === 1 && !nameOwned(AGENT_NAME), 'unregistered');
    check(true, 'destroy() unregisters the agent and releases its name');
}

let status = 0;
const loop = GLib.MainLoop.new(null, false);
main().catch(e => {
    logError(e);
    status = 1;
}).finally(() => loop.quit());
loop.run();
System.exit(status);
