// Whole-frame redraws while a background blur shows.
//
// Shell.BlurEffect in BACKGROUND mode blurs what the framebuffer already holds under its actor. Mutter redraws only
// what changed (the clip, with the buffer's age), so outside that clip the framebuffer holds a finished earlier frame:
// the blur's own result, the tint over it and everything drawn over that. Within its radius the blur takes those in,
// and every small change comes out as a grey box the size of its clip: the PIN's dots and the message over the
// bouncer, a box over the shade (POCO, 2026-11-09). A screenshot draws the whole stage at once and never shows it.
// So while such a blur shows, every frame is drawn whole: mutter's DISABLE_CLIPPED_REDRAWS, the flag Looking Glass
// toggles. Nothing changing means no frame at all, so it costs only frames that are drawn anyway.
import Clutter from 'gi://Clutter';
import Meta from 'gi://Meta';

const FLAG = Clutter.DrawDebugFlag.DISABLE_CLIPPED_REDRAWS;
// mutter 51 has them in Clutter; before, Meta.add_clutter_debug_flags
const addFlags = Clutter.add_debug_flags ?? Meta.add_clutter_debug_flags;
const removeFlags = Clutter.remove_debug_flags ?? Meta.remove_clutter_debug_flags;
const holders = new Set();

// @owner needs whole frames (on) or no longer does
export function wholeFrames(owner, on) {
    const before = holders.size > 0;
    if (on)
        holders.add(owner);
    else
        holders.delete(owner);
    const after = holders.size > 0;
    if (before === after)
        return;
    if (after) {
        addFlags(0, FLAG, 0);
        // the frame on screen may already carry a box: draw it whole once
        global.stage.queue_redraw();
    } else {
        removeFlags(0, FLAG, 0);
    }
}
