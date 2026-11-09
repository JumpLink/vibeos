/* The event's info screen: Etar's EventInfoActivity / EventInfoFragment (spec/calendar.md §6).
 *
 * A toolbar with Back and the actions the event allows (Edit and Delete when its calendar is writable; Duplicate;
 * Export), the headline in the event's colour (title, when, the repeat rule, where), then its calendar, notes and
 * link, and its reminders, which can be changed here: they are saved on the way back, as Etar saves them when the
 * screen goes.
 *
 * Like every page pushed over the views it has a "back" signal (returns TRUE when it handled Back itself). */
#pragma once

#include "cal-model.h"

#include <gtk/gtk.h>

G_BEGIN_DECLS

GtkWidget *event_info_new (GtkWidget *window, CalEvent *event);

G_END_DECLS
