// The power key's presses and releases handed straight to power.js's handler (power-keys.sh runs it on copies whose
// GNOME Shell imports are stand-ins): a short press, a hold for Agent (unlocked, locked, the screen off), the switch
// off, Agent missing, power and volume up for the power menu, power and volume down for a screenshot, a volume key on
// its own, and the double press. Agent is this test, on the test's own bus: the "hold" actions it is sent are checked.
import Gio from 'gi://Gio';
import GLib from 'gi://GLib';
import System from 'system';

import {state} from './shell.js';
import {Power} from './power.js';
import {ScreenshotChord} from './chord.js';
import {KeyLaunch} from './keylaunch.js';

const AGENT_IFACE = `<node>
  <interface name="org.freedesktop.Application">
    <method name="Activate"><arg type="a{sv}" direction="in"/></method>
    <method name="Open"><arg type="as" direction="in"/><arg type="a{sv}" direction="in"/></method>
    <method name="ActivateAction">
      <arg type="s" direction="in"/><arg type="av" direction="in"/><arg type="a{sv}" direction="in"/>
    </method>
  </interface>
</node>`;

const FEEDBACK_IFACE = `<node>
  <interface name="org.sigxcpu.Feedback">
    <method name="TriggerFeedback">
      <arg type="s" direction="in"/><arg type="s" direction="in"/><arg type="a{sv}" direction="in"/>
      <arg type="i" direction="in"/><arg type="u" direction="out"/>
    </method>
  </interface>
</node>`;

const AGENT = 'vibeos.Agent.desktop';
let holds = [];
// feedbackd on the test's bus too, so that none is started: the long press's ticks
let ticks = 0;
const feedback = Gio.DBusExportedObject.wrapJSObject(FEEDBACK_IFACE, {
    TriggerFeedback() {
        ticks++;
        return 0;
    },
});
const unpack = v => (v instanceof GLib.Variant ? v.deepUnpack() : v);
const agent = Gio.DBusExportedObject.wrapJSObject(AGENT_IFACE, {
    Activate() {},
    Open() {},
    ActivateAction(name, params) {
        holds.push(`${name} ${JSON.stringify(params.map(unpack)[0])}`);
    },
});

const wait = ms => new Promise(resolve => GLib.timeout_add(GLib.PRIORITY_DEFAULT, ms, () => {
    resolve();
    return GLib.SOURCE_REMOVE;
}));

let failed = 0;
function check(what, got, want) {
    const ok = JSON.stringify(got) === JSON.stringify(want);
    if (!ok)
        failed++;
    print(`${ok ? 'ok  ' : 'FAIL'} ${what}${ok ? '' : `: ${JSON.stringify(got)}, not ${JSON.stringify(want)}`}`);
}

// a Power with only what the key's handler uses (its constructor builds the screen-off layer and talks to mutter),
// wired as extension.js wires it: the chord, the power menu, KeyLaunch
function phone({locked = false, holdAgent = true, agentInstalled = true, asleep = false} = {}) {
    ticks = 0;
    const keyguard = {showing: locked, dismissThen: then => then()};
    const menu = {opened: 0, open() {
        this.opened++;
    }, closeNow() {}};
    const chord = new ScreenshotChord();
    chord.shots = 0;
    chord._shoot = () => chord.shots++;
    const power = Object.create(Power.prototype);
    Object.assign(power, {
        _keyguard: keyguard, _menu: menu, _chord: chord, _longPressId: 0, _agentHeld: false, doublePress: null,
        _tapAt: 0, _doublePressed: false, _lockId: 0, _state: asleep ? 'asleep' : 'awake', _wokeAt: 0,
        slept: 0, woke: 0,
    });
    chord.onPowerMenu = () => power._chordMenu();
    power.goToSleep = () => {
        power.slept++;
        power._state = 'asleep';
    };
    power.wakeUp = () => {
        power.woke++;
        power._state = 'awake';
    };
    const service = {launched: [], navigation: {recentsShown: false}, launchApp(app) {
        this.launched.push(app.get_id());
    }};
    const launch = new KeyLaunch({service, keyguard, power, shade: null});
    launch._settings.set_boolean('power-hold-agent', holdAgent);
    launch._settings.reset('power-double-press-app');
    power.doublePress = launch;
    state.apps.clear();
    if (agentInstalled)
        state.apps.set(AGENT, {get_id: () => AGENT});
    holds = [];
    return {power, menu, chord, service};
}

