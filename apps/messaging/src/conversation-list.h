/* The conversation list, and with @archived the archived conversations.
 *
 * Under a bar with the title, a search button and the account's picture, the conversations on a sheet with round
 * top corners, newest first: a row each with its avatar, its people, the newest message (or the draft) and when it
 * was. A tap opens it; a long press selects it, and the bar then archives, deletes or blocks the selected ones. A
 * swipe to either side archives one, with Undo. The extended FAB starts a new chat; scrolled away from the top it
 * shrinks to its icon and a button appears that goes back to the newest. */
#pragma once

#include <gtk/gtk.h>

#include "contacts.h"
#include "sms-client.h"

G_BEGIN_DECLS

#define MSG_TYPE_CONVERSATION_LIST (conversation_list_get_type ())
G_DECLARE_FINAL_TYPE (ConversationList, conversation_list, MSG, CONVERSATION_LIST, GtkWidget)

GtkWidget *conversation_list_new (SmsClient *client, Contacts *contacts, gboolean archived);
/* Forward message's list: the conversations that are not archived, without the action bar, the
 * FAB, swiping or select mode; a tap or a long press picks one ("open-conversation") */
GtkWidget *conversation_list_new_forward (SmsClient *client, Contacts *contacts);
/* scrolled to its top, with nothing selected: what comes is then seen (SetVisible 0) */
gboolean   conversation_list_is_at_top (ConversationList *self);
/* leaves select mode; FALSE when it was not in it (Back then closes the screen) */
gboolean   conversation_list_back (ConversationList *self);
/* the conversation open beside the list (0: none): its row shows it */
void       conversation_list_set_current (ConversationList *self, gint64 id);
/* during "open-conversation" from a tap: the row tapped, for its press highlight; else NULL */
GtkWidget *conversation_list_get_pressed_row (ConversationList *self);
/* marks every unread conversation read, archived ones too */
void       conversation_list_mark_all_read (SmsClient *client);

/* Signals: "open-conversation" (gint64), "start-chat" (), "show-search" (), "show-account" () (the account's
 * picture), "closed" () (the back arrow), "at-top-changed" () */

G_END_DECLS
