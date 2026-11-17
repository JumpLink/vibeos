/* SwMenu: the overflow ⋮ menu of a page (Settings, About, Report), shown inside the app's own window. A copy of the
 * Clock's (clock-popup-menu.h), the VibeOS apps' overflow menu: a rounded card, its end a little in from the
 * window's end and its top on the ⋮'s top, as wide as its longest item. A tap on a row runs the row's action and
 * closes the menu; a tap outside, Back, Escape or Alt+Left (VibeOS's back gesture) just closes it. */
#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define SW_TYPE_MENU (sw_menu_get_type ())
G_DECLARE_FINAL_TYPE (SwMenu, sw_menu, SW, MENU, GtkWidget)

typedef void (*SwMenuAction) (gpointer data);

SwMenu *sw_menu_new (void);
void            sw_menu_append (SwMenu *self, const char *label, SwMenuAction action,
                                         gpointer data);
/* shows it over @anchor's window (one whose root overlay is its "vibeos-overlay"), at the anchor's end and top; it
 * destroys itself once closed */
void            sw_menu_present (SwMenu *self, GtkWidget *anchor);

G_END_DECLS
