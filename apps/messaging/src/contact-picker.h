/* The new conversation's contact picker, for its first person.
 *
 * The "To" field in the action bar, and under it the Frequents and All contacts tabs; once something is typed, the
 * contacts that match it instead, the typed number first. A tap on a contact (or Enter on a typed number) picks it,
 * which opens the conversation with that number. Group conversations (the chips and
 * "add people") are not here yet. */
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
