/* The Calendar's window: its toolbar, the four views, the FAB and the drawer, and the screens opened over it (event
 * info, the editor, settings) as pages pushed on top, each with its own way back. */
#pragma once

#include "cal-model.h"
#include "cal-store.h"

#include <gtk/gtk.h>

G_BEGIN_DECLS

GtkWidget *cal_window_new (GtkApplication *app, CalStore *store, GSettings *settings);

CalStore  *cal_window_get_store    (GtkWidget *window);
GSettings *cal_window_get_settings (GtkWidget *window);

/* a screen over the views, and back from it */
void       cal_window_push (GtkWidget *window, GtkWidget *page);
void       cal_window_pop  (GtkWidget *window);

/* the event's info screen */
void       cal_window_show_event   (GtkWidget *window, CalEvent *event);
/* the editor: a new event from @start (rounded as the FAB rounds it when @round), or @event changed */
void       cal_window_new_event    (GtkWidget *window, gint64 start, gboolean all_day);
void       cal_window_edit_event   (GtkWidget *window, CalEvent *event, gboolean duplicate);
/* the views at @day (Unix seconds, from a reminder or "Go to…") */
void       cal_window_go_to        (GtkWidget *window, int day);
/* the event's info from a reminder: its calendar, UID and RECURRENCE-ID, near @start */
void       cal_window_open_reminder (GtkWidget *window, const char *calendar, const char *uid, const char *rid,
                                     gint64 start);
/* a toast over everything ("Event saved.") */
void       cal_window_toast        (GtkWidget *window, const char *text);
/* events from an iCalendar file, into the editor */
void       cal_window_import       (GtkWidget *window, GFile *file);

G_END_DECLS
