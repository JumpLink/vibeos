/* Call history's third tab, Statistics: Lineage's call statistics.
 *
 * One row per number (a contact's or not), most talked-to first, or most called with "Sort by call count": the
 * picture, the name, its share of the whole, a bar of its incoming, outgoing (and, by count, missed and blocked)
 * calls against the top row's, and the kind of number and the number. Over the rows, "Total: 12 calls, 1 hr 5 mins".
 * Sorted by duration, a number never talked to does not show. A tap opens the number's statistics:
 * Call durations, Call count and Average call duration, each against the whole, then Copy
 * number and Edit number before call; a long press has the same two in a menu.
 *
 * Left out: the time range ("Adjust time range") and the two filter spinners: their band stays, empty. */
#pragma once

#include <gtk/gtk.h>
#include <neo.h>

#include "contacts.h"

G_BEGIN_DECLS

#define CALL_TYPE_STATS (call_stats_get_type ())
G_DECLARE_FINAL_TYPE (CallStats, call_stats, CALL, STATS, GtkWidget)

GtkWidget *call_stats_new (Contacts *contacts, NeoSnackbar *snackbar);
/* the order: by the time talked (the default), or by the number of calls */
gboolean   call_stats_get_sort_by_duration (CallStats *self);
void       call_stats_set_sort_by_duration (CallStats *self, gboolean by_duration);

/* Signals: "show-screen" (GtkWidget *screen): a number's statistics, for the window to put over Call history; the
 * screen has "closed" () and "edit-number" (s), as Call details has. "edit-number" (s): the long-press menu's
 * Edit number before call */

G_END_DECLS
