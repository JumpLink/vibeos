/* NeoSnackbar: Material 3's snackbar, one at a time, at the bottom of whatever it is put over.
 *
 * Put it in a GtkOverlay above the content (neo_snackbar_new sets it to the bottom, full width). A message slides
 * up from the bottom edge, stays `duration` ms and slides back down; showing another one first replaces it. Its
 * "lift" is how far it currently covers the bottom (its height plus its margin, animated): the Clock's FAB row rises
 * by exactly that, so the snackbar never covers it. */
#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define NEO_SNACKBAR_SHORT 1500     /* ms: a short message */
#define NEO_SNACKBAR_LONG  2750     /* a long one */

#define NEO_TYPE_SNACKBAR (neo_snackbar_get_type ())
G_DECLARE_FINAL_TYPE (NeoSnackbar, neo_snackbar, NEO, SNACKBAR, GtkWidget)

typedef void (*NeoSnackbarAction) (gpointer data);

GtkWidget *neo_snackbar_new      (void);
/* `action` NULL for none; `action_cb` runs once, when the action is tapped (the snackbar then hides) */
void       neo_snackbar_show     (NeoSnackbar *self, const char *text, const char *action,
                                  NeoSnackbarAction action_cb, gpointer data, guint duration);
void       neo_snackbar_dismiss  (NeoSnackbar *self);
/* the space around the bar; 0 for an edge-to-edge one (Messaging's) */
void       neo_snackbar_set_margin (NeoSnackbar *self, int margin);
double     neo_snackbar_get_lift (NeoSnackbar *self);

/* Properties: "lift" (double, read-only) */

G_END_DECLS
