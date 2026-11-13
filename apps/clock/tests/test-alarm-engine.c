/* alarm-engine's state changes against DeskClock's, on a clock the test sets (Berlin) */
#include "../src/alarm-engine.h"

typedef struct {
  AlarmEngine *engine;
  GDateTime *now;
  GString *log;                 /* the signals, as "5:FIRED 5:gone ..." */
} Fixture;

static const char *state_names[] = {
  "SILENT", "LOW", "HIDE", "HIGH", "SNOOZE", "FIRED", "MISSED", "DISMISSED", "PREDISMISSED",
};

static GDateTime *
fake_now (gpointer data)
{
  Fixture *fixture = data;

  return g_date_time_ref (fixture->now);
}

static void
set_now (Fixture *fixture, int y, int mo, int d, int h, int mi, int s)
{
  g_autoptr(GTimeZone) zone = g_time_zone_new_identifier ("Europe/Berlin");

  g_clear_pointer (&fixture->now, g_date_time_unref);
  fixture->now = g_date_time_new (zone, y, mo, d, h, mi, s);
}

/* the clock moves to the next change and the engine makes it, as the alarm service does */
static void
run_to_next_change (Fixture *fixture)
{
  GDateTime *next = alarm_engine_next_change (fixture->engine);

  g_assert_nonnull (next);
  g_date_time_unref (fixture->now);
  fixture->now = next;
  alarm_engine_tick (fixture->engine);
}

static void
on_instance_changed (AlarmEngine *engine, guint id, int state, Fixture *fixture)
{
  g_string_append_printf (fixture->log, "%u:%s ", id, state_names[state]);
}

static void
on_instance_removed (AlarmEngine *engine, guint id, Fixture *fixture)
{
  g_string_append_printf (fixture->log, "%u:gone ", id);
}

static void
fixture_set_up (Fixture *fixture, gconstpointer data)
{
  AlarmStore *store = g_new0 (AlarmStore, 1);
  AlarmEngineSettings settings = { .snooze_minutes = 10, .auto_silence_minutes = 10 };

  store->alarms = g_ptr_array_new_with_free_func ((GDestroyNotify) alarm_free);
  store->instances = g_ptr_array_new_with_free_func ((GDestroyNotify) alarm_instance_free);
  store->next_id = 1;
  fixture->engine = alarm_engine_new (store);
  alarm_engine_set_settings (fixture->engine, &settings);
  alarm_engine_set_clock (fixture->engine, fake_now, fixture);
  fixture->log = g_string_new (NULL);
  g_signal_connect (fixture->engine, "instance-changed", G_CALLBACK (on_instance_changed), fixture);
  g_signal_connect (fixture->engine, "instance-removed", G_CALLBACK (on_instance_removed), fixture);
}

static void
fixture_tear_down (Fixture *fixture, gconstpointer data)
{
  g_object_unref (fixture->engine);
  g_date_time_unref (fixture->now);
  g_string_free (fixture->log, TRUE);
}

/* the log since the last look, emptied */
static char *
take_log (Fixture *fixture)
{
  char *log = g_strdup (g_strchomp (fixture->log->str));

  g_string_truncate (fixture->log, 0);
  return log;
}

#define assert_log(fixture, expected) G_STMT_START { \
    g_autofree char *log_ = take_log (fixture); \
    g_assert_cmpstr (log_, ==, expected); \
  } G_STMT_END

static guint
add_alarm (Fixture *fixture, int hour, int minute, guint days)
{
  g_autoptr(Alarm) alarm = alarm_new ();

  alarm->hour = hour;
  alarm->minute = minute;
  alarm->days = days;
  alarm->enabled = TRUE;
  return alarm_engine_add_alarm (fixture->engine, alarm);
}

static char *
format (GDateTime *time)
{
  g_autoptr(GDateTime) owned = time;

  return owned ? g_date_time_format (owned, "%a %F %T") : g_strdup ("none");
}

#define assert_next_change(fixture, expected) G_STMT_START { \
    g_autofree char *next_ = format (alarm_engine_next_change ((fixture)->engine)); \
    g_assert_cmpstr (next_, ==, expected); \
  } G_STMT_END

static char *
instance_text (AlarmInstance *instance)
{
  g_autoptr(GDateTime) time = NULL;
  g_autofree char *when = NULL;

  if (!instance)
    return g_strdup ("none");
  time = alarm_instance_time (instance);
  when = g_date_time_format (time, "%a %R");
  return g_strdup_printf ("%s %s", when, state_names[instance->state]);
}

