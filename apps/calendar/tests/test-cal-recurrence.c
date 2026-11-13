/* The repeat dialog's rules (cal-recurrence-picker.c): the RRULE parser and writer, cal_recurrence_picker_can_handle,
 * and the round trip a rule makes through the dialog's model (parse → model → rule). Built with CAL_RECURRENCE_NO_UI:
 * the dialog itself is left out. */
#include "../src/cal-recurrence-picker.h"

#include <locale.h>

/* Tuesday, November 10 2026, 10:00 in Berlin (09:00 UTC) */
static gint64
start_at (const char *tzid, int year, int month, int day, int hour)
{
  g_autoptr(GTimeZone) zone = g_time_zone_new_identifier (tzid);
  g_autoptr(GDateTime) time = g_date_time_new (zone, year, month, day, hour, 0, 0);

  return g_date_time_to_unix (time);
}

static int
day_number (int year, int month, int day)
{
  GDate date;

  g_date_clear (&date, 1);
  g_date_set_dmy (&date, day, month, year);
  return (int) g_date_get_julian (&date);
}

static void
test_parse (void)
{
  struct { const char *rule, *written; } cases[] = {
    { "FREQ=WEEKLY;BYDAY=MO,WE", "FREQ=WEEKLY;WKST=MO;BYDAY=MO,WE" },
    { "freq=daily;interval=2", "FREQ=DAILY;INTERVAL=2;WKST=MO" },                    /* lower case allowed */
    { "FREQ=MONTHLY;BYDAY=-1FR;X-NAME=1", "FREQ=MONTHLY;WKST=MO;BYDAY=-1FR" },       /* X- parts dropped */
    { "BYMONTH=3;FREQ=YEARLY;BYHOUR=9", "FREQ=YEARLY;WKST=MO;BYHOUR=9;BYMONTH=3" },  /* the writer's order */
    { "FREQ=DAILY;INTERVAL=+2", "FREQ=DAILY;INTERVAL=2;WKST=MO" },
    { "FREQ=DAILY;COUNT=-3;INTERVAL=0", "FREQ=DAILY;COUNT=1;INTERVAL=1;WKST=MO" },   /* forced to 1 */
    { "FREQ=DAILY;;COUNT=3", "FREQ=DAILY;COUNT=3;WKST=MO" },
    { "FREQ=WEEKLY;WKST=SU;BYDAY= TU,TH ", "FREQ=WEEKLY;WKST=SU;BYDAY=TU,TH" },      /* BYDAY trimmed */
    { "FREQ=YEARLY;BYMONTHDAY=1,-1,", "FREQ=YEARLY;WKST=MO;BYMONTHDAY=1,-1" },
    { "FREQ=DAILY;UNTIL=20261231T225959Z;COUNT=3", "FREQ=DAILY;UNTIL=20261231T225959Z;COUNT=3;WKST=MO" },
    { "FREQ=MONTHLY;BYDAY=1SU,+2MO;BYSETPOS=-1,0", "FREQ=MONTHLY;WKST=MO;BYDAY=1SU,2MO;BYSETPOS=-1,0" },
  };
  const char *refused[] = {
    "", "BYDAY=MO", "FREQ=DAILY;FREQ=DAILY", "FREQ=FORTNIGHTLY", "FREQ=DAILY;FOO=1", "FREQ=DAILY;COUNT=",
    "FREQ=WEEKLY;BYDAY=XX", "FREQ=WEEKLY;BYDAY=0MO", "FREQ=WEEKLY;BYDAY=54MO", "FREQ=YEARLY;BYMONTH=13",
    "FREQ=DAILY;COUNT=1x", "FREQ=DAILY;=1", "FREQ=DAILY;COUNT", "FREQ=YEARLY;BYMONTHDAY=0", "FREQ=DAILY;BYHOUR=24",
    "FREQ=DAILY;COUNT=99999999999", "FREQ=WEEKLY;WKST=XY", "FREQ=YEARLY;BYMONTH=,3",
  };

  for (guint i = 0; i < G_N_ELEMENTS (cases); i++)
    {
      g_autoptr(CalRecurrence) er = cal_recurrence_parse (cases[i].rule);
      g_autofree char *written = NULL;

      g_assert_nonnull (er);
      written = cal_recurrence_to_string (er);
      g_assert_cmpstr (written, ==, cases[i].written);
    }
  g_assert_null (cal_recurrence_parse (NULL));
  for (guint i = 0; i < G_N_ELEMENTS (refused); i++)
    {
      g_autoptr(CalRecurrence) er = cal_recurrence_parse (refused[i]);

      if (er)
        g_error ("parsed %s", refused[i]);
    }
}

