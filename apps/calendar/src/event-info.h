/* The event's info screen: Etar's EventInfoActivity / EventInfoFragment (spec/calendar.md §6).
 *
 * A toolbar with Back and the actions the event allows, in Etar's order: Edit (a writable calendar, the user the
 * organizer), Delete (writable), Duplicate, Share (with an app to share an .ics with), Export event, Choose event
 * color (writable); as many as ActionBarPolicy fits (3 on a phone), the rest under ⋮. Then the headline in the
 * event's colour (title, when, the repeat rule, where), its calendar and organizer, notes and link; the guests
 * (AttendeesView) and Email guests (with a mail app); and its reminders, which can be changed here: they are saved
 * on the way back, as Etar saves them when the screen goes.
 *
 * Like every page pushed over the views it has a "back" signal (returns TRUE when it handled Back itself). */
#pragma once

#include "cal-model.h"

#include <gtk/gtk.h>

G_BEGIN_DECLS

GtkWidget *event_info_new (GtkWidget *window, CalEvent *event);

G_END_DECLS
