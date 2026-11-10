/* The klaxon (alarm-klaxon.h): GStreamer for the sound, PulseAudio's API (PipeWire's pulse server) for the speaker's
 * volume, feedbackd for the vibration. */
#include "alarm-klaxon.h"

#include <gst/gst.h>
#include <math.h>
#include <pulse/glib-mainloop.h>
#include <pulse/pulseaudio.h>

#define FALLBACK_SOUND "neo-clock/sounds/alarm_expire.ogg"     /* DeskClock's R.raw.alarm_expire */
#define CRESCENDO_STEP_MS 50
#define VIBRATE_PERIOD_MS 1000                                  /* AlarmKlaxon.VIBRATE_PATTERN: 500 off, 500 on */

/* playbin's GstPlayFlags: audio, with its own volume */
#define PLAY_FLAG_AUDIO 0x02
#define PLAY_FLAG_SOFT_VOLUME 0x10

/* each kind's settings, and its sound when the setting is empty */
static const struct {
  const char *ringtone_key, *crescendo_key, *default_sound;
} kinds[] = {
  /* LineageOS's ro.config.alarm_alert */
  [ALARM_KLAXON_ALARM] = { "default-alarm-ringtone", "alarm-crescendo-duration", "sounds/android/alarms/Hassium.ogg" },
  /* DeskClock's R.raw.timer_expire, "Timer Expired" */
  [ALARM_KLAXON_TIMER] = { "timer-ringtone", "timer-crescendo-duration", "neo-clock/sounds/timer_expire.ogg" },
  /* a preview names its sound; without one it is the default alarm sound */
  [ALARM_KLAXON_PREVIEW] = { "default-alarm-ringtone", NULL, "sounds/android/alarms/Hassium.ogg" },
};

/* every klaxon, so that an alarm or a timer can take the speaker over from a preview */
static GSList *klaxons;

struct _AlarmKlaxon {
  GSettings *settings;
  AlarmKlaxonKind kind;

  GstElement *player;
  char *uri;
  gboolean fell_back;           /* playing the fallback sound, after the chosen one failed */
  gint64 crescendo_start, crescendo_end;  /* monotonic µs; 0: no crescendo */
  guint crescendo_id;

  guint vibrate_id;

  /* the speaker's volume: Android's alarm stream, kept while ringing */
  pa_glib_mainloop *pa_loop;
  pa_context *pa;
  gboolean ringing;
  gboolean saved;               /* saved_volume and saved_mute hold what the speaker had before */
  pa_cvolume saved_volume;
  int saved_mute;
  pa_volume_t set_volume;       /* what it was set to: the user changing it meanwhile keeps theirs */
};

char *
alarm_klaxon_find_sound (const char *relative_path)
{
  const char * const *dirs = g_get_system_data_dirs ();

  for (guint i = 0; dirs[i]; i++)
    {
      g_autofree char *path = g_build_filename (dirs[i], relative_path, NULL);

      if (g_file_test (path, G_FILE_TEST_IS_REGULAR))
        return g_filename_to_uri (path, NULL, NULL);
    }
  return NULL;
}

/* --- the speaker's volume */

static void
on_sink_restore (pa_context *pa, const pa_sink_info *info, int eol, void *data)
{
  AlarmKlaxon *self = data;

  if (eol || !info || !self->saved)
    return;
  self->saved = FALSE;
  /* moved by the user while it rang (the volume keys): theirs stays */
  if (pa_cvolume_max (&info->volume) != self->set_volume)
    return;
  pa_operation_unref (pa_context_set_sink_volume_by_index (pa, info->index, &self->saved_volume, NULL, NULL));
  pa_operation_unref (pa_context_set_sink_mute_by_index (pa, info->index, self->saved_mute, NULL, NULL));
}

