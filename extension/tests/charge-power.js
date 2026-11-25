// The charging label's rule on fake power supply trees (charge-power.sh). The trees are the phones' real readings
// (the numbers are theirs, nothing else), and each device's sign convention is written down where its tree is built.
//
//   status    STATUS Full is "Charged", STATUS Not charging is "Not charging" (and only that)
//   slowly    under 5 W into the battery, or STATUS Discharging while a supply is online
//   speed     the battery's measured power: from 20 W super fast, from 12 W fast, from 5 W plain "Charging"
//   wireless  only a pad online
//   unknown   STATUS Unknown: the capacity's trend gives the direction
//   smoothing one spike changes nothing
//   unplugged no label, nothing read, nothing polled
import GLib from 'gi://GLib';
import Gio from 'gi://Gio';
import System from 'system';

import {ChargeMeter, chargeReading, readSample} from './chargepower.js';

let failures = 0;
function check(name, ok, detail = '') {
    print(ok ? 'PASS' : 'FAIL', name, detail);
    if (!ok)
        failures++;
}

const base = GLib.dir_make_tmp('charge-power-XXXXXX');

function rmTree(path) {
    const file = Gio.File.new_for_path(path);
    const info = file.query_info('standard::type', Gio.FileQueryInfoFlags.NOFOLLOW_SYMLINKS, null);
    if (info.get_file_type() === Gio.FileType.DIRECTORY) {
        const en = file.enumerate_children('standard::name', Gio.FileQueryInfoFlags.NOFOLLOW_SYMLINKS, null);
        for (let i; (i = en.next_file(null));)
            rmTree(`${path}/${i.get_name()}`);
    }
    file.delete(null);
}

let serial = 0;
// a power_supply class: {name: {attribute: value}}
function tree(supplies) {
    const root = `${base}/${serial++}`;
    for (const [name, attrs] of Object.entries(supplies)) {
        GLib.mkdir_with_parents(`${root}/${name}`, 0o755);
        for (const [attr, value] of Object.entries(attrs))
            GLib.file_set_contents(`${root}/${name}/${attr}`, `${value}\n`);
    }
    return root;
}

function put(root, name, attr, value) {
    GLib.file_set_contents(`${root}/${name}/${attr}`, `${value}\n`);
}

// the reading of a tree after n samples taken 3 s apart (a clock of ours, so none is replaced as a burst)
function settle(root, n = 5) {
    let t = 0;
    const meter = new ChargeMeter({root, now: () => (t += 3000)});
    meter.setPlugged(true);
    for (let i = 1; i < n; i++)
        meter.refresh();
    const reading = meter.reading;
    meter.destroy();
    return reading;
}

const label = r => `${r.speed}${r.amps === null ? '' : ` ${r.amps.toFixed(2)}A`}`;

// ---- the devices --------------------------------------------------------------------------------------------------
// Each device builds its supplies from a status, a power in watts INTO the battery (negative: out of it) and its own
// sign convention. The numbers of the real readings are noted where they come from.

// S10e: max170xx_battery's sign while it charges is not established: the one reading, -11250 at STATUS Charging on a
// computer's port, is next to nothing flowing (an older comment in the code said positive). The trees use negative;
// the rule never reads the sign. No POWER_NOW. The charger chip and the USB-C port are online with a cable.
function s10e(status, watts, capacity = 50) {
    const volts = 3.988750;
    return tree({
        max170xx_battery: {type: 'Battery', status, capacity, voltage_now: Math.round(volts * 1e6),
            current_now: -Math.round(watts / volts * 1e6)},
        'max77705-charger': {type: 'USB', online: 1},
        'max77705-usbc': {type: 'USB', online: 1, usb_type: 'Unknown SDP DCP CDP C [PD] PD_DRP', current_max: 500000},
    });
}

// S20: the same gauge family, its sign as little established (CURRENT_NOW -71250 at STATUS Charging on a wireless pad,
// a quarter of a watt); the pad is the only supply online, the USB-C one and the charger are offline.
function s20(status, watts, capacity = 50, cable = false) {
    const volts = 3.835156;
    return tree({
        max170xx_battery: {type: 'Battery', status, capacity, voltage_now: Math.round(volts * 1e6),
            current_now: -Math.round(watts / volts * 1e6)},
        'max77705-wireless': {type: 'Wireless', online: 1},
        'max77705-usbc': {type: 'USB', online: cable ? 1 : 0},
        'max77705-charger': {type: 'USB', online: cable ? 1 : 0},
    });
}

