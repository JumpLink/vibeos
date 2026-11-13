/* The action bar of the Clock's second-level pages, the cities picker and Settings:
 * the Up arrow at the start, an optional title, and action icons at the end.
 *
 * The Up button's accessible label is "Back": that is what the shell's back gesture presses (its description is
 * "Navigate up"). */
#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

/* @title: NULL for none (the cities picker) */
GtkWidget *clock_bar_new (const char *title);
GtkWidget *clock_bar_get_up (GtkWidget *bar);
/* an icon button @width px wide at the end, after the ones added before it; @label is its accessible name */
GtkWidget *clock_bar_add_icon (GtkWidget *bar, const char *icon, int width, const char *label);
/* a widget between the Up button and the end icons, taking the free width (the search field) */
void       clock_bar_set_custom (GtkWidget *bar, GtkWidget *custom);

G_END_DECLS
