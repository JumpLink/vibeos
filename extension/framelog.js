// Frame times, for measuring animations on a device: on while the file vibeos-frame-log is in the session's runtime
// directory (flagfile.js). Off, nothing is connected. On, every stage paint is timed, and each burst of frames (paints
// less than 100 ms apart) is logged as it ends: how many, over how long, the intervals between paints (mean, 95th
// percentile, longest), how many took longer than one and a half refresh periods (a frame missed), and the paints'
// own lengths.
import GLib from 'gi://GLib';

import * as Main from 'resource:///org/gnome/shell/ui/main.js';

import {FlagFile} from './flagfile.js';

const BURST_GAP_MS = 100;
const MIN_FRAMES = 5;                   // fewer: a cursor blink, a clock tick

export class FrameLog {
    constructor() {
        this._ids = null;
        this._flushId = 0;
        this._flag = new FlagFile('vibeos-frame-log', on => this._sync(on));
        this._sync(this._flag.on);
    }

    _sync(on) {
        if (on === !!this._ids)
            return;
        if (!on) {
            this._stop();
            return;
        }
        this._burst = null;
        this._paintStart = 0;
        this._ids = [
            global.stage.connect('before-paint', () => {
                this._paintStart = GLib.get_monotonic_time();
            }),
            global.stage.connect('after-paint', () => this._painted()),
        ];
        console.log('vibeos: frames: logging');
    }

    _stop() {
        this._flush();
        if (this._flushId)
            GLib.source_remove(this._flushId);
        this._flushId = 0;
        for (const id of this._ids ?? [])
            global.stage.disconnect(id);
        this._ids = null;
    }

    _painted() {
        const now = GLib.get_monotonic_time();
        const paint = this._paintStart ? (now - this._paintStart) / 1000 : 0;
        const b = this._burst;
        if (b && (now - b.last) / 1000 > BURST_GAP_MS)
            this._flush();
        if (!this._burst) {
            this._burst = {first: now, last: now, intervals: [], paints: [paint]};
            this._scheduleFlush();
            return;
        }
        this._burst.intervals.push((now - this._burst.last) / 1000);
        this._burst.paints.push(paint);
        this._burst.last = now;
    }

    // a burst ends with no paint for BURST_GAP_MS: logged then, not at the next paint, which may be minutes later
    _scheduleFlush() {
        if (this._flushId)
            return;
        this._flushId = GLib.timeout_add(GLib.PRIORITY_LOW, BURST_GAP_MS, () => {
            const b = this._burst;
            if (b && (GLib.get_monotonic_time() - b.last) / 1000 <= BURST_GAP_MS)
                return GLib.SOURCE_CONTINUE;
            this._flushId = 0;
            this._flush();
            return GLib.SOURCE_REMOVE;
        });
    }

    _flush() {
        const b = this._burst;
        this._burst = null;
        if (!b || b.paints.length < MIN_FRAMES)
            return;
        const refresh = 1000 / (Main.layoutManager.primaryMonitor?.refresh_rate || 60);
        const sorted = [...b.intervals].sort((x, y) => x - y);
        const mean = sorted.reduce((s, v) => s + v, 0) / sorted.length;
        const p95 = sorted[Math.min(sorted.length - 1, Math.floor(sorted.length * 0.95))];
        const missed = sorted.filter(v => v > refresh * 1.5).length;
        const paints = [...b.paints].sort((x, y) => x - y);
        const paintMean = paints.reduce((s, v) => s + v, 0) / paints.length;
        const f = v => v.toFixed(1);
        console.log(`vibeos: frames: ${b.paints.length} in ${f((b.last - b.first) / 1000)} ms, interval mean ` +
            `${f(mean)} p95 ${f(p95)} max ${f(sorted.at(-1))} ms, ${missed} missed (refresh ${f(refresh)} ms), ` +
            `paint mean ${f(paintMean)} max ${f(paints.at(-1))} ms`);
    }

    destroy() {
        this._stop();
        this._flag.destroy();
    }
}
