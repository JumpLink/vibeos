/* The call recorder (call-recorder.h): PulseAudio's API (PipeWire's pulse server) to find the source and the sink of
 * the "Voice Call" profile by their PCMs, GStreamer to record and to play the announcement. */
#include "call-recorder.h"
#include "call-recording.h"

#include <fcntl.h>
#include <gio/gio.h>
#include <glib/gstdio.h>
#include <gst/gst.h>
#include <pulse/glib-mainloop.h>
#include <pulse/pulseaudio.h>
#include <sys/stat.h>
#include <unistd.h>

#define STOP_MS 2000            /* for the recording's end to go through the encoder into the file */
#define BITRATE 24000           /* Opus for voice; Lineage records AMR-WB, 23.85 kbit/s */
#define RECORDED_BYTES 4096     /* a file bigger than this has audio in it, past the Ogg Opus headers */
#define CLIENT_NAME "Phone call recorder"

typedef enum {
  IDLE,
  WAITING,                      /* for the source on the capture PCM, and the sink on the playback one */
  RECORDING,
} State;

struct _CallRecorder {
  CallRecorderStopped stopped;
  gpointer data;
  State state;
  char *path, *capture, *playback, *announcement;
  char *source, *sink;          /* their names in the sound server, once found */
  gboolean file_made;           /* the file is ours: it goes if the recording fails */
  guint wait_id;

  pa_glib_mainloop *pa_loop;
  pa_context *pa;
  guint lookups;                /* lists asked for and not yet answered */
  gboolean look_again;          /* a source or sink came meanwhile */

  GstElement *recording;
  GstElement *uplink, *local;   /* the announcement, into the call and on the phone */
  gboolean announced;

  char *test_source, *test_sink;
};

static void lookup (CallRecorder *self);

/* --- the end */

static void
drop_pipeline (GstElement **pipeline)
{
  g_autoptr(GstBus) bus = NULL;

  if (!*pipeline)
    return;
  bus = gst_element_get_bus (*pipeline);
  gst_bus_remove_watch (bus);
  gst_element_set_state (*pipeline, GST_STATE_NULL);
  g_clear_object (pipeline);
}

/* the recording's end through the encoder and the muxer, so the file ends as Ogg should */
static void
drain (GstElement *pipeline)
{
  g_autoptr(GstBus) bus = gst_element_get_bus (pipeline);
  g_autoptr(GstMessage) message = NULL;

  gst_bus_remove_watch (bus);
  gst_element_send_event (pipeline, gst_event_new_eos ());
  message = gst_bus_timed_pop_filtered (bus, STOP_MS * GST_MSECOND, GST_MESSAGE_EOS | GST_MESSAGE_ERROR);
  if (!message)
    g_warning ("recorder: the recording did not end in %d ms", STOP_MS);
}

/* the recording over (its file complete when it was drained first), or failed and its file gone */
static void
finish (CallRecorder *self, gboolean failed, gboolean report)
{
  g_clear_handle_id (&self->wait_id, g_source_remove);
  drop_pipeline (&self->uplink);
  drop_pipeline (&self->local);
  drop_pipeline (&self->recording);
  if (failed && self->file_made && g_unlink (self->path) < 0)
    g_warning ("recorder: cannot remove %s", self->path);
  self->file_made = FALSE;
  self->announced = FALSE;
  self->state = IDLE;
  g_clear_pointer (&self->path, g_free);
  g_clear_pointer (&self->capture, g_free);
  g_clear_pointer (&self->playback, g_free);
  g_clear_pointer (&self->announcement, g_free);
  g_clear_pointer (&self->source, g_free);
  g_clear_pointer (&self->sink, g_free);
  if (report)
    self->stopped (failed, self->data);
}

/* --- the announcement */

static gboolean
on_uplink_message (GstBus *bus, GstMessage *message, gpointer data)
{
  CallRecorder *self = data;
  g_autoptr(GError) error = NULL;

  switch (GST_MESSAGE_TYPE (message))
    {
    case GST_MESSAGE_EOS:
      drop_pipeline (&self->uplink);
      return G_SOURCE_REMOVE;
    case GST_MESSAGE_ERROR:
      /* the other side did not hear it: no recording */
      gst_message_parse_error (message, &error, NULL);
      g_warning ("recorder: cannot play the announcement into the call: %s", error->message);
      finish (self, TRUE, TRUE);
      return G_SOURCE_REMOVE;
    default:
      return G_SOURCE_CONTINUE;
    }
}

