/* ClockPage: the Clock tab.
 *
 * The time, large; under it the date and, when an alarm is set, when the next one rings.
 *
 * Signals: "show-cities" (): for the cities picker; "show-screensaver" (): a long press on the tab, which starts the
 * screen saver. */
#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define CLOCK_TYPE_PAGE (clock_page_get_type ())
G_DECLARE_FINAL_TYPE (ClockPage, clock_page, CLOCK, PAGE, GtkWidget)

GtkWidget *clock_page_new (GSettings *settings);

G_END_DECLS
