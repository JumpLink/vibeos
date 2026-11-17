/* A conversation's page.
 *
 * A bar with the back arrow, the person's avatar and name (a tap opens the details), Call and ⋮. Under it, on a
 * container with round top corners, the messages, oldest at the top and the newest at the bottom, where it opens:
 * incoming bubbles on the left, ours on the right in the primary colour, stacked into runs (msg-thread.h), with a
 * date line where the day or the hour changes and the newest message's time under it. A failed one says so under
 * it, with "Check options" (Try again, Delete). At the bottom the compose bar: ⊕, the pill with the text, and the
 * round send button once there is text; for a sender who cannot be answered, a note instead. A long press selects
 * a message (forward, copy, details, delete; send again when it failed). */
#pragma once

#include <gtk/gtk.h>

#include "contacts.h"
#include "sms-client.h"

G_BEGIN_DECLS

#define MSG_TYPE_CONVERSATION_VIEW (conversation_view_get_type ())
G_DECLARE_FINAL_TYPE (ConversationView, conversation_view, MSG, CONVERSATION_VIEW, GtkWidget)

GtkWidget *conversation_view_new (SmsClient *client, Contacts *contacts, gint64 conversation);
gint64     conversation_view_get_id (ConversationView *self);
/* keeps what is typed and not sent as the conversation's draft */
void       conversation_view_save_draft (ConversationView *self);
/* puts @text in the compose field (an sms: link's body) */
void       conversation_view_set_text (ConversationView *self, const char *text);
/* leaves select mode; FALSE when it was not in it (Back then closes the conversation) */
gboolean   conversation_view_back (ConversationView *self);

/* Signals: "closed" (): the up arrow, the title, or the conversation deleted; "show-options" (): the overflow's
 * "People & options"; "forward" (const char *text): the selected message's Forward */

G_END_DECLS
