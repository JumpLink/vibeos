/* The reminder screens Etar opens from a notification, in the app: AlertActivity (the reminders page: the
 * reminders fired and not dismissed), SnoozeDelayActivity (how long Snooze waits) and QuickResponseActivity (Email
 * guests). The reminder service (reminders-main.c) asks for them through the app's actions (cal-main.c), and they
 * answer through its de.yesman.Calendar.Reminders interface, whose state file stays the only record of what is
 * fired. */
#pragma once

#include "cal-model.h"

#include <gtk/gtk.h>

G_BEGIN_DECLS

/* AlertActivity (Pop-up notification, "show-alerts"): "Calendar notifications" over @window, the fired reminders by
 * start and title, "Dismiss all". A tap on one dismisses it and opens its event. It follows the service as reminders
 * fire and are dismissed, and goes when none is left; nothing shows when none is fired. Shown already: brought up
 * to date (singleInstance). */
void cal_alert_page_show (GtkWidget *window);

/* SnoozeDelayActivity ("snooze-delay"): a time picker at @minutes (the default snooze delay) as hours and minutes;
 * OK snoozes @occurrence that long, Cancel leaves the reminder as it was */
void cal_alert_snooze_delay (GtkWidget *window, const char *occurrence, int minutes);

/* QuickResponseActivity (Email guests, from a notification or an event's info): "Choose quick response", the quick
 * responses sorted, then "Write your own…"; the mail app then opens to the guests other than @me with it as the
 * body (none for "Write your own…"). Nothing is sent from here. */
void cal_alert_email_guests (GtkWidget *window, GSettings *settings, const CalEvent *event, const char *me);

G_END_DECLS
