/* ClockPopupMenu: the overflow ⋮ menu of the cities picker (an AppCompat action bar's overflow under
 * Theme.Material3Expressive), shown inside the app's own window. A copy of the Dialer's popup-menu.c with the
 * Material 3 measurements the Calculator took for the same menu (calc-popup-menu.h, still:calc-menu): a card in
 * surfaceContainer with 4 dp corners, its end 4 dp in from the window's end and its top on the ⋮'s top, as wide as
 * its longest item, 16 dp either side of 16 sp text, 48 dp rows, 8 dp above and below them. A tap on a row runs the
 * row's action and closes the menu; a tap outside, Back or Escape just closes it. */
#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define CLOCK_TYPE_POPUP_MENU (clock_popup_menu_get_type ())
G_DECLARE_FINAL_TYPE (ClockPopupMenu, clock_popup_menu, CLOCK, POPUP_MENU, GtkWidget)

typedef void (*ClockMenuAction) (gpointer data);

ClockPopupMenu *clock_popup_menu_new (void);
void            clock_popup_menu_append (ClockPopupMenu *self, const char *label, ClockMenuAction action,
                                         gpointer data);
/* shows it over @anchor's window (one made by neo_window_new), at the anchor's end and top; it destroys itself
 * once closed */
void            clock_popup_menu_present (ClockPopupMenu *self, GtkWidget *anchor);

G_END_DECLS
