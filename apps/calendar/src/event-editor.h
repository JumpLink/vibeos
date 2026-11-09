/* The event editor: Etar's EditEventActivity (EditEventFragment, EditEventView; spec/calendar.md §7).
 *
 * "✕ CANCEL │ ✓ DONE" at the top; the title; the calendar (chosen for a new event, shown for an existing one);
 * All day; the start and end dates and times; location, description, URL; the repeat rule; the reminders.
 *
 * Left out, each because nothing behind it would do what it shows:
 *  - Guests: Etar hides them for a calendar without attendee support, and inviting needs a mail account the phone
 *    does not have.
 *  - Time zone: events are written in the phone's zone; there is no zone picker.
 *  - Event colour: no event-colour palette on EDS calendars (Etar shows it only for accounts with one).
 *  - Availability and visibility (TRANSP, CLASS): not read or written by the store yet.
 * The repeat button offers Etar's common rules (one-time, daily, weekly on the start's weekday, monthly, yearly)
 * rather than the full RecurrencePickerDialog; a rule made elsewhere is kept as it is unless another is chosen. */
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