const press = p => p._powerKey(true, 0);
const release = p => p._powerKey(false, 0);
// a volume key: what it did of its own, so far
function volume(chord, direction) {
    const key = {acted: 0};
    chord.volumeKey(direction, () => key.acted++, false);
    return key;
}

async function main() {
    let t;

    t = phone();
    press(t.power);
    await wait(100);
    release(t.power);
    await wait(100);
    check('short press: the screen goes off', t.power.slept, 1);
    check('short press: no power menu, Agent not told', [t.menu.opened, holds], [0, []]);

    t = phone();
    press(t.power);
    await wait(700);
    check('held, unlocked: Agent opened and listening', [t.service.launched, holds], [[AGENT], ['hold [true,false]']]);
    check('held: no power menu, one tick', [t.menu.opened, ticks], [0, 1]);
    release(t.power);
    await wait(150);
    check('let go: Agent told, the screen stays on', [holds, t.power.slept], [['hold [true,false]', 'hold [false,false]'], 0]);

    t = phone({locked: true});
    press(t.power);
    await wait(700);
    release(t.power);
    await wait(150);
    check('held, locked: no window, Agent told the phone is locked', [t.service.launched, holds],
        [[], ['hold [true,true]', 'hold [false,true]']]);

    t = phone({locked: true, asleep: true});
    press(t.power);
    await wait(700);
    release(t.power);
    await wait(150);
    check('held from a dark screen: woken, Agent on the locked phone', [t.power.woke, t.power.slept, holds],
        [1, 0, ['hold [true,true]', 'hold [false,true]']]);

    t = phone({holdAgent: false});
    press(t.power);
    await wait(700);
    release(t.power);
    await wait(150);
    check('switch off: held opens the power menu', [t.menu.opened, holds, t.power.slept], [1, [], 0]);

    t = phone({agentInstalled: false});
    press(t.power);
    await wait(700);
    release(t.power);
    await wait(150);
    check('Agent not installed: held opens the power menu', [t.menu.opened, holds], [1, []]);

    t = phone();
    press(t.power);
    await wait(50);
    let key = volume(t.chord, +1);
    await wait(700);
    release(t.power);
    t.chord.volumeRelease();
    await wait(150);
    check('power, then volume up: the power menu only', [t.menu.opened, key.acted, holds, t.power.slept], [1, 0, [], 0]);

    t = phone();
    key = volume(t.chord, +1);
    await wait(60);
    press(t.power);
    await wait(700);
    release(t.power);
    t.chord.volumeRelease();
    await wait(150);
    check('volume up, then power: the power menu only', [t.menu.opened, key.acted, holds, t.power.slept], [1, 0, [], 0]);

    t = phone();
    key = volume(t.chord, +1);
    await wait(250);
    t.chord.volumeRelease();
    check('volume up alone: the volume, after the chord\'s 150 ms', [key.acted, t.menu.opened], [1, 0]);

    t = phone();
    press(t.power);
    await wait(50);
    key = volume(t.chord, -1);
    await wait(100);
    release(t.power);
    t.chord.volumeRelease();
    await wait(150);
    check('power and volume down: a screenshot', [t.chord.shots, t.menu.opened, key.acted, t.power.slept], [1, 0, 0, 0]);

    t = phone();
    press(t.power);
    await wait(60);
    release(t.power);
    await wait(80);
    press(t.power);
    await wait(700);
    release(t.power);
    await wait(150);
    check('double press: the app, and its held second press starts no Agent',
        [t.power.slept, t.power.woke, t.service.launched, holds, t.menu.opened], [1, 1, [AGENT], [], 0]);
}

agent.export(Gio.DBus.session, '/vibeos/Agent');
feedback.export(Gio.DBus.session, '/org/sigxcpu/Feedback');
Gio.bus_own_name_on_connection(Gio.DBus.session, 'org.sigxcpu.Feedback', Gio.BusNameOwnerFlags.NONE, null, null);
const loop = new GLib.MainLoop(null, false);
Gio.bus_own_name_on_connection(Gio.DBus.session, 'vibeos.Agent', Gio.BusNameOwnerFlags.NONE, () => {
    main().catch(e => {
        failed++;
        logError(e);
    }).finally(() => loop.quit());
}, () => {
    print('FAIL could not be Agent on the test bus');
    failed++;
    loop.quit();
});
loop.run();
print(failed ? `${failed} failed` : 'all passed');
System.exit(failed ? 1 : 0);
