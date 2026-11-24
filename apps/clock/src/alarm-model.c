/* The clock's alarms and their instances (alarm-model.h). */
#include "alarm-model.h"

#include <stdio.h>
#include <string.h>
#include <vibeos-locale.h>

/* offsets from the moment an instance rings */
#define LOW_NOTIFICATION_HOURS      -2      /* "Upcoming alarm" shows */
#define HIGH_NOTIFICATION_MINUTES  -30      /* ... and again, higher up */
#define MISSED_TTL_HOURS            12      /* a missed alarm's notification stays this long */

Alarm *
alarm_new (void)
{
  Alarm *alarm = g_new0 (Alarm, 1);

  /* a new alarm: on, vibrating, the default sound, no label */
  alarm->enabled = TRUE;
  alarm->vibrate = TRUE;
  alarm->label = g_strdup ("");
  return alarm;
}

Alarm *
alarm_copy (const Alarm *alarm)
{
  Alarm *copy = g_memdup2 (alarm, sizeof *alarm);

  copy->label = g_strdup (alarm->label);
  copy->ringtone = g_strdup (alarm->ringtone);
  return copy;
}

void
alarm_free (Alarm *alarm)
{
  if (!alarm)
    return;
  g_free (alarm->label);
  g_free (alarm->ringtone);
  g_free (alarm);
}

AlarmInstance *
alarm_instance_new (void)
{
  AlarmInstance *instance = g_new0 (AlarmInstance, 1);

  instance->label = g_strdup ("");
  return instance;
}

AlarmInstance *
alarm_instance_copy (const AlarmInstance *instance)
{
  AlarmInstance *copy = g_memdup2 (instance, sizeof *instance);

  copy->label = g_strdup (instance->label);
  copy->ringtone = g_strdup (instance->ringtone);
  return copy;
}

void
alarm_instance_free (AlarmInstance *instance)
{
  if (!instance)
    return;
  g_free (instance->label);
  g_free (instance->ringtone);
  g_free (instance);
}

/* --- times */

/* the local time zone as it is now: a new one each time, so a change of zone is seen (g_time_zone_new_local caches).
 * TZ first, as g_time_zone_new_local has it: a NULL identifier reads /etc/localtime only */
static GTimeZone *
local_zone (void)
{
  return g_time_zone_new_identifier (g_getenv ("TZ"));
}

static GDateTime *
wall_time (int year, int month, int day, int hour, int minute)
{
  g_autoptr(GTimeZone) zone = local_zone ();
  g_autoptr(GDateTime) time = g_date_time_new (zone, year, month, day, hour, minute, 0);
  g_autoptr(GDateTime) day_before = NULL;
  g_autoptr(GDateTime) utc = NULL;
  g_autoptr(GDateTime) moment = NULL;

  if (g_date_time_get_hour (time) == hour && g_date_time_get_minute (time) == minute)
    return g_steal_pointer (&time);

  /* a wall time that summer time skips (02:30 on the spring-forward night). GLib moves it to the start of the new
   * hour (03:00); it is read here with the UTC offset from before the jump, the offset of a day before, so it comes
   * out the skipped hour later (03:30) */
  day_before = g_date_time_add_days (time, -1);
  utc = g_date_time_new_utc (year, month, day, hour, minute, 0);
  moment = g_date_time_add (utc, -g_date_time_get_utc_offset (day_before));
  return g_date_time_to_timezone (moment, zone);
}

/* Monday 0 .. Sunday 6, as the AlarmDays bits */
static int
weekday_index (GDate *date)
{
  return g_date_get_weekday (date) - G_DATE_MONDAY;
}

