/* What the Dialer and Messaging leave to the Contacts app (vibeos.Contacts): a contact's card (QuickContact),
 * a new contact with a number in it ("insert", "Create new contact"), and a number added to a contact the user
 * picks there ("insert-or-edit", "Add to a contact"). They are its actions (apps/contacts, ct-main.c), activated
 * over D-Bus with org.freedesktop.Application.ActivateAction, so the app starts if it is not running.
 *
 * Without the Contacts app installed the rows that would open it are not shown (contacts_app_installed); a contact
 * tapped then opens GNOME Contacts at its list if that is there, else "No contacts app available". */
#pragma once

#include <vibeos.h>

G_BEGIN_DECLS

/* vibeos.Contacts is installed: "Create new contact" and "Add to a contact" can be offered */
gboolean contacts_app_installed (void);

/* the Contacts app at its list; @snackbar says so when there is none */
void contacts_app_open (GtkWidget *from, VibeosSnackbar *snackbar);
/* the card of the contact with this uid (the vCard UID, as Contacts' uid has it) */
void contacts_app_view (GtkWidget *from, const char *uid, VibeosSnackbar *snackbar);
/* the editor for a new contact, with @destination as its phone number, or its e-mail address if it has an @ in it
 * (NULL: an empty one) */
void contacts_app_insert (GtkWidget *from, const char *destination);
/* "Choose a contact" to add @destination to, "Create new contact" first in the list */
void contacts_app_insert_or_edit (GtkWidget *from, const char *destination);

G_END_DECLS
