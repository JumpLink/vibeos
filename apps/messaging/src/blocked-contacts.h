/* Blocked contacts: the numbers this app and the Phone block (one list,
 * blocked-numbers.h), each with its person and "UNBLOCK". A tap on a row unblocks it at once; the conversation stays
 * where it is (only the block's own Undo unarchives it). */
#pragma once

#include <gtk/gtk.h>

#include "contacts.h"
#include "sms-client.h"

G_BEGIN_DECLS

#define MSG_TYPE_BLOCKED_CONTACTS (blocked_contacts_get_type ())
G_DECLARE_FINAL_TYPE (BlockedContacts, blocked_contacts, MSG, BLOCKED_CONTACTS, GtkWidget)

GtkWidget *blocked_contacts_new (SmsClient *client, Contacts *contacts);

/* Signals: "closed" () (the up arrow) */

G_END_DECLS