static gboolean
on_local_message (GstBus *bus, GstMessage *message, gpointer data)
{
  CallRecorder *self = data;
  g_autoptr(GError) error = NULL;

  switch (GST_MESSAGE_TYPE (message))
    {
    case GST_MESSAGE_ERROR:
      gst_message_parse_error (message, &error, NULL);
      g_warning ("recorder: cannot play the announcement on the phone: %s", error->message);
      G_GNUC_FALLTHROUGH;
    case GST_MESSAGE_EOS:
      drop_pipeline (&self->local);
      return G_SOURCE_REMOVE;
    default:
      return G_SOURCE_CONTINUE;
    }
}

/* the announcement into @device, the sound server's sink, or the default one for NULL */
static GstElement *
announce_pipeline (CallRecorder *self, const char *device, GstBusFunc on_message, GError **error)
{
  g_autofree char *description = g_strdup_printf ("filesrc name=file ! oggdemux ! opusdec ! audioconvert ! "
                                                  "audioresample ! %s name=sink",
                                                  self->test_sink ? self->test_sink : "pulsesink");
  GstElement *pipeline = gst_parse_launch_full (description, NULL, GST_PARSE_FLAG_FATAL_ERRORS, error);
  g_autoptr(GstElement) file = NULL, sink = NULL;
  g_autoptr(GstBus) bus = NULL;

  if (!pipeline)
    return NULL;
  file = gst_bin_get_by_name (GST_BIN (pipeline), "file");
  sink = gst_bin_get_by_name (GST_BIN (pipeline), "sink");
  g_object_set (file, "location", self->announcement, NULL);
  if (!self->test_sink)
    {
      /* the call's sink only: if it goes, the announcement does not move to another */
      g_autoptr(GstStructure) props = gst_structure_new ("props", "application.id", G_TYPE_STRING, "vibeos.Dialer",
                                                         "media.name", G_TYPE_STRING, "Call recording announcement",
                                                         "node.dont-reconnect", G_TYPE_STRING, "true", NULL);

      g_object_set (sink, "client-name", CLIENT_NAME, "stream-properties", props, NULL);
      if (device)
        g_object_set (sink, "device", device, NULL);
    }
  bus = gst_element_get_bus (pipeline);
  gst_bus_add_watch (bus, on_message, self);
  return pipeline;
}

static void
announce (CallRecorder *self)
{
  g_autoptr(GError) error = NULL;

  self->announced = TRUE;
  self->uplink = announce_pipeline (self, self->sink, on_uplink_message, &error);
  if (!self->uplink)
    {
      g_warning ("recorder: no announcement: %s", error->message);
      finish (self, TRUE, TRUE);
      return;
    }
  g_clear_error (&error);
  self->local = announce_pipeline (self, NULL, on_local_message, &error);
  if (!self->local)
    g_warning ("recorder: no announcement on the phone: %s", error->message);
  gst_element_set_state (self->uplink, GST_STATE_PLAYING);
  if (self->local)
    gst_element_set_state (self->local, GST_STATE_PLAYING);
}

/* --- the recording */

static gboolean
recorded (CallRecorder *self)
{
  GStatBuf st;

  return self->path && g_stat (self->path, &st) == 0 && st.st_size > RECORDED_BYTES;
}

static gboolean
on_recording_message (GstBus *bus, GstMessage *message, gpointer data)
{
  CallRecorder *self = data;
  g_autoptr(GError) error = NULL;
  GstState old, now;

  switch (GST_MESSAGE_TYPE (message))
    {
    case GST_MESSAGE_STATE_CHANGED:
      /* it records: the announcement, once */
      gst_message_parse_state_changed (message, &old, &now, NULL);
      if (GST_MESSAGE_SRC (message) == GST_OBJECT (self->recording) && now == GST_STATE_PLAYING
          && self->announcement && !self->announced)
        announce (self);
      return G_SOURCE_CONTINUE;
    case GST_MESSAGE_ERROR:
      /* the source gone (the call's profile left before the call ended here), or the file not written */
      gst_message_parse_error (message, &error, NULL);
      g_warning ("recorder: the recording stopped: %s", error->message);
      finish (self, !recorded (self), TRUE);
      return G_SOURCE_REMOVE;
    case GST_MESSAGE_EOS:
      finish (self, !recorded (self), TRUE);
      return G_SOURCE_REMOVE;
    default:
      return G_SOURCE_CONTINUE;
    }
}

