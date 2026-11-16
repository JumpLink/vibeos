/* VibeosTopBar: Material 3's small top app bar, with its large and contextual forms.
 *
 * A title on one line, ellipsized, and icon buttons at the end. A page under another has a back button at the start:
 * an arrow in a tonal circle, a larger circle on a tablet; a tap on it emits "back". The bar has no shadow and no
 * fill of its own unless given one (vibeos_top_bar_set_fill), so it takes the page's colour.
 *
 * Large: the title starts big, in a taller band under the bar, and as the content scrolls (the adjustment given to
 * vibeos_top_bar_set_scroll) the band collapses and the big title gives way to the small one. A large bar goes over
 * its content in an overlay, the content starting under the expanded band; it takes touches only where it is drawn.
 *
 * Contextual (selection mode, vibeos_top_bar_set_selection): the back button becomes a close button, which emits
 * "selection-closed", the title says what is selected, and the selection's actions replace the usual ones. */
#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define VIBEOS_TYPE_TOP_BAR (vibeos_top_bar_get_type ())
G_DECLARE_FINAL_TYPE (VibeosTopBar, vibeos_top_bar, VIBEOS, TOP_BAR, GtkWidget)

GtkWidget *vibeos_top_bar_new           (const char *title);
void       vibeos_top_bar_set_title     (VibeosTopBar *self, const char *title);
/* the back button at the start */
void       vibeos_top_bar_set_back      (VibeosTopBar *self, gboolean back);
/* an icon button at the end, after those added before; returns it (a VibeosPressable: connect to "clicked") */
GtkWidget *vibeos_top_bar_add_action    (VibeosTopBar *self, const char *icon_name);
/* the lower bar settings pages have, rather than the usual one */
void       vibeos_top_bar_set_compact   (VibeosTopBar *self, gboolean compact);
/* a fill behind the bar (NULL: none, the default) */
void       vibeos_top_bar_set_fill      (VibeosTopBar *self, const GdkRGBA *fill);

/* the large title; it collapses as `vadjustment` scrolls (NULL: it stays expanded) */
void       vibeos_top_bar_set_large     (VibeosTopBar *self, gboolean large);
void       vibeos_top_bar_set_scroll    (VibeosTopBar *self, GtkAdjustment *vadjustment);
/* how much taller the expanded large bar is than the small one, in px: the content's top margin under it */
int        vibeos_top_bar_get_large_extra (VibeosTopBar *self);

/* selection mode with `title` ("3 selected"), or out of it with NULL */
void       vibeos_top_bar_set_selection (VibeosTopBar *self, const char *title);
/* an icon button shown in selection mode only */
GtkWidget *vibeos_top_bar_add_selection_action (VibeosTopBar *self, const char *icon_name);

/* Signals: "back" (), "selection-closed" () */

G_END_DECLS
