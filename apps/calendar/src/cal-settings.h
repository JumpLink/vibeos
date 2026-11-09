/* Settings (Etar's SettingsActivity: MainListPreferences, GeneralPreferences, CalendarPreferences) and About
 * (AboutActivity); spec/calendar.md §9, measured still:cal-settings and still:cal-settings-general.
 *
 * Only the settings something in this app acts on are offered. Left out of "General settings", each because nothing
 * here would do what it says:
 *  - Theme, Pure black night mode: the app is dark only (the phone's theme).
 *  - Display options (the month view's lines and times), Staggered display, Show week number, Days per week: the
 *    views draw one way (seven days, non-staggered, no week numbers).
 *  - Hide declined events: the store does not read attendee status.
 *  - Use home time zone, Home time zone: events show in the phone's zone.
 *  - Clear search history: there is no search.
 *  - Sound, Vibrate, Pop-up notification, Ask for reminder snooze delay, Quick responses: a reminder is a desktop
 *    notification; its sound and vibration are the notification server's, and there are no guests to answer.
 * Left out of the main list: "Add CalDAV calendar", "Add EteSync calendar", "Add offline calendar" (accounts are
 * set up outside the app). A calendar's page has "Display events" only: changing its colour or name or deleting it
 * is not built. */
#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

GtkWidget *cal_settings_page_new (GtkWidget *window);
GtkWidget *cal_about_page_new    (GtkWidget *window);

G_END_DECLS