static void
test_can_handle (void)
{
  const char *handled[] = {
    "FREQ=DAILY", "FREQ=WEEKLY;BYDAY=MO,TU", "FREQ=MONTHLY;BYDAY=2TU", "FREQ=MONTHLY;BYDAY=5SU",
    "FREQ=MONTHLY;BYDAY=-1FR", "FREQ=MONTHLY;BYMONTHDAY=15", "FREQ=YEARLY", "FREQ=YEARLY;BYMONTH=3",
    "FREQ=WEEKLY;UNTIL=20261231T225959Z;BYDAY=TU", "FREQ=DAILY;COUNT=10;INTERVAL=3", "FREQ=DAILY;UNTIL=nonsense",
  };
  const char *refused[] = {
    "FREQ=HOURLY", "FREQ=SECONDLY", "FREQ=DAILY;COUNT=5;UNTIL=20261231",      /* not a frequency it has; both ends */
    "FREQ=MONTHLY;BYDAY=1MO,3MO", "FREQ=WEEKLY;BYDAY=1MO",                    /* two nths; an nth not monthly */
    "FREQ=MONTHLY;BYMONTHDAY=1,15", "FREQ=MONTHLY;BYDAY=2TU;BYMONTHDAY=3",    /* two days; nth and day */
    "FREQ=MONTHLY;BYDAY=MO,TU", "FREQ=MONTHLY;BYDAY=-2MO",
    /* the parser takes these, but the dialog's model refuses them (cal_recurrence_model_init) */
    "FREQ=MONTHLY;BYDAY=MO", "FREQ=MONTHLY;BYMONTH=1,2",
    "FREQ=WEEKLY;BYDAY=XX", "garbage",
  };

  g_assert_true (cal_recurrence_picker_can_handle (NULL));
  g_assert_true (cal_recurrence_picker_can_handle (""));
  for (guint i = 0; i < G_N_ELEMENTS (handled); i++)
    if (!cal_recurrence_picker_can_handle (handled[i]))
      g_error ("cannot handle %s", handled[i]);
  for (guint i = 0; i < G_N_ELEMENTS (refused); i++)
    if (cal_recurrence_picker_can_handle (refused[i]))
      g_error ("handles %s", refused[i]);
}

/* parse → model → rule */
static char *
round_trip (const char *rule, gint64 start, const char *tzid, CalRecurrenceModel *model_out)
{
  g_autoptr(CalRecurrence) er = cal_recurrence_parse (rule);
  CalRecurrenceModel model;

  g_assert_nonnull (er);
  g_assert_true (cal_recurrence_model_init (&model, er, start, tzid));
  if (model_out)
    *model_out = model;
  return cal_recurrence_model_store (&model, er, tzid);
}

