// How the battery charges, from what flows into it, as UPower works it out (up-device-supply-battery.c): the direction
// from the battery's STATUS (the capacity's trend when it says nothing), the magnitude from its POWER_NOW, or else
// |CURRENT_NOW x VOLTAGE_NOW|, several samples smoothed so one spike does not flip a label. What the charger is allowed
// to give (input_current_limit, a USB-C port's current_max or its tcpm offer) says nothing about what flows, and is
// never read.
//
// The sign of CURRENT_NOW and POWER_NOW differs from gauge to gauge (the Mi Pad's qcom-battery is negative while it
// charges, the Tab S5e's sm5705 gives a positive POWER_NOW as a magnitude), so the sign is never the direction.
//
// Only the power supply class's own attributes: /sys/class/power_supply/*/ (or a test's tree in its place).
import GLib from 'gi://GLib';

export const POWER_SUPPLY = '/sys/class/power_supply';
// Android's speeds: below SLOW_W "Charging slowly", from FAST_W fast, from SUPER_FAST_W super fast. Below FLAT_W
// nothing worth a number goes into the battery, and no current is shown.
export const FLAT_W = 1;
export const SLOW_W = 5;
export const FAST_W = 12;
export const SUPER_FAST_W = 20;
// the samples power and STATUS are smoothed over (the median of the power, the majority of the STATUS), and the ones
// kept for the capacity's trend; at the sample period that is 25 s and a minute
export const SMOOTH_SAMPLES = 5;
export const TREND_SAMPLES = 12;
export const SAMPLE_MS = 5000;
// a sample within this of the one before (a burst of uevents) replaces it, so a burst cannot fill the window
const MIN_GAP_MS = 2000;

function readSysfs(path) {
    try {
        return new TextDecoder().decode(GLib.file_get_contents(path)[1]).trim();
    } catch (e) {
        return null;
    }
}

const int = text => {
    const n = parseInt(text ?? '');
    return isNaN(n) ? null : n;
};

// from STATUS: 'in' (the battery takes power), 'out' (it gives some), 'full', 'held' (the charger keeps it where it
// is: a temperature or a charge limit) or 'unknown'
function statusDirection(status) {
    switch (status) {
    case 'Charging':
        return 'in';
    case 'Discharging':
        return 'out';
    case 'Full':
        return 'full';
    case 'Not charging':
        return 'held';
    default:
        return 'unknown';
    }
}

// One look at the supplies: {t, cable, pad, status, capacity, watts, amps}; cable: a supply that is not a pad is
// online, pad: a Wireless one is. watts and amps are magnitudes (null when the gauge gives neither power nor current).
export function readSample(root = POWER_SUPPLY, t = GLib.get_monotonic_time() / 1000) {
    let dir;
    try {
        dir = GLib.Dir.open(root, 0);
    } catch (e) {
        return null;
    }
    const names = [];
    for (let name; (name = dir.read_name()) !== null;)
        names.push(name);
    dir.close();
    names.sort();
    const sample = {t, cable: false, pad: false, status: null, capacity: null, watts: null, amps: null};
    let battery = null;
    for (const name of names) {
        const at = attr => readSysfs(`${root}/${name}/${attr}`);
        const type = at('type');
        if (type === 'Battery') {
            // a peripheral's battery (a mouse's) is not the phone's
            if (!battery && at('scope') !== 'Device' && at('present') !== '0')
                battery = at;
        } else if (at('online') === '1') {
            if (type === 'Wireless')
                sample.pad = true;
            else
                sample.cable = true;
        }
    }
    if (!battery)
        return sample;
    sample.status = battery('status');
    sample.capacity = int(battery('capacity'));
    const uv = int(battery('voltage_now'));
    const ua = int(battery('current_now'));
    const uw = int(battery('power_now'));
    if (uw)
        sample.watts = Math.abs(uw) / 1e6;
    else if (ua !== null && uv)
        sample.watts = Math.abs(ua) * Math.abs(uv) / 1e12;
    if (ua !== null)
        sample.amps = Math.abs(ua) / 1e6;
    else if (sample.watts !== null && uv)
        sample.amps = sample.watts / (Math.abs(uv) / 1e6);
    return sample;
}

function median(values) {
    const sorted = values.filter(v => v !== null).sort((a, b) => a - b);
    if (!sorted.length)
        return null;
    const mid = sorted.length >> 1;
    return sorted.length % 2 ? sorted[mid] : (sorted[mid - 1] + sorted[mid]) / 2;
}

