/* ClockPage: the Clock tab (DeskClock's ClockFragment, L/main_clock_frame.xml; spec/clock.md §6.1).
 *
 * The time, large; under it the date and, when an alarm is set, when the next one rings. */
#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define CLOCK_TYPE_PAGE (clock_page_get_type ())
G_DECLARE_FINAL_TYPE (ClockPage, clock_page, CLOCK, PAGE, GtkWidget)

GtkWidget *clock_page_new (GSettings *settings);

G_END_DECLS