static void
test_round_trip (void)
{
  gint64 tuesday = start_at ("Europe/Berlin", 2026, 11, 10, 10);
  struct { const char *rule, *tzid, *written; } cases[] = {
    { "FREQ=WEEKLY;BYDAY=TU,TH", "Europe/Berlin", "FREQ=WEEKLY;WKST=MO;BYDAY=TU,TH" },
    { "FREQ=WEEKLY;WKST=SU;BYDAY=SA,MO", "Europe/Berlin", "FREQ=WEEKLY;WKST=SU;BYDAY=MO,SA" },   /* Sunday first */
    { "FREQ=WEEKLY", "Europe/Berlin", "FREQ=WEEKLY;WKST=MO;BYDAY=TU" },             /* the start's weekday */
    { "FREQ=DAILY;INTERVAL=3;COUNT=10", "Europe/Berlin", "FREQ=DAILY;COUNT=10;INTERVAL=3;WKST=MO" },
    { "FREQ=DAILY;INTERVAL=1", "Europe/Berlin", "FREQ=DAILY;WKST=MO" },             /* every one: no INTERVAL */
    { "FREQ=MONTHLY;BYDAY=-1FR", "Europe/Berlin", "FREQ=MONTHLY;WKST=MO;BYDAY=-1FR" },
    { "FREQ=MONTHLY;BYDAY=2TU", "Europe/Berlin", "FREQ=MONTHLY;WKST=MO;BYDAY=2TU" },
    { "FREQ=MONTHLY;BYMONTHDAY=15", "Europe/Berlin", "FREQ=MONTHLY;WKST=MO;BYMONTHDAY=15" },
    { "FREQ=MONTHLY", "Europe/Berlin", "FREQ=MONTHLY;WKST=MO" },
    { "FREQ=YEARLY;BYMONTH=3;BYSETPOS=1", "Europe/Berlin", "FREQ=YEARLY;WKST=MO;BYMONTH=3;BYSETPOS=1" },  /* kept */
    /* BYDAY and BYMONTHDAY are rewritten from what the dialog shows: a yearly or daily rule loses its own, a
     * monthly one its BYMONTHDAY=-1 */
    { "FREQ=YEARLY;BYMONTH=3;BYMONTHDAY=8", "Europe/Berlin", "FREQ=YEARLY;WKST=MO;BYMONTH=3" },
    { "FREQ=DAILY;BYDAY=MO,FR", "Europe/Berlin", "FREQ=DAILY;WKST=MO" },
    { "FREQ=MONTHLY;BYMONTHDAY=-1", "Europe/Berlin", "FREQ=MONTHLY;WKST=MO" },
    /* the end date in UTC comes back as it was … */
    { "FREQ=WEEKLY;UNTIL=20261231T225959Z;BYDAY=TU", "Europe/Berlin",
      "FREQ=WEEKLY;UNTIL=20261231T225959Z;WKST=MO;BYDAY=TU" },
    /* … a floating one is in the event's zone, a date at the start's time of day … */
    { "FREQ=DAILY;UNTIL=20261231T120000", "America/New_York", "FREQ=DAILY;UNTIL=20261231T170000Z;WKST=MO" },
    { "FREQ=DAILY;UNTIL=20261231", "Europe/Berlin", "FREQ=DAILY;UNTIL=20261231T090000Z;WKST=MO" },
    /* … one that does not parse is the default end: a month after the start */
    { "FREQ=DAILY;UNTIL=nonsense", "Europe/Berlin", "FREQ=DAILY;UNTIL=20261210T090000Z;WKST=MO" },
    /* an all-day event (no zone): a DATE */
    { "FREQ=DAILY;UNTIL=20261231", NULL, "FREQ=DAILY;UNTIL=20261231;WKST=MO" },
    { "FREQ=DAILY;UNTIL=20261231T225959Z", NULL, "FREQ=DAILY;UNTIL=20261231;WKST=MO" },
  };

  for (guint i = 0; i < G_N_ELEMENTS (cases); i++)
    {
      gint64 start = cases[i].tzid ? tuesday : start_at ("Europe/Berlin", 2026, 11, 10, 0);
      g_autofree char *written = round_trip (cases[i].rule, start, cases[i].tzid, NULL);

      g_assert_cmpstr (written, ==, cases[i].written);
    }
}

static void
test_model (void)
{
  gint64 tuesday = start_at ("Europe/Berlin", 2026, 11, 10, 10);
  CalRecurrenceModel model;
  g_autofree char *rule = NULL;

  /* the rule into the model */
  g_free (round_trip ("FREQ=DAILY;INTERVAL=3;COUNT=10", tuesday, "Europe/Berlin", &model));
  g_assert_true (model.repeats);
  g_assert_cmpint (model.freq, ==, CAL_REPEAT_DAILY);
  g_assert_cmpint (model.interval, ==, 3);
  g_assert_cmpint (model.end, ==, CAL_REPEAT_END_BY_COUNT);
  g_assert_cmpint (model.end_count, ==, 10);
  g_assert_true (model.weekly_by_day_of_week[2]);           /* the start's Tuesday, for a switch to weekly */

  g_free (round_trip ("FREQ=MONTHLY;BYDAY=-1FR", tuesday, "Europe/Berlin", &model));
  g_assert_cmpint (model.monthly_repeat, ==, CAL_REPEAT_MONTHLY_BY_NTH_DAY_OF_WEEK);
  g_assert_cmpint (model.monthly_by_day_of_week, ==, 5);
  g_assert_cmpint (model.monthly_by_nth_day_of_week, ==, -1);
  g_assert_false (model.weekly_by_day_of_week[2]);          /* BYDAY given: only its days */
  g_assert_true (model.weekly_by_day_of_week[5]);

  g_free (round_trip ("FREQ=WEEKLY;UNTIL=20261231T225959Z;BYDAY=TU", tuesday, "Europe/Berlin", &model));
  g_assert_cmpint (model.end, ==, CAL_REPEAT_END_BY_DATE);
  g_assert_cmpint (model.end_day, ==, day_number (2026, 12, 31));
  g_assert_cmpint (model.end_second, ==, 23 * 3600 + 59 * 60 + 59);
  /* the model without a COUNT holds 0 ("For a number of events" then makes it 1) */
  g_assert_cmpint (model.end_count, ==, 0);

  /* a picked date keeps the time of day */
  {
    g_autoptr(CalRecurrence) er = cal_recurrence_parse ("FREQ=WEEKLY;UNTIL=20261231T225959Z;BYDAY=TU");

    g_assert_true (cal_recurrence_model_init (&model, er, tuesday, "Europe/Berlin"));
    model.end_day = day_number (2027, 1, 15);
    rule = cal_recurrence_model_store (&model, er, "Europe/Berlin");
    g_assert_cmpstr (rule, ==, "FREQ=WEEKLY;UNTIL=20270115T225959Z;WKST=MO;BYDAY=TU");
    g_clear_pointer (&rule, g_free);
  }

  /* turned off: no rule */
  {
    g_autoptr(CalRecurrence) er = cal_recurrence_parse ("FREQ=DAILY");

    g_assert_true (cal_recurrence_model_init (&model, er, tuesday, "Europe/Berlin"));
    model.repeats = FALSE;
    g_assert_null (cal_recurrence_model_store (&model, er, "Europe/Berlin"));
  }
}

