/* StopwatchPage: the Stopwatch tab (DeskClock's StopwatchFragment, L/stopwatch_fragment.xml; spec/clock.md §8).
 *
 * The time in a ring, with the laps under it once there are any: newest first, the one still running on top. The
 * ring then measures the lap running against the first one. A tap in the ring starts or pauses the stopwatch, as
 * the FAB does; the buttons beside it reset, take a lap or (paused) share the times. */
#pragma once

#include <gtk/gtk.h>
#include <neo.h>

G_BEGIN_DECLS

#define STOPWATCH_TYPE_PAGE (stopwatch_page_get_type ())
G_DECLARE_FINAL_TYPE (StopwatchPage, stopwatch_page, STOPWATCH, PAGE, GtkWidget)

GtkWidget *stopwatch_page_new (NeoSnackbar *snackbar);

G_END_DECLS
