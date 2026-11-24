/* Settings (the main list, General settings, a calendar's page) and About.
 *
 * Every setting is one the app or its reminder service acts on, kept in GSettings (vibeos.Calendar): Theme and Pure black
 * (cal-main's apply_look), Use real event colors, Default view, Display options (the Month view), Hide declined events,
 * Staggered display (the Day and Week views), Show week number, Week starts on, Days per week, Default event duration,
 * Use home time zone / Home time zone (cal_set_home_zone), Clear search history; Notifications (its own page: Show
 * notifications, Sound, Vibrate), Pop-up notification, Ask for reminder snooze delay, Default snooze delay time,
 * Default reminder time, Quick responses (with a mail app only: Email guests is their only use), Skip Reminders.
 *
 * Left out, each because nothing here would do what it says:
 *  - Vibrate on a phone without a vibrator (feedbackd's force-feedback device), Quick responses without a mail app.
 *  - "Battery optimization" (Experimental): the reminder service is woken by vibeos-wakeup and needs no exemption.
 *  - SMS reminders and "Add EteSync calendar": no SMS reminders exist, and no EteSync backend for EDS is installed. */
#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

GtkWidget *cal_settings_page_new (GtkWidget *window);
/* Display options: General settings' row, and the Month view's "View settings" */
GtkWidget *cal_display_options_page_new (GtkWidget *window);
/* the reminders' Notifications page (General settings' "Notifications") */
GtkWidget *cal_notifications_page_new   (GtkWidget *window);
GtkWidget *cal_quick_responses_page_new (GtkWidget *window);
/* a mail app is there to write with (the mailto: handler): Email guests and Quick responses need one */
gboolean   cal_can_mail                 (void);

G_END_DECLS
