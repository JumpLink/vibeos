/* The contact editor (spec §4): ContactEditorActivity for a new contact or an existing one. */
#pragma once

#include <gtk/gtk.h>
#include <libebook/libebook.h>

G_BEGIN_DECLS

/* the editor for the contact @uid (CT_PROFILE_UID: "My info"), or for a new one when @uid is NULL, with @prefill's
 * fields filled in (a number or an email address another app sent) when @prefill is not NULL */
GtkWidget *ct_editor_new (GtkWidget *window, const char *uid, EContact *prefill);

G_END_DECLS
