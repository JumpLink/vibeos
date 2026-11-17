/* Segmented cards: rows stacked into one card with 2 px gaps, the card's outer corners round and the corners
 * between rows small, as the settings, the account menu and New chat's contacts show them. The colours and radii
 * are the stylesheet's (.msg-card, .msg-card.first, .msg-card.last). */
#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

/* an empty group of cards */
GtkWidget *msg_card_group_new    (void);
/* `row` at the group's end, its corners and the one before it set again */
void       msg_card_group_append (GtkWidget *group, GtkWidget *row);
/* a tappable row: `child` in a pressable with the ripple clipped to the card (connect to "clicked") */
GtkWidget *msg_card_row_new      (GtkWidget *child);
/* a row of the menus: a 24 px icon (or none) and a title, `height` tall */
GtkWidget *msg_card_menu_row_new (const char *icon_name, const char *title, int height);

G_END_DECLS
