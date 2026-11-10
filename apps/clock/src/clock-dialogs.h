/* The Settings list dialog (ListPreferenceDialogFragmentCompat; spec/clock.md §10.5): the row's title, the choices
 * as a radio list with the current one checked, and Cancel. A choice answers at once; there is no OK. A copy of the
 * Calendar's cal_list_dialog. */
#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

/* @chosen: the index picked, or -1 for Cancel (or Back, or a tap outside) */
typedef void (*ClockChoiceDone) (int chosen, gpointer data);

void clock_list_dialog (GtkWidget *parent, const char *title, const char *const *labels, int n_labels, int selected,
                        ClockChoiceDone done, gpointer data);

G_END_DECLS