GDateTime *
alarm_next_time (const Alarm *alarm, GDateTime *after)
{
  g_autoptr(GTimeZone) zone = local_zone ();
  g_autoptr(GDateTime) local = g_date_time_to_timezone (after, zone);
  g_autoptr(GDateTime) today = NULL;
  GDate date;

  g_date_clear (&date, 1);
  g_date_set_dmy (&date, g_date_time_get_day_of_month (local), g_date_time_get_month (local),
                  g_date_time_get_year (local));

  /* the alarm's time today; tomorrow if that is not after @after */
  today = wall_time (g_date_get_year (&date), g_date_get_month (&date), g_date_get_day (&date),
                     alarm->hour, alarm->minute);
  if (g_date_time_compare (today, local) <= 0)
    g_date_add_days (&date, 1);

  /* then on to the first day it repeats on */
  if (alarm->days)
    {
      for (int i = 0; i < 7 && !(alarm->days & (1u << weekday_index (&date))); i++)
        g_date_add_days (&date, 1);
    }

  /* the hour and minute set on that day, so a summer-time change between today and then does not move them */
  return wall_time (g_date_get_year (&date), g_date_get_month (&date), g_date_get_day (&date),
                    alarm->hour, alarm->minute);
}

void
alarm_instance_set_time (AlarmInstance *instance, GDateTime *time)
{
  g_autoptr(GTimeZone) zone = local_zone ();
  g_autoptr(GDateTime) local = g_date_time_to_timezone (time, zone);

  instance->year = g_date_time_get_year (local);
  instance->month = g_date_time_get_month (local);
  instance->day = g_date_time_get_day_of_month (local);
  instance->hour = g_date_time_get_hour (local);
  instance->minute = g_date_time_get_minute (local);
}

AlarmInstance *
alarm_instance_after (const Alarm *alarm, GDateTime *after)
{
  AlarmInstance *instance = alarm_instance_new ();
  g_autoptr(GDateTime) time = alarm_next_time (alarm, after);

  instance->alarm_id = alarm->id;
  alarm_instance_set_time (instance, time);
  instance->state = ALARM_STATE_SILENT;
  instance->vibrate = alarm->vibrate;
  g_free (instance->label);
  instance->label = g_strdup (alarm->label);
  instance->ringtone = g_strdup (alarm->ringtone);
  return instance;
}

GDateTime *
alarm_instance_time (const AlarmInstance *instance)
{
  return wall_time (instance->year, instance->month, instance->day, instance->hour, instance->minute);
}

GDateTime *
alarm_instance_low_notification_time (const AlarmInstance *instance)
{
  g_autoptr(GDateTime) time = alarm_instance_time (instance);

  return g_date_time_add_hours (time, LOW_NOTIFICATION_HOURS);
}

GDateTime *
alarm_instance_high_notification_time (const AlarmInstance *instance)
{
  g_autoptr(GDateTime) time = alarm_instance_time (instance);

  return g_date_time_add_minutes (time, HIGH_NOTIFICATION_MINUTES);
}

GDateTime *
alarm_instance_missed_ttl (const AlarmInstance *instance)
{
  g_autoptr(GDateTime) time = alarm_instance_time (instance);

  return g_date_time_add_hours (time, MISSED_TTL_HOURS);
}

char *
alarm_days_text (guint days, GDateWeekday first_day)
{
  GString *text;
  /* a week of known weekdays to name them by: 2024-01-01 was a Monday */
  g_autoptr(GDateTime) monday = g_date_time_new_utc (2024, 1, 1, 12, 0, 0);

  if ((days & ALARM_EVERY_DAY) == ALARM_EVERY_DAY)
    return g_strdup (g_dgettext ("vibeos-clock", "Every day"));
  /* one day is named in full ("Monday"), several by their short names */
  if (days && !(days & (days - 1)))
    {
      g_autoptr(GDateTime) day = g_date_time_add_days (monday, __builtin_ctz (days));

      return vibeos_format_date (day, "EEEE");
    }

  text = g_string_new (NULL);
  for (int i = 0; i < 7; i++)
    {
      int index = (first_day - G_DATE_MONDAY + i) % 7;     /* Monday 0 .. Sunday 6 */
      g_autoptr(GDateTime) day = NULL;
      g_autofree char *name = NULL;

      if (!(days & (1u << index)))
        continue;
      day = g_date_time_add_days (monday, index);
      name = vibeos_format_date (day, "EEE");
      g_string_append_printf (text, "%s%s", text->len ? ", " : "", name);
    }
  return g_string_free (text, FALSE);
}

