/* timer-model and stopwatch-model against what DeskClock does */
#include "../src/stopwatch-model.h"
#include "../src/timer-model.h"

#include <glib/gstdio.h>

#define SECOND 1000
#define MINUTE 60000

static void
test_count_down (void)
{
  g_autoptr(Timer) timer = timer_new (1, 90 * SECOND, "", FALSE);

  /* reset: the length, still */
  g_assert_cmpint (timer_remaining (timer, 5000), ==, 90 * SECOND);
  timer_start (timer, 1000, 1700000000000);
  g_assert_cmpint (timer->state, ==, TIMER_RUNNING);
  g_assert_cmpint (timer_remaining (timer, 11000), ==, 80 * SECOND);
  g_assert_cmpint (timer_elapsed (timer, 11000), ==, 10 * SECOND);
  g_assert_cmpint (timer_expiration (timer), ==, 91000);

  /* paused 30 s in: it keeps those 60 s however long it waits */
  timer_pause (timer, 31000);
  g_assert_cmpint (timer->state, ==, TIMER_PAUSED);
  g_assert_cmpint (timer_remaining (timer, 999999), ==, 60 * SECOND);
  timer_start (timer, 100000, 1700000100000);
  g_assert_cmpint (timer_expiration (timer), ==, 160000);

  /* expired at its time, then 5 s of overtime: the total is gone, the overtime counts */
  timer_expire (timer, 160000, 1700000160000);
  g_assert_cmpint (timer->state, ==, TIMER_EXPIRED);
  g_assert_cmpint (timer->total_length, ==, 0);
  g_assert_cmpint (timer_remaining (timer, 165000), ==, -5 * SECOND);

  /* stop (pause on an expired one): back to its length */
  timer_pause (timer, 166000);
  g_assert_cmpint (timer->state, ==, TIMER_RESET);
  g_assert_cmpint (timer_remaining (timer, 170000), ==, 90 * SECOND);
  g_assert_cmpint (timer->total_length, ==, 90 * SECOND);
}

static void
test_add_minute (void)
{
  g_autoptr(Timer) timer = timer_new (1, 30 * SECOND, "", FALSE);

  /* a reset timer keeps its length */
  timer_add_minute (timer, 0, 0);
  g_assert_cmpint (timer->remaining, ==, 30 * SECOND);

  /* running: a minute more left, and in the total the ring counts down from */
  timer_start (timer, 0, 0);
  timer_add_minute (timer, 10000, 0);
  g_assert_cmpint (timer_remaining (timer, 10000), ==, 80 * SECOND);
  g_assert_cmpint (timer_total_length (timer), ==, 90 * SECOND);

  /* expired 3 s ago: it runs again, with a minute from now */
  timer_expire (timer, 90000, 0);
  timer_add_minute (timer, 93000, 0);
  g_assert_cmpint (timer->state, ==, TIMER_RUNNING);
  g_assert_cmpint (timer_remaining (timer, 93000), ==, 60 * SECOND);
  g_assert_cmpint (timer_expiration (timer), ==, 153000);
}

static void
test_reboot (void)
{
  g_autoptr(Timer) timer = timer_new (1, 10 * MINUTE, "", FALSE);
  g_autoptr(Timer) paused = timer_new (2, 10 * MINUTE, "", FALSE);

  /* started at 12:00:00 on the wall clock; the phone off from 12:02:00 to 12:05:00; the elapsed clock at 4 s after
   * the boot: 3 min off and 2 min before it, so 5 min left */
  timer_start (timer, 500000, 1700000000000);
  timer_update_after_reboot (timer, 4000, 1700000000000 + 5 * MINUTE);
  g_assert_cmpint (timer_remaining (timer, 4000), ==, 5 * MINUTE);
  g_assert_cmpint (timer_expiration (timer), ==, 4000 + 5 * MINUTE);

  /* a paused one does not move */
  timer_start (paused, 0, 0);
  timer_pause (paused, MINUTE);
  timer_update_after_reboot (paused, 4000, 1700000000000 + 60 * MINUTE);
  g_assert_cmpint (paused->remaining, ==, 9 * MINUTE);
}

static void
test_order (void)
{
  g_autoptr(Timer) running = timer_new (1, MINUTE, "", FALSE);
  g_autoptr(Timer) expired = timer_new (2, MINUTE, "", FALSE);
  g_autoptr(Timer) reset = timer_new (3, MINUTE, "", FALSE);

  timer_start (running, 0, 0);
  timer_start (expired, 0, 0);
  timer_expire (expired, MINUTE, 0);
  g_assert_cmpint (timer_compare_expiry (expired, running, MINUTE), <, 0);
  g_assert_cmpint (timer_compare_expiry (running, reset, MINUTE), <, 0);
}

