/* Call details (OldCallDetailsActivity, call_details_activity.xml; spec §5.5): one entry of the call log on a screen
 * of its own, over the main one or Call history.
 *
 * Under a toolbar with ✕ and "Call details": who it was (the picture, the name, the kind of number and the number,
 * a button that calls back), then each call of the entry, newest first, with its arrow, what it was, when and for
 * how long, and at the foot "Copy number", "Edit number before call" and "Delete" (every call of the entry, no
 * question asked). The picture of a contact opens the contact's card in the Contacts app (QuickContactBadge).
 *
 * ✕ is the screen's back: its accessible name is "Back", the button the shell's back gesture clicks (a11y.js). */
#pragma once

#include <gtk/gtk.h>
#include <neo.h>

#include "call-types.h"
#include "contacts.h"

G_BEGIN_DECLS

#define CALL_TYPE_DETAILS (call_details_get_type ())
G_DECLARE_FINAL_TYPE (CallDetails, call_details, CALL, DETAILS, GtkWidget)

/* the calls with these ids (the entry's, newest first), all to or from @number; @snackbar: the window's, where a
 * call that could not be made says why */
GtkWidget *call_details_new (Contacts *contacts, NeoSnackbar *snackbar, const char *number, Presentation presentation,
                             const gint64 *ids, guint n_ids);

/* Signals: "closed" (): ✕, Back, Escape, or Delete, the window takes the screen away; "edit-number" (s number):
 * "Edit number before call", the main screen's dialpad with the number in it */

G_END_DECLS
