/* VibeosFabColumn: a page's floating action buttons beside or under its content.
 *
 * A main FAB and up to two smaller buttons, one before it and one after it (the "left" and "right" buttons of a
 * clock's tabs). Where the widget is wider than tall (a turned phone, a tablet on its side) the buttons stand in a
 * column of their own at the end, which the content does not reach: the FAB in the middle, the button after it
 * above, the button before it below, each a little nearer the FAB than the edge. Elsewhere they sit in a row at the
 * bottom, over the content: the FAB in the middle, the one before it to its left, the one after it to its right.
 *
 * A hidden button (gtk_widget_set_visible) keeps the others where they are. */
#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define VIBEOS_TYPE_FAB_COLUMN (vibeos_fab_column_get_type ())
G_DECLARE_FINAL_TYPE (VibeosFabColumn, vibeos_fab_column, VIBEOS, FAB_COLUMN, GtkWidget)

GtkWidget *vibeos_fab_column_new         (void);
void       vibeos_fab_column_set_content (VibeosFabColumn *self, GtkWidget *content);
void       vibeos_fab_column_set_fab     (VibeosFabColumn *self, GtkWidget *fab);
/* the smaller buttons before and after the FAB; either may be NULL */
void       vibeos_fab_column_set_buttons (VibeosFabColumn *self, GtkWidget *before, GtkWidget *after);
/* TRUE while the buttons are in the column at the end */
gboolean   vibeos_fab_column_is_column   (VibeosFabColumn *self);

G_END_DECLS
