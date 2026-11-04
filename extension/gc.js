// GJS collects the whole JS heap, in one piece, 10 s after any object made in JS loses its last other reference: an
// actor destroyed, an ease finished, which is nearly every gesture. Here that is a 25-30 ms stop of the shell, and it
// fell in whatever animation ran 10 s later: the next gesture's, a third of the time (2026-11-04). Once the screen
// has been still for a moment after drawing, GJS's collection timer is run there and then instead.
//
// GJS keeps the timer's id to itself, so it is looked for among the main context's sources by the name GJS gives it
// ("[gjs] Garbage Collection", with " (Big Hammer)" when it was a full collection from the start; a timer for a mere
// check becomes a full collection too once an object goes while it waits). Source ids only ever grow: each look
// starts where the last one ended, a few hundred ids after a gesture, about 2 ms.
import GLib from 'gi://GLib';

const STILL_MS = 300;
const NAME = '[gjs] Garbage Collection';
const FIRST_LOOK = 20000;       // ids looked back over at the start: 20-55 ms, once

export class CollectWhenStill {
    constructor() {
        this._looked = Math.max(0, newestSourceId() - FIRST_LOOK);
        this._painted = 0;
        this._timer = 0;
        this._paintId = global.stage.connect('after-paint', () => {
            this._painted = GLib.get_monotonic_time();
            this._timer ||= GLib.timeout_add(GLib.PRIORITY_DEFAULT, STILL_MS, () => this._still());
        });
    }

    _still() {
        const wait = this._painted / 1000 + STILL_MS - GLib.get_monotonic_time() / 1000;
        if (wait > 0) {
            this._timer = GLib.timeout_add(GLib.PRIORITY_DEFAULT, Math.ceil(wait), () => this._still());
            return GLib.SOURCE_REMOVE;
        }
        this._timer = 0;
        const context = GLib.MainContext.default();
        const newest = newestSourceId();
        for (let id = this._looked + 1; id < newest; id++) {
            const source = context.find_source_by_id(id);
            if (source?.get_name()?.startsWith(NAME))
                source.set_ready_time(0);
        }
        this._looked = newest;
        return GLib.SOURCE_REMOVE;
    }

    destroy() {
        global.stage.disconnect(this._paintId);
        if (this._timer)
            GLib.source_remove(this._timer);
        this._timer = 0;
    }
}

// the newest source id: a source's, made and dropped at once
function newestSourceId() {
    const id = GLib.idle_add(GLib.PRIORITY_LOW, () => GLib.SOURCE_REMOVE);
    GLib.source_remove(id);
    return id;
}
