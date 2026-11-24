/* The contact editor, for a new contact or an existing one. */
#pragma once

#include <gtk/gtk.h>
#include <libebook/libebook.h>

G_BEGIN_DECLS

/* the editor for the contact @uid (CT_PROFILE_UID: "My info"), or for a new one when @uid is NULL, with @prefill's
 * fields filled in (a number or an email address another app sent) when @prefill is not NULL */
GtkWidget *ct_editor_new (GtkWidget *window, const char *uid, EContact *prefill);

/* @page (an editor) was opened by another app ("insert": the Dialer's Create new contact): closing it, saved or not,
 * goes back to that app */
void ct_editor_set_external (GtkWidget *page);

/* @page is an editor another app opened, with nothing changed since: a new request replaces it rather than stack */
gboolean ct_editor_is_untouched_external (GtkWidget *page);

G_END_DECLS
