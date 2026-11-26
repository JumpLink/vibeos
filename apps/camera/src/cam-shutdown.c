/* cam-shutdown: work the process must not end before (cam-shutdown.h) */
#include "cam-shutdown.h"

static gint held;

void
cam_shutdown_hold (void)
{
  g_atomic_int_inc (&held);
}

void
cam_shutdown_release (void)
{
  gint was = g_atomic_int_add (&held, -1);

  g_warn_if_fail (was > 0);
  /* the last one: a wait blocked in the main context returns (or its next iteration does not block) */
  if (was == 1)
    g_main_context_wakeup (g_main_context_default ());
}

static gboolean
on_cap (gpointer data)
{
  *(gboolean *) data = TRUE;
  return G_SOURCE_REMOVE;
}

gboolean
cam_shutdown_wait (guint timeout_ms)
{
  GMainContext *context = g_main_context_default ();
  gint64 end = g_get_monotonic_time () + (gint64) timeout_ms * G_TIME_SPAN_MILLISECOND;
  gboolean late = FALSE;
  guint cap;

  if (g_atomic_int_get (&held) == 0)
    return TRUE;
  /* owned by this thread already inside g_application_run (); only a thread running an invoke holds it otherwise */
  while (!g_main_context_acquire (context))
    {
      if (g_get_monotonic_time () >= end)
        return g_atomic_int_get (&held) == 0;
      g_usleep (G_TIME_SPAN_MILLISECOND);
    }
  cap = g_timeout_add (timeout_ms, on_cap, &late);
  while (g_atomic_int_get (&held) > 0 && !late)
    g_main_context_iteration (context, TRUE);
  if (!late)
    g_source_remove (cap);
  g_main_context_release (context);
  return g_atomic_int_get (&held) == 0;
}
