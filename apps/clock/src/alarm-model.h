/* The clock's alarms, as DeskClock keeps them.
 *
 * An Alarm is what the user sets: a time of day, the days it repeats on, its sound, vibration and label. An
 * AlarmInstance is one coming occurrence of it: a date and time on the wall clock, and where that occurrence is in
 * its life (the AlarmState below: quiet, announced as upcoming, snoozed, ringing, missed...). An enabled alarm has
 * one instance at a time; when that one is over (dismissed, missed), the next is made from the alarm.
 *
 * Instances keep wall-clock fields (year, month, day, hour, minute), not a Unix time: "7:30 on Monday" stays 7:30
 * across a change of time zone or summer time: the moment is worked out from them each time.
 *
 * Both the alarm service (vibeos-clock-alarms) and the app use this file; only the service writes the store. */
#pragma once

#include <glib.h>

/* the days of the week as bits: Monday is bit 0, Sunday bit 6 */
typedef enum {
  ALARM_MONDAY    = 1 << 0,
  ALARM_TUESDAY   = 1 << 1,
  ALARM_WEDNESDAY = 1 << 2,
  ALARM_THURSDAY  = 1 << 3,
  ALARM_FRIDAY    = 1 << 4,
  ALARM_SATURDAY  = 1 << 5,
  ALARM_SUNDAY    = 1 << 6,
  ALARM_EVERY_DAY = 0x7f,
} AlarmDays;

/* an AlarmInstance's states, by the numbers the store keeps */
typedef enum {
  ALARM_STATE_SILENT = 0,               /* coming, nothing shown yet */
  ALARM_STATE_LOW_NOTIFICATION = 1,     /* "Upcoming alarm" in the shade, from 2 h before */
  ALARM_STATE_HIDE_NOTIFICATION = 2,    /* ... swiped away by the user */
  ALARM_STATE_HIGH_NOTIFICATION = 3,    /* ... again, from 30 min before */
  ALARM_STATE_SNOOZE = 4,               /* snoozed: rings again at its (moved) time */
  ALARM_STATE_FIRED = 5,                /* ringing */
  ALARM_STATE_MISSED = 6,               /* rang out unanswered (auto-silence) */
  ALARM_STATE_DISMISSED = 7,            /* over */
  ALARM_STATE_PREDISMISSED = 8,         /* dismissed before it rang ("Dismiss now") */
} AlarmState;

typedef struct {
  guint id;
  int hour;                     /* 0-23 */
  int minute;
  guint days;                   /* AlarmDays it repeats on; 0: once */
  gboolean enabled;
  gboolean vibrate;
  gboolean delete_after_use;    /* one made by another app's "set an alarm" request, gone once it rang */
  char *label;                  /* "" for none */
  char *ringtone;               /* a sound file's URI; "" silent; NULL the default alarm sound */
} Alarm;

typedef struct {
  guint id;
  guint alarm_id;               /* its alarm; 0 once that was deleted */
  int year, month, day;         /* when it rings, on the wall clock; month 1-12 */
  int hour, minute;
  AlarmState state;
  /* the alarm's settings when the instance was made: changing the alarm makes a new instance */
  gboolean vibrate;
  char *label;
  char *ringtone;
} AlarmInstance;

Alarm *alarm_new (void);
Alarm *alarm_copy (const Alarm *alarm);
void   alarm_free (Alarm *alarm);
G_DEFINE_AUTOPTR_CLEANUP_FUNC (Alarm, alarm_free)

AlarmInstance *alarm_instance_new (void);
AlarmInstance *alarm_instance_copy (const AlarmInstance *instance);
void           alarm_instance_free (AlarmInstance *instance);
G_DEFINE_AUTOPTR_CLEANUP_FUNC (AlarmInstance, alarm_instance_free)

/* the first moment after @after (local time) the alarm goes off */
GDateTime *alarm_next_time (const Alarm *alarm, GDateTime *after);

/* an instance of @alarm at its next time after @after */
AlarmInstance *alarm_instance_after (const Alarm *alarm, GDateTime *after);

/* the moments in an instance's life, in the current local time zone */
GDateTime *alarm_instance_time (const AlarmInstance *instance);             /* when it rings */
GDateTime *alarm_instance_low_notification_time (const AlarmInstance *instance);   /* 2 h before */
GDateTime *alarm_instance_high_notification_time (const AlarmInstance *instance);  /* 30 min before */
GDateTime *alarm_instance_missed_ttl (const AlarmInstance *instance);       /* 12 h after: forgotten */
void       alarm_instance_set_time (AlarmInstance *instance, GDateTime *time);

/* "Mon, Tue, Wed" / "Monday" / "Every day", as the alarm list says it; "" for no days;
 * @first_day: the week's first day (G_DATE_MONDAY .. G_DATE_SUNDAY) */
char *alarm_days_text (guint days, GDateWeekday first_day);

/* --- the store: one key file, groups [alarm N] and [instance N] */

typedef struct {
  GPtrArray *alarms;            /* Alarm */
  GPtrArray *instances;         /* AlarmInstance */
  guint next_id;
} AlarmStore;

/* the store in the file; with no file yet, a new store holding the first-run alarms */
AlarmStore *alarm_store_load (const char *path, GError **error);
gboolean    alarm_store_save (AlarmStore *store, const char *path, GError **error);
void        alarm_store_free (AlarmStore *store);
G_DEFINE_AUTOPTR_CLEANUP_FUNC (AlarmStore, alarm_store_free)

Alarm         *alarm_store_find_alarm (AlarmStore *store, guint id);
AlarmInstance *alarm_store_find_instance (AlarmStore *store, guint id);
/* the instance of an alarm, or NULL */
AlarmInstance *alarm_store_instance_of (AlarmStore *store, guint alarm_id);
