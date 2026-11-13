/* MsgNavigator: Messaging's screens as pages in one window, with a transition between them: a page opened comes
 * in from 96 px to the right while fading in, over the one it covers, which moves 96 px left; going back reverses it.
 * The page on top is the one shown and the one with the focus. */
#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define MSG_TYPE_NAVIGATOR (msg_navigator_get_type ())
G_DECLARE_FINAL_TYPE (MsgNavigator, msg_navigator, MSG, NAVIGATOR, GtkWidget)

GtkWidget *msg_navigator_new (void);
/* opens @page over the others */
void       msg_navigator_push (MsgNavigator *self, GtkWidget *page);
/* closes the top page, back to the one under it (finish); the first page stays. FALSE when there was none */
gboolean   msg_navigator_pop (MsgNavigator *self);
/* swaps the top page for @page with the open transition: the contact picker becomes the conversation it picked */
void       msg_navigator_replace (MsgNavigator *self, GtkWidget *page);
/* closes every page down to the first */
void       msg_navigator_pop_to_root (MsgNavigator *self);
GtkWidget *msg_navigator_get_top (MsgNavigator *self);
guint      msg_navigator_get_depth (MsgNavigator *self);

/* Signals: "top-changed" (): a page came to the top (its transition may still run) */

G_END_DECLS