/* a new rule: the dialog's defaults, from the start */
static void
test_new_rule (void)
{
  gint64 tuesday = start_at ("Europe/Berlin", 2026, 11, 10, 10);
  CalRecurrenceModel model, initial;
  g_autoptr(CalRecurrence) er = cal_recurrence_new (0);       /* a week starting on Sunday */
  char *rule;

  g_assert_true (cal_recurrence_model_init (&model, NULL, tuesday, "Europe/Berlin"));
  initial = model;
  g_assert_false (model.repeats);
  g_assert_cmpint (model.freq, ==, CAL_REPEAT_WEEKLY);
  g_assert_cmpint (model.interval, ==, 1);
  g_assert_cmpint (model.end, ==, CAL_REPEAT_END_NEVER);
  g_assert_cmpint (model.end_count, ==, CAL_REPEAT_COUNT_DEFAULT);
  g_assert_cmpint (model.end_day, ==, day_number (2026, 12, 10));  /* a month on */
  g_assert_cmpint (model.end_second, ==, 10 * 3600);
  g_assert_cmpint (model.monthly_by_nth_day_of_week, ==, 2);       /* the second Tuesday */
  g_assert_cmpint (model.monthly_by_day_of_week, ==, 2);
  g_assert_null (cal_recurrence_model_store (&model, er, "Europe/Berlin"));
  g_assert_true (cal_recurrence_model_equal (&model, &initial));

  model.repeats = TRUE;
  g_assert_false (cal_recurrence_model_equal (&model, &initial));
  rule = cal_recurrence_model_store (&model, er, "Europe/Berlin");
  g_assert_cmpstr (rule, ==, "FREQ=WEEKLY;WKST=SU;BYDAY=TU");
  g_free (rule);

  model.weekly_by_day_of_week[4] = TRUE;
  model.interval = 2;
  model.end = CAL_REPEAT_END_BY_COUNT;
  rule = cal_recurrence_model_store (&model, er, "Europe/Berlin");
  g_assert_cmpstr (rule, ==, "FREQ=WEEKLY;COUNT=5;INTERVAL=2;WKST=SU;BYDAY=TU,TH");
  g_free (rule);

  model.freq = CAL_REPEAT_MONTHLY;
  model.monthly_repeat = CAL_REPEAT_MONTHLY_BY_NTH_DAY_OF_WEEK;
  model.end = CAL_REPEAT_END_BY_DATE;
  model.interval = 1;
  rule = cal_recurrence_model_store (&model, er, "Europe/Berlin");
  g_assert_cmpstr (rule, ==, "FREQ=MONTHLY;UNTIL=20261210T090000Z;WKST=SU;BYDAY=2TU");
  g_free (rule);

  model.monthly_repeat = CAL_REPEAT_MONTHLY_BY_DATE;
  model.end = CAL_REPEAT_END_NEVER;
  rule = cal_recurrence_model_store (&model, er, "Europe/Berlin");
  g_assert_cmpstr (rule, ==, "FREQ=MONTHLY;WKST=SU");
  g_free (rule);

  model.freq = CAL_REPEAT_YEARLY;
  rule = cal_recurrence_model_store (&model, er, "Europe/Berlin");
  g_assert_cmpstr (rule, ==, "FREQ=YEARLY;WKST=SU");
  g_free (rule);

  /* the fifth Monday is the last one */
  g_assert_true (cal_recurrence_model_init (&model, NULL, start_at ("Europe/Berlin", 2026, 11, 30, 9),
                                            "Europe/Berlin"));
  g_assert_cmpint (model.monthly_by_nth_day_of_week, ==, -1);
  g_assert_cmpint (model.monthly_by_day_of_week, ==, 1);

  /* an all-day event: the end a DATE */
  g_assert_true (cal_recurrence_model_init (&model, NULL, start_at ("Europe/Berlin", 2026, 11, 10, 0), NULL));
  model.repeats = TRUE;
  model.freq = CAL_REPEAT_DAILY;
  model.end = CAL_REPEAT_END_BY_DATE;
  rule = cal_recurrence_model_store (&model, er, NULL);
  g_assert_cmpstr (rule, ==, "FREQ=DAILY;UNTIL=20261210;WKST=SU");
  g_free (rule);
}

