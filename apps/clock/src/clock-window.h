/* The Clock's window (DeskClock's DeskClock activity, L/desk_clock.xml; spec/clock.md §1).
 *
 * Top to bottom: a 64 dp app bar with the tab's name; the tab's page, with the row of buttons over its bottom (the
 * floating action button and one either side, each tab's own: clock-tab.h) and the snackbar under that; the bottom
 * navigation bar with the four tabs. Tabs change at once, with no animation. */
#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

GtkWidget *clock_window_new        (GtkApplication *app, GSettings *settings);
/* the Alarm tab, at that alarm (the alarm service's "show-alarm") */
void       clock_window_show_alarm (GtkWidget *window, guint alarm_id);
/* the Timer tab, at that timer, and the Stopwatch tab (a tap on their notifications: "show-timer",
 * "show-stopwatch") */
void       clock_window_show_timer (GtkWidget *window, guint timer_id);
void       clock_window_show_stopwatch (GtkWidget *window);

G_END_DECLS
