/* MsgBubble: a message's bubble: its child on a rounded rectangle in the bubble's colour, as wide as the text needs,
 * never wider than @max_width, and never less tall than one line's bubble.
 *
 * Its corners are round, but where it touches a neighbour in a run of messages from the same side the two corners
 * on that side facing the neighbour are small (msg-thread.h): on the left for an incoming message, on the right for
 * an outgoing one. */
#pragma once

#include <gtk/gtk.h>

#include "msg-thread.h"

G_BEGIN_DECLS

#define MSG_TYPE_BUBBLE (msg_bubble_get_type ())
G_DECLARE_FINAL_TYPE (MsgBubble, msg_bubble, MSG, BUBBLE, GtkWidget)

GtkWidget *msg_bubble_new (GtkWidget *child);
void       msg_bubble_set_color (MsgBubble *self, const GdkRGBA *color);
void       msg_bubble_set_max_width (MsgBubble *self, int max_width);
/* its place in its run, and its side (the small corners on the right for an outgoing message) */
void       msg_bubble_set_run (MsgBubble *self, MsgRunPosition position, gboolean outgoing);

G_END_DECLS
