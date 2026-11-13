/* The Recents tab: the call log.
 *
 * Calls in a row with the same number make one entry, with up to three arrows for what they were and "(4)" when
 * there are more; the entries fall under "Today", "Yesterday" and "Older". Each shows who (a contact's name and
 * picture, else the number), what kind of number or where it was, and when, with a button that calls back.
 * A tap opens the entry into a card of things to do with the number; opening a missed call marks it seen. */
#pragma once

#include <gtk/gtk.h>
#include <neo.h>

#include "contacts.h"

G_BEGIN_DECLS

#define RECENTS_TYPE_PAGE (recents_page_get_type ())
G_DECLARE_FINAL_TYPE (RecentsPage, recents_page, RECENTS, PAGE, GtkWidget)

/* @snackbar: where a call that could not be made says why */
GtkWidget *recents_page_new (Contacts *contacts, NeoSnackbar *snackbar);
/* Call history's All and Missed pages: the same list, with @missed_only the missed calls alone, and
 * nothing to do on an empty one */
GtkWidget *recents_page_new_history (Contacts *contacts, NeoSnackbar *snackbar, gboolean missed_only);
/* the top of the list, the entries closed (the tab is shown afresh) */
void       recents_page_reset (RecentsPage *self);
/* whether the log had any calls, missed or not, when it was last read (Call history's "Clear call history") */
gboolean   recents_page_has_calls (RecentsPage *self);

/* Signals: "open-dialpad" (): "Make a call" on the empty list; "edit-number" (s number): the context menu's "Edit
 * number before call", the dialpad with the number in it; "loaded" (): the log was read again; "show-details"
 * (s number, i presentation, GArray ids): an entry's "Call details", the screen of its calls (call-details.h) */

G_END_DECLS
