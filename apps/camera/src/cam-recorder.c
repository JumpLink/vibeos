/* cam-recorder: see cam-recorder.h. */
#include "cam-recorder.h"
#include "cam-encoder.h"
#include "cam-startup.h"

#include <errno.h>
#include <fcntl.h>
#include <gst/app/gstappsrc.h>
#include <glib/gstdio.h>
#include <unistd.h>

#define FINISH_TIMEOUT_S 10

static const char *const aac_encoders[] = { "voaacenc", "avenc_aac", "fdkaacenc", NULL };

struct _CamRecorder {
  CamPipeline    *camera;
  GstElement     *pipeline, *video, *audio, *mic;
  GstClock       *clock;
  GstClockTime    base, camera_base;
  char           *path, *pending, *format;
  gboolean        mirror;
  int             rotation;

  GMutex          lock;         /* the pause, between the main loop and the two streaming threads */
  gboolean        paused, started;
  GstClockTime    pause_start, paused_total, last_pts;
  gboolean        caps_set;

  guint           bus_watch, timeout;
  CamRecorderDone done;
  gpointer        done_data;
  GError         *error;
};

static const char *
aac_encoder (void)
{
  for (int i = 0; aac_encoders[i]; i++)
    {
      g_autoptr(GstElementFactory) factory = gst_element_factory_find (aac_encoders[i]);
      if (factory)
        return aac_encoders[i];
    }
  return NULL;
}

gboolean
cam_recorder_can_record_audio (void)
{
  g_autoptr(GstElementFactory) source = NULL;

  /* the answer comes with GStreamer's start (cam-startup.h) */
  if (!cam_gst_ready ())
    return FALSE;
  source = gst_element_factory_find ("pulsesrc");
  return source && aac_encoder ();
}

/* ---- the frames ------------------------------------------------------------------------------------------------ */

static void
on_frame (GstSample *sample, gpointer data)
{
  CamRecorder *self = data;
  GstBuffer *in = gst_sample_get_buffer (sample);
  GstClockTime pts = in ? GST_BUFFER_PTS (in) : GST_CLOCK_TIME_NONE;
  GstClockTime at, offset;
  GstBuffer *out;

  if (!GST_CLOCK_TIME_IS_VALID (pts))
    return;
  /* the frame's capture time on the shared clock, then the recording's time of it */
  at = pts + self->camera_base;
  g_mutex_lock (&self->lock);
  if (self->paused)
    {
      g_mutex_unlock (&self->lock);
      return;
    }
  offset = self->base + self->paused_total;
  if (at < offset || (self->started && at - offset <= self->last_pts))
    {
      g_mutex_unlock (&self->lock);
      return;
    }
  self->last_pts = at - offset;
  self->started = TRUE;
  g_mutex_unlock (&self->lock);

  if (!self->caps_set)
    {
      gst_app_src_set_caps (GST_APP_SRC (self->video), gst_sample_get_caps (sample));
      self->caps_set = TRUE;
    }
  /* shares the pixels; the pool behind the converter lets go of a buffer that is still shared */
  out = gst_buffer_copy (in);
  GST_BUFFER_PTS (out) = at - offset;
  GST_BUFFER_DTS (out) = GST_CLOCK_TIME_NONE;
  gst_app_src_push_buffer (GST_APP_SRC (self->video), out);
}

static GstPadProbeReturn
on_audio (GstPad *pad, GstPadProbeInfo *info, gpointer data)
{
  CamRecorder *self = data;
  GstBuffer *buffer;
  gboolean paused;
  GstClockTime total;

  g_mutex_lock (&self->lock);
  paused = self->paused;
  total = self->paused_total;
  g_mutex_unlock (&self->lock);
  if (paused)
    return GST_PAD_PROBE_DROP;
  if (!total)
    return GST_PAD_PROBE_OK;
  buffer = GST_PAD_PROBE_INFO_BUFFER (info);
  if (!GST_BUFFER_PTS_IS_VALID (buffer) || GST_BUFFER_PTS (buffer) < total)
    return GST_PAD_PROBE_DROP;
  buffer = gst_buffer_make_writable (buffer);
  GST_BUFFER_PTS (buffer) -= total;
  GST_PAD_PROBE_INFO_DATA (info) = buffer;
  return GST_PAD_PROBE_OK;
}

/* ---- finishing ------------------------------------------------------------------------------------------------- */

