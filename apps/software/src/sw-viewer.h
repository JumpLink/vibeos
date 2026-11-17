/* SwViewer: an app's screenshots, full screen on black, each fitted to the screen.
 *
 * Swiped sideways it goes to the next or previous picture, one at a time, with a dot per picture; dragged up or down
 * the picture follows the finger while the black fades, and let go far enough (or flung) it closes, otherwise it
 * springs back. The close button at the top right, the back arrow at the top left, Back, Escape, Alt+Left and the
 * shell's back gesture close it too; Left and Right page with a keyboard. It grows out of the thumbnail tapped and
 * shrinks back into the thumbnail of the picture it shows.
 *
 * Two fingers pinch a picture larger (up to 4 times) or smaller, a double tap zooms in where it is tapped and out
 * again, and one finger moves a zoomed-in picture; zoomed in, it neither pages nor drags away. A single tap hides the
 * buttons and the dots, or shows them.
 *
 * Each picture shows the store's 540 px screenshot at once (the thumbnail's own, in memory already) and, where the
 * store has a full-size one (a screenshot's `full`), fades over to it once it has loaded; zoomed in, that is what
 * shows. A full-size picture is fetched the first time its page shows, not before. A store without them leaves the
 * 540 px pictures, scaled up. */
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
