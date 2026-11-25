/* cam-window: the camera screen and its Settings page.
 *
 * The window is full screen and black; the screen is chained bottom-up from the window's bottom edge: the mode
 * selector, the primary bar (gallery, shutter, flip or pause), the secondary bar (pro button, lens buttons, flash,
 * with its divider) and the viewfinder over the rest, with the pro row sliding up over the viewfinder's bottom. One
 * camera stream runs while the screen shows; it stops when the window is hidden or Settings is open. The window is
 * up before GStreamer has started: GStreamer, the cameras and the stream come from a thread of their own
 * (cam-startup.h), and the viewfinder says "Starting camera…", or "No camera found" if none answers. */
#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

GtkWindow *cam_window_new        (GtkApplication *app);
/* the app was started again while its window is there: a start that found no camera looks again */
void       cam_window_activated  (GtkWindow *window);
/* the shell's "volume-key" action: +1 up, -1 down */
void       cam_window_volume_key (GtkWindow *window, int direction);

G_END_DECLS