// POCO: qcom_qg's sign is not known: the real readings are -1068 at STATUS Unknown and +152 at Full, both on a PC's
// port, and neither is a charge. So a sign is a parameter: +1 and -1 are both tried. No POWER_NOW. The port is a
// BC1.2 SDP one whose current_max (700 mA) and a tcpm source's 3 A offer are caps and offers, which are not read.
function poco(status, watts, sign, capacity = 50) {
    const volts = 4.489886;
    return tree({
        qcom_qg: {type: 'Battery', status, capacity, voltage_now: Math.round(volts * 1e6),
            current_now: sign * Math.round(watts / volts * 1e6)},
        'pm8150b-charger': {type: 'USB', online: 1, usb_type: 'Unknown [SDP] DCP CDP', current_max: 700000,
            voltage_now: 0},
        'tcpm-source-psy': {type: 'USB', online: 1, voltage_now: 5000000, current_max: 3000000},
    });
}

// Tab S5e: sm5705-battery gives POWER_NOW as a magnitude (+954135 while it discharged) and CURRENT_NOW negative while
// it discharges (-233000), so positive while it charges (inferred, nothing charging was read). The charger supply is
// online only with a cable.
function s5e(status, watts, capacity = 50, online = 1) {
    const volts = 4.0;
    return tree({
        'sm5705-battery': {type: 'Battery', status, capacity, voltage_now: Math.round(volts * 1e6),
            power_now: Math.round(Math.abs(watts) * 1e6),
            current_now: Math.round(watts / volts * 1e6)},
        'sm5705-charger': {type: 'USB', online},
    });
}

// Mi Pad 5: qcom-battery is negative while it charges, in both CURRENT_NOW (-12695 µA) and POWER_NOW (-52004 µW) at
// STATUS Charging. qcom-usb's own CURRENT_NOW/VOLTAGE_NOW (1.16 A at 4.24 V) and its 3 A input limit are the charger
// side, not the battery, and not read; tcpm is at 5 V; the wireless supply is offline.
function nabu(status, watts, capacity = 50) {
    const volts = 4.1;
    return tree({
        'qcom-battery': {type: 'Battery', status, capacity, voltage_now: Math.round(volts * 1e6),
            current_now: -Math.round(watts / volts * 1e6), power_now: -Math.round(watts * 1e6)},
        'qcom-usb': {type: 'USB', online: 1, current_now: 1160000, voltage_now: 4240000, input_current_limit: 3000000},
        tcpm: {type: 'USB', online: 1, voltage_now: 5000000, current_max: 3000000},
        idtp9418: {type: 'Wireless', online: 0},
    });
}

// ---- the real flat cases ------------------------------------------------------------------------------------------

let r = settle(tree({
    max170xx_battery: {type: 'Battery', status: 'Charging', capacity: 60, current_now: -11250, voltage_now: 3988750},
    'max77705-charger': {type: 'USB', online: 1},
    'max77705-usbc': {type: 'USB', online: 1, usb_type: 'Unknown SDP DCP CDP [C] PD PD_DRP', current_max: 500000},
}));
check('S10e on a PC port (11 mA, STATUS Charging): "Charging slowly", no amps', r.speed === 'slow' && r.amps === null,
    label(r));

r = settle(tree({
    qcom_qg: {type: 'Battery', status: 'Unknown', capacity: 100, current_now: -1068, voltage_now: 4489886},
    'pm8150b-charger': {type: 'USB', online: 1, usb_type: 'Unknown [SDP] DCP CDP', current_max: 700000,
        voltage_now: 0, status: 'Full'},
    'tcpm-source-psy': {type: 'USB', online: 1, voltage_now: 5000000, current_max: 3000000},
}));
check('POCO on a PC port (1 mA, STATUS Unknown at 100 %, 3 A offer): "Charged"', r.speed === 'full' && r.amps === null,
    label(r));

