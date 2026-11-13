/* MsgSwipe: a conversation row that can be swiped away sideways.
 *
 * Past the touch slop a mostly sideways drag takes the row (the press under it is cancelled, the list does not
 * scroll): the row follows the finger over the window-coloured background, which shows the archive icon on the side
 * it uncovers. Let go past 40 % of the width, or flung that way after moving 5 % of it, the row goes off that side
 * and "swiped" is emitted; otherwise it settles back. */
#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define MSG_TYPE_SWIPE (msg_swipe_get_type ())
G_DECLARE_FINAL_TYPE (MsgSwipe, msg_swipe, MSG, SWIPE, GtkWidget)

/* @row: what is swiped; @start_icon: what a swipe to the right uncovers at the start, @end_icon: what a swipe to the
 * left uncovers at the end */
GtkWidget *msg_swipe_new (GtkWidget *row, const char *start_icon, const char *end_icon);
/* off in the archived list and in select mode */
void       msg_swipe_set_enabled (MsgSwipe *self, gboolean enabled);

/* Signals: "swiped" (gboolean right): the row went off the side, after its animation; @right: to the right */

G_END_DECLS