static void
test_store (void)
{
  g_autofree char *dir = g_dir_make_tmp ("timers-XXXXXX", NULL);
  g_autofree char *path = g_build_filename (dir, "timers.ini", NULL);
  g_autoptr(TimerStore) store = timer_store_load (path, NULL);
  g_autoptr(TimerStore) back = NULL;
  Timer *first, *second;

  /* no file: no timers */
  g_assert_nonnull (store);
  g_assert_cmpuint (store->timers->len, ==, 0);
  first = timer_store_add (store, timer_new (0, MINUTE, "Tea", FALSE));
  second = timer_store_add (store, timer_new (0, 2 * MINUTE, "", TRUE));
  timer_start (second, 1234, 1700000000000);
  g_free (store->boot_id);
  store->boot_id = g_strdup ("boot-a");
  g_assert_true (timer_store_save (store, path, NULL));

  back = timer_store_load (path, NULL);
  g_assert_cmpuint (back->timers->len, ==, 2);
  /* the newest first */
  g_assert_cmpuint (((Timer *) back->timers->pdata[0])->id, ==, second->id);
  g_assert_cmpstr (timer_store_find (back, first->id)->label, ==, "Tea");
  g_assert_cmpint (timer_store_find (back, second->id)->state, ==, TIMER_RUNNING);
  g_assert_cmpint (timer_store_find (back, second->id)->last_start, ==, 1234);
  g_assert_true (timer_store_find (back, second->id)->delete_after_use);
  g_assert_cmpstr (back->boot_id, ==, "boot-a");
  g_assert_cmpuint (back->next_id, ==, second->id + 1);

  g_unlink (path);
  g_rmdir (dir);
}

static void
test_stopwatch (void)
{
  g_autofree char *dir = g_dir_make_tmp ("stopwatch-XXXXXX", NULL);
  g_autofree char *path = g_build_filename (dir, "stopwatch.ini", NULL);
  g_autoptr(Stopwatch) stopwatch = stopwatch_load (path, NULL);
  g_autoptr(Stopwatch) back = NULL;

  g_assert_cmpint (stopwatch->state, ==, STOPWATCH_RESET);
  /* no lap before it runs */
  g_assert_false (stopwatch_lap (stopwatch, 0));
  stopwatch_start (stopwatch, 1000, 1700000000000);
  g_assert_true (stopwatch_lap (stopwatch, 14800));     /* lap 1: 13.8 s */
  g_assert_true (stopwatch_lap (stopwatch, 17030));     /* lap 2: 2.23 s */
  stopwatch_pause (stopwatch, 18000);
  g_assert_cmpint (stopwatch_total (stopwatch, 99999), ==, 17000);
  g_assert_cmpint (stopwatch_lap_time (stopwatch, 1), ==, 13800);
  g_assert_cmpint (stopwatch_lap_time (stopwatch, 2), ==, 2230);
  /* the third lap, going since the second ended at 16.03 s */
  g_assert_cmpint (stopwatch_current_lap_time (stopwatch, 17000), ==, 970);
  g_assert_cmpint (stopwatch_longest_lap_time (stopwatch, 17000), ==, 13800);
  /* paused: no laps */
  g_assert_false (stopwatch_lap (stopwatch, 18000));

  /* on again: it goes on from 17 s */
  stopwatch_start (stopwatch, 20000, 1700000019000);
  g_assert_cmpint (stopwatch_total (stopwatch, 21000), ==, 18000);
  g_assert_true (stopwatch_save (stopwatch, path, NULL));
  back = stopwatch_load (path, NULL);
  g_assert_cmpint (back->state, ==, STOPWATCH_RUNNING);
  g_assert_cmpuint (back->laps->len, ==, 2);
  g_assert_cmpint (stopwatch_total (back, 21000), ==, 18000);

  /* after a reboot 10 s of wall clock later: those 10 s count */
  stopwatch_update_after_reboot (back, 3000, 1700000029000);
  g_assert_cmpint (stopwatch_total (back, 3000), ==, 27000);

  stopwatch_reset (back);
  g_assert_cmpint (stopwatch_total (back, 5000), ==, 0);
  g_assert_cmpuint (back->laps->len, ==, 0);

  g_unlink (path);
  g_rmdir (dir);
}

static void
test_lap_limit (void)
{
  g_autoptr(Stopwatch) stopwatch = stopwatch_new ();

  stopwatch_start (stopwatch, 0, 0);
  for (int i = 1; i <= STOPWATCH_MAX_LAPS; i++)
    g_assert_true (stopwatch_lap (stopwatch, i * 1000));
  g_assert_false (stopwatch_can_lap (stopwatch));
  g_assert_false (stopwatch_lap (stopwatch, 999000));
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/timer/count-down", test_count_down);
  g_test_add_func ("/timer/add-minute", test_add_minute);
  g_test_add_func ("/timer/reboot", test_reboot);
  g_test_add_func ("/timer/order", test_order);
  g_test_add_func ("/timer/store", test_store);
  g_test_add_func ("/stopwatch/laps", test_stopwatch);
  g_test_add_func ("/stopwatch/lap-limit", test_lap_limit);
  return g_test_run ();
}
