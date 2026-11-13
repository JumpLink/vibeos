/* SettingsPage: the Clock's settings, opened from the gear in the app bar or the cities picker's menu.
 *
 * The rows are DeskClock's, each writing its neo.Clock key at once: lists open a radio dialog, switches toggle
 * from anywhere on the row, the alarm volume is a slider that plays the alarm sound for two seconds when let go.
 * Some of its rows are not here, because nothing on this phone could do what they say (see settings-page.c).
 *
 * settings_page_new_screensaver is the screen saver's own settings page (Settings ▸ Screen saver ▸ Settings).
 *
 * Signals: "closed" (): Up was pressed; "pick-timer-sound" (): "Timer sound", for the window to open the ringtone
 * picker; "show-screensaver" (): "Preview"; "show-screensaver-settings" (): Screen saver ▸ "Settings". */
#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define CLOCK_TYPE_SETTINGS_PAGE (settings_page_get_type ())
G_DECLARE_FINAL_TYPE (SettingsPage, settings_page, CLOCK, SETTINGS_PAGE, GtkWidget)

GtkWidget *settings_page_new (GSettings *settings);
GtkWidget *settings_page_new_screensaver (GSettings *settings);

G_END_DECLS
