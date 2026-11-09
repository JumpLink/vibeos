/* When reminders are due (cal-alerts.c): Etar's AlertService rules. */
#include "../src/cal-alerts.h"

static CalEvent *
event (const char *uid, gint64 start, gint64 end, const int *minutes, int n)
{
  CalEvent *e = cal_event_new ();

  e->calendar = g_strdup ("cal");
  e->uid = g_strdup (uid);
  e->start = start;
  e->end = end;
  for (int i = 0; i < n; i++)
    {
      CalReminder r = { minutes[i], CAL_METHOD_NOTIFICATION };

      g_array_append_val (e->reminders, r);
    }
  return e;
}

static void
test_due_and_next (void)
{
  g_autoptr(GPtrArray) events = g_ptr_array_new_with_free_func ((GDestroyNotify) cal_event_unref);
  g_autoptr(GHashTable) fired = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  g_autoptr(GPtrArray) due = g_ptr_array_new_with_free_func ((GDestroyNotify) cal_alert_free);
  const int ten[] = { 10 }, two[] = { 30, 10 }, none[] = { 0 };
  gint64 now = 1800000000, next;

  /* due 5 min ago, event not started: due */
  g_ptr_array_add (events, event ("a", now + 300, now + 3900, ten, 1));
  /* due in 20 min: the next */
  g_ptr_array_add (events, event ("b", now + 1800, now + 5400, ten, 1));
  /* missed and over: dropped */
  g_ptr_array_add (events, event ("c", now - 7200, now - 3600, ten, 1));
  /* two reminders both past, still going: one alert, for the later one, both keys */
  g_ptr_array_add (events, event ("d", now + 60, now + 3600, two, 2));
  /* at start, started a minute ago, still going: due (missed) */
  g_ptr_array_add (events, event ("e", now - 60, now + 600, none, 1));

  cal_alerts_compute (events, fired, now, due, &next);
  g_assert_cmpint (next, ==, now + 1800 - 600);
  g_assert_cmpuint (due->len, ==, 3);
  {
    CalAlert *d = NULL;

    for (guint i = 0; i < due->len; i++)
      if (g_str_equal (((CalAlert *) due->pdata[i])->event->uid, "d"))
        d = due->pdata[i];
    g_assert_nonnull (d);
    g_assert_cmpint (d->minutes, ==, 10);
    g_assert_cmpuint (d->keys->len, ==, 2);
    for (guint k = 0; k < d->keys->len; k++)
      g_hash_table_add (fired, g_strdup (d->keys->pdata[k]));
  }
  /* once handled, not again */
  for (guint i = 0; i < due->len; i++)
    {
      CalAlert *a = due->pdata[i];

      for (guint k = 0; k < a->keys->len; k++)
        g_hash_table_add (fired, g_strdup (a->keys->pdata[k]));
    }
  g_ptr_array_set_size (due, 0);
  cal_alerts_compute (events, fired, now, due, &next);
  g_assert_cmpuint (due->len, ==, 0);
}

static void
test_methods_and_keys (void)
{
  g_autoptr(CalEvent) a = event ("x", 1000, 2000, NULL, 0);
  g_autoptr(CalEvent) b = event ("x", 1000 + 86400, 2000 + 86400, NULL, 0);
  CalReminder email = { 10, CAL_METHOD_EMAIL }, alarm = { 10, CAL_METHOD_ALARM };
  g_autofree char *ka = cal_alert_key (a, 10), *kb = cal_alert_key (b, 10), *ka5 = cal_alert_key (a, 5);

  g_assert_false (cal_alert_notifies (&email));
  g_assert_true (cal_alert_notifies (&alarm));
  /* each occurrence and each reminder its own key */
  g_assert_cmpstr (ka, !=, kb);
  g_assert_cmpstr (ka, !=, ka5);
  g_assert_cmpuint (strlen (ka), ==, 16);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/alerts/due-and-next", test_due_and_next);
  g_test_add_func ("/alerts/methods-and-keys", test_methods_and_keys);
  return g_test_run ();
}
