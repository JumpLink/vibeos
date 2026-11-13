/* The reminder screens a notification opens in the app: the reminders page (the reminders fired and not
 * dismissed), the snooze delay (how long Snooze waits) and Email guests. The reminder service (reminders-main.c)
 * asks for them through the app's actions (cal-main.c), and they answer through its neo.Calendar.Reminders
 * interface, whose state file stays the only record of what is fired. */
#pragma once

#include "cal-model.h"

#include <gtk/gtk.h>

G_BEGIN_DECLS

/* The reminders page (Pop-up notification, "show-alerts"): "Calendar notifications" over @window, the fired reminders
 * by start and title, "Dismiss all". A tap on one dismisses it and opens its event. It follows the service as reminders
 * fire and are dismissed, and goes when none is left; nothing shows when none is fired. Shown already: brought up to
 * date. */
void cal_alert_page_show (GtkWidget *window);

/* The snooze delay ("snooze-delay"): a time picker at @minutes (the default snooze delay) as hours and minutes;
 * OK snoozes @occurrence that long, Cancel leaves the reminder as it was */
void cal_alert_snooze_delay (GtkWidget *window, const char *occurrence, int minutes);

/* Email guests (from a notification or an event's info): "Choose quick response", the quick responses
 * sorted, then "Write your own…"; the mail app then opens to the guests other than @me with it as the
 * body (none for "Write your own…"). Nothing is sent from here. */
void cal_alert_email_guests (GtkWidget *window, GSettings *settings, const CalEvent *event, const char *me);

G_END_DECLS
