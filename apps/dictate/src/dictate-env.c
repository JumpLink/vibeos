/* dictate-env: see dictate-env.h. */
#include "dictate-env.h"
#include "dictate-gst.h"

#include <gio/gio.h>
#include <glib/gi18n.h>
#include <pulse/glib-mainloop.h>
#include <pulse/pulseaudio.h>

struct _DictateEnv {
  GObject parent;
  GNetworkMonitor *monitor;
  pa_glib_mainloop *loop;
  pa_context *context;
  gboolean sound_known;         /* the sound server has answered, or has failed */
  gboolean microphone;          /* a capture source is there */
  gboolean refreshing, refresh_again;
  guint sources;                /* counted by the list being read */
  GFileMonitor *runtime;        /* the runtime directory: the sound server's socket appears in it */
  gboolean gst_ready;           /* as last told, for "changed" */
  gboolean network_known;       /* GNetworkMonitor has reported, or has had its time to */
  guint settle_id;
};

G_DEFINE_FINAL_TYPE (DictateEnv, dictate_env, G_TYPE_OBJECT)

enum { SIGNAL_CHANGED, N_SIGNALS };
static guint signals[N_SIGNALS];
static DictateEnv *the_env;

static void connect_sound (DictateEnv *self);

static void
dictate_env_init (DictateEnv *self)
{
  self->microphone = TRUE;
}

static void
dictate_env_class_init (DictateEnvClass *class)
{
  signals[SIGNAL_CHANGED] = g_signal_new ("changed", G_TYPE_FROM_CLASS (class), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                                          G_TYPE_NONE, 0);
}

static void
changed (DictateEnv *self)
{
  g_signal_emit (self, signals[SIGNAL_CHANGED], 0);
}

/* --- GStreamer */

static void
on_gst_done (gpointer data)
{
  DictateEnv *self = data;

  g_message ("env: GStreamer is ready");
  self->gst_ready = TRUE;
  changed (self);
}

static void
on_gst_timeout (gpointer data)
{
  g_warning ("env: GStreamer has not started after %d s; going on without audio until it does",
             DICTATE_GST_TIMEOUT_S);
  changed (data);
}

/* --- the sound server's capture sources */

static void
set_microphone (DictateEnv *self, gboolean present)
{
  gboolean was = self->microphone;

  self->sound_known = TRUE;
  self->microphone = present;
  if (was != present)
    {
      g_message ("env: %s", present ? "a microphone is there" : "no microphone");
      changed (self);
    }
}

static void
on_source (pa_context *c, const pa_source_info *info, int eol, void *data)
{
  DictateEnv *self = data;

  if (eol > 0)
    {
      self->refreshing = FALSE;
      set_microphone (self, self->sources > 0);
      if (self->refresh_again)
        {
          self->refresh_again = FALSE;
          if (self->context && pa_context_get_state (self->context) == PA_CONTEXT_READY)
            {
              self->refreshing = TRUE;
              self->sources = 0;
              pa_operation_unref (pa_context_get_source_info_list (self->context, on_source, self));
            }
        }
      return;
    }
  if (eol < 0)
    {
      self->refreshing = FALSE;
      return;
    }
  /* a source that is the monitor of an output is what plays, not a microphone */
  if (info && info->monitor_of_sink == PA_INVALID_INDEX)
    self->sources++;
}

static void
refresh_sources (DictateEnv *self)
{
  if (!self->context || pa_context_get_state (self->context) != PA_CONTEXT_READY)
    return;
  if (self->refreshing)
    {
      self->refresh_again = TRUE;
      return;
    }
  self->refreshing = TRUE;
  self->sources = 0;
  pa_operation_unref (pa_context_get_source_info_list (self->context, on_source, self));
}

static void
on_subscription (pa_context *c, pa_subscription_event_type_t type, uint32_t index, void *data)
{
  refresh_sources (data);
}

static void
drop_sound (DictateEnv *self)
{
  if (self->context)
    {
      pa_context_set_state_callback (self->context, NULL, NULL);
      pa_context_set_subscribe_callback (self->context, NULL, NULL);
      pa_context_disconnect (self->context);
      pa_context_unref (self->context);
      self->context = NULL;
    }
  self->refreshing = self->refresh_again = FALSE;
}

static void
on_context_state (pa_context *c, void *data)
{
  DictateEnv *self = data;

  switch (pa_context_get_state (c))
    {
    case PA_CONTEXT_READY:
      pa_context_set_subscribe_callback (c, on_subscription, self);
      pa_operation_unref (pa_context_subscribe (c, PA_SUBSCRIPTION_MASK_SOURCE | PA_SUBSCRIPTION_MASK_SERVER, NULL,
                                                NULL));
      refresh_sources (self);
      break;
    case PA_CONTEXT_FAILED:
    case PA_CONTEXT_TERMINATED:
      /* no sound server (yet, or any more): no microphone, until it is there again */
      g_debug ("env: the sound server is not there: %s", pa_strerror (pa_context_errno (c)));
      drop_sound (self);
      set_microphone (self, FALSE);
      break;
    default:
      break;
    }
}

