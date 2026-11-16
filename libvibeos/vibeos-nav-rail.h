/* VibeosNavRail: Material 3's navigation rail, the tablet's (and a turned phone's) form of the bottom navigation bar.
 *
 * A narrow column at the window's start with the destinations stacked in its middle, each an icon in a pill with its
 * label under it. The selected one has the pill filled, and its icon and label in the primary colour; the others
 * are dimmed, as on VibeosNavBar. Without labels (vibeos_nav_rail_set_labels) each destination is just the icon in a
 * taller, rounder pill.
 *
 * A header (a menu button, a FAB) can sit at the top; the destinations then stay below it.
 * A tap selects an item and emits "selected" (its index). */
#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define VIBEOS_TYPE_NAV_RAIL (vibeos_nav_rail_get_type ())
G_DECLARE_FINAL_TYPE (VibeosNavRail, vibeos_nav_rail, VIBEOS, NAV_RAIL, GtkWidget)

GtkWidget *vibeos_nav_rail_new          (void);
/* returns the item's index */
int        vibeos_nav_rail_append       (VibeosNavRail *self, const char *icon_name, const char *label);
/* selects without emitting "selected" and without the indicator's animation */
void       vibeos_nav_rail_set_selected (VibeosNavRail *self, int index);
int        vibeos_nav_rail_get_selected (VibeosNavRail *self);
/* labelled (the default), or icons only */
void       vibeos_nav_rail_set_labels   (VibeosNavRail *self, gboolean labels);
/* a widget at the top, over the destinations (NULL for none) */
void       vibeos_nav_rail_set_header   (VibeosNavRail *self, GtkWidget *header);

/* Signals: "selected" (int index) */

G_END_DECLS
