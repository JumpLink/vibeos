/* New chat: the new conversation's contact picker, for its first person.
 *
 * The bar with the back arrow and "New chat", the "To:" field in a pill under it, then the contacts on segmented
 * cards: the ones written to most first, then every contact under its letter; once something is typed, the
 * contacts that match it instead, the typed number first. A tap on a contact (or Enter on a typed number) picks it,
 * which opens the conversation with that number. Group conversations (the chips and "Create group") are not here
 * yet: one conversation can only be started with one number. */
#pragma once

#include <gtk/gtk.h>

#include "contacts.h"
#include "sms-client.h"

G_BEGIN_DECLS

#define MSG_TYPE_CONTACT_PICKER (contact_picker_get_type ())
G_DECLARE_FINAL_TYPE (ContactPicker, contact_picker, MSG, CONTACT_PICKER, GtkWidget)

GtkWidget *contact_picker_new (SmsClient *client, Contacts *contacts);

/* Signals: "picked" (const char *number), "closed" () (the back arrow) */

G_END_DECLS