static void
connect_sound (DictateEnv *self)
{
  if (self->context)
    return;
  if (!self->loop)
    self->loop = pa_glib_mainloop_new (NULL);
  self->context = pa_context_new (pa_glib_mainloop_get_api (self->loop), "Vibe");
  pa_context_set_state_callback (self->context, on_context_state, self);
  /* without autospawn: the app does not start a sound server, it waits for the session's */
  if (pa_context_connect (self->context, NULL, PA_CONTEXT_NOAUTOSPAWN, NULL) < 0)
    {
      drop_sound (self);
      set_microphone (self, FALSE);
    }
}

/* the sound server's socket appears in the runtime directory: connect then, instead of asking now and then */
static void
on_runtime_changed (GFileMonitor *monitor, GFile *file, GFile *other, GFileMonitorEvent event, gpointer data)
{
  DictateEnv *self = data;
  g_autofree char *name = g_file_get_basename (file);

  if (event == G_FILE_MONITOR_EVENT_CREATED && g_str_has_prefix (name, "pulse") && !self->context)
    connect_sound (self);
}

/* --- the network */

/* GNetworkMonitor reads the routes once the main loop runs: until it has reported, "no network" is only its first
 * guess, and a phone with no network at all reports nothing: so it is taken at its word after a few seconds */
#define NETWORK_SETTLE_S 3

static gboolean
on_network_settled (gpointer data)
{
  DictateEnv *self = data;

  self->settle_id = 0;
  if (!self->network_known)
    {
      self->network_known = TRUE;
      g_message ("env: the network is %s", dictate_env_online (self) ? "up" : "down");
      changed (self);
    }
  return G_SOURCE_REMOVE;
}

static void
on_network_changed (GNetworkMonitor *monitor, gboolean available, DictateEnv *self)
{
  self->network_known = TRUE;
  g_clear_handle_id (&self->settle_id, g_source_remove);
  g_message ("env: the network is %s", available ? "up" : "down");
  changed (self);
}

/* --- the interface */

DictateEnv *
dictate_env_start (void)
{
  DictateEnv *self;

  if (the_env)
    return the_env;
  self = the_env = g_object_new (DICTATE_TYPE_ENV, NULL);
  self->monitor = g_object_ref (g_network_monitor_get_default ());
  g_signal_connect (self->monitor, "network-changed", G_CALLBACK (on_network_changed), self);
  self->network_known = g_network_monitor_get_network_available (self->monitor);
  if (!self->network_known)
    self->settle_id = g_timeout_add_seconds (NETWORK_SETTLE_S, on_network_settled, self);
  /* a sound file stands in for the microphone in the tests: nothing to ask the sound server */
  if (!g_getenv ("DICTATE_TEST_INPUT"))
    {
      const char *runtime = g_get_user_runtime_dir ();

      connect_sound (self);
      if (runtime)
        {
          g_autoptr (GFile) dir = g_file_new_for_path (runtime);

          self->runtime = g_file_monitor_directory (dir, G_FILE_MONITOR_NONE, NULL, NULL);
          if (self->runtime)
            g_signal_connect (self->runtime, "changed", G_CALLBACK (on_runtime_changed), self);
        }
    }
  dictate_gst_start (on_gst_done, on_gst_timeout, self);
  return self;
}

gboolean
dictate_env_audio_ready (DictateEnv *self)
{
  return dictate_gst_ready ();
}

gboolean
dictate_env_has_microphone (DictateEnv *self)
{
  return self->microphone;
}

const char *
dictate_env_audio_notice (DictateEnv *self)
{
  if (!dictate_gst_ready ())
    /* TRANSLATORS: a notice at the top of Vibe's and Dictate's window while the audio (GStreamer) has not started,
     * and the microphone and the voice cannot be used */
    return _("Audio unavailable");
  if (!self->microphone)
    /* TRANSLATORS: a notice at the top of Vibe's and Dictate's window when the phone has no microphone to listen to
     * (none plugged in, or the sound server is not running); it goes when one appears */
    return _("No microphone");
  return NULL;
}

gboolean
dictate_env_online (DictateEnv *self)
{
  /* until the monitor has had its say, the network is taken to be there: no notice for a guess */
  return !self->network_known || g_network_monitor_get_network_available (self->monitor);
}

void
dictate_env_recheck (DictateEnv *self)
{
  gboolean ready = dictate_gst_ready ();

  /* GStreamer finished while the main loop was busy: the notice goes now and not when its callback runs */
  if (ready != self->gst_ready)
    {
      self->gst_ready = ready;
      changed (self);
    }
  if (!self->context && !g_getenv ("DICTATE_TEST_INPUT"))
    connect_sound (self);
  else
    refresh_sources (self);
}
