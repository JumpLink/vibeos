/* The event editor: Etar's EditEventActivity (EditEventFragment, EditEventView; spec/calendar.md §7).
 *
 * "✕ CANCEL │ ✓ DONE" at the top; the title; the calendar (chosen for a new event, shown for an existing one) and
 * the event's colour (EventColorPickerDialog); All day; the start and end dates and times; the time zone
 * (cal-tz-picker); guests, location, description and URL in Material 3 outlined fields (cal-outlined-field); the
 * repeat rule; the reminders; Show me as (TRANSP) and Privacy (CLASS).
 *
 * Guests are kept with the event and never invited: they go in with SCHEDULE-AGENT=CLIENT and no ORGANIZER, so no
 * server mails them either, and a helper line under the field says so (Etar has none: its provider invites).
 * Left out, each because nothing behind it would do what it shows:
 *  - the reminders' method spinner (cal-reminders.h);
 *  - "Tentative" in Show me as: iCalendar's TRANSP has only OPAQUE (Busy) and TRANSPARENT (Available).
 * The repeat button opens RecurrencePickerDialog (cal-recurrence-picker.h); it is off for a rule the dialog cannot
 * show or has no words for, as Etar's is, and a rule made elsewhere is kept as it is unless another is chosen. */
#pragma once

#include "cal-model.h"

#include <gtk/gtk.h>

G_BEGIN_DECLS

/* @event NULL: a new event from @start (all-day if @all_day), lasting the default duration. Else @event changed,
 * or with @duplicate a new event with its contents (Etar's Duplicate). */
GtkWidget *event_editor_new    (GtkWidget *window, CalEvent *event, gint64 start, gboolean all_day,
                                gboolean duplicate);
/* the first event of an iCalendar file, in the editor as a new event (Etar's ImportActivity) */
void       event_editor_import (GtkWidget *window, GFile *file);

G_END_DECLS
