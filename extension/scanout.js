// Mutter puts a window that covers the whole screen (the launcher's) straight on the display, and then nothing the
// shell draws reaches the panel: the home screen had no status bar, and the flight home lost its icon halfway, at a
// missed frame where the switch happened (2026-11-04; the shell's screenshots paint the stage, so they showed it all
// the same). What the shell draws over windows keeps that off while it is on screen, as GNOME Shell's own OSDs and
// popups do. Under a fullscreen app nothing is (the status bar hides), and the app is scanned out.
//
// Keeping it off is a count in mutter that every caller raises and lowers, and one lowering from anywhere without
// its raise leaves the count one short for good. With only ours left on screen it then reaches zero while the
// status bar is still up: the launcher goes straight to the display and the bar is gone from the home screen until
// the shell restarts (2026-11-13, an S10e after 15 hours). Mutter ignores a lowering at zero, so a lowering and a
// raise together leave a right count where it was and lift one that was taken to zero back to one: after every
// lowering, ours or the shell's, while anything of ours is on screen, that is done.
import Meta from 'gi://Meta';

let holding = 0;            // our actors on screen now
let compositorEnable = null; // mutter's own enable_unredirect, while ours stands in for it

function patchEnable() {
    const proto = Meta.Compositor.prototype;
    compositorEnable = proto.enable_unredirect;
    const enable = compositorEnable;
    proto.enable_unredirect = function () {
        enable.call(this);
        if (holding > 0) {
            enable.call(this);
            this.disable_unredirect();
        }
    };
}

// Gives mutter's enable_unredirect back (the extension's disable, once every actor here is gone)
export function restoreUnredirect() {
    if (compositorEnable)
        Meta.Compositor.prototype.enable_unredirect = compositorEnable;
    compositorEnable = null;
    holding = 0;
}

export function overWindows(actor) {
    if (!compositorEnable)
        patchEnable();
    let held = false;
    const hold = on => {
        if (on === held)
            return;
        held = on;
        if (on) {
            holding++;
            global.compositor.disable_unredirect();
        } else {
            holding--;
            global.compositor.enable_unredirect();
        }
    };
    actor.connect('notify::mapped', () => hold(actor.mapped));
    actor.connect('destroy', () => hold(false));
    hold(actor.mapped);
}
