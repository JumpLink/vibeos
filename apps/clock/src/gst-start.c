/* GStreamer's start-up and its state changes, off the main thread (gst-start.h). */
#include "gst-start.h"

#include <gio/gio.h>

typedef enum {
  START_IDLE,
  START_RUNNING,
  START_READY,
  START_FAILED,
} StartState;

typedef struct {
  guint id;
  GstStartReady ready;
  gpointer data;
  guint timeout_id;
} Waiter;

typedef struct {
  GstElement *element;
  GstState state;
  GstStartDone done;
  gpointer data;
} Change;

static StartState start_state = START_IDLE;
static GSList *waiters;
static guint last_waiter;
static GThreadPool *changes;

static void
waiter_free (Waiter *waiter)
{
  g_clear_handle_id (&waiter->timeout_id, g_source_remove);
  g_free (waiter);
}

/* each waiter, once: with GStreamer up, or with no more waiting */
static void
finish_waiter (Waiter *waiter, gboolean ready)
{
  waiters = g_slist_remove (waiters, waiter);
  waiter->ready (ready, waiter->data);
  waiter_free (waiter);
}

static gboolean
on_wait_timeout (gpointer data)
{
  Waiter *waiter = data;

  waiter->timeout_id = 0;
  g_warning ("gstreamer: not up after %d ms: no sound this time", GST_START_WAIT_MS);
  finish_waiter (waiter, FALSE);
  return G_SOURCE_REMOVE;
}

static gboolean
on_failed_wait (gpointer data)
{
  Waiter *waiter = data;

  waiter->timeout_id = 0;
  finish_waiter (waiter, FALSE);
  return G_SOURCE_REMOVE;
}

/* --- the start */

static void
start_thread (GTask *task, gpointer source, gpointer data, GCancellable *cancellable)
{
  const char * const *factories = data;
  gboolean up = gst_init_check (NULL, NULL, NULL);

  for (guint i = 0; up && factories && factories[i]; i++)
    {
      GstElementFactory *factory = gst_element_factory_find (factories[i]);

      if (factory)
        {
          GstPluginFeature *loaded = gst_plugin_feature_load (GST_PLUGIN_FEATURE (factory));

          g_clear_object (&loaded);
          gst_object_unref (factory);
        }
    }
  g_task_return_boolean (task, up);
}

static void
on_started (GObject *source, GAsyncResult *result, gpointer data)
{
  gboolean up = g_task_propagate_boolean (G_TASK (result), NULL);

  start_state = up ? START_READY : START_FAILED;
  if (up)
    g_message ("gstreamer: up");
  else
    g_warning ("gstreamer: cannot start");
  while (waiters)
    finish_waiter (waiters->data, up);
}

void
gst_start_begin (const char * const *factories)
{
  g_autoptr(GTask) task = NULL;

  if (start_state != START_IDLE)
    return;
  start_state = START_RUNNING;
  task = g_task_new (NULL, NULL, on_started, NULL);
  g_task_set_task_data (task, g_strdupv ((char **) factories), (GDestroyNotify) g_strfreev);
  g_task_run_in_thread (task, start_thread);
}

gboolean
gst_start_is_ready (void)
{
  return start_state == START_READY;
}

guint
gst_start_wait (GstStartReady ready, gpointer data)
{
  Waiter *waiter = g_new0 (Waiter, 1);

  waiter->id = ++last_waiter;
  waiter->ready = ready;
  waiter->data = data;
  /* never started or failed: no waiting for it */
  if (start_state == START_IDLE || start_state == START_FAILED)
    waiter->timeout_id = g_idle_add (on_failed_wait, waiter);
  else
    waiter->timeout_id = g_timeout_add (GST_START_WAIT_MS, on_wait_timeout, waiter);
  waiters = g_slist_prepend (waiters, waiter);
  return waiter->id;
}

void
gst_start_cancel (guint id)
{
  for (GSList *l = waiters; l; l = l->next)
    {
      Waiter *waiter = l->data;

      if (waiter->id == id)
        {
          waiters = g_slist_remove (waiters, waiter);
          waiter_free (waiter);
          return;
        }
    }
}

/* --- the state changes */

static void
change_thread (gpointer item, gpointer data)
{
  Change *change = item;

  gst_element_set_state (change->element, change->state);
  if (change->done)
    change->done (change->data);
  gst_object_unref (change->element);
  g_free (change);
}

void
gst_start_set_state (GstElement *element, GstState state, GstStartDone done, gpointer data)
{
  Change *change = g_new0 (Change, 1);

  /* one thread: the changes happen in the order they were asked */
  if (!changes)
    changes = g_thread_pool_new (change_thread, NULL, 1, FALSE, NULL);
  change->element = gst_object_ref (element);
  change->state = state;
  change->done = done;
  change->data = data;
  g_thread_pool_push (changes, change, NULL);
}
