/* cam-startup: see cam-startup.h. */
#include "cam-startup.h"
#include "cam-devices.h"

#include <gst/gst.h>

static gint gst_started;

gboolean
cam_gst_ready (void)
{
  return g_atomic_int_get (&gst_started) != 0;
}

typedef struct {
  CamStartupDone  done;
  gpointer        data;
  GPtrArray      *cameras;
  CamPipeline    *pipeline;
  GError         *error;
} Run;

static gboolean
on_finished (gpointer data)
{
  Run *run = data;

  run->done (run->cameras, run->pipeline, run->error, run->data);
  g_free (run);
  return G_SOURCE_REMOVE;
}

static gpointer
run_thread (gpointer data)
{
  Run *run = data;

  /* only one run is alive at a time (the window starts the next after this one is delivered), so this is the one
   * caller of gst_init */
  if (!cam_gst_ready ())
    {
      gst_init (NULL, NULL);
      g_atomic_int_set (&gst_started, 1);
    }
  run->cameras = cam_devices_enumerate ();
  if (run->cameras->len)
    run->pipeline = cam_pipeline_new (&run->error);
  g_main_context_invoke (NULL, on_finished, run);
  return NULL;
}

void
cam_startup_run (CamStartupDone done, gpointer data)
{
  Run *run = g_new0 (Run, 1);
  GThread *thread;

  run->done = done;
  run->data = data;
  thread = g_thread_new ("camera-start", run_thread, run);
  g_thread_unref (thread);
}
