/* CtList: a contact list on cards.
 *
 * Each section is one segmented card under its header (the favourites' star with "Favorites", or a letter): its
 * rows touch but for a thin gap, the card's outer corners are round and the corners between rows small. A row is
 * the round picture and the name; in a search, the line that matched goes under the name, and the results are one
 * card with no headers. The header of the section at the top stays pinned there as the list scrolls under it,
 * until the next section's header pushes it up.
 *
 * Only the rows on screen are widgets: the list keeps a few and gives them other contacts as it scrolls, so a book
 * of thousands scrolls as lightly as one of ten. What goes above the rows (a search pill, filters, a card of one's
 * own) is the head (ct_list_set_head), and scrolls away with them.
 *
 * A tap on a row is "row-activated", a press held 400 ms "row-long-pressed". In selection mode every row has a
 * checkbox at its end. */
#pragma once

#include "ct-rows.h"

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define CT_TYPE_LIST (ct_list_get_type ())
G_DECLARE_FINAL_TYPE (CtList, ct_list, CT, LIST, GtkWidget)

GtkWidget *ct_list_new (void);

/* the rows to show (taken); the list keeps its scroll position unless @to_top */
void       ct_list_set_rows (CtList *self, CtRows *rows, gboolean to_top);
CtRows    *ct_list_get_rows (CtList *self);
CtEntry   *ct_list_get_entry (CtList *self, guint index);

/* search results: one card, no headers, the matching line under the name */
void       ct_list_set_search (CtList *self, gboolean search);
/* what scrolls above the rows (NULL for nothing) */
void       ct_list_set_head (CtList *self, GtkWidget *head);
/* the space under the last row (room for a button floating over the list's bottom) */
void       ct_list_set_bottom_padding (CtList *self, double padding);
/* selection mode: a checkbox on every row, checked for the uids in @selected (borrowed, NULL to leave it) */
void       ct_list_set_selection (CtList *self, GHashTable *selected);
/* redraw after the selection changed */
void       ct_list_update_selection (CtList *self);
/* the row of the contact with @uid marked as the one open beside the list (NULL: none) */
void       ct_list_set_current (CtList *self, const char *uid);

/* the rows on screen fade in and rise a little, one after another */
void       ct_list_animate_load (CtList *self);
void       ct_list_scroll_to_top (CtList *self);

/* Signals: "row-activated" (guint index), "row-long-pressed" (guint index) */

G_END_DECLS
