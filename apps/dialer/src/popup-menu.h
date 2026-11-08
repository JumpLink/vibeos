/* The Dialer's pop-up menus: android.widget.PopupMenu as the Dialer shows it (the main ⋮ menu, MainToolbarMenu;
 * a favourite's ContextMenu; spec §2.3, §6.4).
 *
 * A card of 48 dp text rows, at least 196 dp wide (abc_popup_menu_item_layout's minWidth), floating 16 dp up over
 * the app inside the app's own window. It overlaps its anchor: its top on the anchor's top, its end on the
 * anchor's end (DialerPopupMenu's END) or its start on the anchor's start (START), kept inside the window. A tap on
 * a row runs the row's action and closes the menu; a tap outside, Back or Escape just closes it. */
#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define DIALER_TYPE_POPUP_MENU (dialer_popup_menu_get_type ())
G_DECLARE_FINAL_TYPE (DialerPopupMenu, dialer_popup_menu, DIALER, POPUP_MENU, GtkWidget)

typedef void (*PopupMenuAction) (gpointer data);

typedef enum {
  POPUP_MENU_START,
  POPUP_MENU_END,
} PopupMenuGravity;

DialerPopupMenu *dialer_popup_menu_new (void);
/* @action NULL: a disabled row (dimmed, untappable), as the context menu's "Mobile +49 …" title row */
void             dialer_popup_menu_append (DialerPopupMenu *self, const char *label, PopupMenuAction action,
                                           gpointer data);
/* a floating context menu (View.showContextMenu): its header row, the platform's popup_menu_header_item_layout,
 * and the colorBackgroundFloating card */
void             dialer_popup_menu_set_header (DialerPopupMenu *self, const char *header);
/* shows it over @anchor's window (one made by neo_window_new); it destroys itself once closed */
void             dialer_popup_menu_present (DialerPopupMenu *self, GtkWidget *anchor, PopupMenuGravity gravity);
/* shows it with its top-left corner at (x, y) in @widget, kept inside the window: a context menu at the touch */
void             dialer_popup_menu_present_at (DialerPopupMenu *self, GtkWidget *widget, double x, double y);

G_END_DECLS
