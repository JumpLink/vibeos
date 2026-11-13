/* Adding someone to the contacts, and their card, through the Contacts app (contacts-app.h).
 *
 * A tap on a person's avatar (in the list, beside a message) or on a person in People & options opens the contact's
 * card if they are a contact, and otherwise asks "Add to Contacts?" with their avatar and number; "Add contact"
 * hands the number to the Contacts app to add to a contact picked there or to a new one ("insert-or-edit").
 * The asking needs the Contacts app: without it a tap on someone who is not a contact does nothing. */
#pragma once

#include <neo.h>

#include "msg-people.h"

G_BEGIN_DECLS

/* the person can be added: not a contact, a destination to add, and the Contacts app there to take it */
gboolean msg_contact_can_add (const MsgPerson *person);
/* "Add to Contacts?" for the person, over the window @from is in */
void     msg_contact_confirm_add (GtkWidget *from, const MsgPerson *person);
/* the contact's card, or "Add to Contacts?" */
void     msg_contact_show_or_add (GtkWidget *from, const MsgPerson *person, NeoSnackbar *snackbar);

G_END_DECLS
