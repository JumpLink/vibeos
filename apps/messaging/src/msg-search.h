/* The search page: a page over the conversation list, opened from the bar's search button.
 *
 * A pill at the top with the back arrow inside it and the field, the keyboard up at once. Under it, until something
 * is typed or picked, the filters as tiles in two columns: Unread, Known, Unknown and Links. Typed words find the
 * messages with them; a filter finds the conversations not read yet, those with a contact or with a number that is
 * not one, or the messages with a link; both can be combined. A result opens its conversation. */
#pragma once

#include <gtk/gtk.h>

#include "contacts.h"
#include "sms-client.h"

G_BEGIN_DECLS

#define MSG_TYPE_SEARCH (msg_search_get_type ())
G_DECLARE_FINAL_TYPE (MsgSearch, msg_search, MSG, SEARCH, GtkWidget)

GtkWidget *msg_search_new  (SmsClient *client, Contacts *contacts);
/* Back: clears the filter or the text first; FALSE when there was nothing to clear (the page then closes) */
gboolean   msg_search_back (MsgSearch *self);

/* Signals: "open-conversation" (gint64), "closed" () (the back arrow) */

G_END_DECLS