static void
recorder_free (CamRecorder *self)
{
  g_clear_handle_id (&self->bus_watch, g_source_remove);
  g_clear_handle_id (&self->timeout, g_source_remove);
  if (self->pipeline)
    gst_element_set_state (self->pipeline, GST_STATE_NULL);
  g_clear_object (&self->pipeline);
  g_clear_object (&self->camera);
  g_clear_error (&self->error);
  g_free (self->path);
  g_free (self->pending);
  g_free (self->format);
  g_mutex_clear (&self->lock);
  g_free (self);
}

static void
finish (CamRecorder *self)
{
  g_autoptr(GError) error = NULL;
  gboolean saved = FALSE;
  int fd;

  g_clear_handle_id (&self->bus_watch, g_source_remove);
  g_clear_handle_id (&self->timeout, g_source_remove);
  gst_element_set_state (self->pipeline, GST_STATE_NULL);
  /* on the disk before it gets its name: the gallery never sees half a file */
  fd = g_open (self->pending, O_RDONLY | O_CLOEXEC, 0);
  if (fd >= 0)
    {
      fsync (fd);
      close (fd);
      if (g_rename (self->pending, self->path) == 0)
        saved = TRUE;
      else
        g_set_error (&error, G_IO_ERROR, g_io_error_from_errno (errno), "%s: %s", self->path, g_strerror (errno));
    }
  else
    g_set_error (&error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "nothing was recorded");
  if (self->error)
    g_message ("camera: the recording ended with an error: %s", self->error->message);
  if (saved && self->error)
    g_message ("camera: kept what was recorded in %s", self->path);
  if (self->done)
    self->done (saved ? self->path : NULL, saved ? NULL : error, self->done_data);
  recorder_free (self);
}

static gboolean
on_bus (GstBus *bus, GstMessage *message, gpointer data)
{
  CamRecorder *self = data;

  switch (GST_MESSAGE_TYPE (message))
    {
    case GST_MESSAGE_ERROR:
      {
        g_autofree char *debug = NULL;

        g_clear_error (&self->error);
        gst_message_parse_error (message, &self->error, &debug);
        g_warning ("camera: recording: %s (%s)", self->error->message, debug ? debug : "");
        cam_pipeline_set_recording (self->camera, NULL, NULL, NULL, FALSE, 0);
        self->bus_watch = 0;
        finish (self);
        return G_SOURCE_REMOVE;
      }
    case GST_MESSAGE_EOS:
      self->bus_watch = 0;
      finish (self);
      return G_SOURCE_REMOVE;
    default:
      return G_SOURCE_CONTINUE;
    }
}

static gboolean
on_finish_timeout (gpointer data)
{
  CamRecorder *self = data;

  self->timeout = 0;
  g_warning ("camera: the recording did not finish in %d s", FINISH_TIMEOUT_S);
  finish (self);
  return G_SOURCE_REMOVE;
}

/* ---- the recorder ---------------------------------------------------------------------------------------------- */