r = settle(tree({
    qcom_qg: {type: 'Battery', status: 'Full', capacity: 100, current_now: 152, voltage_now: 4489886},
    'pm8150b-charger': {type: 'USB', online: 1, usb_type: 'Unknown [SDP] DCP CDP', current_max: 700000},
    'tcpm-source-psy': {type: 'USB', online: 1, voltage_now: 5000000, current_max: 3000000},
}));
check('POCO at Full: "Charged"', r.speed === 'full', label(r));

r = settle(tree({
    'qcom-battery': {type: 'Battery', status: 'Charging', capacity: 80, current_now: -12695, power_now: -52004,
        voltage_now: 4100000},
    'qcom-usb': {type: 'USB', online: 1, current_now: 1160000, voltage_now: 4240000, input_current_limit: 3000000},
    tcpm: {type: 'USB', online: 1, voltage_now: 5000000, current_max: 3000000},
    idtp9418: {type: 'Wireless', online: 0},
}));
check('Mi Pad 5 (52 mW, 15 W limit, STATUS Charging): "Charging slowly", never fast',
    r.speed === 'slow' && r.amps === null, label(r));

r = settle(s5e('Discharging', -0.954135, 50, 0));
check('Tab S5e unplugged (954 mW out, charger offline): no label', r.speed === null && r.amps === null, label(r));

// ---- a charge at 3, 6, 13 and 25 W, in each device's convention ---------------------------------------------
// (6 W, not 5: a tree's current is rounded to the µA, which puts 5 W a hair under the threshold)

const cases = [
    ['S10e', w => s10e('Charging', w)],
    ['POCO (current positive in)', w => poco('Charging', w, +1)],
    ['POCO (current negative in)', w => poco('Charging', w, -1)],
    ['Tab S5e', w => s5e('Charging', w)],
    ['Mi Pad 5', w => nabu('Charging', w)],
];
for (const [name, build] of cases) {
    for (const [w, speed] of [[3, 'slow'], [6, 'normal'], [13, 'fast'], [25, 'superfast']]) {
        r = settle(build(w));
        check(`${name} charging at ${w} W: ${speed}`, r.speed === speed, label(r));
    }
    // 13 W into the battery at 4 V is 3.25 A (the Mi Pad's 4.1 V: 3.17 A), 5 W 1.25 A
    r = settle(build(5));
    check(`${name}: amps are the magnitude into the battery`, r.amps !== null && r.amps > 1 && r.amps < 1.4, label(r));
}
r = settle(s20('Charging', 5));
check('S20 on its pad, charging at 5 W: wireless, with amps', r.speed === 'wireless' && r.amps > 1, label(r));

// ---- slowly: a charger is in, the battery gives power -------------------------------------------------------------

for (const [name, tr] of [['S10e', s10e('Discharging', -2)], ['Tab S5e', s5e('Discharging', -2)],
    ['Mi Pad 5', nabu('Discharging', -2)], ['POCO', poco('Discharging', -2, -1)],
    ['S10e with 0 W', s10e('Discharging', 0)]]) {
    r = settle(tr);
    check(`${name} Discharging while online: slowly, no amps`, r.speed === 'slow' && r.amps === null, label(r));
}

// ---- STATUS Not charging and Full --------------------------------------------------------------------------------

for (const [name, tr] of [['Mi Pad 5 at a charge limit', nabu('Not charging', 0, 80)],
    ['S10e too warm', s10e('Not charging', 0.05)], ['POCO held, 2 W still read', poco('Not charging', 2, -1)]]) {
    r = settle(tr);
    check(`${name} (STATUS Not charging): "Not charging", no amps`, r.speed === 'idle' && r.amps === null, label(r));
}
r = settle(s20('Not charging', 0, 90));
check('S20 held on its pad (STATUS Not charging): "Not charging", not wireless', r.speed === 'idle', label(r));
r = settle(s10e('Full', 0, 99));
check('S10e at Full at 99 %: "Charged"', r.speed === 'full' && r.amps === null, label(r));
r = settle(s10e('Charging', 0.3, 40));
check('S10e under 1 W at STATUS Charging: "Charging slowly", no amps', r.speed === 'slow' && r.amps === null, label(r));

// ---- STATUS Unknown: the capacity's trend --------------------------------------------------------------------------

