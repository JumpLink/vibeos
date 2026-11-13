/* The alarm notifications in the shade: "Upcoming alarm" from 2 h before, "Snoozing until ...", the ringing alarm with
 * Snooze and Dismiss, "Missed alarm". Each instance has at most one, the one for its state.
 *
 * Some states answer the user swiping their notification away: the upcoming one then stays hidden, a missed one is
 * over. clock-notifier.c says why that takes org.freedesktop.Notifications. */
#pragma once

#include "alarm-model.h"

typedef struct _AlarmNotifications AlarmNotifications;

/* the user acted on an instance's notification: @action is "dismiss" or "snooze" (its buttons), "open" (tapped) or
 * "closed" (swiped away) */
typedef void (*AlarmNotificationCallback) (guint instance_id, const char *action, gpointer data);

AlarmNotifications *alarm_notifications_new (AlarmNotificationCallback callback, gpointer data);
void                alarm_notifications_free (AlarmNotifications *notifications);

/* the notification for the instance's state now, in place of what it had; none for some states */
void                alarm_notifications_show (AlarmNotifications *notifications, const AlarmInstance *instance);
void                alarm_notifications_clear (AlarmNotifications *notifications, guint instance_id);

/* "Mon 07:30" or "Mon 7:30 AM", as the user's clock format */
char               *alarm_format_time (GDateTime *time);
