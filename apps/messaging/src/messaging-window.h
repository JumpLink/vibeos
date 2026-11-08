/* Messaging's window: its activities as pages over one another (msg-navigator.h), the conversation list first.
 *
 * It tells the SMS service what is on screen (SetVisible), so that a message coming into the conversation being
 * looked at is read at once and makes no notification, and one coming while the list is open at its top is seen. */
#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

GtkWidget *messaging_window_new (GtkApplication *app);
/* the conversation, over the list (a notification's tap, an sms: link); @draft: text for its field, or NULL */
void       messaging_window_open_conversation (GtkWidget *window, gint64 conversation, const char *draft);
/* the conversation with @numbers, made if there is none yet */
void       messaging_window_open_numbers (GtkWidget *window, const char * const *numbers, const char *draft);
/* back to the conversation list (the notification for several conversations) */
void       messaging_window_show_conversations (GtkWidget *window);

G_END_DECLS
