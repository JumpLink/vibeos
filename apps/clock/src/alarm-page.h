/* AlarmPage: the Alarm tab (DeskClock's AlarmClockFragment, L/alarm_clock.xml; spec/clock.md §2.1).
 *
 * The alarms as cards in a list that scrolls the Android way, at most one of them expanded, or "No Alarms" when
 * there are none. It shows its snackbars ("Alarm set for ...", "Alarm deleted" with undo) on the window's. */
#pragma once

#include <gtk/gtk.h>
#include <neo.h>

G_BEGIN_DECLS

#define ALARM_TYPE_PAGE (alarm_page_get_type ())
G_DECLARE_FINAL_TYPE (AlarmPage, alarm_page, ALARM, PAGE, GtkWidget)

GtkWidget *alarm_page_new        (GSettings *settings, NeoSnackbar *snackbar);
/* the FAB: the time picker at the current time, then a new alarm, expanded and scrolled to */
void       alarm_page_add_alarm  (AlarmPage *self);
/* expands the alarm and scrolls to it (a notification's "show alarm"); a snackbar if it is gone */
void       alarm_page_show_alarm (AlarmPage *self, guint id);

G_END_DECLS
