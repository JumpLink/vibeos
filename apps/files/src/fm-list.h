/* FmList: the file list (spec §3): a GtkListView of FmItem rows, or a GtkGridView of tiles (the View dialog's
 * Grid), in a SpongyScrollView, the filter of the search bar, selection mode with its check column, thumbnails
 * decoded in a worker, and the message that takes the list's place ("Empty folder", "No results", an error).
 *
 * A row: the 44 dp icon 8 dp in, the name (18 sp #dbdbdb, wrapping) from 60 dp, under it the count or size (left)
 * and the date (right) in 14 sp #a7a7a7, and for a search result or a Recycle Bin item the folder it is (or was) in.
 * A 1 dp #272727 line under each.
 *
 * A tile (File Manager+'s filegrid_item): the 64 dp icon and under it the name, 14 sp, centred, at most two lines;
 * the columns at least 66 dp + 2 dp apart, as many as fit (five on a 360 dp phone), stretched to the width. */
#pragma once

#include <gtk/gtk.h>

#include "fm-item.h"

G_BEGIN_DECLS

#define FM_TYPE_LIST (fm_list_get_type ())
G_DECLARE_FINAL_TYPE (FmList, fm_list, FM, LIST, GtkWidget)

GtkWidget  *fm_list_new                (void);
/* the rows (FmItem), replacing those there; the scroll position is kept when @keep_offset */
void        fm_list_set_items          (FmList *self, GPtrArray *items, gboolean keep_offset);
void        fm_list_append_items       (FmList *self, GPtrArray *items);
/* every row, filtered or not (FmItem) */
GListModel *fm_list_get_items          (FmList *self);
/* the search bar's text: only the rows whose names hold it show; NULL or "" for all */
void        fm_list_set_filter         (FmList *self, const char *text);
/* a message in the list's place, or NULL for the list */
void        fm_list_set_message        (FmList *self, const char *message);
void        fm_list_set_show_thumbnails (FmList *self, gboolean show);
/* tiles (TRUE) or rows */
void        fm_list_set_grid           (FmList *self, gboolean grid);
gboolean    fm_list_get_grid           (FmList *self);

void        fm_list_set_selecting      (FmList *self, gboolean selecting);
gboolean    fm_list_get_selecting      (FmList *self);
void        fm_list_set_selected       (FmList *self, FmItem *item, gboolean selected);
/* the selected rows that show, in their order (FmItem, a new array) */
GPtrArray  *fm_list_get_selected       (FmList *self);
guint       fm_list_count_selected     (FmList *self);
void        fm_list_select_all         (FmList *self);
/* every row between the first selected and the last (Select a range) */
void        fm_list_select_range       (FmList *self);

void        fm_list_scroll_to          (FmList *self, FmItem *item);

/* Signals: "activate" (FmItem), "long-press" (FmItem, with the press's x and y in the list),
 * "selection-changed" () */

G_END_DECLS
