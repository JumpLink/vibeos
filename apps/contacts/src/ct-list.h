/* CtList: the contact list (ContactEntryListFragment's PinnedHeaderListView, spec §1.3 to §1.5, §2.3), drawn by
 * hand rather than built of a widget per row: thousands of contacts scroll without a widget each, and the parts
 * that are not rows (the pinned letter, the fast scroller) are drawn over them in the same pass.
 *
 * A row is 56 dp: the section column (56 dp, the letter or the favourites' star on a section's first row), the
 * 40 dp round picture, the name (16 sp #dadada), and in a search the line that matched under it. The list is in
 * the subtree Android leaves out of force dark, so its colours are the night resources as they are.
 *
 * A tap on a row is "row-activated", a press held 400 ms "row-long-pressed" (ViewConfiguration's timeouts; the
 * ripple waits 100 ms in case the finger scrolls instead, as a ListView's pressed state does). In selection mode
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
/* the space above the first row: 7 dp (list_item_padding_top), 0 under the empty-query search (§2.1) */
void       ct_list_set_top_padding (CtList *self, double padding);
/* selection mode: a checkbox on every row, checked for the uids in @selected (borrowed, NULL to leave it) */
void       ct_list_set_selection (CtList *self, GHashTable *selected);
/* redraw after the selection changed */
void       ct_list_update_selection (CtList *self);

/* slide_and_fade_in_layout_animation (§7.6): the rows on screen fade in and rise 5 % of a row, 19 ms apart */
void       ct_list_animate_load (CtList *self);
void       ct_list_scroll_to_top (CtList *self);

/* Signals: "row-activated" (guint index), "row-long-pressed" (guint index) */

G_END_DECLS
