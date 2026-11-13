/* CalPage: a screen pushed over the views (event info, the editor, settings). A vertical box on the surface colour,
 * with its toolbar (cal_page_toolbar) at the top.
 *
 * Back on a page: its back handler first (the editor's "discard changes?"), else the page goes (cal_window_pop).
 * The shell's back gesture finds the toolbar's "Back" button by its name and presses it, which does the same. */
#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define CAL_TYPE_PAGE (cal_page_get_type ())
G_DECLARE_FINAL_TYPE (CalPage, cal_page, CAL, PAGE, GtkBox)

/* TRUE when the handler dealt with Back itself */
typedef gboolean (*CalPageBack) (CalPage *page, gpointer data);

GtkWidget *cal_page_new              (GtkWidget *window);
GtkWidget *cal_page_get_window       (CalPage *self);
void       cal_page_set_back_handler (CalPage *self, CalPageBack back, gpointer data);
/* Back, as the window's Escape and the toolbar's arrow do: the handler, else pop */
void       cal_page_back             (CalPage *self);

/* A toolbar with the back arrow at the start, an optional title, and a box at the end for the actions, which it
 * returns. */
GtkWidget *cal_page_toolbar          (CalPage *self, const char *title, GtkWidget **actions);
/* an action: an icon button, with its name for assistive technology */
GtkWidget *cal_page_action           (const char *icon_name, const char *name);

G_END_DECLS
