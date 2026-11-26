/* The process waits for what is still closing before it ends, and no longer than it is told: the camera closes in
 * the pipeline's worker after the window has gone, and exit () under libcamera's software ISP crashed in Mesa
 * (cam-shutdown.h). Work the close hands to the main thread runs there meanwhile, never in the closing thread. */
#include "cam-shutdown.h"

static GThread *main_thread;

static gpointer
release_later (gpointer data)
{
  g_usleep (GPOINTER_TO_UINT (data) * G_TIME_SPAN_MILLISECOND);
  cam_shutdown_release ();
  return NULL;
}

static gint64
elapsed_ms (gint64 since)
{
  return (g_get_monotonic_time () - since) / G_TIME_SPAN_MILLISECOND;
}

typedef struct
{
  GMutex   lock;
  GCond    cond;
  GThread *ran_in;
} Handed;

static gboolean
on_handed (gpointer data)
{
  Handed *h = data;

  g_mutex_lock (&h->lock);
  h->ran_in = g_thread_self ();
  g_cond_signal (&h->cond);
  g_mutex_unlock (&h->lock);
  return G_SOURCE_REMOVE;
}

/* as gtk4paintablesink's state change to NULL: work for the main thread, waited for or not, then the close ends */
static gpointer
close_handing (gpointer data)
{
  Handed *h = data;

  g_usleep (100 * G_TIME_SPAN_MILLISECOND);
  g_main_context_invoke (NULL, on_handed, h);
  g_mutex_lock (&h->lock);
  while (!h->ran_in)
    g_cond_wait (&h->cond, &h->lock);
  g_mutex_unlock (&h->lock);
  cam_shutdown_release ();
  return NULL;
}

static void
test_nothing_held (void)
{
  gint64 since = g_get_monotonic_time ();

  g_assert_true (cam_shutdown_wait (5000));
  g_assert_cmpint (elapsed_ms (since), <, 100);
}

static void
test_waits_for_release (void)
{
  gint64 since;
  GThread *thread;

  /* two requests queued, the second released by the worker later: the wait ends with the last */
  cam_shutdown_hold ();
  cam_shutdown_hold ();
  cam_shutdown_release ();
  since = g_get_monotonic_time ();
  thread = g_thread_new ("closing", release_later, GUINT_TO_POINTER (300));
  g_assert_true (cam_shutdown_wait (5000));
  g_assert_cmpint (elapsed_ms (since), >=, 290);
  g_assert_cmpint (elapsed_ms (since), <, 2000);
  g_thread_join (thread);
  g_assert_true (cam_shutdown_wait (0));
}

static void
test_runs_handed_work_here (void)
{
  Handed h = { 0 };
  gint64 since = g_get_monotonic_time ();
  GThread *thread;

  /* nobody owns the main context before the wait, as after g_application_run (): the wait owns it, so the invoke is
   * queued for this thread instead of running in the closing one, and the close waiting on it does not hold it up */
  g_mutex_init (&h.lock);
  g_cond_init (&h.cond);
  cam_shutdown_hold ();
  thread = g_thread_new ("closing", close_handing, &h);
  g_assert_true (cam_shutdown_wait (5000));
  g_assert_cmpint (elapsed_ms (since), <, 2000);
  g_thread_join (thread);
  g_assert_true (h.ran_in == main_thread);
  g_mutex_clear (&h.lock);
  g_cond_clear (&h.cond);
}

static void
test_gives_up_in_time (void)
{
  gint64 since = g_get_monotonic_time ();

  /* a close that hangs (a driver stuck in STREAMOFF) holds the process no longer than the cap */
  cam_shutdown_hold ();
  g_assert_false (cam_shutdown_wait (200));
  g_assert_cmpint (elapsed_ms (since), >=, 190);
  g_assert_cmpint (elapsed_ms (since), <, 2000);
  cam_shutdown_release ();
  g_assert_true (cam_shutdown_wait (0));
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  main_thread = g_thread_self ();
  g_test_add_func ("/shutdown/nothing-held", test_nothing_held);
  g_test_add_func ("/shutdown/waits-for-release", test_waits_for_release);
  g_test_add_func ("/shutdown/runs-handed-work-here", test_runs_handed_work_here);
  g_test_add_func ("/shutdown/gives-up-in-time", test_gives_up_in_time);
  return g_test_run ();
}
