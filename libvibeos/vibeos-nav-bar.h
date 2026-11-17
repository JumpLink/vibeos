/* VibeosNavBar: Material 3's bottom navigation bar, every item labelled.
 *
 * The items sit in equal columns up to a maximum width: on a wider bar they stay together in its middle. The selected
 * item has a pill behind its icon, and its icon and label in the primary colour; the others are dimmed. With
 * horizontal items (a phone turned on its side) each label sits beside its icon, inside the pill, on a lower bar.
 * A tap selects an item and emits "selected" (its index); there is no swipe between tabs. */
#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define VIBEOS_TYPE_NAV_BAR (vibeos_nav_bar_get_type ())
G_DECLARE_FINAL_TYPE (VibeosNavBar, vibeos_nav_bar, VIBEOS, NAV_BAR, GtkWidget)

GtkWidget *vibeos_nav_bar_new          (void);
/* returns the item's index */
int        vibeos_nav_bar_append       (VibeosNavBar *self, const char *icon_name, const char *label);
/* selects without emitting "selected" and without the indicator's animation */
void       vibeos_nav_bar_set_selected (VibeosNavBar *self, int index);
int        vibeos_nav_bar_get_selected (VibeosNavBar *self);
/* a badge on an item's icon: a number ("3") in a small pill in the error colour, a dot for "", none for NULL */
void       vibeos_nav_bar_set_badge    (VibeosNavBar *self, int index, const char *text);
/* the label beside the icon rather than under it; off by default */
void       vibeos_nav_bar_set_horizontal_items (VibeosNavBar *self, gboolean horizontal);
gboolean   vibeos_nav_bar_get_horizontal_items (VibeosNavBar *self);

/* Signals: "selected" (int index) */

G_END_DECLS