static void
on_sink_ring (pa_context *pa, const pa_sink_info *info, int eol, void *data)
{
  AlarmKlaxon *self = data;
  pa_cvolume volume;

  if (eol || !info || !self->ringing)
    return;
  if (!self->saved)
    {
      self->saved_volume = info->volume;
      self->saved_mute = info->mute;
      self->saved = TRUE;
    }
  /* the settings' 0..1 is the volume slider's scale, as PulseAudio's own volume is */
  self->set_volume = (pa_volume_t) round (PA_VOLUME_NORM * g_settings_get_double (self->settings, "alarm-volume"));
  pa_cvolume_set (&volume, info->volume.channels, self->set_volume);
  pa_operation_unref (pa_context_set_sink_volume_by_index (pa, info->index, &volume, NULL, NULL));
  pa_operation_unref (pa_context_set_sink_mute_by_index (pa, info->index, 0, NULL, NULL));
}

/* the default sink to the alarm volume while ringing, back to what it was after */
static void
sync_volume (AlarmKlaxon *self)
{
  if (!self->pa || pa_context_get_state (self->pa) != PA_CONTEXT_READY)
    return;
  if (self->ringing)
    pa_operation_unref (pa_context_get_sink_info_by_name (self->pa, "@DEFAULT_SINK@", on_sink_ring, self));
  else if (self->saved)
    pa_operation_unref (pa_context_get_sink_info_by_name (self->pa, "@DEFAULT_SINK@", on_sink_restore, self));
}

static void
on_pa_state (pa_context *pa, void *data)
{
  AlarmKlaxon *self = data;

  switch (pa_context_get_state (pa))
    {
    case PA_CONTEXT_READY:
      sync_volume (self);
      break;
    case PA_CONTEXT_FAILED:
      /* the sound server went (restarted): connect_sound_server makes a new connection when next needed */
      g_warning ("klaxon: lost the sound server: %s", pa_strerror (pa_context_errno (pa)));
      break;
    default:
      break;
    }
}

static void
connect_sound_server (AlarmKlaxon *self)
{
  if (self->pa && PA_CONTEXT_IS_GOOD (pa_context_get_state (self->pa)))
    return;
  g_clear_pointer (&self->pa, pa_context_unref);
  self->pa = pa_context_new (pa_glib_mainloop_get_api (self->pa_loop), "Clock alarms");
  pa_context_set_state_callback (self->pa, on_pa_state, self);
  if (pa_context_connect (self->pa, NULL, PA_CONTEXT_NOFAIL, NULL) < 0)
    g_warning ("klaxon: cannot reach the sound server: %s", pa_strerror (pa_context_errno (self->pa)));
}

/* --- the sound */

static void
set_player_volume (AlarmKlaxon *self, double volume)
{
  g_object_set (self->player, "volume", volume, NULL);
}

/* AsyncRingtonePlayer.computeVolume: a straight rise in decibels, from -40 dB to full */
static gboolean
crescendo_step (gpointer data)
{
  AlarmKlaxon *self = data;
  gint64 now = g_get_monotonic_time ();
  double fraction;

  if (now >= self->crescendo_end)
    {
      set_player_volume (self, 1.0);
      self->crescendo_id = 0;
      return G_SOURCE_REMOVE;
    }
  fraction = (double) (now - self->crescendo_start) / (self->crescendo_end - self->crescendo_start);
  set_player_volume (self, pow (10.0, (fraction * 40.0 - 40.0) / 20.0));
  return G_SOURCE_CONTINUE;
}

/* near the end of the sound: the same again, without a gap (Ringtone.setLooping) */
static void
on_about_to_finish (GstElement *player, gpointer data)
{
  AlarmKlaxon *self = data;

  g_object_set (player, "uri", self->uri, NULL);
}

static void play (AlarmKlaxon *self, const char *uri);

static gboolean
on_player_message (GstBus *bus, GstMessage *message, gpointer data)
{
  AlarmKlaxon *self = data;
  g_autoptr(GError) error = NULL;
  g_autofree char *fallback = NULL;

  if (GST_MESSAGE_TYPE (message) != GST_MESSAGE_ERROR)
    return G_SOURCE_CONTINUE;
  gst_message_parse_error (message, &error, NULL);
  g_warning ("klaxon: cannot play %s: %s", self->uri, error->message);
  /* some sound, any sound: the fallback, once */
  if (self->fell_back || !(fallback = alarm_klaxon_find_sound (FALLBACK_SOUND)))
    return G_SOURCE_CONTINUE;
  self->fell_back = TRUE;
  play (self, fallback);
  return G_SOURCE_CONTINUE;
}

