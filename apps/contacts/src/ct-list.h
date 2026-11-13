/* CtList: the contact list, drawn by
 * hand rather than built of a widget per row: thousands of contacts scroll without a widget each, and the parts
 * that are not rows (the pinned letter, the fast scroller) are drawn over them in the same pass.
 *
 * A row is the section column (the letter or the favourites' star on a section's first row), the round picture, the
 * name, and in a search the line that matched under it. Its colours are the dark ones as they are, not inverted.
 *
 * A tap on a row is "row-activated", a press held 400 ms "row-long-pressed" (the
 * ripple waits 100 ms in case the finger scrolls instead). In selection mode
 * every row has a checkbox at its end. */
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

/* search results: no section column, no pinned letter, a plain scroll bar in place of the fast scroller */
void       ct_list_set_search (CtList *self, gboolean search);
/* the space above the first row: CT_LIST_PAD_TOP_DP, 0 under the empty-query search */
void       ct_list_set_top_padding (CtList *self, double padding);
/* selection mode: a checkbox on every row, checked for the uids in @selected (borrowed, NULL to leave it) */
void       ct_list_set_selection (CtList *self, GHashTable *selected);
/* redraw after the selection changed */
void       ct_list_update_selection (CtList *self);

/* the rows on screen fade in and rise 5 % of a row, 19 ms apart */
void       ct_list_animate_load (CtList *self);
void       ct_list_scroll_to_top (CtList *self);

/* Signals: "row-activated" (guint index), "row-long-pressed" (guint index) */

G_END_DECLS
