/* The events' rules (cal-model.c). */
#include "../src/cal-model.h"

#include <stdlib.h>

static void
test_display_color (void)
{
  /* Utils.getDisplayColorFromColor, checked on the still: #16A765 is drawn #008649 */
  g_assert_cmphex (cal_display_color (0x16a765, FALSE), ==, 0x008649);
  g_assert_cmphex (cal_display_color (0x16a765, TRUE), ==, 0x16a765);
}

static void
test_days (void)
{
  g_autoptr(GDateTime) noon = g_date_time_new_local (2026, 11, 9, 12, 0, 0);
  int day = cal_day_number (g_date_time_to_unix (noon));
  g_autoptr(CalEvent) all_day = cal_event_new ();

  g_assert_cmpint (cal_day_of_week (day), ==, 1);       /* a Monday */
  g_assert_cmpint (cal_minute_of_day (g_date_time_to_unix (noon)), ==, 720);
  g_assert_cmpint (cal_day_number (cal_day_start (day)), ==, day);
  /* an all-day event's end is exclusive: Nov 9 to Nov 11 covers the 9th and 10th */
  all_day->all_day = TRUE;
  all_day->start = cal_day_start (day);
  all_day->end = cal_day_start (day + 2);
  g_assert_cmpint (cal_event_first_day (all_day), ==, day);
  g_assert_cmpint (cal_event_last_day (all_day), ==, day + 1);
  g_assert_true (cal_event_draws_as_all_day (all_day));
}

static void
test_layout (void)
{
  g_autoptr(GPtrArray) events = g_ptr_array_new_with_free_func ((GDestroyNotify) cal_event_unref);
  gint64 base = 1800000000;
  const gint64 spans[][2] = { { 0, 7200 }, { 0, 3600 }, { 1800, 2700 }, { 10800, 14400 } };

  for (guint i = 0; i < G_N_ELEMENTS (spans); i++)
    {
      CalEvent *e = cal_event_new ();

      e->start = base + spans[i][0];
      e->end = base + spans[i][1];
      g_ptr_array_add (events, e);
    }
  g_ptr_array_sort (events, cal_event_compare);
  cal_layout_events (events, FALSE, 900);
  /* the three overlapping ones share three columns; the later one has the day to itself */
  g_assert_cmpint (((CalEvent *) events->pdata[0])->max_columns, ==, 3);
  g_assert_cmpint (((CalEvent *) events->pdata[2])->column, ==, 2);
  g_assert_cmpint (((CalEvent *) events->pdata[3])->max_columns, ==, 1);
}

int
main (int argc, char **argv)
{
  g_setenv ("TZ", "Europe/Berlin", TRUE);
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/model/display-color", test_display_color);
  g_test_add_func ("/model/days", test_days);
  g_test_add_func ("/model/layout", test_layout);
  return g_test_run ();
}
