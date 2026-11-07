/* alarm-model's times against what DeskClock does, in Berlin (summer time changes on 2026-03-29 and 2026-10-25) */
#include "../src/alarm-model.h"

#include <glib/gstdio.h>

static GDateTime *
berlin (int y, int mo, int d, int h, int mi, int s)
{
  g_autoptr(GTimeZone) zone = g_time_zone_new_identifier ("Europe/Berlin");

  return g_date_time_new (zone, y, mo, d, h, mi, s);
}

static void
check_next (guint days, int hour, int minute, GDateTime *after, const char *expected)
{
  g_autoptr(Alarm) alarm = alarm_new ();
  g_autoptr(GDateTime) next = NULL;
  g_autofree char *text = NULL;

  alarm->days = days;
  alarm->hour = hour;
  alarm->minute = minute;
  next = alarm_next_time (alarm, after);
  text = g_date_time_format (next, "%a %F %R %Z");
  g_assert_cmpstr (text, ==, expected);
  g_date_time_unref (after);
}

static void
test_next_time (void)
{
  guint weekdays = ALARM_MONDAY | ALARM_TUESDAY | ALARM_WEDNESDAY | ALARM_THURSDAY | ALARM_FRIDAY;

  /* a weekday alarm on Friday after its time: Monday */
  check_next (weekdays, 7, 30, berlin (2026, 11, 6, 8, 0, 0), "Mon 2026-11-09 07:30 CET");
  /* once, a second before: today; at the very minute: tomorrow (Android's <=) */
  check_next (0, 7, 30, berlin (2026, 11, 7, 7, 29, 59), "Sat 2026-11-07 07:30 CET");
  check_next (0, 7, 30, berlin (2026, 11, 7, 7, 30, 0), "Sun 2026-11-08 07:30 CET");
  /* Sundays only, from a Sunday after it: a week on */
  check_next (ALARM_SUNDAY, 7, 30, berlin (2026, 11, 8, 7, 31, 0), "Sun 2026-11-15 07:30 CET");
  /* 02:30 does not exist on the spring-forward night: an hour later, as Android's lenient Calendar */
  check_next (0, 2, 30, berlin (2026, 3, 28, 23, 0, 0), "Sun 2026-03-29 03:30 CEST");
  /* the hour and minute hold across the change: 07:30 CEST on the Monday after */
  check_next (ALARM_MONDAY, 7, 30, berlin (2026, 3, 27, 9, 0, 0), "Mon 2026-03-30 07:30 CEST");
  check_next (ALARM_MONDAY, 7, 30, berlin (2026, 10, 23, 9, 0, 0), "Mon 2026-10-26 07:30 CET");
}

static void
test_days_text (void)
{
  g_autofree char *weekdays = alarm_days_text (0x1f, G_DATE_SUNDAY);
  g_autofree char *weekend = alarm_days_text (ALARM_SATURDAY | ALARM_SUNDAY, G_DATE_SUNDAY);
  g_autofree char *weekend_monday_first = alarm_days_text (ALARM_SATURDAY | ALARM_SUNDAY, G_DATE_MONDAY);
  g_autofree char *every = alarm_days_text (ALARM_EVERY_DAY, G_DATE_SUNDAY);
  g_autofree char *one = alarm_days_text (ALARM_WEDNESDAY, G_DATE_SUNDAY);

  /* as the alarm list of the reference still clock-alarm.png */
  g_assert_cmpstr (weekdays, ==, "Mon, Tue, Wed, Thu, Fri");
  g_assert_cmpstr (weekend, ==, "Sun, Sat");
  g_assert_cmpstr (weekend_monday_first, ==, "Sat, Sun");
  g_assert_cmpstr (every, ==, "Every day");
  g_assert_cmpstr (one, ==, "Wednesday");
}

static void
test_store_round_trip (void)
{
  g_autofree char *dir = g_dir_make_tmp ("alarm-model-XXXXXX", NULL);
  g_autofree char *path = g_build_filename (dir, "alarms.ini", NULL);
  g_autoptr(AlarmStore) store = alarm_store_load (path, NULL);
  g_autoptr(AlarmStore) again = NULL;
  Alarm *alarm = alarm_new ();
  AlarmInstance *instance;
  Alarm *first;
  g_autoptr(GDateTime) now = berlin (2026, 11, 7, 6, 0, 0);

  /* a new store: DeskClock's two first alarms, both off */
  g_assert_nonnull (store);
  g_assert_cmpuint (store->alarms->len, ==, 2);
  first = g_ptr_array_index (store->alarms, 0);
  g_assert_cmpint (first->hour, ==, 8);
  g_assert_cmpint (first->minute, ==, 30);
  g_assert_cmpuint (first->days, ==, 31);
  g_assert_false (first->enabled);
  g_assert_true (first->vibrate);
  first = g_ptr_array_index (store->alarms, 1);
  g_assert_cmpint (first->hour, ==, 9);
  g_assert_cmpuint (first->days, ==, 96);
  g_assert_false (first->enabled);
  g_ptr_array_set_size (store->alarms, 0);
  store->next_id = 1;

  alarm->id = store->next_id++;
  alarm->hour = 8;
  alarm->minute = 30;
  alarm->days = 0x1f;
  g_free (alarm->label);
  alarm->label = g_strdup ("Work = [early]");
  g_ptr_array_add (store->alarms, alarm);
  instance = alarm_instance_after (alarm, now);
  instance->id = store->next_id++;
  g_ptr_array_add (store->instances, instance);
  g_assert_true (alarm_store_save (store, path, NULL));

  again = alarm_store_load (path, NULL);
  g_assert_cmpuint (again->alarms->len, ==, 1);
  g_assert_cmpuint (again->next_id, ==, 3);
  g_assert_cmpstr (alarm_store_find_alarm (again, 1)->label, ==, "Work = [early]");
  g_assert_null (alarm_store_find_alarm (again, 1)->ringtone);
  g_assert_cmpint (alarm_store_instance_of (again, 1)->day, ==, 9);       /* Saturday 6:00: Monday the 9th */
  g_remove (path);
  g_rmdir (dir);
}

int
main (int argc, char **argv)
{
  g_setenv ("TZ", "Europe/Berlin", TRUE);
  g_setenv ("LC_ALL", "C", TRUE);
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/alarm-model/next-time", test_next_time);
  g_test_add_func ("/alarm-model/days-text", test_days_text);
  g_test_add_func ("/alarm-model/store", test_store_round_trip);
  return g_test_run ();
}
