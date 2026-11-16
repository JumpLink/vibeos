/* The Dialer's pop-up menus (the main ⋮ menu; a favourite's context menu).
 *
 * A card of text rows with a minimum width, floating with a shadow over the app inside the app's own window. It
 * overlaps its anchor: its top on the anchor's top, its end on the anchor's end (DialerPopupMenu's END) or its start
 * on the anchor's start (START), kept inside the window; or it drops below the anchor without overlapping it
 * (BELOW); or it is an action bar's overflow menu (OVERFLOW). A tap on a row runs the row's action and closes the
 * menu; a tap outside, Back or Escape just closes it. */
#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define DIALER_TYPE_POPUP_MENU (dialer_popup_menu_get_type ())
G_DECLARE_FINAL_TYPE (DialerPopupMenu, dialer_popup_menu, DIALER, POPUP_MENU, GtkWidget)

typedef void (*PopupMenuAction) (gpointer data);

typedef enum {
  POPUP_MENU_START,
  POPUP_MENU_END,
  POPUP_MENU_BELOW,             /* a drop-down: under the anchor, its start on the anchor's (the dialpad's ⋮) */
  POPUP_MENU_OVERFLOW,          /* an action bar's ⋮: its end a little in from the anchor's, its top on the top
                                 * of an overflow button centred on the anchor */
} PopupMenuGravity;

DialerPopupMenu *dialer_popup_menu_new (void);
/* @action NULL: a disabled row (dimmed, untappable), as the context menu's "Mobile +49 …" title row */
void             dialer_popup_menu_append (DialerPopupMenu *self, const char *label, PopupMenuAction action,
                                           gpointer data);
/* makes it a context menu: a header row over the items, on a lighter card */
void             dialer_popup_menu_set_header (DialerPopupMenu *self, const char *header);
/* the card's corner radius, which its shadow follows (the app's CSS draws the card itself): 2 unless set;
 * Messaging's menus use 4 */
void             dialer_popup_menu_set_corner (DialerPopupMenu *self, int radius);
/* shows it over @anchor's window (one made by vibeos_window_new); it destroys itself once closed */
void             dialer_popup_menu_present (DialerPopupMenu *self, GtkWidget *anchor, PopupMenuGravity gravity);
/* shows it with its top-left corner at (x, y) in @widget, kept inside the window: a context menu at the touch */
void             dialer_popup_menu_present_at (DialerPopupMenu *self, GtkWidget *widget, double x, double y);

G_END_DECLS
