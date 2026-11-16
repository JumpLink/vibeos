/* VibeosCardList: rows on cards, in groups, as settings and contact lists show them.
 *
 * Each group is one segmented card: its rows touch but for a thin gap where the background shows through, the
 * group's outer corners are rounded and the corners between rows are small. Groups are further apart, and a group can
 * have a header over it, in the primary colour. The cards keep a margin from the list's sides.
 *
 *   list = vibeos_card_list_new ();
 *   vibeos_card_list_append (list, row);             rows of the first group
 *   vibeos_card_list_add_group (list, "Privacy");    a new group, with a header (or NULL)
 *   vibeos_card_list_append (list, row);
 *
 * The rows are usually VibeosListRows; any widget can be one (it gets the CSS classes vibeos-card-first and
 * vibeos-card-last where its group starts and ends, to round its own corners). The list does not scroll: put it in a
 * scroll view. */
#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define VIBEOS_TYPE_CARD_LIST (vibeos_card_list_get_type ())
G_DECLARE_FINAL_TYPE (VibeosCardList, vibeos_card_list, VIBEOS, CARD_LIST, GtkWidget)

GtkWidget *vibeos_card_list_new         (void);
/* a row at the end of the last group */
void       vibeos_card_list_append      (VibeosCardList *self, GtkWidget *row);
/* starts a group, with a header over it when `header` is not NULL (returned: the header's label, or NULL) */
GtkWidget *vibeos_card_list_add_group   (VibeosCardList *self, const char *header);
void       vibeos_card_list_remove      (VibeosCardList *self, GtkWidget *row);
void       vibeos_card_list_remove_all  (VibeosCardList *self);
/* space over the first group and under the last, in px; none by default */
void       vibeos_card_list_set_margins (VibeosCardList *self, int top, int bottom);

G_END_DECLS
