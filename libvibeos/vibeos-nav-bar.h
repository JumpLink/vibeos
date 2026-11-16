/* VibeosNavBar: Material 3's bottom navigation bar, every item labelled.
 *
 * The items sit in equal columns. The selected item has a pill behind its icon, and its icon and label in the primary
 * colour; the others are dimmed.
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

/* Signals: "selected" (int index) */

G_END_DECLS
