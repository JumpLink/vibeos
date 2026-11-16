/* VibeosSnackbar: Material 3's snackbar, one at a time, at the bottom of whatever it is put over.
 *
 * Put it in a GtkOverlay above the content (vibeos_snackbar_new sets it to the bottom, full width). A message slides
 * up from the bottom edge, stays `duration` ms and slides back down; showing another one first replaces it. Its
 * "lift" is how far it currently covers the bottom (its height plus its margin, animated): the Clock's FAB row rises
 * by exactly that, so the snackbar never covers it. */
#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define VIBEOS_SNACKBAR_SHORT 1500     /* ms: a short message */
#define VIBEOS_SNACKBAR_LONG  2750     /* a long one */

#define VIBEOS_TYPE_SNACKBAR (vibeos_snackbar_get_type ())
G_DECLARE_FINAL_TYPE (VibeosSnackbar, vibeos_snackbar, VIBEOS, SNACKBAR, GtkWidget)

typedef void (*VibeosSnackbarAction) (gpointer data);

GtkWidget *vibeos_snackbar_new      (void);
/* `action` NULL for none; `action_cb` runs once, when the action is tapped (the snackbar then hides) */
void       vibeos_snackbar_show     (VibeosSnackbar *self, const char *text, const char *action,
                                  VibeosSnackbarAction action_cb, gpointer data, guint duration);
void       vibeos_snackbar_dismiss  (VibeosSnackbar *self);
/* the space around the bar; 0 for an edge-to-edge one (Messaging's) */
void       vibeos_snackbar_set_margin (VibeosSnackbar *self, int margin);
double     vibeos_snackbar_get_lift (VibeosSnackbar *self);

/* Properties: "lift" (double, read-only) */

G_END_DECLS
