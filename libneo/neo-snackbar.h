/* NeoSnackbar: Material 3's snackbar, one at a time, at the bottom of whatever it is put over.
 *
 * Put it in a GtkOverlay above the content (neo_snackbar_new sets it to the bottom, full width). A message slides
 * up from the bottom edge, stays `duration` ms and slides back down; showing another one first replaces it, as
 * SnackbarManager dismisses the old one. Its "lift" is how far it currently covers the bottom (its height plus its
 * 8 dp margin, animated): DeskClock's FAB row rises by exactly that (SnackbarSlidingBehavior). */
#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define NEO_SNACKBAR_SHORT 1500     /* Snackbar.LENGTH_SHORT */
#define NEO_SNACKBAR_LONG  2750     /* LENGTH_LONG */

#define NEO_TYPE_SNACKBAR (neo_snackbar_get_type ())
G_DECLARE_FINAL_TYPE (NeoSnackbar, neo_snackbar, NEO, SNACKBAR, GtkWidget)

typedef void (*NeoSnackbarAction) (gpointer data);

GtkWidget *neo_snackbar_new      (void);
/* `action` NULL for none; `action_cb` runs once, when the action is tapped (the snackbar then hides) */
void       neo_snackbar_show     (NeoSnackbar *self, const char *text, const char *action,
                                  NeoSnackbarAction action_cb, gpointer data, guint duration);
void       neo_snackbar_dismiss  (NeoSnackbar *self);
/* the space around the bar, 8 dp by default; 0 for an edge-to-edge one (the older Material, AOSP Messaging's) */
void       neo_snackbar_set_margin (NeoSnackbar *self, int margin);
double     neo_snackbar_get_lift (NeoSnackbar *self);

/* Properties: "lift" (double, read-only) */

G_END_DECLS
