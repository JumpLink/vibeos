// The shade's look, chosen by measurement: the screen behind it blurred and dimmed (translucent cards over it), or an
// opaque layer where the device's GPU does not hold the frame rate under a blur.
//
// A new device (or one whose kernel, Mesa, shell, extension or screen refresh changed since it was measured) starts
// with the blur and times the shade's own animations: every stage paint while the shade opens or closes with the blur
// showing, as the interval since the paint before. After SETTLED_OPENINGS openings (each followed by its closing) with
// at least MIN_INTERVALS intervals in all, the 95th percentile of the intervals is compared with the refresh period:
// within 1.2 periods the blur stays, over it the opaque look takes its place. The opaque look takes over only once two
// shell sessions (two starts of the shell) have each measured over the limit: one slow measurement (a charger plugged
// in, the device busy) keeps the blur and measures again at the next start, and a measurement within the limit
// forgets an earlier slow one. The verdict and what it was measured on are kept in shade-look.json in the user's state
// directory (deleting the file measures again), and `blur` changes only when a measurement ends: the shade takes it up
// when it is next closed, never mid-animation.
//
// The environment variable VIBEOS_SHADE_SLOW_MS (the shell's, a number) holds every paint for that many milliseconds
// while the shade animates with the blur, so that a fast device measures slow.
import Gio from 'gi://Gio';
import GLib from 'gi://GLib';

import * as Config from 'resource:///org/gnome/shell/misc/config.js';
import * as Main from 'resource:///org/gnome/shell/ui/main.js';

import {TABLET} from './units.js';

const SETTLED_OPENINGS = 3;
const MIN_INTERVALS = 30;
const MIN_SESSION_FRAMES = 6;           // fewer paints in one animation: a flick that never went anywhere
const BUDGET_MARGIN = 1.2;              // the 95th percentile may be 20 % over the refresh period
const STATE_VERSION = 1;
const OPAQUE_SESSIONS = 2;              // shell sessions that measure over the limit before the opaque look is kept

const readText = path => {
    try {
        const [, bytes] = Gio.File.new_for_path(path).load_contents(null);
        return new TextDecoder().decode(bytes).replace(/\0/g, ' ').trim();
    } catch (e) {
        return '';
    }
};

const listDir = path => {
    const names = [];
    try {
        const e = Gio.File.new_for_path(path).enumerate_children('standard::name', Gio.FileQueryInfoFlags.NONE, null);
        let info;
        while ((info = e.next_file(null)))
            names.push(info.get_name());
    } catch (e) {
    }
    return names.sort();
};

// The GPU from sysfs (never debugfs): each render node's driver and PCI id, and the compatible string of a platform
// GPU (an Adreno or a Mali sits on its own platform device, not on the display's)
export function gpuName() {
    const parts = [];
    for (const node of listDir('/sys/class/drm').filter(n => /^renderD\d+$/.test(n))) {
        const uevent = readText(`/sys/class/drm/${node}/device/uevent`);
        const driver = /^DRIVER=(.*)$/m.exec(uevent)?.[1];
        const pci = /^PCI_ID=(.*)$/m.exec(uevent)?.[1];
        const compat = readText(`/sys/class/drm/${node}/device/of_node/compatible`);
        parts.push([driver, pci, compat].filter(Boolean).join(' '));
    }
    for (const dev of listDir('/sys/bus/platform/devices').filter(n => /gpu|adreno|mali/i.test(n))) {
        const compat = readText(`/sys/bus/platform/devices/${dev}/of_node/compatible`);
        if (compat)
            parts.push(compat);
    }
    return parts.join(' / ') || 'unknown';
}

// Mesa as this shell loaded it, from the files mapped into the process: libgallium carries its version in its name
// (Mesa 25.1 and later), an older Mesa's driver is a *_dri.so, whose size and date stand for its version
export function mesaName() {
    const paths = new Set();
    for (const line of readText('/proc/self/maps').split('\n')) {
        const path = line.split(/\s+/)[5];
        if (path && /libgallium|_dri\.so/.test(path))
            paths.add(path);
    }
    const names = [];
    for (const path of [...paths].sort()) {
        const base = GLib.path_get_basename(path);
        if (/libgallium/.test(base)) {
            names.push(base);
            continue;
        }
        try {
            const info = Gio.File.new_for_path(path).query_info('standard::size,time::modified',
                Gio.FileQueryInfoFlags.NONE, null);
            names.push(`${base}:${info.get_size()}:${info.get_attribute_uint64('time::modified')}`);
        } catch (e) {
            names.push(base);
        }
    }
    return names.join(' ') || 'unknown';
}

