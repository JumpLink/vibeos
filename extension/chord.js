// Power and volume down together take a screenshot: the second key's down within 150 ms of the first's, the first
// still held, and the screen on. Neither key then does its own thing: the power key neither sleeps on its up nor
// opens the menu when held, and the volume stays as it was. A volume down waits those 150 ms for the power key before
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
        this._volumeAt = 0;             // volume down's, while held
        this._powerTaken = false;       // the held key was part of a chord: it does nothing more until its up
        this._volumeTaken = false;
        this._pending = null;           // volume down's own action, waiting for the power key
        this._pendingId = 0;
        this._anim = null;

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
        if (this._powerAt && this._volumeAt && this._powerAt - this._volumeAt <= COMBINE_MS && !this._volumeTaken)
            this._fire();
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

    // a volume down, before it does `act`
    volumeDown(act, repeat) {
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
        if (this._powerAt && this._volumeAt - this._powerAt <= COMBINE_MS && !this._powerTaken) {
            this._fire();
            return;
        }
        this._pending = act;
        this._pendingId = GLib.timeout_add(GLib.PRIORITY_DEFAULT, COMBINE_MS, () => {
            this._pendingId = 0;
            this._flush();
            return GLib.SOURCE_REMOVE;
        });
    }

    volumeUp() {
        this._flush();
        this._volumeAt = 0;
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

    _fire() {
        this._powerTaken = this._volumeTaken = true;
        this._cancelPending();
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
