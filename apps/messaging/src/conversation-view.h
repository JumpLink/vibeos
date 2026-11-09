/* A conversation (spec §2): ConversationActivity with its ConversationFragment.
 *
 * The messages, oldest at the top and the newest at the bottom, where it opens: bubbles in the sender's colour
 * (white for those sent from here), clustered when one person writes several in a row, each cluster's time under
 * its last. Under them the compose bar: the text field, and the send button once there is text. A long press
 * selects a message (forward, copy, details, delete; send again when it failed); a tap on a failed one sends it
 * again. */
#pragma once

#include <gtk/gtk.h>

#include "contacts.h"
#include "sms-client.h"

G_BEGIN_DECLS

#define MSG_TYPE_CONVERSATION_VIEW (conversation_view_get_type ())
G_DECLARE_FINAL_TYPE (ConversationView, conversation_view, MSG, CONVERSATION_VIEW, GtkWidget)

GtkWidget *conversation_view_new (SmsClient *client, Contacts *contacts, gint64 conversation);
gint64     conversation_view_get_id (ConversationView *self);
/* keeps what is typed and not sent as the conversation's draft (the fragment's onPause) */
void       conversation_view_save_draft (ConversationView *self);
/* puts @text in the compose field (an sms: link's body) */
void       conversation_view_set_text (ConversationView *self, const char *text);
/* leaves select mode; FALSE when it was not in it (Back then closes the conversation) */
gboolean   conversation_view_back (ConversationView *self);

/* Signals: "closed" (): the up arrow, the title, or the conversation deleted; "show-options" (): the overflow's
 * "People & options"; "forward" (const char *text): the selected message's Forward */

G_END_DECLS
