/* Calendars as Settings manages them: "Add offline calendar", "Add CalDAV calendar" (Settings' main list) and a
 * calendar's own page (Display events, Color, Display name, Delete). */
#pragma once

#include "cal-model.h"

#include <gtk/gtk.h>

G_BEGIN_DECLS

void       cal_accounts_add_offline (GtkWidget *window);
void       cal_accounts_add_caldav  (GtkWidget *window);
/* TRUE when this system can add a CalDAV calendar (EDS's webdav backend is there) */
gboolean   cal_accounts_can_caldav  (void);
/* a calendar's page, pushed from the main list */
GtkWidget *cal_calendar_page_new    (GtkWidget *window, const CalCalendar *calendar);

G_END_DECLS
