/* Call history: the whole call log on a screen of its own, over the main one.
 *
 * Its action bar has Up, the title and the ⋮ with "Clear call history"; under it three tabs: All and Missed, each
 * the Recents list (recents-page.c), Missed with the missed calls alone, and Lineage's Statistics (call-stats.c),
 * whose ⋮ also has the sort. A tap on a tab or a sideways fling goes to the page next to it; going to All or Missed
 * marks the missed calls seen.
 *
 * Up is the screen's back: its accessible name is "Back", the button the shell's back gesture clicks (a11y.js). */
#pragma once

#include <gtk/gtk.h>
#include <neo.h>

#include "contacts.h"

G_BEGIN_DECLS

#define CALL_TYPE_HISTORY (call_history_get_type ())
G_DECLARE_FINAL_TYPE (CallHistory, call_history, CALL, HISTORY, GtkWidget)

/* @snackbar: the window's, where a call that could not be made says why */
GtkWidget *call_history_new (Contacts *contacts, NeoSnackbar *snackbar);

/* Signals: "closed" (): Up, Back or Escape, the window takes the screen away; "edit-number" (s number): the
 * context menu's "Edit number before call", the main screen's dialpad with the number in it; "show-details"
 * (s number, i presentation, GArray ids): an entry's "Call details", which opens over this screen; "show-screen"
 * (GtkWidget screen): a number's statistics, to put over this screen (it has "closed" and "edit-number") */

G_END_DECLS
