/* The event editor.
 *
 * "✕ CANCEL │ ✓ DONE" at the top; the title; the calendar (chosen for a new event, shown for an existing one) and
 * the event's colour (cal-color-picker); All day; the start and end dates and times; the time zone
 * (cal-tz-picker); guests, location, description and URL in outlined fields (cal-outlined-field); the repeat rule;
 * the reminders; Show me as (TRANSP) and Privacy (CLASS).
 *
 * Guests are kept with the event and never invited: they go in with SCHEDULE-AGENT=CLIENT and no ORGANIZER, so no
 * server mails them either, and a helper line under the field says so.
 * Left out, each because nothing behind it would do what it shows:
 *  - the reminders' method spinner (cal-reminders.h);
 *  - "Tentative" in Show me as: iCalendar's TRANSP has only OPAQUE (Busy) and TRANSPARENT (Available).
 * The repeat button opens the repeat dialog (cal-recurrence-picker.h); it is off for a rule the dialog cannot
 * show or has no words for, and a rule made elsewhere is kept as it is unless another is chosen. */
#pragma once

#include "cal-model.h"

#include <gtk/gtk.h>

G_BEGIN_DECLS

/* @event NULL: a new event from @start (all-day if @all_day), lasting the default duration. Else @event changed,
 * or with @duplicate a new event with its contents (Duplicate). */
GtkWidget *event_editor_new    (GtkWidget *window, CalEvent *event, gint64 start, gboolean all_day,
                                gboolean duplicate);
/* the first event of an iCalendar file, in the editor as a new event */
void       event_editor_import (GtkWidget *window, GFile *file);

G_END_DECLS
