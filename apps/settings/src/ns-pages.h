/* VibeOS Settings' own pages; each pushes itself onto the navigation. */
#pragma once

#include <gtk/gtk.h>
#include "ns-ui.h"

/* ns-pages.c: the hubs and the small pages */
/* Network & internet, Connected devices */
void ns_page_connections (GtkWidget *row, gpointer data);
void ns_page_connected_devices (GtkWidget *row, gpointer data);
void ns_page_display (GtkWidget *row, gpointer data);
/* Display size and text: Font size, Display size, Bold text */
void ns_page_display_size (GtkWidget *row, gpointer data);
void ns_page_night_light (GtkWidget *row, gpointer data);
void ns_page_battery (GtkWidget *row, gpointer data);
void ns_page_battery_info (GtkWidget *row, gpointer data);
void ns_page_wallpaper (GtkWidget *row, gpointer data);
void ns_page_home (GtkWidget *row, gpointer data);
void ns_page_general (GtkWidget *row, gpointer data);
void ns_page_about (GtkWidget *row, gpointer data);

/* ns-net.c: NetworkManager */
void ns_page_wifi (GtkWidget *row, gpointer data);
void ns_page_mobile (GtkWidget *row, gpointer data);
void ns_page_hotspot (GtkWidget *row, gpointer data);
void ns_page_network (GtkWidget *row, gpointer data);
void ns_page_printers (GtkWidget *row, gpointer data);

/* ns-bt.c: BlueZ */
void ns_page_bluetooth (GtkWidget *row, gpointer data);
void ns_page_sharing (GtkWidget *row, gpointer data);

/* ns-sound.c: PulseAudio (PipeWire's), feedbackd, the Phone's and the Clock's sounds */
void ns_page_sound (GtkWidget *row, gpointer data);

/* ns-apps.c: apps, their notifications, default apps */
void ns_page_notifications (GtkWidget *row, gpointer data);
void ns_page_apps (GtkWidget *row, gpointer data);
void ns_page_default_apps (GtkWidget *row, gpointer data);
void ns_page_app (const char *id);
/* the same as a page function: `id` is the app's desktop file id */
void ns_page_app_row (GtkWidget *row, gpointer id);

/* ns-shell.c: the lock screen (VibeOS's keyguard), the on-screen keyboard (GNOME Shell's) */
void ns_page_lock_screen (GtkWidget *row, gpointer data);
void ns_page_keyboard (GtkWidget *row, gpointer data);
/* whether the screen is an OLED panel with a low power mode: the always-on display is offered only there */
gboolean ns_panel_is_oled (void);

/* ns-privacy.c */
void ns_page_privacy (GtkWidget *row, gpointer data);
void ns_page_location (GtkWidget *row, gpointer data);
void ns_page_accounts (GtkWidget *row, gpointer data);
void ns_page_accessibility (GtkWidget *row, gpointer data);

/* ns-system.c */
void ns_page_language (GtkWidget *row, gpointer data);
void ns_page_datetime (GtkWidget *row, gpointer data);
void ns_page_users (GtkWidget *row, gpointer data);
void ns_page_power (GtkWidget *row, gpointer data);
/* whether a daemon of the phone's own suspends it whenever the screen is off (then GNOME's automatic suspend has
 * nothing to do) */
gboolean ns_screen_off_sleeper (void);
void ns_page_mouse (GtkWidget *row, gpointer data);
void ns_page_wallpapers (GtkWidget *row, gpointer data);
void ns_wallpaper_ask (GtkWidget *w, const char *path, const char *dark);
void ns_page_software (GtkWidget *row, gpointer data);

/* ns-displays.c: Display's displays, through the compositor's DisplayConfig. The cards of the connected displays
 * (Join or Mirror, the main display, each display's own page), added to `content` and shown while more than one
 * display is connected; and the "Screen scale" row, added to `group` once the compositor has answered. `page` is the
 * Display page, which both follow the compositor for. */
void ns_displays_cards (GtkWidget *page, GtkWidget *content);
void ns_displays_scale_row (GtkWidget *page, GtkWidget *group);
/* Display size: the built-in panel's scale in steps from its own (vibeos.Interface display-size); `enabled` FALSE
 * shows it without letting it change */
void ns_displays_size_row (GtkWidget *page, GtkWidget *group, gboolean enabled);
/* Display size back at the screen's own scale (step 0), when the page's row can change it */
void ns_displays_size_reset (GtkWidget *page);
