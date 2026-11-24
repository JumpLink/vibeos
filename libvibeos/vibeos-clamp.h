/* VibeosClamp: one child, no wider than a maximum, centred in the width the clamp is given: a page's content kept to
 * a column on a tablet, under a bar that runs the window's whole width.
 *
 * The clamp asks for no more than its child does. A column made with margins worked out from the window's width asked
 * for those margins too, so a window turned upright could not shrink to the upright width (mutter keeps a maximized
 * window at its size when its minimum is wider than the monitor) and stayed sideways, half off the screen. */
#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define VIBEOS_TYPE_CLAMP (vibeos_clamp_get_type ())
G_DECLARE_FINAL_TYPE (VibeosClamp, vibeos_clamp, VIBEOS, CLAMP, GtkWidget)

/* a settings page's rows on a tablet: a column this wide in the middle rather than rows across the whole screen with
 * their switches far from their text. A phone is narrower, so there the rows keep the screen's width */
#define VIBEOS_CLAMP_SETTINGS 720

/* `maximum` in px; -1 for none (the child takes the whole width) */
GtkWidget *vibeos_clamp_new         (GtkWidget *child, int maximum);
void       vibeos_clamp_set_maximum (VibeosClamp *self, int maximum);
int        vibeos_clamp_get_maximum (VibeosClamp *self);
GtkWidget *vibeos_clamp_get_child   (VibeosClamp *self);

G_END_DECLS
