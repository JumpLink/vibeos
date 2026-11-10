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

static void
test_skipped (void)
{
  g_autoptr(CalEvent) own = cal_event_new (), invited = cal_event_new ();
  CalAttendee *me = g_new0 (CalAttendee, 1);

  /* an event of one's own: never skipped */
  g_assert_false (cal_alert_skipped (own, ""));
  g_assert_false (cal_alert_skipped (own, "not_responded"));
  /* declined: always skipped, whatever the setting */
  own->self_status = CAL_STATUS_DECLINED;
  g_assert_true (cal_alert_skipped (own, ""));
  g_assert_true (cal_alert_skipped (own, "declined"));
  /* an invitation not answered: skipped only "If declined or not responded" */
  me->email = g_strdup ("me@x.org");
  me->status = CAL_STATUS_INVITED;
  g_ptr_array_add (invited->attendees, me);
  invited->organizer = g_strdup ("boss@x.org");
  invited->self_status = CAL_STATUS_INVITED;
  g_assert_false (cal_alert_skipped (invited, "declined"));
  g_assert_true (cal_alert_skipped (invited, "not_responded"));
  invited->self_status = CAL_STATUS_TENTATIVE;
  g_assert_false (cal_alert_skipped (invited, "not_responded"));
}

static void
links (const char *location, const char *region, const char *geo, const char *tel)
{
  g_autofree char *g = NULL, *t = NULL;

  cal_alert_location_links (location, region, &g, &t);
  g_assert_cmpstr (g, ==, geo);
  g_assert_cmpstr (t, ==, tel);
}

static void
test_links (void)
{
  /* outside the US: a place is looked up as a whole */
  links ("Café Mitte", "DE", "geo:0,0?q=Caf%C3%A9%20Mitte", NULL);
  links ("  Hauptstraße 5, 10115 Berlin ", "DE", "geo:0,0?q=Hauptstra%C3%9Fe%205%2C%2010115%20Berlin", NULL);
  /* one phone number from end to end: Call, no Map */
  links ("+49 (30) 123 45-67", "DE", NULL, "tel:+49301234567");
  links ("030/1234567", "DE", NULL, "tel:0301234567");
  /* a number among other words: the map gets it all, no Call */
  links ("Call 030 1234567 first", "DE", "geo:0,0?q=Call%20030%201234567%20first", NULL);
  /* one web address: neither */
  links ("https://meet.example.org/planning", "DE", NULL, NULL);
  links ("meet.example.org/planning", "DE", NULL, NULL);
  links ("", "DE", NULL, NULL);
  links (NULL, "DE", NULL, NULL);
  /* the US: numbers found among words, coordinates, the map for plain text */
  links ("Dial (650) 555-1212 now", "US", NULL, "tel:6505551212");
  links ("1-650-555-1212", "US", NULL, "tel:16505551212");
  links ("1600 Amphitheatre Parkway, 94043", "US", "geo:0,0?q=1600%20Amphitheatre%20Parkway%2C%2094043", NULL);
  links ("37.422, -122.084", "US", "geo:0,0?q=37.422%2C%20-122.084", NULL);
  links ("see www.example.com", "US", NULL, NULL);
}

static void
test_actions (void)
{
  CalAlertAction a[CAL_ALERT_MAX_ACTIONS];

  /* all four: Snooze is the one left off */
  g_assert_cmpint (cal_alert_actions (TRUE, TRUE, TRUE, TRUE, a), ==, 3);
  g_assert_cmpint (a[0], ==, CAL_ALERT_ACTION_MAP);
  g_assert_cmpint (a[1], ==, CAL_ALERT_ACTION_CALL);
  g_assert_cmpint (a[2], ==, CAL_ALERT_ACTION_EMAIL);
  g_assert_cmpint (cal_alert_actions (TRUE, FALSE, TRUE, TRUE, a), ==, 3);
  g_assert_cmpint (a[2], ==, CAL_ALERT_ACTION_SNOOZE);
  g_assert_cmpint (cal_alert_actions (FALSE, FALSE, FALSE, TRUE, a), ==, 1);
  g_assert_cmpint (a[0], ==, CAL_ALERT_ACTION_SNOOZE);
  g_assert_cmpint (cal_alert_actions (FALSE, FALSE, FALSE, FALSE, a), ==, 0);
}

static gint64
local (int y, int m, int d, int h, int min)
{
  g_autoptr(GDateTime) t = g_date_time_new_local (y, m, d, h, min, 0);

  return g_date_time_to_unix (t);
}

static void
test_when (void)
{
  gint64 now = local (2026, 11, 10, 8, 0);
  g_autofree char *a = cal_alert_when (local (2026, 11, 8, 0, 0), local (2026, 11, 9, 0, 0), TRUE, now);
  g_autofree char *b = cal_alert_when (local (2026, 11, 8, 0, 0), local (2026, 11, 11, 0, 0), TRUE, now);
  g_autofree char *c = NULL, *d = NULL, *e = NULL;

  g_assert_cmpstr (a, ==, "Sunday, November 8");
  g_assert_cmpstr (b, ==, "Sunday, November 8 – Tuesday, November 10");
  c = cal_alert_when (local (2026, 11, 8, 10, 0), local (2026, 11, 8, 11, 0), FALSE, now);
  g_assert_true (g_str_has_prefix (c, "November 8, "));
  d = cal_alert_when (local (2026, 11, 8, 22, 0), local (2026, 11, 9, 1, 0), FALSE, now);
  g_assert_true (g_str_has_prefix (d, "November 8, "));
  g_assert_nonnull (strstr (d, "– November 9, "));
  /* another year: the year shown */
  e = cal_alert_when (local (2027, 1, 3, 0, 0), local (2027, 1, 4, 0, 0), TRUE, now);
  g_assert_cmpstr (e, ==, "Sunday, January 3, 2027");
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/alerts/due-and-next", test_due_and_next);
  g_test_add_func ("/alerts/methods-and-keys", test_methods_and_keys);
  g_test_add_func ("/alerts/skipped", test_skipped);
  g_test_add_func ("/alerts/links", test_links);
  g_test_add_func ("/alerts/actions", test_actions);
  g_test_add_func ("/alerts/when", test_when);
  return g_test_run ();
}
