/* MsgBubble: a message's bubble (spec §2.3, MessageBubbleBackground): its child on a rectangle with 28 dp corners in
 * the bubble's colour, as wide as the text needs rounded up to a multiple of 20 dp, never wider than @max_width. */
#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define MSG_TYPE_BUBBLE (msg_bubble_get_type ())
G_DECLARE_FINAL_TYPE (MsgBubble, msg_bubble, MSG, BUBBLE, GtkWidget)

GtkWidget *msg_bubble_new (GtkWidget *child);
void       msg_bubble_set_color (MsgBubble *self, const GdkRGBA *color);
void       msg_bubble_set_max_width (MsgBubble *self, int max_width);

G_END_DECLS
