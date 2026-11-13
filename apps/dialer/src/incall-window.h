/* The in-call app's one window: the answer screen while a call rings, the in-call screen otherwise, chosen
 * again on each change of the calls.
 *
 * It follows the call service: it opens on the call the screen is about, changes with it, and closes once no call
 * is left. Closing it by hand (Back) leaves a call that is up going; the ongoing-call notification brings it back.
 * A ringing call cannot be put away with Back. */
#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

/* brings the window up, opening it if there is a call to show; with no call there is none */
void incall_window_present (GtkApplication *app);
/* a volume key, which the shell passes on while the answer screen is in front: it silences the ringer */
void incall_window_volume_key (int direction);

G_END_DECLS
