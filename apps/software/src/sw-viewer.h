/* SwViewer: an app's screenshots, full screen on black, each fitted to the screen.
 *
 * Swiped sideways it goes to the next or previous picture, one at a time, with a dot per picture; dragged up or down
 * the picture follows the finger while the black fades, and let go far enough (or flung) it closes, otherwise it
 * springs back. Back, Escape, Alt+Left and the shell's back gesture close it too; Left and Right page with a
 * keyboard. It grows out of the thumbnail tapped and shrinks back into the thumbnail of the picture it shows.
 *
 * The store serves screenshots 540 px wide only (no larger picture), so the viewer shows the thumbnails' own
 * pictures, scaled up. */
#pragma once

#include <gtk/gtk.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

#define SW_TYPE_VIEWER (sw_viewer_get_type ())
G_DECLARE_FINAL_TYPE (SwViewer, sw_viewer, SW, VIEWER, GtkWidget)

/* opens the screenshots `shots` (the API's Screenshot objects) at `index`, from `thumb`, the thumbnail tapped;
 * `thumbs` are all the thumbnails, in the same order, to close into */
void sw_viewer_present (GtkWidget *thumb, JsonArray *shots, guint index, GPtrArray *thumbs);

G_END_DECLS
