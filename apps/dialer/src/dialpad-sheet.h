/* The main screen's dialpad: the Dialer's DialpadFragment (spec §3).
 *
 * It covers the screen: at the foot the dialpad (the digits typed, the keys) with the green dial button over it,
 * and above that a clear space, where the search shows what the digits match. It slides up from the bottom and
 * back down; a tap in the clear space, Back, or a placed call put it away.
 *
 * The dial button calls the number typed; with no number typed it brings back the last one called. Once the call
 * service has the call, the in-call screen opens and the digits clear. */
#pragma once

#include <gtk/gtk.h>
#include <neo.h>

G_BEGIN_DECLS

#define DIALPAD_TYPE_SHEET (dialpad_sheet_get_type ())
G_DECLARE_FINAL_TYPE (DialpadSheet, dialpad_sheet, DIALPAD, SHEET, GtkWidget)

/* @snackbar: where a call that could not be made says why */
GtkWidget  *dialpad_sheet_new (NeoSnackbar *snackbar);
/* slides it up, with @number in it (NULL: as it was) */
void        dialpad_sheet_show (DialpadSheet *self, const char *number);
void        dialpad_sheet_hide (DialpadSheet *self);
gboolean    dialpad_sheet_get_shown (DialpadSheet *self);
const char *dialpad_sheet_get_digits (DialpadSheet *self);
/* empties it (closeSearch: a number is not kept from one search to the next) */
void        dialpad_sheet_clear (DialpadSheet *self);

/* Signals: "shown-changed" () as it starts to come or go, "digits-changed" () */

G_END_DECLS