#define assert_next_alarm(fixture, expected) G_STMT_START { \
    g_autofree char *text_ = instance_text (alarm_engine_next_alarm ((fixture)->engine)); \
    g_assert_cmpstr (text_, ==, expected); \
  } G_STMT_END

/* a one-time alarm through its whole life, unanswered: upcoming 2 h and 30 min before, ringing, missed after the
 * auto-silence, forgotten 12 h after; the alarm switches off */
static void
test_rings_out (Fixture *fixture, gconstpointer data)
{
  guint alarm_id;

  set_now (fixture, 2026, 11, 9, 4, 0, 0);
  alarm_id = add_alarm (fixture, 7, 30, 0);
  assert_log (fixture, "2:SILENT");
  assert_next_alarm (fixture, "Mon 07:30 SILENT");
  assert_next_change (fixture, "Mon 2026-11-09 05:30:00");

  run_to_next_change (fixture);
  assert_log (fixture, "2:LOW");
  assert_next_change (fixture, "Mon 2026-11-09 07:00:00");
  run_to_next_change (fixture);
  assert_log (fixture, "2:HIGH");
  run_to_next_change (fixture);
  assert_log (fixture, "2:FIRED");
  g_assert_cmpuint (alarm_engine_ringing (fixture->engine)->id, ==, 2);
  assert_next_alarm (fixture, "none");
  assert_next_change (fixture, "Mon 2026-11-09 07:40:00");

  run_to_next_change (fixture);
  assert_log (fixture, "2:MISSED");
  g_assert_null (alarm_engine_ringing (fixture->engine));
  g_assert_false (alarm_store_find_alarm (alarm_engine_get_store (fixture->engine), alarm_id)->enabled);
  assert_next_change (fixture, "Mon 2026-11-09 19:30:00");
  run_to_next_change (fixture);
  assert_log (fixture, "2:gone");
  assert_next_change (fixture, "none");
}

/* snoozed 40 s into ringing: it rings again at the next whole minute 10 minutes on (seconds are dropped) */
static void
test_snooze (Fixture *fixture, gconstpointer data)
{
  guint alarm_id;

  set_now (fixture, 2026, 11, 9, 7, 29, 0);
  alarm_id = add_alarm (fixture, 7, 30, 0);
  run_to_next_change (fixture);
  set_now (fixture, 2026, 11, 9, 7, 30, 40);
  g_string_truncate (fixture->log, 0);

  g_assert_true (alarm_engine_snooze (fixture->engine, 2));
  assert_log (fixture, "2:SNOOZE");
  assert_next_alarm (fixture, "Mon 07:40 SNOOZE");
  assert_next_change (fixture, "Mon 2026-11-09 07:40:00");
  run_to_next_change (fixture);
  assert_log (fixture, "2:FIRED");
  /* the auto-silence counts from the snoozed time */
  assert_next_change (fixture, "Mon 2026-11-09 07:50:00");

  g_assert_true (alarm_engine_dismiss (fixture->engine, 2));
  assert_log (fixture, "2:gone");
  g_assert_false (alarm_store_find_alarm (alarm_engine_get_store (fixture->engine), alarm_id)->enabled);
  g_assert_false (alarm_engine_snooze (fixture->engine, 2));
}

/* a weekday alarm dismissed on Friday comes back on Monday */
static void
test_repeating (Fixture *fixture, gconstpointer data)
{
  guint weekdays = ALARM_MONDAY | ALARM_TUESDAY | ALARM_WEDNESDAY | ALARM_THURSDAY | ALARM_FRIDAY;
  guint alarm_id;

  set_now (fixture, 2026, 11, 13, 7, 29, 0);
  alarm_id = add_alarm (fixture, 7, 30, weekdays);
  run_to_next_change (fixture);
  assert_log (fixture, "2:HIGH 2:FIRED");
  set_now (fixture, 2026, 11, 13, 7, 31, 0);

  alarm_engine_dismiss (fixture->engine, 2);
  assert_log (fixture, "2:gone 3:SILENT");
  assert_next_alarm (fixture, "Mon 07:30 SILENT");
  g_assert_true (alarm_store_find_alarm (alarm_engine_get_store (fixture->engine), alarm_id)->enabled);
}

