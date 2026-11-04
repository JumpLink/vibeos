// Mutter puts a window that covers the whole screen (the launcher's) straight on the display, and then nothing the
// shell draws reaches the panel: the home screen had no status bar, and the flight home lost its icon halfway, at a
// missed frame where the switch happened (2026-11-04; the shell's screenshots paint the stage, so they showed it all
// the same). What the shell draws over windows keeps that off while it is on screen, as GNOME Shell's own OSDs and
// popups do. Under a fullscreen app nothing is (the status bar hides), and the app is scanned out.
export function overWindows(actor) {
    let held = false;
    const hold = on => {
        if (on === held)
            return;
        held = on;
        if (on)
            global.compositor.disable_unredirect();
        else
            global.compositor.enable_unredirect();
    };
    actor.connect('notify::mapped', () => hold(actor.mapped));
    actor.connect('destroy', () => hold(false));
    hold(actor.mapped);
}