/* --- the store */

static char *
string_or_null (GKeyFile *file, const char *group, const char *key)
{
  return g_key_file_has_key (file, group, key, NULL) ? g_key_file_get_string (file, group, key, NULL) : NULL;
}

static Alarm *
read_alarm (GKeyFile *file, const char *group, guint id)
{
  Alarm *alarm = alarm_new ();

  alarm->id = id;
  alarm->hour = g_key_file_get_integer (file, group, "hour", NULL);
  alarm->minute = g_key_file_get_integer (file, group, "minute", NULL);
  alarm->days = g_key_file_get_integer (file, group, "days", NULL);
  alarm->enabled = g_key_file_get_boolean (file, group, "enabled", NULL);
  alarm->vibrate = g_key_file_get_boolean (file, group, "vibrate", NULL);
  alarm->delete_after_use = g_key_file_get_boolean (file, group, "delete-after-use", NULL);
  g_free (alarm->label);
  alarm->label = string_or_null (file, group, "label");
  if (!alarm->label)
    alarm->label = g_strdup ("");
  alarm->ringtone = string_or_null (file, group, "ringtone");
  return alarm;
}

static AlarmInstance *
read_instance (GKeyFile *file, const char *group, guint id)
{
  AlarmInstance *instance = alarm_instance_new ();

  instance->id = id;
  instance->alarm_id = g_key_file_get_integer (file, group, "alarm", NULL);
  instance->year = g_key_file_get_integer (file, group, "year", NULL);
  instance->month = g_key_file_get_integer (file, group, "month", NULL);
  instance->day = g_key_file_get_integer (file, group, "day", NULL);
  instance->hour = g_key_file_get_integer (file, group, "hour", NULL);
  instance->minute = g_key_file_get_integer (file, group, "minute", NULL);
  instance->state = g_key_file_get_integer (file, group, "state", NULL);
  instance->vibrate = g_key_file_get_boolean (file, group, "vibrate", NULL);
  g_free (instance->label);
  instance->label = string_or_null (file, group, "label");
  if (!instance->label)
    instance->label = g_strdup ("");
  instance->ringtone = string_or_null (file, group, "ringtone");
  return instance;
}

/* a new store's alarms: off, vibrating, no label, the default sound */
static void
add_first_alarm (AlarmStore *store, int hour, int minute, guint days)
{
  Alarm *alarm = alarm_new ();

  alarm->id = store->next_id++;
  alarm->hour = hour;
  alarm->minute = minute;
  alarm->days = days;
  alarm->enabled = FALSE;
  g_ptr_array_add (store->alarms, alarm);
}

AlarmStore *
alarm_store_load (const char *path, GError **error)
{
  g_autoptr(AlarmStore) store = g_new0 (AlarmStore, 1);
  g_autoptr(GKeyFile) file = g_key_file_new ();
  g_autoptr(GError) local_error = NULL;
  g_auto(GStrv) groups = NULL;

  store->alarms = g_ptr_array_new_with_free_func ((GDestroyNotify) alarm_free);
  store->instances = g_ptr_array_new_with_free_func ((GDestroyNotify) alarm_instance_free);
  store->next_id = 1;

  if (!g_key_file_load_from_file (file, path, G_KEY_FILE_KEEP_COMMENTS, &local_error))
    {
      /* no store yet: a new one has two alarms, 08:30 on weekdays and 09:00 at weekends */
      if (g_error_matches (local_error, G_FILE_ERROR, G_FILE_ERROR_NOENT))
        {
          add_first_alarm (store, 8, 30, ALARM_MONDAY | ALARM_TUESDAY | ALARM_WEDNESDAY | ALARM_THURSDAY
                                         | ALARM_FRIDAY);
          add_first_alarm (store, 9, 0, ALARM_SATURDAY | ALARM_SUNDAY);
          return g_steal_pointer (&store);
        }
      g_propagate_error (error, g_steal_pointer (&local_error));
      return NULL;
    }

  groups = g_key_file_get_groups (file, NULL);
  for (char **group = groups; *group; group++)
    {
      guint id;

      if (sscanf (*group, "alarm %u", &id) == 1)
        g_ptr_array_add (store->alarms, read_alarm (file, *group, id));
      else if (sscanf (*group, "instance %u", &id) == 1)
        g_ptr_array_add (store->instances, read_instance (file, *group, id));
      else
        continue;
      store->next_id = MAX (store->next_id, id + 1);
    }
  return g_steal_pointer (&store);
}