static GstElement *
make_player (AlarmKlaxon *self)
{
  GstElement *player, *sink;
  g_autoptr(GstBus) bus = NULL;

  /* GStreamer from the first alarm on: the service waits most of its life, in less memory without it */
  gst_init (NULL, NULL);
  player = gst_element_factory_make ("playbin", "klaxon");
  sink = gst_element_factory_make ("pulsesink", NULL);
  bus = gst_element_get_bus (player);

  /* an alarm, to the sound server's role rules */
  if (sink)
    {
      g_autoptr(GstStructure) props = gst_structure_new ("props", "media.role", G_TYPE_STRING, "alarm",
                                                         "application.id", G_TYPE_STRING, "de.yesman.Clock", NULL);

      g_object_set (sink, "stream-properties", props, NULL);
      g_object_set (player, "audio-sink", sink, NULL);
    }
  g_object_set (player, "flags", PLAY_FLAG_AUDIO | PLAY_FLAG_SOFT_VOLUME, NULL);
  g_signal_connect (player, "about-to-finish", G_CALLBACK (on_about_to_finish), self);
  gst_bus_add_watch (bus, on_player_message, self);
  return player;
}

static void
play (AlarmKlaxon *self, const char *uri)
{
  const char *key = kinds[self->kind].crescendo_key;
  int crescendo = key ? g_settings_get_int (self->settings, key) : 0;

  if (!self->player)
    self->player = make_player (self);
  gst_element_set_state (self->player, GST_STATE_NULL);
  g_free (self->uri);
  self->uri = g_strdup (uri);
  g_object_set (self->player, "uri", uri, NULL);

  g_clear_handle_id (&self->crescendo_id, g_source_remove);
  if (crescendo > 0)
    {
      self->crescendo_start = g_get_monotonic_time ();
      self->crescendo_end = self->crescendo_start + crescendo * G_USEC_PER_SEC;
      set_player_volume (self, 0.0);
      self->crescendo_id = g_timeout_add (CRESCENDO_STEP_MS, crescendo_step, self);
    }
  else
    set_player_volume (self, 1.0);
  gst_element_set_state (self->player, GST_STATE_PLAYING);
}

/* --- the vibration */

static void
vibrate_once (AlarmKlaxon *self)
{
  /* feedbackd's haptic pattern, (strength, ms) pairs: one period of Android's {500, 500} */
  g_autoptr(GDBusConnection) bus = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, NULL);
  GVariantBuilder pattern;

  if (!bus)
    return;
  g_variant_builder_init (&pattern, G_VARIANT_TYPE ("a(du)"));
  g_variant_builder_add (&pattern, "(du)", 0.0, 500);
  g_variant_builder_add (&pattern, "(du)", 1.0, 500);
  g_dbus_connection_call (bus, "org.sigxcpu.Feedback", "/org/sigxcpu/Feedback", "org.sigxcpu.Feedback.Haptic",
                          "Vibrate", g_variant_new ("(sa(du))", "de.yesman.Clock", &pattern), NULL,
                          G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);
}

static gboolean
vibrate_step (gpointer data)
{
  vibrate_once (data);
  return G_SOURCE_CONTINUE;
}

/* --- start and stop */

/* the sound and the vibration off; the speaker's volume is left as it is */
static void
silence (AlarmKlaxon *self)
{
  if (self->player)
    gst_element_set_state (self->player, GST_STATE_NULL);
  g_clear_handle_id (&self->crescendo_id, g_source_remove);
  g_clear_handle_id (&self->vibrate_id, g_source_remove);
}

