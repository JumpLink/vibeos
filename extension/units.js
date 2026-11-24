// The shell's geometry: every length in the shell is a pixel of the reference phone's screen (the Galaxy S10e,
// 1080 x 2280 at 3 screen pixels per Android dp), a "reference pixel", and a third of a dp on any device. Some sizes
// are in units of 3 of them (dp() below). "Screen px" in the modules' comments means these reference pixels; on the
// phones (S10e, POCO X3: 3 screen pixels per dp) they are the panel's own pixels.
//
// Three factors, kept apart:
//   SCREEN_SCALE   screen pixels per logical pixel: mutter's scale for the primary monitor (3 on the phones, 2 on the
//                  Galaxy Tab S5e), for drawing at the panel's resolution only
//   DENSITY        screen pixels per dp: VIBEOS_DENSITY when the session sets it, else the scale (the scale is the
//                  density on every VibeOS device, so a dp is a logical pixel)
//   SCALE          reference pixels per logical pixel: 3 x SCREEN_SCALE / DENSITY (3 wherever the scale is the
//                  density), what a stage coordinate is multiplied by to compare it with the constants
//
// The device class is Android's: a smallest width of 600 dp or more is a tablet, whichever way it is turned.
//
// Nothing here is fixed when the module loads: start() (the extension's enable()) reads the primary monitor and reads
// it again on every monitors-changed, before any other handler of that signal runs, and 'changed' follows. The
// exports are live bindings: a module that reads SCREEN_W when it lays out gets the size of now.
import GLib from 'gi://GLib';

import * as Main from 'resource:///org/gnome/shell/ui/main.js';
import * as Signals from 'resource:///org/gnome/shell/misc/signals.js';

// a phone's panel, until there is a monitor to read
const FALLBACK_SCALE = 3;
const FALLBACK_W = 1080, FALLBACK_H = 2280;
// Android's smallest width from which a device is a tablet (sw600dp)
const TABLET_SW_DP = 600;

export let SCREEN_SCALE = FALLBACK_SCALE;
export let DENSITY = FALLBACK_SCALE;
export let SCALE = 3;
// the panel upright (its short side across), reference px
export let SCREEN_W = FALLBACK_W;
export let SCREEN_H = FALLBACK_H;
// the screen as it is turned now, reference px
export let WIDTH = FALLBACK_W;
export let HEIGHT = FALLBACK_H;
// a tablet (sw600dp), whichever way it is turned
export let TABLET = false;
// the smallest width, dp
export let SMALLEST_WIDTH_DP = FALLBACK_W / 3;

// a reference pixel length in logical pixels
export const px = v => v / SCALE;

// a length in units of 3 reference px (a dp), and a text size in the same units, in logical pixels
export const dp = v => v * 3 / SCALE;
export const sp = dp;

// Cairo drawing in reference pixels: scale a logical-pixel context so 1 unit is 1 reference pixel
export function screenPixels(cr) {
    cr.scale(1 / SCALE, 1 / SCALE);
}

// the screen as it is turned now, wider than tall
export function landscape() {
    return WIDTH > HEIGHT;
}

class Geometry extends Signals.EventEmitter {
    constructor() {
        super();
        this._started = false;
    }

    // reads the primary monitor; true when anything changed
    read() {
        const display = global.display;
        const primary = display.get_primary_monitor();
        const monitor = primary >= 0 ? display.get_monitor_geometry(primary) : null;
        const scale = (primary >= 0 && display.get_monitor_scale(primary)) || FALLBACK_SCALE;
        const env = Number.parseFloat(GLib.getenv('VIBEOS_DENSITY') ?? '');
        const density = env > 0 ? env : scale;
        const ref = 3 * scale / density;
        const w = monitor?.width ? monitor.width * ref : FALLBACK_W;
        const h = monitor?.height ? monitor.height * ref : FALLBACK_H;
        const before = [SCREEN_SCALE, DENSITY, SCALE, WIDTH, HEIGHT].join();
        SCREEN_SCALE = scale;
        DENSITY = density;
        SCALE = ref;
        WIDTH = Math.round(w);
        HEIGHT = Math.round(h);
        SCREEN_W = Math.min(WIDTH, HEIGHT);
        SCREEN_H = Math.max(WIDTH, HEIGHT);
        SMALLEST_WIDTH_DP = SCREEN_W / 3;
        TABLET = SMALLEST_WIDTH_DP >= TABLET_SW_DP;
        return before !== [SCREEN_SCALE, DENSITY, SCALE, WIDTH, HEIGHT].join();
    }

    // Read now and after every change of the monitors. Called before any module of the extension connects to
    // monitors-changed, so this handler runs before theirs and they lay out with the new geometry.
    start() {
        if (this._started)
            return;
        this._started = true;
        this.read();
        this._id = Main.layoutManager.connect('monitors-changed', () => {
            if (this.read())
                this.emit('changed');
        });
    }

    stop() {
        if (!this._started)
            return;
        this._started = false;
        Main.layoutManager.disconnect(this._id);
        this._id = 0;
    }
}

export const geometry = new Geometry();
geometry.read();