const sample = (t, capacity, o = {}) => ({t, cable: true, pad: false, status: 'Unknown', capacity, watts: 5, amps: 1.2, ...o});
check('Unknown, capacity rising, 5 W: "Charging"',
    chargeReading([sample(0, 50), sample(3000, 50), sample(6000, 51)]).speed === 'normal');
check('Unknown, capacity rising, 13 W: fast',
    chargeReading([50, 50, 51].map((c, i) => sample(i * 3000, c, {watts: 13}))).speed === 'fast');
check('Unknown, capacity falling: slowly',
    chargeReading([sample(0, 51), sample(3000, 51), sample(6000, 50)]).speed === 'slow');
check('Unknown, capacity steady at 50 %, 5 W: "Charging" (only STATUS says "Not charging")',
    chargeReading([sample(0, 50), sample(3000, 50), sample(6000, 50)]).speed === 'normal');
check('Unknown, capacity steady at 50 %, flat power: "Charging slowly"',
    chargeReading([50, 50, 50].map((c, i) => sample(i * 3000, c, {watts: 0.2}))).speed === 'slow');
check('Unknown, capacity steady at 100 %: "Charged"',
    chargeReading([100, 100].map((c, i) => sample(i * 3000, c, {watts: 0.2}))).speed === 'full');
check('Unknown, capacity rising, flat power: "Charging slowly"',
    chargeReading([50, 51].map((c, i) => sample(i * 3000, c, {watts: 0.2}))).speed === 'slow');
// the same through a tree whose capacity moves between samples
{
    const root = poco('Unknown', 6, -1, 70);
    let t = 0;
    const meter = new ChargeMeter({root, now: () => (t += 3000)});
    meter.setPlugged(true);
    put(root, 'qcom_qg', 'capacity', 71);
    meter.refresh();
    check('Unknown through a tree, capacity 70 to 71: "Charging" with amps', meter.reading.speed === 'normal' &&
        meter.reading.amps > 1, label(meter.reading));
    put(root, 'qcom_qg', 'capacity', 69);
    meter.refresh();
    check('Unknown through a tree, then 69: slowly', meter.reading.speed === 'slow', label(meter.reading));
    meter.destroy();
}

// ---- wireless ----------------------------------------------------------------------------------------------------

r = settle(s20('Charging', 0.273));
check('S20 on its pad (71 mA, STATUS Charging): "Wireless charging"', r.speed === 'wireless', label(r));
r = settle(s20('Charging', 5, 50, true));
check('S20 with a cable on the pad: the cable wins (plain "Charging")', r.speed === 'normal', label(r));
r = settle(s20('Charging', 20, 50, true));
check('S20 with a cable on the pad at 20 W: super fast', r.speed === 'superfast', label(r));

// ---- smoothing ---------------------------------------------------------------------------------------------------

const watts = list => chargeReading(list.map((w, i) => ({t: i * 3000, cable: true, pad: false, status: 'Charging',
    capacity: 50, watts: w, amps: w / 4})));
check('one 60 W spike among 5 W: still "Charging"', watts([5, 5, 60, 5, 5]).speed === 'normal');
check('one 0 W dip among 25 W: still super fast', watts([25, 25, 0, 25, 25]).speed === 'superfast');
check('one 13 W blip among 5 W: still "Charging"', watts([5, 5, 5, 13, 5]).speed === 'normal');
check('a lasting change does win: 5 W then 25 W x3', watts([5, 5, 25, 25, 25]).speed === 'superfast');
check('spike amps are smoothed too', Math.abs(watts([5, 5, 60, 5, 5]).amps - 1.25) < 1e-9);
check('a single STATUS flicker to Discharging does not make it slow',
    chargeReading(['Charging', 'Charging', 'Discharging', 'Charging', 'Charging'].map((status, i) =>
        ({t: i * 3000, cable: true, pad: false, status, capacity: 50, watts: 13, amps: 3}))).speed === 'fast');
check('a lasting Discharging does', chargeReading(['Charging', 'Discharging', 'Discharging', 'Discharging', 'Charging']
    .map((status, i) => ({t: i * 3000, cable: true, pad: false, status, capacity: 50, watts: 13, amps: 3}))).speed === 'slow');
{
    // a burst of samples within 2 s of each other replaces one another instead of filling the window
    const root = nabu('Charging', 5);
    let t = 0;
    const meter = new ChargeMeter({root, now: () => (t += 100)});
    meter.setPlugged(true);
    for (let i = 0; i < 6; i++)
        meter.refresh();
    check('a burst of uevents is one sample, not a full window', meter._samples.length === 1, `${meter._samples.length}`);
    meter.destroy();
}

