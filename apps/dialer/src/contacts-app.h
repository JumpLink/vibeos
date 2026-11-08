/* What the Dialer leaves to the Contacts app: a new contact (ACTION_INSERT), a contact's card (QuickContact,
 * "Contact info"). Without a Contacts app: "No contacts app available", as AddContactViewHolder toasts. */
#pragma once

#include <neo.h>

G_BEGIN_DECLS

/* opens the Contacts app; @snackbar says so when there is none */
void contacts_app_open (NeoSnackbar *snackbar);

G_END_DECLS
