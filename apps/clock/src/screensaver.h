/* The screen saver: DeskClock's ScreensaverActivity, the "Preview" (spec/clock.md §10.7), opened from Settings ▸
 * Screen saver ▸ Preview or a long press on the Clock tab.
 *
 * A black full-screen window with the clock floating on it: the digital time (or the analog clock, as Settings ▸
 * Screen saver settings ▸ Style says), the date and the next alarm. It fades in somewhere at random, and once a
 * minute it fades out, shrinking, and comes back somewhere else (MoveScreensaverRunnable), so no part of the screen
 * shows the same thing for long. Its colour and dimness are the screen saver settings' (Utils.dimClockView). Any
 * touch or key closes it.
 *
 * Android also runs the same clock as a Daydream, started by the system when the phone is docked or charging; GNOME
 * has nothing that starts one, so only the Preview is here (the owner's choice, spec/clock.md B5). */
#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

/* opens the screen saver window over the app */
void screensaver_present (GtkApplication *app, GSettings *settings);

G_END_DECLS
