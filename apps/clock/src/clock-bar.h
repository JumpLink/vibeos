/* The action bar of the Clock's second-level pages, the cities picker and Settings (AppCompat's Toolbar, 64 dp,
 * spec/clock.md §6.2 and §10.1): the Up arrow in a 56 dp button at the start, an optional title 72 dp in, and
 * action icons at the end.
 *
 * The Up button's accessible label is "Back": that is what the shell's back gesture presses ("Navigate up", what
 * Android reads out, is its description). */
#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

/* @title: NULL for none (the cities picker) */
GtkWidget *clock_bar_new (const char *title);
GtkWidget *clock_bar_get_up (GtkWidget *bar);
/* an icon button @width dp wide at the end, after the ones added before it; @label is its accessible name */
GtkWidget *clock_bar_add_icon (GtkWidget *bar, const char *icon, int width, const char *label);
/* a widget between the Up button and the end icons, taking the free width (the search field) */
void       clock_bar_set_custom (GtkWidget *bar, GtkWidget *custom);

G_END_DECLS