/* "Dismiss now" on the upcoming notification: the instance stays until its time, so it is not made again; the
 * alarm's next instance is there at once */
static void
test_dismiss_now (Fixture *fixture, gconstpointer data)
{
  set_now (fixture, 2026, 11, 9, 7, 10, 0);
  add_alarm (fixture, 7, 30, ALARM_EVERY_DAY);
  assert_log (fixture, "2:HIGH");

  g_assert_true (alarm_engine_dismiss (fixture->engine, 2));
  assert_log (fixture, "2:PREDISMISSED 3:SILENT");
  assert_next_alarm (fixture, "Tue 07:30 SILENT");
  assert_next_change (fixture, "Mon 2026-11-09 07:30:00");
  run_to_next_change (fixture);
  /* its going makes Tuesday's instance again, which takes the place of the one there */
  assert_log (fixture, "2:gone 3:SILENT");
  g_assert_null (alarm_engine_ringing (fixture->engine));
  g_assert_cmpuint (alarm_engine_get_store (fixture->engine)->instances->len, ==, 1);
}

/* the "Upcoming alarm" swiped away stays away until it is 30 min to go */
static void
test_hide_notification (Fixture *fixture, gconstpointer data)
{
  set_now (fixture, 2026, 11, 9, 6, 0, 0);
  add_alarm (fixture, 7, 30, 0);
  assert_log (fixture, "2:LOW");
  g_assert_true (alarm_engine_hide_notification (fixture->engine, 2));
  assert_log (fixture, "2:HIDE");
  /* after a reboot too */
  alarm_engine_fix (fixture->engine);
  assert_log (fixture, "2:HIDE");
  run_to_next_change (fixture);
  assert_log (fixture, "2:HIGH");
}

/* a second alarm going off while the first rings: the first is missed */
static void
test_one_rings_at_a_time (Fixture *fixture, gconstpointer data)
{
  set_now (fixture, 2026, 11, 9, 7, 29, 0);
  add_alarm (fixture, 7, 30, 0);
  add_alarm (fixture, 7, 32, 0);
  g_string_truncate (fixture->log, 0);
  run_to_next_change (fixture);
  assert_log (fixture, "2:FIRED");
  run_to_next_change (fixture);
  assert_log (fixture, "2:MISSED 4:FIRED");
  g_assert_cmpuint (alarm_engine_ringing (fixture->engine)->id, ==, 4);
}

/* the phone was off over the alarm: on start (fix), seconds late it still rings, a minute late it was missed */
static void
test_fire_buffer (Fixture *fixture, gconstpointer data)
{
  set_now (fixture, 2026, 11, 9, 7, 0, 0);
  add_alarm (fixture, 7, 30, 0);
  add_alarm (fixture, 7, 40, 0);
  g_string_truncate (fixture->log, 0);

  set_now (fixture, 2026, 11, 9, 7, 30, 10);
  alarm_engine_fix (fixture->engine);
  assert_log (fixture, "4:HIGH 2:FIRED");
  set_now (fixture, 2026, 11, 9, 7, 41, 0);
  alarm_engine_fix (fixture->engine);
  /* latest first: 4 is past its 15 s and missed; 2's auto-silence (7:40) is over too */
  assert_log (fixture, "4:MISSED 2:MISSED");
}

/* asleep past the alarm's time (a late wake): it still rings, unless it would already have rung out */
static void
test_late_wake (Fixture *fixture, gconstpointer data)
{
  set_now (fixture, 2026, 11, 9, 4, 0, 0);
  add_alarm (fixture, 7, 30, 0);
  add_alarm (fixture, 8, 30, 0);
  g_string_truncate (fixture->log, 0);

  set_now (fixture, 2026, 11, 9, 7, 32, 0);
  alarm_engine_tick (fixture->engine);
  assert_log (fixture, "2:LOW 2:HIGH 2:FIRED 4:LOW");
  set_now (fixture, 2026, 11, 9, 8, 45, 0);
  alarm_engine_tick (fixture->engine);
  assert_log (fixture, "2:MISSED 4:HIGH 4:MISSED");
}

