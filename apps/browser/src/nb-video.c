/* nb-video: web video decodes in software. A phone's V4L2 video decoders (stateful, or stateless from v4l2codecs) rank
 * above FFmpeg's and libvpx's, so WebKit picked them, and their frames failed in WebKit's GL video sink. On the S10e
 * (Exynos MFC, Mali on Panfrost) a frame whose rows are not aligned as Panfrost wants, a 540 px wide video's, is not
 * imported into GL ("WSI pitch not properly aligned"): the video played black, the poster gone. And after a loop or
 * a seek the decoder only ended the stream again, so a looping video stopped at 0. The web processes read
 * GST_PLUGIN_FEATURE_RANK at their start: every V4L2 video decoder in the registry (one per codec and device) goes in
 * as NAME:NONE, ahead of what the environment already says, which then still wins.
 *
 * Listing the decoders takes GStreamer's start, which loads the registry (and with no cache scans every plugin, the
 * V4L2 ones open every /dev/video*) and may wait on a driver for ever. It runs in a child process
 * (`vibeos-browser --probe-decoders`, which prints one decoder per line) with a time limit, while the window comes up.
 * The first web view waits for the answer (nb_tab_ensure_view); a child that does not answer in time is taken as a
 * broken video stack, and the decoders every V4L2 driver registers under their usual names go in as NONE: software
 * decoding is the safe answer, and the worst it costs is a phone's hardware decoder not used. */
#include "nb.h"

#include <gst/gst.h>
#include <string.h>

#define PROBE_TIMEOUT_S 8

/* what the V4L2 plugins register without a device number (v4l2codecs' stateless ones start v4l2sl) */
static const char *const usual_decoders[] = {
  "v4l2h264dec", "v4l2h265dec", "v4l2h263dec", "v4l2mpeg2dec", "v4l2mpeg4dec", "v4l2vp8dec", "v4l2vp9dec",
  "v4l2av1dec", "v4l2slh264dec", "v4l2slh265dec", "v4l2slmpeg2dec", "v4l2slvp8dec", "v4l2slvp9dec", "v4l2slav1dec",
  NULL,
};

static gboolean answered;
static GSubprocess *child;
static guint timeout_id;

gboolean
nb_video_ready (void)
{
  return answered;
}

/* ---- the child's side ---- */

int
nb_video_probe_main (void)
{
  if (!gst_init_check (NULL, NULL, NULL))
    return 1;
  static const char *const plugins[] = { "video4linux2", "v4l2codecs" };
  for (guint i = 0; i < G_N_ELEMENTS (plugins); i++) {
    GList *features = gst_registry_get_feature_list_by_plugin (gst_registry_get (), plugins[i]);
    for (GList *l = features; l; l = l->next)
      if (GST_IS_ELEMENT_FACTORY (l->data) &&
          gst_element_factory_list_is_type (GST_ELEMENT_FACTORY (l->data),
                                            GST_ELEMENT_FACTORY_TYPE_DECODER | GST_ELEMENT_FACTORY_TYPE_MEDIA_VIDEO))
        g_print ("%s\n", gst_plugin_feature_get_name (GST_PLUGIN_FEATURE (l->data)));
    gst_plugin_feature_list_free (features);
  }
  return 0;
}

/* ---- the answer ---- */

/* the decoders (names) as NAME:NONE, ahead of what the environment already says */
static void
set_ranks (const char *const *names)
{
  GString *ranks = g_string_new (NULL);
  for (guint i = 0; names[i]; i++)
    if (*names[i])
      g_string_append_printf (ranks, "%s%s:NONE", ranks->len ? "," : "", names[i]);
  const char *env = g_getenv ("GST_PLUGIN_FEATURE_RANK");
  if (ranks->len && env && *env)
    g_string_append_printf (ranks, ",%s", env);
  if (ranks->len)
    g_setenv ("GST_PLUGIN_FEATURE_RANK", ranks->str, TRUE);
  g_string_free (ranks, TRUE);
}

static void
finish (const char *const *names)
{
  if (answered)
    return;
  set_ranks (names);
  answered = TRUE;
  g_clear_handle_id (&timeout_id, g_source_remove);
  nb_app_video_ready ();
}

static gboolean
on_timeout (gpointer data)
{
  timeout_id = 0;
  g_warning ("the video decoder list did not come in %d s: V4L2 decoders off", PROBE_TIMEOUT_S);
  if (child)
    g_subprocess_force_exit (child);
  finish (usual_decoders);
  return G_SOURCE_REMOVE;
}

static void
on_child_done (GObject *object, GAsyncResult *result, gpointer data)
{
  GSubprocess *p = G_SUBPROCESS (object);
  g_autofree char *out = NULL;
  gboolean ok = g_subprocess_communicate_utf8_finish (p, result, &out, NULL, NULL);

  if (answered) {
    g_clear_object (&child);
    return;
  }
  /* a child that could not start GStreamer, or died, lists nothing, as GStreamer failing to start did before */
  g_auto (GStrv) names = ok && out && g_subprocess_get_if_exited (p) && g_subprocess_get_exit_status (p) == 0
                         ? g_strsplit (out, "\n", -1) : g_new0 (char *, 1);
  g_clear_object (&child);
  finish ((const char *const *) names);
}

void
nb_video_stop (void)
{
  /* the app goes before the list came: the child does not outlive it */
  if (child)
    g_subprocess_force_exit (child);
}

void
nb_video_start (void)
{
  g_autoptr (GError) error = NULL;

  child = g_subprocess_new (G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_SILENCE, &error,
                            "/proc/self/exe", "--probe-decoders", NULL);
  if (!child) {
    g_warning ("cannot list the video decoders: %s", error->message);
    finish (usual_decoders);
    return;
  }
  timeout_id = g_timeout_add_seconds (PROBE_TIMEOUT_S, on_timeout, NULL);
  g_subprocess_communicate_utf8_async (child, NULL, NULL, on_child_done, NULL);
}