/* the folder (0700 when it is made here) and the file, 0600, new */
static gboolean
make_file (const char *path, GError **error)
{
  g_autofree char *folder = g_path_get_dirname (path);
  int fd;

  if (g_mkdir_with_parents (folder, 0700) < 0)
    {
      int code = errno;

      g_set_error (error, G_IO_ERROR, g_io_error_from_errno (code), "%s: %s", folder, g_strerror (code));
      return FALSE;
    }
  fd = g_open (path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
  if (fd < 0)
    {
      int code = errno;

      g_set_error (error, G_IO_ERROR, g_io_error_from_errno (code), "%s: %s", path, g_strerror (code));
      return FALSE;
    }
  close (fd);
  return TRUE;
}

static GstElement *
record_pipeline (CallRecorder *self, GError **error)
{
  /* both sides into one: the channels mixed, as a phone's recording is */
  g_autofree char *description = g_strdup_printf ("%s name=source ! audioconvert ! audioresample ! "
                                                  "audio/x-raw,channels=1 ! opusenc name=encoder ! oggmux ! "
                                                  "filesink name=file",
                                                  self->test_source ? self->test_source : "pulsesrc");
  GstElement *pipeline = gst_parse_launch_full (description, NULL, GST_PARSE_FLAG_FATAL_ERRORS, error);
  g_autoptr(GstElement) source = NULL, encoder = NULL, file = NULL;
  g_autoptr(GstBus) bus = NULL;

  if (!pipeline)
    return NULL;
  source = gst_bin_get_by_name (GST_BIN (pipeline), "source");
  encoder = gst_bin_get_by_name (GST_BIN (pipeline), "encoder");
  file = gst_bin_get_by_name (GST_BIN (pipeline), "file");
  if (!self->test_source)
    {
      /* this source only: when the call's profile goes, the recording ends instead of going on from the mics */
      g_autoptr(GstStructure) props = gst_structure_new ("props", "application.id", G_TYPE_STRING, "vibeos.Dialer",
                                                         "media.name", G_TYPE_STRING, "Call recording",
                                                         "node.dont-reconnect", G_TYPE_STRING, "true", NULL);

      g_object_set (source, "device", self->source, "client-name", CLIENT_NAME, "stream-properties", props, NULL);
    }
  g_object_set (encoder, "bitrate", BITRATE, NULL);
  gst_util_set_object_arg (G_OBJECT (encoder), "audio-type", "voice");
  g_object_set (file, "location", self->path, NULL);
  bus = gst_element_get_bus (pipeline);
  gst_bus_add_watch (bus, on_recording_message, self);
  return pipeline;
}

/* the source (and the sink) found: the recording starts */
static void
begin (CallRecorder *self)
{
  g_autoptr(GError) error = NULL;

  g_clear_handle_id (&self->wait_id, g_source_remove);
  if (!make_file (self->path, &error))
    {
      g_warning ("recorder: cannot make the file: %s", error->message);
      finish (self, TRUE, TRUE);
      return;
    }
  self->file_made = TRUE;
  self->recording = record_pipeline (self, &error);
  if (!self->recording)
    {
      g_warning ("recorder: cannot record: %s", error->message);
      finish (self, TRUE, TRUE);
      return;
    }
  self->state = RECORDING;
  g_message ("recorder: recording from %s", self->source ? self->source : self->test_source);
  if (gst_element_set_state (self->recording, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE)
    finish (self, TRUE, TRUE);
}

static gboolean
on_test_begin (gpointer data)
{
  CallRecorder *self = data;

  self->wait_id = 0;
  begin (self);
  return G_SOURCE_REMOVE;
}

static gboolean
on_wait_over (gpointer data)
{
  CallRecorder *self = data;

  self->wait_id = 0;
  g_warning ("recorder: no %s on %s in %d ms", self->source ? "sink" : "source",
             self->source ? self->playback : self->capture, CALL_RECORDER_WAIT_MS);
  finish (self, TRUE, TRUE);
  return G_SOURCE_REMOVE;
}

/* --- the sound server: the source and the sink by their PCMs */

static void
lookup_done (CallRecorder *self)
{
  if (--self->lookups > 0)
    return;
  if (self->state != WAITING)
    return;
  if (self->source && (!self->playback || self->sink))
    begin (self);
  else if (self->look_again)
    lookup (self);
}

static void
on_source_info (pa_context *pa, const pa_source_info *info, int eol, void *data)
{
  CallRecorder *self = data;

  if (eol)
    {
      lookup_done (self);
      return;
    }
  /* a sink's monitor has its sink's PCM too */
  if (self->state == WAITING && !self->source && info->monitor_of_sink == PA_INVALID_INDEX
      && g_strcmp0 (pa_proplist_gets (info->proplist, "api.alsa.path"), self->capture) == 0)
    self->source = g_strdup (info->name);
}

static void
on_sink_info (pa_context *pa, const pa_sink_info *info, int eol, void *data)
{
  CallRecorder *self = data;

  if (eol)
    {
      lookup_done (self);
      return;
    }
  if (self->state == WAITING && !self->sink
      && g_strcmp0 (pa_proplist_gets (info->proplist, "api.alsa.path"), self->playback) == 0)
    self->sink = g_strdup (info->name);
}

static void
lookup (CallRecorder *self)
{
  self->look_again = FALSE;
  if (self->state != WAITING || !self->pa || pa_context_get_state (self->pa) != PA_CONTEXT_READY)
    return;
  if (self->lookups)
    {
      self->look_again = TRUE;
      return;
    }
  self->lookups++;
  pa_operation_unref (pa_context_get_source_info_list (self->pa, on_source_info, self));
  if (self->playback)
    {
      self->lookups++;
      pa_operation_unref (pa_context_get_sink_info_list (self->pa, on_sink_info, self));
    }
}

static void
on_pa_event (pa_context *pa, pa_subscription_event_type_t type, uint32_t index, void *data)
{
  if ((type & PA_SUBSCRIPTION_EVENT_TYPE_MASK) != PA_SUBSCRIPTION_EVENT_REMOVE)
    lookup (data);
}

static void
on_pa_state (pa_context *pa, void *data)
{
  CallRecorder *self = data;

  switch (pa_context_get_state (pa))
    {
    case PA_CONTEXT_READY:
      pa_context_set_subscribe_callback (pa, on_pa_event, self);
      pa_operation_unref (pa_context_subscribe (pa, PA_SUBSCRIPTION_MASK_SOURCE | PA_SUBSCRIPTION_MASK_SINK,
                                                NULL, NULL));
      lookup (self);
      break;
    case PA_CONTEXT_FAILED:
    case PA_CONTEXT_TERMINATED:
      /* lists asked for are not answered now; a waiting recording waits out its time */
      self->lookups = 0;
      break;
    default:
      break;
    }
}

static void
connect_sound_server (CallRecorder *self)
{
  if (self->pa && PA_CONTEXT_IS_GOOD (pa_context_get_state (self->pa)))
    {
      lookup (self);
      return;
    }
  if (self->pa)
    {
      pa_context_set_state_callback (self->pa, NULL, NULL);
      pa_context_set_subscribe_callback (self->pa, NULL, NULL);
      pa_context_unref (self->pa);
    }
  self->lookups = 0;
  self->pa = pa_context_new (pa_glib_mainloop_get_api (self->pa_loop), CLIENT_NAME);
  pa_context_set_state_callback (self->pa, on_pa_state, self);
  if (pa_context_connect (self->pa, NULL, PA_CONTEXT_NOFAIL, NULL) < 0)
    g_warning ("recorder: cannot reach the sound server: %s", pa_strerror (pa_context_errno (self->pa)));
}

/* --- the recorder */

CallRecorder *
call_recorder_new (CallRecorderStopped stopped, gpointer data)
{
  CallRecorder *self = g_new0 (CallRecorder, 1);

  self->stopped = stopped;
  self->data = data;
  self->pa_loop = pa_glib_mainloop_new (NULL);
  return self;
}

void
call_recorder_free (CallRecorder *self)
{
  call_recorder_stop (self);
  if (self->pa)
    {
      pa_context_set_state_callback (self->pa, NULL, NULL);
      pa_context_set_subscribe_callback (self->pa, NULL, NULL);
      pa_context_disconnect (self->pa);
      pa_context_unref (self->pa);
    }
  pa_glib_mainloop_free (self->pa_loop);
  g_free (self->test_source);
  g_free (self->test_sink);
  g_free (self);
}

gboolean
call_recorder_start (CallRecorder *self, const char *path, const char *capture_pcm, const char *playback_pcm,
                     const char *announcement, GError **error)
{
  g_return_val_if_fail (path && capture_pcm && (!announcement || playback_pcm), FALSE);

  if (self->state != IDLE)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_BUSY, "a call is being recorded already");
      return FALSE;
    }
  /* GStreamer from the first recording on, as the ringer has it */
  gst_init (NULL, NULL);
  self->path = g_strdup (path);
  self->capture = g_strdup (capture_pcm);
  self->playback = announcement ? g_strdup (playback_pcm) : NULL;
  self->announcement = g_strdup (announcement);
  self->state = WAITING;
  if (self->test_source)
    {
      self->wait_id = g_idle_add (on_test_begin, self);
      return TRUE;
    }
  self->wait_id = g_timeout_add (CALL_RECORDER_WAIT_MS, on_wait_over, self);
  connect_sound_server (self);
  return TRUE;
}

void
call_recorder_stop (CallRecorder *self)
{
  if (self->state == RECORDING)
    drain (self->recording);
  finish (self, FALSE, FALSE);
}

gboolean
call_recorder_is_recording (CallRecorder *self)
{
  return self->state != IDLE;
}

char *
call_recorder_announcement (void)
{
  return call_recording_announcement (g_get_language_names (), g_get_system_data_dirs ());
}

void
call_recorder_set_test_elements (CallRecorder *self, const char *source, const char *sink)
{
  g_free (self->test_source);
  g_free (self->test_sink);
  self->test_source = g_strdup (source);
  self->test_sink = g_strdup (sink);
}
