/* The dates and times as text (cal-format.c), in the C locale's English. */
#include "../src/cal-format.h"
#include "../src/cal-model.h"

#include <locale.h>

static void
test_rrule (void)
{
  g_autoptr(GDateTime) monday = g_date_time_new_local (2026, 11, 9, 10, 0, 0);
  gint64 start = g_date_time_to_unix (monday);
  struct { const char *rule, *text; } cases[] = {
    { "FREQ=DAILY", "Daily" },
    { "FREQ=DAILY;INTERVAL=3", "Every 3 days" },
    { "FREQ=WEEKLY;BYDAY=MO", "Weekly on Monday" },
    { "FREQ=WEEKLY;BYDAY=MO,TU,WE,TH,FR", "Every weekday (Mon–Fri)" },
    { "FREQ=YEARLY", "Yearly" },
    { "FREQ=DAILY;COUNT=5", "Daily; for 5 times" },
  };

  for (guint i = 0; i < G_N_ELEMENTS (cases); i++)
    {
      g_autofree char *text = cal_format_rrule (cases[i].rule, start);

      g_assert_cmpstr (text, ==, cases[i].text);
    }
}

static void
test_reminder_minutes (void)
{
  struct { int minutes; const char *text; } cases[] = {
    { 0, "0 minutes" }, { 1, "1 minute" }, { 90, "90 minutes" }, { 60, "1 hour" }, { 1440, "1 day" },
    { 2880, "2 days" }, { 10080, "1 week" },
  };

  for (guint i = 0; i < G_N_ELEMENTS (cases); i++)
    {
      g_autofree char *text = cal_format_reminder_minutes (cases[i].minutes);

      g_assert_cmpstr (text, ==, cases[i].text);
    }
}

static void
test_week_start (void)
{
  g_assert_cmpint (cal_week_start (1), ==, 0);   /* Sunday */
  g_assert_cmpint (cal_week_start (2), ==, 1);   /* Monday */
  g_assert_cmpint (cal_week_start (7), ==, 6);   /* Saturday */
}

int
main (int argc, char **argv)
{
  g_setenv ("TZ", "Europe/Berlin", TRUE);
  setlocale (LC_ALL, "C.UTF-8");
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/format/rrule", test_rrule);
  g_test_add_func ("/format/reminder-minutes", test_reminder_minutes);
  g_test_add_func ("/format/week-start", test_week_start);
  return g_test_run ();
}
