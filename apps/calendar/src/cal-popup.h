/* A popup list of choices over the window: Android's ListPopupWindow, as a Spinner's dropdown and the toolbar's
 * overflow menu show it (M3: surfaceContainer #211F26, 4 dp corners, 48 dp rows with 16 sp text 16 dp in; measured
 * still:cal-menu). A tap outside closes it, as Back does. CalSpinner: a Spinner, its value and a drop-down arrow,
 * that opens one. Both are libneo candidates. */
#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

typedef void (*CalPopupChosen) (int index, gpointer data);

/* the list at @anchor's top-left (or its top-right with @end), @width wide (0: the anchor's), @selected (−1 none)
 * marked; @chosen runs with the tapped row's index, not on a dismiss */
void cal_popup_show (GtkWidget *anchor, const char *const *labels, int n_labels, int selected, double width,
                     gboolean end, CalPopupChosen chosen, gpointer data);
/* closes the popup of @widget's window, if one shows; TRUE if it did (Back) */
gboolean cal_popup_dismiss (GtkWidget *widget);

#define CAL_TYPE_SPINNER (cal_spinner_get_type ())
G_DECLARE_FINAL_TYPE (CalSpinner, cal_spinner, CAL, SPINNER, GtkWidget)

GtkWidget *cal_spinner_new          (void);
void       cal_spinner_set_items    (CalSpinner *self, const char *const *labels, int n_labels);
void       cal_spinner_set_selected (CalSpinner *self, int index);
int        cal_spinner_get_selected (CalSpinner *self);
/* Signals: "changed" () when a choice is made */

G_END_DECLS