gboolean
alarm_store_save (AlarmStore *store, const char *path, GError **error)
{
  g_autoptr(GKeyFile) file = g_key_file_new ();
  g_autofree char *dir = g_path_get_dirname (path);

  g_key_file_set_comment (file, NULL, NULL,
                          " The clock's alarms ([alarm N]) and their coming occurrences ([instance N]).\n"
                          " vibeos-clock-alarms keeps this file: edit it only while that is stopped.", NULL);
  for (guint i = 0; i < store->alarms->len; i++)
    {
      Alarm *alarm = g_ptr_array_index (store->alarms, i);
      g_autofree char *group = g_strdup_printf ("alarm %u", alarm->id);

      g_key_file_set_integer (file, group, "hour", alarm->hour);
      g_key_file_set_integer (file, group, "minute", alarm->minute);
      g_key_file_set_integer (file, group, "days", alarm->days);
      g_key_file_set_boolean (file, group, "enabled", alarm->enabled);
      g_key_file_set_boolean (file, group, "vibrate", alarm->vibrate);
      g_key_file_set_boolean (file, group, "delete-after-use", alarm->delete_after_use);
      g_key_file_set_string (file, group, "label", alarm->label);
      if (alarm->ringtone)
        g_key_file_set_string (file, group, "ringtone", alarm->ringtone);
    }
  for (guint i = 0; i < store->instances->len; i++)
    {
      AlarmInstance *instance = g_ptr_array_index (store->instances, i);
      g_autofree char *group = g_strdup_printf ("instance %u", instance->id);

      g_key_file_set_integer (file, group, "alarm", instance->alarm_id);
      g_key_file_set_integer (file, group, "year", instance->year);
      g_key_file_set_integer (file, group, "month", instance->month);
      g_key_file_set_integer (file, group, "day", instance->day);
      g_key_file_set_integer (file, group, "hour", instance->hour);
      g_key_file_set_integer (file, group, "minute", instance->minute);
      g_key_file_set_integer (file, group, "state", instance->state);
      g_key_file_set_boolean (file, group, "vibrate", instance->vibrate);
      g_key_file_set_string (file, group, "label", instance->label);
      if (instance->ringtone)
        g_key_file_set_string (file, group, "ringtone", instance->ringtone);
    }

  g_mkdir_with_parents (dir, 0700);
  /* written to a new file and renamed over the old one: a crash never leaves half a store */
  return g_key_file_save_to_file (file, path, error);
}

void
alarm_store_free (AlarmStore *store)
{
  if (!store)
    return;
  g_clear_pointer (&store->alarms, g_ptr_array_unref);
  g_clear_pointer (&store->instances, g_ptr_array_unref);
  g_free (store);
}

Alarm *
alarm_store_find_alarm (AlarmStore *store, guint id)
{
  for (guint i = 0; i < store->alarms->len; i++)
    {
      Alarm *alarm = g_ptr_array_index (store->alarms, i);

      if (alarm->id == id)
        return alarm;
    }
  return NULL;
}

AlarmInstance *
alarm_store_find_instance (AlarmStore *store, guint id)
{
  for (guint i = 0; i < store->instances->len; i++)
    {
      AlarmInstance *instance = g_ptr_array_index (store->instances, i);

      if (instance->id == id)
        return instance;
    }
  return NULL;
}

AlarmInstance *
alarm_store_instance_of (AlarmStore *store, guint alarm_id)
{
  for (guint i = 0; i < store->instances->len; i++)
    {
      AlarmInstance *instance = g_ptr_array_index (store->instances, i);

      if (alarm_id && instance->alarm_id == alarm_id)
        return instance;
    }
  return NULL;
}
