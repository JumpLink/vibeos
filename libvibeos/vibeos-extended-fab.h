/* VibeosExtendedFab: Material 3's extended floating action button, which collapses to an icon button on scroll.
 *
 * Extended, it is an icon and a label on a 56 px tall card with 16 px corners, in the primary container's colours and
 * raised with a shadow. Collapsed, only the icon shows, on a 56 px square with the same corners. The icon stays where
 * it is; the card's end moves and the label fades out with it (and back), over 250 ms. A list collapses it when it is
 * scrolled away from its top and extends it again at the top.
 *
 * A tap emits "clicked". Its accessible name is the label, extended or not. */
#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define VIBEOS_TYPE_EXTENDED_FAB (vibeos_extended_fab_get_type ())
G_DECLARE_FINAL_TYPE (VibeosExtendedFab, vibeos_extended_fab, VIBEOS, EXTENDED_FAB, GtkWidget)

GtkWidget *vibeos_extended_fab_new          (const char *icon_name, const char *label);
/* extended (TRUE, the default) or collapsed; animated when on screen */
void       vibeos_extended_fab_set_extended (VibeosExtendedFab *self, gboolean extended);
gboolean   vibeos_extended_fab_get_extended (VibeosExtendedFab *self);

/* Signals: "clicked" () */

G_END_DECLS