/* the default end date: a month on (daily, weekly), three (monthly), three years (yearly); past a month's end it runs
 * into the next */
static void
test_default_end (void)
{
  struct { const char *rule; int y, m, d, ey, em, ed; } cases[] = {
    { "FREQ=WEEKLY", 2027, 1, 31, 2027, 3, 3 },
    { "FREQ=DAILY", 2026, 12, 15, 2027, 1, 15 },
    { "FREQ=MONTHLY", 2026, 11, 30, 2027, 3, 2 },
    { "FREQ=YEARLY", 2028, 2, 29, 2031, 3, 1 },
  };

  for (guint i = 0; i < G_N_ELEMENTS (cases); i++)
    {
      g_autoptr(CalRecurrence) er = cal_recurrence_parse (cases[i].rule);
      CalRecurrenceModel model;

      g_assert_true (cal_recurrence_model_init (&model, er,
                                                start_at ("Europe/Berlin", cases[i].y, cases[i].m, cases[i].d, 8),
                                                "Europe/Berlin"));
      g_assert_cmpint (model.end_day, ==, day_number (cases[i].ey, cases[i].em, cases[i].ed));
    }
}

/* All day switched under a rule: its UNTIL becomes the start's kind */
static void
test_fit_until (void)
{
  /* 10:00 Berlin on 9 November 2026 */
  g_autoptr(GDateTime) start = g_date_time_new_local (2026, 11, 9, 10, 0, 0);
  gint64 t = g_date_time_to_unix (start);
  g_autofree char *to_date = cal_recurrence_fit_until ("FREQ=DAILY;UNTIL=20261130T235959Z;INTERVAL=2", t, NULL);
  g_autofree char *to_time = cal_recurrence_fit_until ("FREQ=DAILY;UNTIL=20261130", t, "Europe/Berlin");
  g_autofree char *same = cal_recurrence_fit_until ("FREQ=DAILY;UNTIL=20261130", t, NULL);
  g_autofree char *none = cal_recurrence_fit_until ("FREQ=WEEKLY;COUNT=3", t, "Europe/Berlin");

  /* 23:59:59 UTC is 00:59:59 on 1 December in Berlin */
  g_assert_cmpstr (to_date, ==, "FREQ=DAILY;UNTIL=20261201;INTERVAL=2");
  /* the end date at the start's 10:00 Berlin, in UTC */
  g_assert_cmpstr (to_time, ==, "FREQ=DAILY;UNTIL=20261130T090000Z");
  g_assert_cmpstr (same, ==, "FREQ=DAILY;UNTIL=20261130");
  g_assert_cmpstr (none, ==, "FREQ=WEEKLY;COUNT=3");
  g_assert_null (cal_recurrence_fit_until (NULL, t, NULL));
}

int
main (int argc, char **argv)
{
  g_setenv ("TZ", "Europe/Berlin", TRUE);
  setlocale (LC_ALL, "C.UTF-8");
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/recurrence/parse", test_parse);
  g_test_add_func ("/recurrence/can-handle", test_can_handle);
  g_test_add_func ("/recurrence/round-trip", test_round_trip);
  g_test_add_func ("/recurrence/model", test_model);
  g_test_add_func ("/recurrence/new-rule", test_new_rule);
  g_test_add_func ("/recurrence/default-end", test_default_end);
  g_test_add_func ("/recurrence/fit-until", test_fit_until);
  return g_test_run ();
}