/* the clock set back days: the daily alarm's instance, now days off, is made anew for the next 7:30 */
static void
test_time_set_back (Fixture *fixture, gconstpointer data)
{
  set_now (fixture, 2026, 11, 9, 8, 0, 0);
  add_alarm (fixture, 7, 30, ALARM_EVERY_DAY);
  assert_next_alarm (fixture, "Tue 07:30 SILENT");
  g_string_truncate (fixture->log, 0);

  set_now (fixture, 2026, 11, 6, 6, 0, 0);
  alarm_engine_fix (fixture->engine);
  assert_log (fixture, "2:gone 3:LOW");
  assert_next_alarm (fixture, "Fri 07:30 LOW");
}

/* "Silence after: Never": it rings until it is answered */
static void
test_never_silenced (Fixture *fixture, gconstpointer data)
{
  AlarmEngineSettings settings = { .snooze_minutes = 5, .auto_silence_minutes = -1 };

  alarm_engine_set_settings (fixture->engine, &settings);
  set_now (fixture, 2026, 11, 9, 7, 29, 0);
  add_alarm (fixture, 7, 30, 0);
  run_to_next_change (fixture);
  assert_next_change (fixture, "none");
  set_now (fixture, 2026, 11, 9, 11, 0, 0);
  alarm_engine_fix (fixture->engine);
  g_assert_nonnull (alarm_engine_ringing (fixture->engine));
}

/* one made by another app to be used once (delete_after_use) is gone once it rang */
static void
test_delete_after_use (Fixture *fixture, gconstpointer data)
{
  g_autoptr(Alarm) alarm = alarm_new ();
  AlarmStore *store = alarm_engine_get_store (fixture->engine);

  set_now (fixture, 2026, 11, 9, 7, 29, 0);
  alarm->hour = 7;
  alarm->minute = 30;
  alarm->enabled = TRUE;
  alarm->delete_after_use = TRUE;
  alarm_engine_add_alarm (fixture->engine, alarm);
  run_to_next_change (fixture);
  g_assert_true (alarm_engine_dismiss (fixture->engine, 2));
  g_assert_cmpuint (store->alarms->len, ==, 0);
  g_assert_cmpuint (store->instances->len, ==, 0);
}

/* changing or switching off an alarm makes its instance anew; a ringing one stops when its alarm is deleted */
static void
test_edits (Fixture *fixture, gconstpointer data)
{
  AlarmStore *store = alarm_engine_get_store (fixture->engine);
  g_autoptr(Alarm) changed = NULL;
  guint alarm_id;

  set_now (fixture, 2026, 11, 9, 7, 0, 0);
  alarm_id = add_alarm (fixture, 7, 30, 0);
  g_string_truncate (fixture->log, 0);

  changed = alarm_copy (alarm_store_find_alarm (store, alarm_id));
  changed->hour = 8;
  g_assert_true (alarm_engine_update_alarm (fixture->engine, changed));
  assert_log (fixture, "2:gone 3:LOW");
  changed->enabled = FALSE;
  alarm_engine_update_alarm (fixture->engine, changed);
  assert_log (fixture, "3:gone");
  assert_next_alarm (fixture, "none");

  changed->enabled = TRUE;
  changed->hour = 7;
  changed->minute = 1;
  alarm_engine_update_alarm (fixture->engine, changed);
  run_to_next_change (fixture);
  assert_log (fixture, "4:HIGH 4:FIRED");
  g_assert_true (alarm_engine_delete_alarm (fixture->engine, alarm_id));
  assert_log (fixture, "4:gone");
  g_assert_null (alarm_store_find_alarm (store, alarm_id));
}

int
main (int argc, char **argv)
{
  g_setenv ("TZ", "Europe/Berlin", TRUE);
  g_setenv ("LC_ALL", "C", TRUE);
  g_test_init (&argc, &argv, NULL);

#define ADD(path, func) g_test_add ("/alarm-engine/" path, Fixture, NULL, fixture_set_up, func, fixture_tear_down)
  ADD ("rings-out", test_rings_out);
  ADD ("snooze", test_snooze);
  ADD ("repeating", test_repeating);
  ADD ("dismiss-now", test_dismiss_now);
  ADD ("hide-notification", test_hide_notification);
  ADD ("one-rings-at-a-time", test_one_rings_at_a_time);
  ADD ("fire-buffer", test_fire_buffer);
  ADD ("late-wake", test_late_wake);
  ADD ("time-set-back", test_time_set_back);
  ADD ("never-silenced", test_never_silenced);
  ADD ("delete-after-use", test_delete_after_use);
  ADD ("edits", test_edits);
  return g_test_run ();
}
