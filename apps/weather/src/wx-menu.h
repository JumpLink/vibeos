/* WxMenu: the page's overflow ⋮ menu (Set City, About), shown inside the app's own window. A copy of the Clock's
 * (clock-popup-menu.h), the VibeOS apps' overflow menu: a rounded card, its end a little in from the window's end and
 * its top on the ⋮'s top, as wide as its longest item. A tap on a row runs the row's action and closes the menu; a
 * tap outside, Back, Escape or Alt+Left (VibeOS's back gesture) just closes it. */
#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define WX_TYPE_MENU (wx_menu_get_type ())
G_DECLARE_FINAL_TYPE (WxMenu, wx_menu, WX, MENU, GtkWidget)

typedef void (*WxMenuAction) (gpointer data);

WxMenu *wx_menu_new (void);
void            wx_menu_append (WxMenu *self, const char *label, WxMenuAction action,
                                         gpointer data);
/* shows it over @anchor's window (one whose root overlay is its "vibeos-overlay"), lined up with the
 * anchor's end, below the anchor (above it when there is no room below), never over it; it destroys itself once
 * closed */
void            wx_menu_present (WxMenu *self, GtkWidget *anchor);

G_END_DECLS
