/* A conversation's layout (msg-thread.h): the date lines, the runs and their gaps, the time under the newest message,
 * and the date line's words. Run in UTC (TZ in meson.build), so the days fall where the test expects. */
#include "../src/msg-thread.h"

#define MINUTE (60 * (gint64) 1000)
#define HOUR (60 * MINUTE)
#define DAY (24 * HOUR)

/* 2026-10-29 14:45 UTC, a Thursday */
#define T0 ((gint64) 1793285100 * 1000)

static void
test_runs (void)
{
  const MsgThreadItem items[] = {
    { T0, FALSE, "+15550100", FALSE },
    { T0 + MINUTE, FALSE, "+15550100", FALSE },
    { T0 + 2 * MINUTE, FALSE, "+15550100", FALSE },
    { T0 + 3 * MINUTE, TRUE, NULL, TRUE },
    { T0 + 4 * MINUTE, TRUE, NULL, FALSE },
    { T0 + 5 * MINUTE, FALSE, "+15550100", FALSE },
  };
  MsgThreadPlace places[G_N_ELEMENTS (items)];

  msg_thread_layout (items, G_N_ELEMENTS (items), places);
  g_assert_true (places[0].separator);
  for (guint i = 1; i < G_N_ELEMENTS (items); i++)
    g_assert_false (places[i].separator);
  g_assert_cmpint (places[0].position, ==, MSG_RUN_FIRST);
  g_assert_cmpint (places[1].position, ==, MSG_RUN_MIDDLE);
  g_assert_cmpint (places[2].position, ==, MSG_RUN_LAST);
  /* a failed message stays in its run */
  g_assert_cmpint (places[3].position, ==, MSG_RUN_FIRST);
  g_assert_cmpint (places[4].position, ==, MSG_RUN_LAST);
  g_assert_cmpint (places[5].position, ==, MSG_RUN_SINGLE);
  /* the gaps: none at the top, 2 in a run, 16 between runs */
  g_assert_cmpint (msg_thread_gap_before (places, 0), ==, 0);
  g_assert_cmpint (msg_thread_gap_before (places, 1), ==, 2);
  g_assert_cmpint (msg_thread_gap_before (places, 2), ==, 2);
  g_assert_cmpint (msg_thread_gap_before (places, 3), ==, 16);
  g_assert_cmpint (msg_thread_gap_before (places, 4), ==, 2);
  g_assert_cmpint (msg_thread_gap_before (places, 5), ==, 16);
  /* only the newest has its time under it */
  for (guint i = 0; i + 1 < G_N_ELEMENTS (items); i++)
    g_assert_false (places[i].time_under);
  g_assert_true (places[5].time_under);
}

static void
test_separators (void)
{
  const MsgThreadItem items[] = {
    { T0, FALSE, "+15550100", FALSE },
    { T0 + HOUR - 1, FALSE, "+15550100", FALSE },
    { T0 + 2 * HOUR - 1, FALSE, "+15550100", FALSE },
    { T0 + 3 * HOUR, TRUE, NULL, TRUE },
  };
  MsgThreadPlace places[G_N_ELEMENTS (items)];

  msg_thread_layout (items, G_N_ELEMENTS (items), places);
  g_assert_true (places[0].separator);
  /* just under an hour: the same run */
  g_assert_false (places[1].separator);
  g_assert_cmpint (places[0].position, ==, MSG_RUN_FIRST);
  g_assert_cmpint (places[1].position, ==, MSG_RUN_LAST);
  /* an hour: a date line, which starts a run again */
  g_assert_true (places[2].separator);
  g_assert_cmpint (places[2].position, ==, MSG_RUN_SINGLE);
  g_assert_cmpint (msg_thread_gap_before (places, 2), ==, 0);
  g_assert_true (places[3].separator);
  /* the newest failed: no time under it */
  g_assert_false (places[3].time_under);
}

static void
test_group_senders (void)
{
  const MsgThreadItem items[] = {
    { T0, FALSE, "+15550100", FALSE },
    { T0 + MINUTE, FALSE, "+15550101", FALSE },
    { T0 + 2 * MINUTE, FALSE, "+15550101", FALSE },
  };
  MsgThreadPlace places[G_N_ELEMENTS (items)];

  msg_thread_layout (items, G_N_ELEMENTS (items), places);
  g_assert_cmpint (places[0].position, ==, MSG_RUN_SINGLE);
  g_assert_cmpint (places[1].position, ==, MSG_RUN_FIRST);
  g_assert_cmpint (places[2].position, ==, MSG_RUN_LAST);
  g_assert_cmpint (msg_thread_gap_before (places, 1), ==, 16);
}

static void
test_empty (void)
{
  msg_thread_layout (NULL, 0, NULL);
}

static void
test_words (void)
{
  g_autofree char *today = msg_thread_separator_text (T0, T0 + HOUR, TRUE);
  g_autofree char *yesterday = msg_thread_separator_text (T0, T0 + DAY, FALSE);
  g_autofree char *week = msg_thread_separator_text (T0, T0 + 3 * DAY, TRUE);
  g_autofree char *year = msg_thread_separator_text (T0, T0 + 400 * DAY, TRUE);
  g_autofree char *clock = msg_thread_time_text (T0, TRUE);

  g_assert_cmpstr (today, ==, "Today • 14:45");
  g_assert_cmpstr (yesterday, ==, "Yesterday • 2:45 PM");
  g_assert_cmpstr (week, ==, "Thursday, Oct 29 • 14:45");
  g_assert_cmpstr (year, ==, "Thursday, Oct 29, 2026 • 14:45");
  g_assert_cmpstr (clock, ==, "14:45");
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/thread/runs", test_runs);
  g_test_add_func ("/thread/separators", test_separators);
  g_test_add_func ("/thread/group-senders", test_group_senders);
  g_test_add_func ("/thread/empty", test_empty);
  g_test_add_func ("/thread/words", test_words);
  return g_test_run ();
}
