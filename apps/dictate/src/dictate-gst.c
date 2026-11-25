/* dictate-gst: see dictate-gst.h. */
#include "dictate-gst.h"

#include <gio/gio.h>
#include <gst/gst.h>

typedef struct {
  DictateGstDone done, timeout;
  gpointer data;
  guint timeout_id;
} Start;

static gint started;            /* dictate_gst_start () was called */
static gint ready;              /* gst_init () has returned */

static void
init_thread (GTask *task, gpointer source, gpointer data, GCancellable *cancellable)
{
  gst_init (NULL, NULL);
  /* set here and not in the callback: a caller that asks on the main thread sees it as soon as it is true */
  g_atomic_int_set (&ready, 1);
  g_task_return_boolean (task, TRUE);
}

static void
on_init_done (GObject *source, GAsyncResult *result, gpointer data)
{
  Start *start = data;

  g_clear_handle_id (&start->timeout_id, g_source_remove);
  if (start->done)
    start->done (start->data);
  g_free (start);
}

static gboolean
on_timeout (gpointer data)
{
  Start *start = data;

  start->timeout_id = 0;
  if (start->timeout)
    start->timeout (start->data);
  return G_SOURCE_REMOVE;
}

void
dictate_gst_start (DictateGstDone done, DictateGstDone timeout, gpointer data)
{
  g_autoptr (GTask) task = NULL;
  Start *start;

  if (!g_atomic_int_compare_and_exchange (&started, 0, 1))
    return;
  start = g_new0 (Start, 1);
  start->done = done;
  start->timeout = timeout;
  start->data = data;
  start->timeout_id = g_timeout_add_seconds (DICTATE_GST_TIMEOUT_S, on_timeout, start);
  task = g_task_new (NULL, NULL, on_init_done, start);
  g_task_run_in_thread (task, init_thread);
}

gboolean
dictate_gst_ready (void)
{
  if (g_atomic_int_get (&ready))
    return TRUE;
  return !g_atomic_int_get (&started) && gst_is_initialized ();
}
