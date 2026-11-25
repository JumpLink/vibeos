// Power and a volume key together, as Android's key chords: with volume down a screenshot, with volume up the power
// menu (onPowerMenu: the menu's own chord, since holding the power key talks to Agent, keylaunch.js), the screen on.
// A screenshot is Android's chord: the second key's down within 150 ms of the first's, the first still held. The power
// menu forgives more: volume up pressed at any time while the power key is held, or the power key pressed at any time
// while volume up is held, as a hand does it, holding one key and then finding the other (a press of volume up 150 ms
// after the power key changed the volume, and one 500 ms after it found Agent listening, keylaunch.js; power.js then
// tells Agent to stop without acting). A key that is part of a chord then does nothing of its own: the power key
// neither sleeps on its up nor does anything when held, and a volume key leaves the volume as it was, except for the
// steps volume up already made, held, before the power key came. A volume key waits 150 ms for the power key before
// it acts. The shot is taken at once under a white flash (133 ms in, 217 ms out, both fast-out-slow-in), and saved,
// copied and announced as the shell's own screenshot key does it.
import Clutter from 'gi://Clutter';
import GLib from 'gi://GLib';
import Shell from 'gi://Shell';

import * as Main from 'resource:///org/gnome/shell/ui/main.js';
import {captureScreenshot} from 'resource:///org/gnome/shell/ui/screenshot.js';

import {animate, FAST_OUT_SLOW_IN} from './motion.js';
import {Color, cogl} from './palette.js';
import {overWindows} from './scanout.js';

const COMBINE_MS = 150;
const FLASH_IN_MS = 133;
const FLASH_OUT_MS = 217;

const now = () => GLib.get_monotonic_time() / 1000;

export class ScreenshotChord {
    constructor() {
        this._powerAt = 0;              // the power key's down (ms) while it is held and the screen was on
        this._volumeAt = 0;             // a volume key's, while held
        this._volume = 0;               // ... which: -1 down, +1 up
        this._powerTaken = false;       // the held key was part of a chord: it does nothing more until its up
        this._volumeTaken = false;
        this._pending = null;           // the volume key's own action, waiting for the power key
        this._pendingId = 0;
        this._anim = null;
        this.onPowerMenu = null;        // power and volume up: the power menu

        const m = Main.layoutManager.primaryMonitor;
        this._flash = new Clutter.Actor({x: m.x, y: m.y, width: m.width, height: m.height,
            background_color: cogl(Color.white),
            opacity: 0, visible: false});
        Main.layoutManager.uiGroup.add_child(this._flash);
        overWindows(this._flash);
    }

    // the power key's down; interactive: the screen was on (a press that wakes it starts no chord)
    powerDown(interactive) {
        this._powerTaken = false;
        this._powerAt = interactive ? now() : 0;
        if (this._powerAt && this._volumeAt && !this._volumeTaken &&
            (this._powerAt - this._volumeAt <= COMBINE_MS || this._volume > 0))
            this._fire(this._volume);
    }

    // its up: whether the press was the chord's
    powerUp() {
        const taken = this._powerTaken;
        this._powerAt = 0;
        this._powerTaken = false;
        return taken;
    }

    get powerTaken() {
        return this._powerTaken;
    }

    // a volume key (direction -1 down, +1 up), before it does `act`
    volumeKey(direction, act, repeat) {
        if (repeat) {
            // held after a chord, it does nothing; else it acts, after its first press if that is still waiting
            if (!this._volumeTaken) {
                this._flush();
                act();
            }
            return;
        }
        this._flush();
        this._volumeTaken = false;
        this._volumeAt = now();
        this._volume = direction;
        if (this._powerAt && !this._powerTaken && (this._volumeAt - this._powerAt <= COMBINE_MS || direction > 0)) {
            this._fire(direction);
            return;
        }
        this._pending = act;
        this._pendingId = GLib.timeout_add(GLib.PRIORITY_DEFAULT, COMBINE_MS, () => {
            this._pendingId = 0;
            this._flush();
            return GLib.SOURCE_REMOVE;
        });
    }

    // the volume key let go
    volumeRelease() {
        this._flush();
        this._volumeAt = 0;
        this._volume = 0;
        this._volumeTaken = false;
    }

    // the waiting volume action, now
    _flush() {
        const act = this._pending;
        this._cancelPending();
        act?.();
    }

    _cancelPending() {
        if (this._pendingId)
            GLib.source_remove(this._pendingId);
        this._pendingId = 0;
        this._pending = null;
    }

    _fire(direction) {
        this._powerTaken = this._volumeTaken = true;
        this._cancelPending();
        if (direction > 0)
            this.onPowerMenu?.();
        else
            this._shoot();
    }

    async _shoot() {
        try {
            const [content] = await new Shell.Screenshot().screenshot_stage_to_content();
            this._flashOnce();
            await captureScreenshot(content.get_texture(), null, 1, null);
        } catch (e) {
            logError(e, 'vibeos: screenshot');
        }
    }

    _flashOnce() {
        this._anim?.stop();
        Main.layoutManager.uiGroup.set_child_above_sibling(this._flash, null);
        this._flash.show();
        this._anim = animate(this._flash, {
            from: this._flash.opacity / 255, to: 1, duration: FLASH_IN_MS, interpolator: FAST_OUT_SLOW_IN,
            onUpdate: a => (this._flash.opacity = Math.round(a * 255)),
            onDone: () => {
                this._anim = animate(this._flash, {
                    from: 1, to: 0, duration: FLASH_OUT_MS, interpolator: FAST_OUT_SLOW_IN,
                    onUpdate: a => (this._flash.opacity = Math.round(a * 255)),
                    onDone: () => {
                        this._anim = null;
                        this._flash.hide();
                    },
                });
            },
        });
    }

    destroy() {
        this._cancelPending();
        this._anim?.stop();
        this._flash.destroy();
    }
}
