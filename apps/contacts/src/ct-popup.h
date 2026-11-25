/* A popup menu over the window (a list's or the toolbar's overflow menu), a
 * shadowed card of one-line rows, as wide as the widest row. A tap outside, Back or Escape closes it; a tap on a
 * row closes it and then runs the choice (which may open a dialog where the menu was).
 *
 * The Dialer's popup-menu.c draws the same thing, but its menus close only by their own key handler; the Contacts
 * window must be able to close the one showing when the back gesture comes (ct_popup_dismiss). */
#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

typedef void (*CtPopupChosen) (int index, gpointer data);

typedef enum {
  CT_POPUP_OVER_END,            /* an overflow menu: below its ⋮ (above it with no room below), never over it, the
                                 * right edge a little in from the anchor's end */
  CT_POPUP_BELOW,               /* a spinner's drop-down: under the anchor, its left edge on the anchor's */
  CT_POPUP_AT,                  /* a context menu: its top-left at (x, y) in the anchor, kept inside the window */
  CT_POPUP_SPINNER,             /* a Spinner's dropdown: over the anchor, its top-left on the anchor's, at least as
                                 * wide, the rows' text nearer the edge than a menu's */
} CtPopupPlace;

/* @width 0: the rows' own width (with a minimum); the menu is kept inside the window */
void     ct_popup_show (GtkWidget *anchor, const char *const *labels, int n_labels, CtPopupPlace place, double x,
                        double y, double width, CtPopupChosen chosen, gpointer data);
/* a multiple-choice list under @anchor and as wide (the editor's labels): the first
 * @n_checks rows have a check box at their end that a tap toggles, the menu staying open (@toggled gets the row and
 * its new state); a tap on a row after them closes the menu and chooses it (@chosen) */
typedef void (*CtPopupToggled) (int index, gboolean checked, gpointer data);
void     ct_popup_show_checks (GtkWidget *anchor, const char *const *labels, int n_labels, const gboolean *checked,
                               int n_checks, CtPopupToggled toggled, CtPopupChosen chosen, gpointer data);

/* closes the menu of @widget's window if one shows; TRUE when it did (the window's Back does this first) */
gboolean ct_popup_dismiss (GtkWidget *widget);

G_END_DECLS