// the extension's build: its metadata's version if it has one, and the date of this directory's modules (a package's
// files keep the date of their release)
function extensionName() {
    const dir = Gio.File.new_for_uri(import.meta.url).get_parent();
    let version = '';
    try {
        const [, bytes] = dir.get_child('metadata.json').load_contents(null);
        const meta = JSON.parse(new TextDecoder().decode(bytes));
        version = meta['version-name'] ?? meta.version ?? '';
    } catch (e) {
    }
    let stamp = 0;
    try {
        stamp = dir.get_child('shade.js').query_info('time::modified', Gio.FileQueryInfoFlags.NONE, null)
            .get_attribute_uint64('time::modified');
    } catch (e) {
    }
    return `${version}@${stamp}`;
}

// this shell session: the boot and the shell's process, the same while the extension is turned off and on again
function shellSession() {
    return `${readText('/proc/sys/kernel/random/boot_id')}:${new Gio.Credentials().get_unix_pid()}`;
}

const percentile95 = values => {
    const sorted = [...values].sort((a, b) => a - b);
    return sorted[Math.min(sorted.length - 1, Math.floor(sorted.length * 0.95))];
};

export class ShadeLook {
    constructor() {
        this._file = Gio.File.new_for_path(GLib.build_filenamev([GLib.get_user_state_dir(), 'vibeos',
            'shade-look.json']));
        this._slowMs = Math.max(0, Number(GLib.getenv('VIBEOS_SHADE_SLOW_MS')) || 0);
        this._ids = null;
        this._active = false;
        this._last = 0;
        this._session = [];
        this._intervals = [];
        this._openings = 0;
        this._decided = false;
        this._sessionId = shellSession();
        this._failures = [];                // the slow measurements of earlier sessions, while one has not settled it
        this.blur = true;

        this.key = {
            kind: TABLET ? 'tablet' : 'phone',
            gpu: gpuName(),
            mesa: mesaName(),
            kernel: readText('/proc/sys/kernel/osrelease'),
            shell: Config.PACKAGE_VERSION,
            extension: extensionName(),
            refresh: this._refresh(),
        };
        const saved = this._load();
        const stale = saved ? Object.keys(this.key).filter(k => saved.key?.[k] !== this.key[k]) : null;
        const keyText = Object.entries(this.key).map(([k, v]) => `${k}=${v}`).join(' ');
        if (saved && !stale.length && Array.isArray(saved.failures) && saved.blur) {
            this._failures = saved.failures.filter(f => f && typeof f.session === 'string');
            const here = this._failures.some(f => f.session === this._sessionId);
            this._decided = here;
            console.log(`shade blur: ${this._failures.length} of ${OPAQUE_SESSIONS} sessions measured slow ` +
                `(last p95 ${this._failures.at(-1)?.p95} ms); ${here ? 'this one already did, keeping the blur'
                    : 'measuring again, starting with the blur'}; ${keyText}`);
        } else if (saved && !stale.length) {
            this._decided = true;
            this.blur = saved.blur;
            console.log(`shade blur: ${saved.blur ? 'blur' : 'opaque'} kept from an earlier measurement (p95 ` +
                `${saved.p95} ms over ${saved.frames} frames); ${keyText}`);
        } else {
            console.log(`shade blur: ${saved ? `measuring again, changed: ${stale.join(', ')}` : 'not measured yet'}; ` +
                `starting with the blur; ${keyText}`);
        }
        this._connect();
    }

    _refresh() {
        return Math.round((Main.layoutManager.primaryMonitor?.refresh_rate || 60) * 100) / 100;
    }

