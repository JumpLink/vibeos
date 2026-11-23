/* vibeos-menu: the overflow menu a ⋮ button opens, the same in every VibeOS app, inside the app's own window.
 *
 * A rounded card at elevation level 2, as wide as its longest item and 196 px at least, its rows edge to edge. It opens
 * next to its anchor and never over it: its top 4 px below the anchor's foot, or, when it does not fit there, its foot
 * 4 px above the anchor's top. It lines up with the anchor on the side the anchor is on: right edges for an anchor in
 * the window's right half (a header's ⋮), left edges for one in the left half (a rail's menu button), and it stays
 * 4 px in from the window's edges. It opens with its clip growing away from the anchor (down, or up for a menu above it)
 * while it fades in. A tap on a row runs the row's action and closes the menu; a tap outside, Back, Escape or Alt+Left
 * (VibeOS's back gesture) just closes it.
 *
 *   VibeosMenu *menu = vibeos_menu_new ();
 *   vibeos_menu_append (menu, "Audio track", choose_audio, self);
 *   vibeos_menu_append_about (menu, &about_info);
 *   vibeos_menu_present (menu, more_button);
 *
 * It goes into the root overlay of the window the anchor is in (vibeos_window_get_overlay). */
#pragma once

#include <gtk/gtk.h>

#include "vibeos-about.h"

G_BEGIN_DECLS

#define VIBEOS_TYPE_MENU (vibeos_menu_get_type ())
G_DECLARE_FINAL_TYPE (VibeosMenu, vibeos_menu, VIBEOS, MENU, GtkWidget)

typedef void (*VibeosMenuAction) (gpointer data);

VibeosMenu *vibeos_menu_new          (void);
/* a row: `action` runs with `data` once the menu has started closing */
void        vibeos_menu_append       (VibeosMenu *self, const char *label, VibeosMenuAction action, gpointer data);
/* every app's ⋮ menu ends with this row: "About", opening the About sheet (an app that quits from its menu appends its
 * "Exit" after it). `info` and what it points to must outlive the menu (a static VibeosAboutInfo) */
void        vibeos_menu_append_about (VibeosMenu *self, const VibeosAboutInfo *info);
/* shows it in `anchor`'s window, below the anchor (above it when there is no room below); it destroys itself once
 * closed */
void        vibeos_menu_present      (VibeosMenu *self, GtkWidget *anchor);

G_END_DECLS