// the direction most of the samples say; a tie goes to the newest
function majority(directions) {
    const count = {};
    for (const d of directions)
        count[d] = (count[d] ?? 0) + 1;
    const best = Math.max(...Object.values(count));
    return directions.findLast(d => count[d] === best);
}

// The label's speed and the current into the battery from the samples, oldest first: {speed, amps}, speed
//   'full'       STATUS Full ("Charged")
//   'idle'       STATUS Not charging: the charger holds the battery (a temperature or a charge limit)
//   'wireless'   on a pad, no cable
//   'slow'       under SLOW_W into the battery, or the battery giving power while a charger is in
//   'normal'     plain "Charging"; 'fast', 'superfast' from FAST_W and SUPER_FAST_W
//   null         nothing is plugged in
// amps in A, or null unless the battery takes power.
export function chargeReading(samples) {
    const last = samples.at(-1);
    if (!last || (!last.cable && !last.pad))
        return {speed: null, amps: null};
    const recent = samples.slice(-SMOOTH_SAMPLES);
    let direction = majority(recent.map(s => statusDirection(s.status)));
    if (direction === 'unknown') {
        // the capacity's trend; a level that holds is charging as slowly as the power says, or full at 100 %
        const levels = samples.map(s => s.capacity).filter(c => c !== null);
        const trend = levels.length ? levels.at(-1) - levels[0] : 0;
        direction = trend > 0 ? 'in' : trend < 0 ? 'out' : levels.at(-1) >= 100 ? 'full' : 'in';
    }
    if (direction === 'full')
        return {speed: 'full', amps: null};
    if (direction === 'held')
        return {speed: 'idle', amps: null};
    const watts = median(recent.map(s => s.watts));
    const amps = direction === 'in' && watts !== null && watts >= FLAT_W ? median(recent.map(s => s.amps)) : null;
    // a pad with no cable on it: no speed is worked out, as on One UI
    if (!last.cable)
        return {speed: 'wireless', amps};
    if (direction === 'out')
        return {speed: 'slow', amps: null};
    if (watts === null)
        return {speed: 'normal', amps};
    return {speed: watts >= SUPER_FAST_W ? 'superfast' : watts >= FAST_W ? 'fast' : watts >= SLOW_W ? 'normal' : 'slow',
        amps};
}

// The samples of one plug-in, and the timer that takes them while something is plugged in (and only then: setPlugged
// false stops it and forgets them). @onChange gets the reading when the speed or the amps (to 0.1 A) changed.
export class ChargeMeter {
    constructor({root = POWER_SUPPLY, onChange = null, periodMs = SAMPLE_MS, now = null} = {}) {
        this._root = root;
        this._onChange = onChange;
        this._periodMs = periodMs;
        this._now = now ?? (() => GLib.get_monotonic_time() / 1000);
        this._samples = [];
        this._timer = 0;
        this._plugged = false;
        this.reading = {speed: null, amps: null};
        this.taken = 0;  // how many times the supplies were read
    }

    get polling() {
        return this._timer !== 0;
    }

    // @plugged: whether the phone is on a charger, from UPower's OnBattery; no timer and no read without it
    setPlugged(plugged) {
        if (plugged === this._plugged)
            return;
        this._plugged = plugged;
        if (plugged) {
            this._take();
            this._timer = GLib.timeout_add(GLib.PRIORITY_DEFAULT, this._periodMs, () => {
                this.refresh();
                return GLib.SOURCE_CONTINUE;
            });
        } else {
            this._stop();
            this._samples = [];
            this.reading = {speed: null, amps: null};
        }
    }

    // read the supplies now (the timer, or a power_supply uevent)
    refresh() {
        if (!this._plugged)
            return this.reading;
        const old = this.reading;
        this._take();
        if (this.reading.speed !== old.speed || Math.round(this.reading.amps * 10) !== Math.round(old.amps * 10))
            this._onChange?.(this.reading);
        return this.reading;
    }

    _take() {
        const sample = readSample(this._root, this._now());
        this.taken++;
        if (!sample)
            return;
        const prev = this._samples.at(-1);
        // a cable or pad coming or going is a new plug-in
        if (prev && (prev.cable !== sample.cable || prev.pad !== sample.pad))
            this._samples = [];
        else if (prev && sample.t - prev.t < MIN_GAP_MS)
            this._samples.pop();
        this._samples.push(sample);
        this._samples = this._samples.slice(-TREND_SAMPLES);
        this.reading = chargeReading(this._samples);
    }

    _stop() {
        if (this._timer)
            GLib.source_remove(this._timer);
        this._timer = 0;
    }

    destroy() {
        this._stop();
        this._plugged = false;
        this._onChange = null;
    }
}
