/* The app's side of the alarm service (vibeos-clock-alarms, vibeos.Clock.Alarms on the session bus).
 *
 * The service owns the alarms and rings them; the app only shows them and asks for changes. AlarmsClient keeps
 * the service's list (GetAlarms, fetched again on every Changed signal) and emits "changed" when it has a new one.
 * Calls the app makes change nothing locally: the list comes back through "changed", so what the screen shows is
 * always what the service holds. Asking starts the service if it is not running (D-Bus activation). */
#pragma once

#include <gio/gio.h>

G_BEGIN_DECLS

/* one alarm as the service reports it, with its coming instance if it has one */
typedef struct
{
  guint id;
  int hour, minute;
  guint days;                   /* AlarmDays */
  gboolean enabled;
  gboolean vibrate;
  gboolean delete_after_use;
  char *label;                  /* "" for none */
  char *ringtone;               /* NULL: the default sound; "": silent */
  guint instance;               /* 0: none */
  int instance_state;           /* AlarmState */
  gint64 instance_time;         /* Unix */
} ClockAlarm;

void clock_alarm_free (ClockAlarm *alarm);

#define ALARMS_TYPE_CLIENT (alarms_client_get_type ())
G_DECLARE_FINAL_TYPE (AlarmsClient, alarms_client, ALARMS, CLIENT, GObject)

/* the one client of the process */
AlarmsClient *alarms_client_get (void);

/* ClockAlarm, by time of day, the newest first among equal times */
GPtrArray  *alarms_client_get_alarms (AlarmsClient *self);
ClockAlarm *alarms_client_find (AlarmsClient *self, guint id);
/* when the next alarm rings (Unix), 0 for none */
gint64      alarms_client_get_next_alarm (AlarmsClient *self);
/* the ringing instance, or FALSE */
gboolean    alarms_client_get_ringing (AlarmsClient *self, guint *instance, guint *alarm, char **label,
                                       gint64 *time);

typedef void (*AlarmsAdded) (guint id, gpointer data);

/* a new alarm like `alarm` (its id and instance fields aside): the "+", or an undone delete; `added` gets
 * the new alarm's id */
void alarms_client_add (AlarmsClient *self, const ClockAlarm *alarm, AlarmsAdded added, gpointer data);
/* the alarm's fields listed (a GVariant a{sv}, floating, of the service's keys) change */
void alarms_client_update (AlarmsClient *self, guint id, GVariant *changes);
void alarms_client_delete (AlarmsClient *self, guint id);
void alarms_client_snooze (AlarmsClient *self, guint instance);
void alarms_client_dismiss (AlarmsClient *self, guint instance);

/* Signals: "changed" (the alarm list), "ringing-changed", "next-alarm-changed" */

G_END_DECLS
