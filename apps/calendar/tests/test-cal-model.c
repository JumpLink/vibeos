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

static GPtrArray *
events_of (const gint64 (*spans)[2], guint n)
{
  GPtrArray *events = g_ptr_array_new_with_free_func ((GDestroyNotify) cal_event_unref);

  for (guint i = 0; i < n; i++)
    {
      CalEvent *e = cal_event_new ();

      e->start = 1800000000 + spans[i][0];
      e->end = 1800000000 + spans[i][1];
      g_ptr_array_add (events, e);
    }
  g_ptr_array_sort (events, cal_event_compare);
  return events;
}

#define EV(i) ((CalEvent *) events->pdata[i])

static void
test_layout_staggered (void)
{
  const gint64 plain[][2] = { { 0, 7200 }, { 0, 3600 }, { 1800, 2700 } };
  const gint64 insert[][2] = { { 0, 14400 }, { 1800, 3600 }, { 2400, 7200 } };
  const gint64 hidden[][2] = { { 0, 14400 }, { 0, 3600 }, { 600, 3600 } };
  g_autoptr(GPtrArray) events = events_of (plain, G_N_ELEMENTS (plain));

  /* each in the next column; the long one's title shows below the hour the second covers */
  cal_layout_events_staggered (events, 900);
  g_assert_true (EV (0)->staggered);
  g_assert_cmpint (EV (0)->column, ==, 0);
  g_assert_cmpint (EV (0)->text_offset, ==, 3600);
  g_assert_cmpint (EV (1)->column, ==, 1);
  g_assert_cmpint (EV (1)->text_offset, ==, 0);
  g_assert_cmpint (EV (2)->column, ==, 2);
  g_assert_cmpint (EV (2)->max_columns, ==, 3);

  /* the third would cover the second wholly: it goes under it, the second moves up, and its title shows below
   * the second's end */
  g_ptr_array_unref (events);
  events = events_of (insert, G_N_ELEMENTS (insert));
  cal_layout_events_staggered (events, 900);
  g_assert_cmpint (EV (0)->column, ==, 0);
  g_assert_cmpint (EV (2)->column, ==, 1);
  g_assert_cmpint (EV (2)->text_offset, ==, 1200);
  g_assert_cmpint (EV (1)->column, ==, 2);
  g_assert_cmpint (EV (1)->max_columns, ==, 3);
  g_assert_true (EV (1)->staggered);

  /* the third would be hidden in any column: the group falls back to columns side by side */
  g_ptr_array_unref (events);
  events = events_of (hidden, G_N_ELEMENTS (hidden));
  cal_layout_events_staggered (events, 900);
  g_assert_false (EV (2)->staggered);
  g_assert_cmpint (EV (2)->column, ==, 2);
  g_assert_cmpint (EV (2)->max_columns, ==, 3);
  g_assert_cmpint (EV (0)->text_offset, ==, 0);
}

static void
add_guest (CalEvent *event, const char *email, CalStatus status)
{
  CalAttendee *guest = g_new0 (CalAttendee, 1);

  guest->email = g_strdup (email);
  guest->status = status;
  g_ptr_array_add (event->attendees, guest);
}

static void
test_mail_uri (void)
{
  g_autoptr(CalEvent) event = cal_event_new ();
  g_autofree char *none = NULL, *guests = NULL, *organizer = NULL;

  event->title = g_strdup ("Lunch & talk");
  none = cal_event_mail_uri (event, "me@x.org", NULL);
  g_assert_null (none);
  /* the owner left out, the declined guest on cc, the rest to; "Re: " and the body escaped */
  add_guest (event, "me@x.org", CAL_STATUS_ACCEPTED);
  add_guest (event, "a@x.org", CAL_STATUS_ACCEPTED);
  add_guest (event, "b@x.org", CAL_STATUS_INVITED);
  add_guest (event, "c@x.org", CAL_STATUS_DECLINED);
  add_guest (event, "room@resource.calendar.google.com", CAL_STATUS_ACCEPTED);
  guests = cal_event_mail_uri (event, "me@x.org", "Be there soon");
  g_assert_cmpstr (guests, ==, "mailto:a@x.org?to=b%40x.org&subject=Re%3A%20Lunch%20%26%20talk&body=Be%20there%20soon"
                               "&cc=c%40x.org");
  /* only the owner: the organizer instead */
  g_ptr_array_set_size (event->attendees, 1);
  event->organizer = g_strdup ("boss@x.org");
  organizer = cal_event_mail_uri (event, "me@x.org", NULL);
  g_assert_cmpstr (organizer, ==, "mailto:boss@x.org?subject=Re%3A%20Lunch%20%26%20talk");
}

int
main (int argc, char **argv)
{
  g_setenv ("TZ", "Europe/Berlin", TRUE);
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/model/display-color", test_display_color);
  g_test_add_func ("/model/days", test_days);
  g_test_add_func ("/model/layout", test_layout);
  g_test_add_func ("/model/layout-staggered", test_layout_staggered);
  g_test_add_func ("/model/mail-uri", test_mail_uri);
  return g_test_run ();
}