    _load() {
        try {
            const [, bytes] = this._file.load_contents(null);
            const data = JSON.parse(new TextDecoder().decode(bytes));
            if (data.version === STATE_VERSION && typeof data.blur === 'boolean')
                return data;
        } catch (e) {
        }
        return null;
    }

    _save(verdict) {
        try {
            try {
                this._file.get_parent().make_directory_with_parents(null);
            } catch (e) {
            }
            this._file.replace_contents(JSON.stringify({version: STATE_VERSION, ...verdict, key: this.key}, null, 2),
                null, false, Gio.FileCreateFlags.REPLACE_DESTINATION, null);
        } catch (e) {
            console.log(`shade blur: the verdict could not be kept: ${e.message}`);
        }
    }

    // paints are watched while a measurement is open, and held while the slow mode is on
    _connect() {
        const needed = !this._decided || this._slowMs > 0;
        if (needed === !!this._ids)
            return;
        if (!needed) {
            for (const id of this._ids)
                global.stage.disconnect(id);
            this._ids = null;
            return;
        }
        this._ids = [
            global.stage.connect('before-paint', () => {
                if (this._active && this._slowMs > 0)
                    GLib.usleep(this._slowMs * 1000);
            }),
            global.stage.connect('after-paint', () => {
                if (!this._active || this._decided)
                    return;
                const now = GLib.get_monotonic_time();
                if (this._last)
                    this._session.push((now - this._last) / 1000);
                this._last = now;
            }),
        ];
    }

    // The shade tells, once a frame, whether it is animating (a fling, the spring after it) with the blur showing.
    // @opened: the shade is fully open; @closed: it is shut. A measurement that settles takes effect here, which is
    // while the shade is shut.
    update({animating, opened, closed}) {
        if (animating && !this._active) {
            this._active = true;
            this._last = 0;
            this._session = [];
        } else if (!animating && this._active) {
            this._active = false;
            this._endSession(opened, closed);
        }
    }

    _endSession(opened, closed) {
        const frames = this._session.length + 1;
        if (this._decided || frames < MIN_SESSION_FRAMES) {
            this._session = [];
            return;
        }
        this._intervals.push(...this._session);
        this._session = [];
        if (opened)
            this._openings++;
        if (closed && this._openings >= SETTLED_OPENINGS && this._intervals.length >= MIN_INTERVALS)
            this._decide();
    }

    _decide() {
        const refresh = this._refresh();
        const period = 1000 / refresh;
        const limit = period * BUDGET_MARGIN;
        const p95 = percentile95(this._intervals);
        const frames = this._intervals.length;
        const mean = this._intervals.reduce((s, v) => s + v, 0) / frames;
        const fast = p95 <= limit;
        const measured = {p95: Number(p95.toFixed(1)), mean: Number(mean.toFixed(1)), frames,
            openings: this._openings, limit: Number(limit.toFixed(1)), at: new Date().toISOString()};
        // a slow measurement counts once per shell session; the opaque look waits for OPAQUE_SESSIONS of them
        const failures = fast ? [] : [...this._failures.filter(f => f.session !== this._sessionId),
            {session: this._sessionId, ...measured}];
        const blur = fast || failures.length < OPAQUE_SESSIONS;
        console.log(`shade blur: measured gpu=${this.key.gpu} mesa=${this.key.mesa} refresh=${refresh} Hz ` +
            `(${period.toFixed(1)} ms) intervals=${frames} openings=${this._openings} mean=${mean.toFixed(1)} ms ` +
            `p95=${p95.toFixed(1)} ms limit=${limit.toFixed(1)} ms -> ${fast ? 'blur'
                : blur ? `slow in ${failures.length} of ${OPAQUE_SESSIONS} sessions, keeping the blur until the next start`
                    : 'opaque'}` +
            `${this._slowMs ? ` (slow mode ${this._slowMs} ms)` : ''}`);
        this._decided = true;
        this.blur = blur;
        this._failures = failures;
        this.key.refresh = refresh;
        this._save({blur, ...measured, ...(failures.length ? {failures} : {})});
        this._intervals = [];
        this._connect();
    }

    destroy() {
        this._decided = true;
        this._slowMs = 0;
        this._connect();
    }
}
