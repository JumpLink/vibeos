/* vibeos-menu: the overflow menu a ⋮ button opens, the same in every VibeOS app, inside the app's own window.
 *
 * A rounded card at elevation level 2, its end a little in from the window's end and its top on the ⋮'s top, as wide
 * as its longest item and 196 px at least, its rows edge to edge. It opens with its clip growing down from the anchor while it fades in. A tap on a row runs the
 * row's action and closes the menu; a tap outside, Back, Escape or Alt+Left (VibeOS's back gesture) just closes it.
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
/* the last row of every app's ⋮ menu: "About", opening the About sheet. `info` and what it points to must outlive the
 * menu (a static VibeosAboutInfo) */
void        vibeos_menu_append_about (VibeosMenu *self, const VibeosAboutInfo *info);
/* shows it over `anchor`'s window, at the anchor's end and top; it destroys itself once closed */
void        vibeos_menu_present      (VibeosMenu *self, GtkWidget *anchor);

G_END_DECLS
