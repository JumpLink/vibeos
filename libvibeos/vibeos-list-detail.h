/* VibeosListDetail: a list and the page of the item chosen in it, side by side on a tablet, one over the other on a
 * phone.
 *
 * Two panes, each a VibeosPageStack: the list on the left, the detail on the right. The list pane takes a share of
 * the width (a little over a third by default) or a fixed width; the detail pane the rest. With nothing open the
 * detail pane shows the placeholder. Pages opened from the detail push inside the detail pane, and the list stays.
 *
 * On a phone it is one pane: the detail pages open over the list, which comes back when they are closed. By default
 * it is two panes on any tablet, upright too (VibeosLayout's device class); with a minimum split width it is two
 * panes on any window at least that wide instead. Turning a phone into a tablet (a display-size change) and back
 * moves the open pages to where they belong, and keeps them open.
 *
 *   ld = vibeos_list_detail_new ();
 *   vibeos_list_detail_set_list (ld, list_page);
 *   ... on a row tapped: vibeos_list_detail_show_from (ld, page_for (row), row);
 *   ... on Back: if (!vibeos_list_detail_back (ld)) close the window;
 *
 * Back pops one level. Back on the detail's first level either closes the detail (back to the list, or to the
 * placeholder) or, with vibeos_list_detail_set_back_closes, tells the app to close, as Settings does on a tablet. */
#pragma once

#include <gtk/gtk.h>
#include "vibeos-page-stack.h"

G_BEGIN_DECLS

#define VIBEOS_TYPE_LIST_DETAIL (vibeos_list_detail_get_type ())
G_DECLARE_FINAL_TYPE (VibeosListDetail, vibeos_list_detail, VIBEOS, LIST_DETAIL, GtkWidget)

GtkWidget       *vibeos_list_detail_new                (void);
/* the list: the first page of the list pane */
void             vibeos_list_detail_set_list           (VibeosListDetail *self, GtkWidget *list);
/* what the detail pane shows with nothing open (NULL: its background) */
void             vibeos_list_detail_set_placeholder    (VibeosListDetail *self, GtkWidget *placeholder);

/* opens `page` as the detail of an item: in two panes it replaces whatever the detail pane showed, in one it opens
 * over the list */
void             vibeos_list_detail_show               (VibeosListDetail *self, GtkWidget *page);
/* the same from a tapped row: in one pane the row shows its press highlight first */
void             vibeos_list_detail_show_from          (VibeosListDetail *self, GtkWidget *page, GtkWidget *row);
/* opens `page` from the detail, one level deeper */
void             vibeos_list_detail_push               (VibeosListDetail *self, GtkWidget *page);
/* Back: TRUE when it closed something, FALSE when there was nothing left to close here (the app closes) */
gboolean         vibeos_list_detail_back               (VibeosListDetail *self);
/* closes every detail page, back to the list (or the placeholder) */
void             vibeos_list_detail_close_detail       (VibeosListDetail *self);
gboolean         vibeos_list_detail_has_detail         (VibeosListDetail *self);

/* two panes now */
gboolean         vibeos_list_detail_is_split           (VibeosListDetail *self);
/* the list pane's share of the width, from 0 to 1: a little over a third by default */
void             vibeos_list_detail_set_list_ratio     (VibeosListDetail *self, double ratio);
/* the list pane's width in px, instead of a share (0: back to the share) */
void             vibeos_list_detail_set_list_width     (VibeosListDetail *self, int width);
/* 0 (the default): two panes on a tablet. Otherwise two panes on a window at least this wide, in px */
void             vibeos_list_detail_set_min_split_width (VibeosListDetail *self, int width);
/* Back on the detail's first level in two panes: FALSE (the default) closes the detail, TRUE answers FALSE (the
 * app closes) */
void             vibeos_list_detail_set_back_closes    (VibeosListDetail *self, gboolean closes);

VibeosPageStack *vibeos_list_detail_get_list_stack     (VibeosListDetail *self);
VibeosPageStack *vibeos_list_detail_get_detail_stack   (VibeosListDetail *self);
/* where the detail is shown: the detail pane in two panes, the whole widget in one. A dialog opened from the detail
 * centres on it (vibeos_dialog_set_anchor) */
GtkWidget       *vibeos_list_detail_get_detail_pane    (VibeosListDetail *self);

/* Properties: "split" (gboolean, read-only, notified). Signals: "detail-closed" (): the last detail page went, so
 * nothing in the list is open any more (the app clears its selection) */

G_END_DECLS