CamRecorder *
cam_recorder_new (CamPipeline *camera, const char *path, const char *encoder, int bitrate, gboolean audio,
                  gboolean mirror, int rotation, const CamLocation *location, CamRecorderDone done,
                  gpointer data, GError **error)
{
  CamRecorder *self;
  g_autofree char *dir = g_path_get_dirname (path);
  g_autofree char *base = g_path_get_basename (path);
  g_autofree char *video = cam_encoder_launch (encoder, bitrate);
  g_autofree char *orientation = NULL;
  g_autofree char *launch = NULL;
  g_autofree char *location_quoted = NULL;
  const char *aac = audio ? aac_encoder () : NULL;
  GstElement *mux;
  g_autoptr(GstBus) bus = NULL;

  self = g_new0 (CamRecorder, 1);
  g_mutex_init (&self->lock);
  self->camera = g_object_ref (camera);
  self->path = g_strdup (path);
  self->pending = g_strdup_printf ("%s/.pending-%s", dir, base);
  self->format = g_strdup (cam_encoder_format (encoder));
  self->mirror = mirror;
  self->rotation = rotation;
  self->done = done;
  self->done_data = data;
  location_quoted = g_strescape (self->pending, NULL);
  /* the turn as the track matrix (mp4mux reads image-orientation from the stream's tags) */
  orientation = g_strdup_printf ("image-orientation=rotate-%d", rotation);
  launch = g_strdup_printf ("appsrc name=video is-live=true format=time max-buffers=8 leaky-type=downstream "
                            "  ! queue max-size-buffers=8 max-size-bytes=0 max-size-time=0 ! %s "
                            "  ! taginject tags=\"%s\" ! queue ! mux. "
                            "%s%s%s"
                            "mp4mux name=mux ! filesink location=\"%s\"",
                            video, orientation,
                            aac ? "pulsesrc name=audio ! audioconvert ! audioresample ! audio/x-raw,rate=48000 "
                                  "  ! volume name=mic ! " : "",
                            aac ? aac : "", aac ? " ! queue ! mux. " : "", location_quoted);
  self->pipeline = gst_parse_launch_full (launch, NULL, GST_PARSE_FLAG_FATAL_ERRORS, error);
  if (!self->pipeline)
    {
      recorder_free (self);
      return NULL;
    }
  gst_object_ref_sink (self->pipeline);
  self->video = gst_bin_get_by_name (GST_BIN (self->pipeline), "video");
  gst_object_unref (self->video);
  if (aac)
    {
      g_autoptr(GstPad) pad = NULL;

      self->audio = gst_bin_get_by_name (GST_BIN (self->pipeline), "audio");
      self->mic = gst_bin_get_by_name (GST_BIN (self->pipeline), "mic");
      gst_object_unref (self->audio);
      gst_object_unref (self->mic);
      pad = gst_element_get_static_pad (self->mic, "sink");
      gst_pad_add_probe (pad, GST_PAD_PROBE_TYPE_BUFFER, on_audio, self, NULL);
      /* PipeWire/PulseAudio: what this stream is, for the mixer and the privacy indicator */
      g_autoptr(GstStructure) props = gst_structure_new ("props", "media.role", G_TYPE_STRING, "camera", NULL);
      g_object_set (self->audio, "stream-properties", props, NULL);
    }
  mux = gst_bin_get_by_name (GST_BIN (self->pipeline), "mux");
  if (location && location->valid)
    {
      gst_tag_setter_add_tags (GST_TAG_SETTER (mux), GST_TAG_MERGE_REPLACE,
                               GST_TAG_GEO_LOCATION_LATITUDE, location->latitude,
                               GST_TAG_GEO_LOCATION_LONGITUDE, location->longitude, NULL);
      if (location->altitude != -G_MAXDOUBLE)
        gst_tag_setter_add_tags (GST_TAG_SETTER (mux), GST_TAG_MERGE_REPLACE, GST_TAG_GEO_LOCATION_ELEVATION,
                                 location->altitude, NULL);
    }
  gst_object_unref (mux);

  self->clock = cam_pipeline_get_clock (camera);
  gst_pipeline_use_clock (GST_PIPELINE (self->pipeline), self->clock);
  bus = gst_element_get_bus (self->pipeline);
  self->bus_watch = gst_bus_add_watch (bus, on_bus, self);
  return self;
}

void
cam_recorder_start (CamRecorder *self)
{
  /* the recording's time 0 is now, on the clock the camera runs on too */
  self->base = gst_clock_get_time (self->clock);
  self->camera_base = cam_pipeline_get_base_time (self->camera);
  gst_element_set_start_time (self->pipeline, GST_CLOCK_TIME_NONE);
  gst_element_set_base_time (self->pipeline, self->base);
  gst_element_set_state (self->pipeline, GST_STATE_PLAYING);
  cam_pipeline_set_recording (self->camera, on_frame, self, self->format, self->mirror, self->rotation);
}

void
cam_recorder_set_paused (CamRecorder *self, gboolean paused)
{
  GstClockTime now = gst_clock_get_time (self->clock);

  g_mutex_lock (&self->lock);
  if (paused && !self->paused)
    self->pause_start = now;
  else if (!paused && self->paused)
    self->paused_total += now - self->pause_start;
  self->paused = paused;
  g_mutex_unlock (&self->lock);
}

void
cam_recorder_set_muted (CamRecorder *self, gboolean muted)
{
  if (self->mic)
    g_object_set (self->mic, "mute", muted, NULL);
}

gint64
cam_recorder_get_duration (CamRecorder *self)
{
  GstClockTime now = gst_clock_get_time (self->clock);
  GstClockTime elapsed;

  g_mutex_lock (&self->lock);
  elapsed = now - self->base - self->paused_total - (self->paused ? now - self->pause_start : 0);
  g_mutex_unlock (&self->lock);
  return elapsed / GST_USECOND;
}

void
cam_recorder_stop (CamRecorder *self)
{
  /* no frame comes in after this returns */
  cam_pipeline_set_recording (self->camera, NULL, NULL, NULL, FALSE, 0);
  g_mutex_lock (&self->lock);
  self->paused = FALSE;
  g_mutex_unlock (&self->lock);
  if (!self->bus_watch)
    {
      finish (self);
      return;
    }
  gst_app_src_end_of_stream (GST_APP_SRC (self->video));
  if (self->audio)
    gst_element_send_event (self->audio, gst_event_new_eos ());
  self->timeout = g_timeout_add_seconds (FINISH_TIMEOUT_S, on_finish_timeout, self);
}
