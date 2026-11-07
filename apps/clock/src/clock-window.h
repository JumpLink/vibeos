/* The Clock's window (DeskClock's DeskClock activity, L/desk_clock.xml; spec/clock.md §1).
 *
 * Top to bottom: a 64 dp app bar with the tab's name; the tab's page, with the floating action button over its
 * bottom and the snackbar under that; the bottom navigation bar. Tabs change at once, with no animation. */
#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

GtkWidget *clock_window_new        (GtkApplication *app, GSettings *settings);
/* the Alarm tab, at that alarm (the alarm service's "show-alarm") */
void       clock_window_show_alarm (GtkWidget *window, guint alarm_id);

G_END_DECLS