gboolean
alarm_klaxon_any_ringing (void)
{
  for (GSList *l = klaxons; l; l = l->next)
    {
      AlarmKlaxon *klaxon = l->data;

      if (klaxon->kind != ALARM_KLAXON_PREVIEW && klaxon->ringing)
        return TRUE;
    }
  return FALSE;
}

char *
alarm_klaxon_default_sound (AlarmKlaxonKind kind)
{
  return alarm_klaxon_find_sound (kinds[kind].default_sound);
}

/* an alarm or a timer starting stops a preview, taking over what the speaker had before it, so that the volume goes
 * back to that once, at the end */
static void
stop_previews (AlarmKlaxon *self)
{
  for (GSList *l = klaxons; l; l = l->next)
    {
      AlarmKlaxon *preview = l->data;

      if (preview->kind != ALARM_KLAXON_PREVIEW || !preview->ringing)
        continue;
      if (preview->saved && !self->saved)
        {
          self->saved_volume = preview->saved_volume;
          self->saved_mute = preview->saved_mute;
          self->saved = TRUE;
        }
      preview->saved = FALSE;
      alarm_klaxon_stop (preview);
    }
}

void
alarm_klaxon_start (AlarmKlaxon *self, const char *ringtone, gboolean vibrate)
{
  if (self->kind == ALARM_KLAXON_PREVIEW && alarm_klaxon_any_ringing ())
    return;
  if (self->kind != ALARM_KLAXON_PREVIEW)
    stop_previews (self);
  /* one alarm after another: the speaker stays at the alarm volume, to go back once to what it was before both */
  silence (self);
  self->ringing = TRUE;

  if (!ringtone)
    {
      /* the default sound may be none: "Silent" picked as the default ("silent", clock-format.h's CLOCK_SILENT) */
      g_autofree char *setting = g_settings_get_string (self->settings, kinds[self->kind].ringtone_key);

      if (g_str_equal (setting, "silent"))
        ringtone = "";
    }
  if (!ringtone || *ringtone)
    {
      g_autofree char *setting = g_settings_get_string (self->settings, kinds[self->kind].ringtone_key);
      g_autofree char *found = NULL;
      const char *uri = ringtone;

      if (!uri)
        uri = *setting ? setting : (found = alarm_klaxon_find_sound (kinds[self->kind].default_sound));
      if (!uri)
        uri = found = alarm_klaxon_find_sound (FALLBACK_SOUND);
      self->fell_back = FALSE;
      if (uri)
        {
          connect_sound_server (self);
          sync_volume (self);
          play (self, uri);
        }
      else
        g_warning ("klaxon: no sound installed");
    }
  if (vibrate)
    {
      vibrate_once (self);
      self->vibrate_id = g_timeout_add (VIBRATE_PERIOD_MS, vibrate_step, self);
    }
}

void
alarm_klaxon_stop (AlarmKlaxon *self)
{
  if (!self->ringing)
    return;
  self->ringing = FALSE;
  silence (self);
  sync_volume (self);
}

AlarmKlaxon *
alarm_klaxon_new (GSettings *settings, AlarmKlaxonKind kind)
{
  AlarmKlaxon *self = g_new0 (AlarmKlaxon, 1);

  self->settings = g_object_ref (settings);
  self->kind = kind;
  self->pa_loop = pa_glib_mainloop_new (NULL);
  klaxons = g_slist_prepend (klaxons, self);
  return self;
}

void
alarm_klaxon_free (AlarmKlaxon *self)
{
  klaxons = g_slist_remove (klaxons, self);
  alarm_klaxon_stop (self);
  if (self->player)
    {
      g_autoptr(GstBus) bus = gst_element_get_bus (self->player);

      gst_bus_remove_watch (bus);
      gst_object_unref (self->player);
    }
  if (self->pa)
    {
      pa_context_disconnect (self->pa);
      pa_context_unref (self->pa);
    }
  pa_glib_mainloop_free (self->pa_loop);
  g_object_unref (self->settings);
  g_free (self->uri);
  g_free (self);
}
