/* The Phone app's window: the Dialer's main screen (OldMainActivityPeer, main_activity.xml; spec §2).
 *
 * Top to bottom: the search bar; the tab's page (Favorites, Recents or Contacts) with the button that opens the
 * dialpad over its corner; the bottom navigation with the three tabs. The dialpad slides up over all of it. */
#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

GtkWidget *dialer_window_new          (GtkApplication *app, GSettings *settings);
/* the Recents tab (a tap on a missed call's notification) */
void       dialer_window_show_recents (GtkWidget *window);
/* the dialpad, with @number typed in (a tel: link) or as it was (NULL: Add call) */
void       dialer_window_show_dialpad (GtkWidget *window, const char *number);

G_END_DECLS
