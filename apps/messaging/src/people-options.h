/* People & options, from a conversation's overflow. "General" with Block / Unblock for a conversation with one person,
 * then "People in this conversation": each one's picture, name and number; a tap opens the Contacts app, a long press
 * offers to copy the number.
 *
 * Left out: the "Notifications" row, which would open the system's settings for the conversation's own
 * notification channel; this phone's notifications have no channels, so the row would lead nowhere. */
#pragma once

#include <gtk/gtk.h>

#include "contacts.h"
#include "sms-client.h"

G_BEGIN_DECLS

#define MSG_TYPE_PEOPLE_OPTIONS (people_options_get_type ())
G_DECLARE_FINAL_TYPE (PeopleOptions, people_options, MSG, PEOPLE_OPTIONS, GtkWidget)

GtkWidget *people_options_new (SmsClient *client, Contacts *contacts, gint64 conversation);

/* Signals: "closed" () (the up arrow, or the conversation deleted), "blocked" () (the person was blocked: this page
 * and its conversation close) */

G_END_DECLS