// ---- the label never comes from a cap or an offer ------------------------------------------------------------------

r = settle(tree({
    'qcom-battery': {type: 'Battery', status: 'Charging', capacity: 50, power_now: -3000000, current_now: -700000,
        voltage_now: 4300000},
    'big-charger': {type: 'Mains', online: 1, input_current_limit: 5000000, current_max: 9000000,
        voltage_now: 20000000, voltage_max: 20000000, usb_type: 'C [PD]'},
}));
check('a 100 W limit and offer with 3 W flowing: "Charging slowly"', r.speed === 'slow', label(r));
r = settle(tree({
    'qcom-battery': {type: 'Battery', status: 'Charging', capacity: 50, power_now: -22000000, current_now: -5000000,
        voltage_now: 4400000},
    'weak-port': {type: 'USB', online: 1, usb_type: 'Unknown [SDP] DCP CDP', current_max: 500000},
}));
check('a 0.5 A SDP port with 22 W measured: super fast (measured, not offered)', r.speed === 'superfast', label(r));
{
    const sample = readSample(s10e('Charging', 5));
    check('readSample: a magnitude and a cable', sample.cable && !sample.pad && Math.abs(sample.watts - 5) < 0.01 &&
        sample.status === 'Charging', JSON.stringify(sample));
}
check('a root that is not there: no sample', readSample(`${base}/nothing`) === null);
{
    // a peripheral's battery (scope Device) is not the phone's
    const root = tree({
        'hid-mouse': {type: 'Battery', scope: 'Device', status: 'Discharging', capacity: 10},
        'max170xx_battery': {type: 'Battery', status: 'Charging', capacity: 50, power_now: 15000000},
        usb: {type: 'USB', online: 1},
    });
    check('a mouse\'s battery is skipped', settle(root).speed === 'fast');
}

// ---- unplugged: nothing read, nothing polled ----------------------------------------------------------------------

{
    const root = s5e('Discharging', -0.954135, 50, 0);
    check('unplugged tree: reading is null', chargeReading([readSample(root)]).speed === null);
    const meter = new ChargeMeter({root, periodMs: 20});
    meter.setPlugged(false);
    meter.refresh();
    check('unplugged: no timer, no read', !meter.polling && meter.taken === 0 && meter.reading.speed === null);
    meter.destroy();
}

// ---- the timer: while plugged, and only then ----------------------------------------------------------------------

function spin(ms) {
    const loop = GLib.MainLoop.new(null, false);
    GLib.timeout_add(GLib.PRIORITY_DEFAULT, ms, () => {
        loop.quit();
        return GLib.SOURCE_REMOVE;
    });
    loop.run();
}
{
    const root = nabu('Charging', 5);
    const changes = [];
    let clock = 0;
    const meter = new ChargeMeter({root, periodMs: 20, now: () => (clock += 5000),
        onChange: reading => changes.push(reading.speed)});
    meter.setPlugged(true);
    check('plugged: a timer, and the supplies read once', meter.polling && meter.taken === 1);
    spin(200);
    check('plugged: re-read by the timer', meter.taken >= 4, `${meter.taken} reads`);
    put(root, 'qcom-battery', 'power_now', -25000000);
    put(root, 'qcom-battery', 'current_now', -6000000);
    spin(250);
    check('a lasting change reaches onChange', changes.includes('superfast'), changes.join(','));
    meter.setPlugged(false);
    const taken = meter.taken;
    check('unplugged: the timer stops and the reading goes', !meter.polling && meter.reading.speed === null);
    spin(150);
    check('unplugged: nothing is read any more', meter.taken === taken);
    meter.setPlugged(true);
    check('plugged again: a new timer', meter.polling);
    meter.destroy();
    const after = meter.taken;
    check('destroy stops the timer', !meter.polling);
    spin(100);
    check('destroyed: nothing is read any more', meter.taken === after);
}

rmTree(base);
print(failures ? `${failures} FAILED` : 'all passed');
System.exit(failures ? 1 : 0);
