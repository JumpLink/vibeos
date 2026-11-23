/* CalcPopupMenu: the calculator's pop-up menus, the toolbar's overflow menu and the history's, shown inside the
 * app's own window.
 *
 * A card at the window's end, its top just under the ⋮'s top, as wide as its longest item (up to the window's
 * width). A tap on a row closes the menu and runs the row's action; a tap outside, Back or Escape just closes it.
 * It comes in growing down from its top while fading in (250 ms) and fades out (150 ms), as the Dialer's.
 *
 * A libvibeos candidate: the Dialer has its own (popup-menu.c), sized differently. */
#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define CALC_TYPE_POPUP_MENU (calc_popup_menu_get_type ())
G_DECLARE_FINAL_TYPE (CalcPopupMenu, calc_popup_menu, CALC, POPUP_MENU, GtkWidget)

typedef void (*CalcMenuAction) (gpointer data);

CalcPopupMenu *calc_popup_menu_new     (void);
void           calc_popup_menu_append  (CalcPopupMenu *self, const char *label, CalcMenuAction action, gpointer data);
/* shows it over the window `anchor` is in (made by vibeos_window_new), lined up with the anchor's end, below the
 * anchor (above it when there is no room below), never over it; it destroys itself once closed */
void           calc_popup_menu_present (CalcPopupMenu *self, GtkWidget *anchor);
void           calc_popup_menu_dismiss (CalcPopupMenu *self);

G_END_DECLS
