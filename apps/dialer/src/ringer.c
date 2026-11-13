/* The ringer (ringer.h): GStreamer for the ringtone, PulseAudio's API (PipeWire's pulse server) for the speaker's
 * volume, feedbackd for the vibration and for the ringer mode. */
#include "ringer.h"

#include <gst/gst.h>
#include <math.h>
#include <pulse/glib-mainloop.h>
#include <pulse/pulseaudio.h>
#include <string.h>

/* the default ringtone, Orion; without it installed, the freedesktop sound theme's ring
 * (sound-theme-freedesktop), so a call never comes in silent */
#define DEFAULT_RINGTONE "sounds/android/ringtones/Orion.ogg"
#define FALLBACK_RINGTONE "sounds/freedesktop/stereo/phone-incoming-call.oga"
/* the default pattern: a second on, a second off */
#define VIBRATE_ON_MS 1000
#define VIBRATE_PERIOD_MS 2000

/* the keypad tone at 80 of 100, the two sines sharing it */
#define TONE_VOLUME 0.4
/* the tone player is let go when the keys have been still a while */
#define TONE_IDLE_MS 5000

/* playbin's GstPlayFlags: audio, with its own volume */
#define PLAY_FLAG_AUDIO 0x02
#define PLAY_FLAG_SOFT_VOLUME 0x10

typedef enum {
  RINGER_MODE_NORMAL,           /* feedbackd's full */
  RINGER_MODE_VIBRATE,          /* quiet */
  RINGER_MODE_SILENT,           /* silent */
} RingerMode;

struct _Ringer {
  GSettings *settings;
  GSettings *feedback;          /* org.sigxcpu.feedbackd, for the profile; NULL without feedbackd */
  gboolean ringing;

  GstElement *player;
  char *uri;
  gboolean previewing;          /* the player plays a ringtone once, for Settings */
  guint vibrate_id;

  GstElement *tone;             /* the keypad tones: two sines into the speaker */
  GstElement *tone_low, *tone_high;
  guint tone_idle_id;

  /* the speaker's volume, kept while ringing */
  pa_glib_mainloop *pa_loop;
  pa_context *pa;
  gboolean saved;               /* saved_volume and saved_mute hold what the speaker had before */
  pa_cvolume saved_volume;
  int saved_mute;
  pa_volume_t set_volume;       /* what it was set to: the user changing it meanwhile keeps theirs */
};

static RingerMode
ringer_mode (Ringer *self)
{
  g_autofree char *profile = self->feedback ? g_settings_get_string (self->feedback, "profile") : NULL;

  if (g_strcmp0 (profile, "silent") == 0)
    return RINGER_MODE_SILENT;
  if (g_strcmp0 (profile, "quiet") == 0)
    return RINGER_MODE_VIBRATE;
  return RINGER_MODE_NORMAL;
}

/* the file of a sound in the data dirs, as a URI, or NULL */
static char *
find_sound (const char *relative_path)
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

/* the default ringtone's file, as a URI, or NULL */
static char *
default_ringtone (void)
{
  char *uri = find_sound (DEFAULT_RINGTONE);

  return uri ? uri : find_sound (FALLBACK_RINGTONE);
}

/* --- the speaker's volume */

static void
on_sink_restore (pa_context *pa, const pa_sink_info *info, int eol, void *data)
{
  Ringer *self = data;

  if (eol || !info || !self->saved)
    return;
  self->saved = FALSE;
  /* moved by the user while it rang: theirs stays */
  if (pa_cvolume_max (&info->volume) != self->set_volume)
    return;
  pa_operation_unref (pa_context_set_sink_volume_by_index (pa, info->index, &self->saved_volume, NULL, NULL));
  pa_operation_unref (pa_context_set_sink_mute_by_index (pa, info->index, self->saved_mute, NULL, NULL));
}

static void
on_sink_ring (pa_context *pa, const pa_sink_info *info, int eol, void *data)
{
  Ringer *self = data;
  pa_cvolume volume;

  if (eol || !info || !(self->ringing || self->previewing))
    return;
  if (!self->saved)
    {
      self->saved_volume = info->volume;
      self->saved_mute = info->mute;
      self->saved = TRUE;
    }
  self->set_volume = (pa_volume_t) round (PA_VOLUME_NORM * g_settings_get_double (self->settings, "ring-volume"));
  pa_cvolume_set (&volume, info->volume.channels, self->set_volume);
  pa_operation_unref (pa_context_set_sink_volume_by_index (pa, info->index, &volume, NULL, NULL));
  pa_operation_unref (pa_context_set_sink_mute_by_index (pa, info->index, 0, NULL, NULL));
}

static void
sync_volume (Ringer *self)
{
  if (!self->pa || pa_context_get_state (self->pa) != PA_CONTEXT_READY)
    return;
  if ((self->ringing || self->previewing) && self->player)
    pa_operation_unref (pa_context_get_sink_info_by_name (self->pa, "@DEFAULT_SINK@", on_sink_ring, self));
  else if (self->saved)
    pa_operation_unref (pa_context_get_sink_info_by_name (self->pa, "@DEFAULT_SINK@", on_sink_restore, self));
}

static void
on_pa_state (pa_context *pa, void *data)
{
  if (pa_context_get_state (pa) == PA_CONTEXT_READY)
    sync_volume (data);
}

static void
connect_sound_server (Ringer *self)
{
  if (self->pa && PA_CONTEXT_IS_GOOD (pa_context_get_state (self->pa)))
    return;
  g_clear_pointer (&self->pa, pa_context_unref);
  self->pa = pa_context_new (pa_glib_mainloop_get_api (self->pa_loop), "Phone ringer");
  pa_context_set_state_callback (self->pa, on_pa_state, self);
  if (pa_context_connect (self->pa, NULL, PA_CONTEXT_NOFAIL, NULL) < 0)
    g_warning ("ringer: cannot reach the sound server: %s", pa_strerror (pa_context_errno (self->pa)));
}

/* --- the ringtone */

/* near the end of the ringtone: the same again, without a gap (Ringtone.setLooping) */
static void
on_about_to_finish (GstElement *player, gpointer data)
{
  Ringer *self = data;

  /* a preview plays once */
  if (!self->previewing)
    g_object_set (player, "uri", self->uri, NULL);
}

static gboolean
on_player_message (GstBus *bus, GstMessage *message, gpointer data)
{
  Ringer *self = data;
  g_autoptr(GError) error = NULL;
  g_autofree char *fallback = NULL;

  /* a preview played to its end: the speaker's volume goes back */
  if (GST_MESSAGE_TYPE (message) == GST_MESSAGE_EOS && self->previewing)
    ringer_preview (self, NULL);
  if (GST_MESSAGE_TYPE (message) != GST_MESSAGE_ERROR)
    return G_SOURCE_CONTINUE;
  gst_message_parse_error (message, &error, NULL);
  g_warning ("ringer: cannot play %s: %s", self->uri, error->message);
  if (self->previewing)
    {
      ringer_preview (self, NULL);
      return G_SOURCE_CONTINUE;
    }
  /* a ringtone that went missing: the default one, Orion, once */
  fallback = default_ringtone ();
  if (fallback && g_strcmp0 (fallback, self->uri) != 0)
    {
      gst_element_set_state (self->player, GST_STATE_NULL);
      g_free (self->uri);
      self->uri = g_steal_pointer (&fallback);
      g_object_set (self->player, "uri", self->uri, NULL);
      gst_element_set_state (self->player, GST_STATE_PLAYING);
    }
  return G_SOURCE_CONTINUE;
}

static GstElement *
make_player (Ringer *self)
{
  GstElement *player, *sink;
  g_autoptr(GstBus) bus = NULL;

  /* GStreamer from the first call on: the service waits most of its life, in less memory without it */
  gst_init (NULL, NULL);
  player = gst_element_factory_make ("playbin", "ringer");
  sink = gst_element_factory_make ("pulsesink", NULL);
  bus = gst_element_get_bus (player);
  if (sink)
    {
      g_autoptr(GstStructure) props = gst_structure_new ("props", "media.role", G_TYPE_STRING, "phone",
                                                         "application.id", G_TYPE_STRING, "neo.Dialer", NULL);

      g_object_set (sink, "stream-properties", props, NULL);
      g_object_set (player, "audio-sink", sink, NULL);
    }
  g_object_set (player, "flags", PLAY_FLAG_AUDIO | PLAY_FLAG_SOFT_VOLUME, NULL);
  g_signal_connect (player, "about-to-finish", G_CALLBACK (on_about_to_finish), self);
  gst_bus_add_watch (bus, on_player_message, self);
  return player;
}

static void
play_ringtone (Ringer *self)
{
  g_autofree char *setting = g_settings_get_string (self->settings, "ringtone");

  /* "None": the phone rings without a sound (and vibrates, if it is to) */
  if (g_str_equal (setting, RINGER_SILENT))
    return;
  if (self->previewing)
    ringer_preview (self, NULL);
  g_free (self->uri);
  self->uri = *setting && !g_str_equal (setting, RINGER_DEFAULT) ? g_steal_pointer (&setting)
                                                                  : default_ringtone ();
  if (!self->uri)
    {
      g_warning ("ringer: no ringtone installed");
      return;
    }
  if (!self->player)
    self->player = make_player (self);
  connect_sound_server (self);
  sync_volume (self);
  g_object_set (self->player, "uri", self->uri, "volume", 1.0, NULL);
  gst_element_set_state (self->player, GST_STATE_PLAYING);
}

void
ringer_preview (Ringer *self, const char *uri)
{
  g_autofree char *file = NULL;

  if (self->previewing)
    {
      gst_element_set_state (self->player, GST_STATE_NULL);
      self->previewing = FALSE;
      sync_volume (self);
    }
  if (!uri || !*uri || self->ringing || ringer_mode (self) != RINGER_MODE_NORMAL)
    return;
  if (g_str_equal (uri, RINGER_DEFAULT) && !(file = default_ringtone ()))
    return;
  if (!self->player)
    self->player = make_player (self);
  g_free (self->uri);
  self->uri = file ? g_steal_pointer (&file) : g_strdup (uri);
  self->previewing = TRUE;
  /* at the ring volume, as the picker plays it on the ring stream */
  connect_sound_server (self);
  sync_volume (self);
  g_object_set (self->player, "uri", self->uri, "volume", 1.0, NULL);
  gst_element_set_state (self->player, GST_STATE_PLAYING);
}

/* --- the keypad tones */

/* the DTMF pairs, 0…D: the row's low tone and the column's high one, Hz */
static gboolean
dtmf_pair (char key, guint *low, guint *high)
{
  static const char keys[] = "123456789*0#";
  static const guint rows[] = { 697, 770, 852, 941 };
  static const guint cols[] = { 1209, 1336, 1477 };
  const char *at = key ? strchr (keys, key) : NULL;

  if (!at)
    return FALSE;
  *low = rows[(at - keys) / 3];
  *high = cols[(at - keys) % 3];
  return TRUE;
}

static gboolean
tone_idle (gpointer data)
{
  Ringer *self = data;

  self->tone_idle_id = 0;
  gst_element_set_state (self->tone, GST_STATE_NULL);
  return G_SOURCE_REMOVE;
}

static GstElement *
make_tone (Ringer *self)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(GstStructure) props = gst_structure_new ("props", "media.role", G_TYPE_STRING, "phone",
                                                     "application.id", G_TYPE_STRING, "neo.Dialer", NULL);
  GstElement *tone, *sink;

  gst_init (NULL, NULL);
  tone = gst_parse_launch ("audiomixer name=mix ! audioconvert ! pulsesink name=sink "
                           "audiotestsrc name=low wave=sine is-live=true ! mix. "
                           "audiotestsrc name=high wave=sine is-live=true ! mix.", &error);
  if (!tone)
    {
      g_warning ("ringer: no keypad tones: %s", error->message);
      return NULL;
    }
  sink = gst_bin_get_by_name (GST_BIN (tone), "sink");
  g_object_set (sink, "stream-properties", props, NULL);
  gst_object_unref (sink);
  /* owned by the bin */
  self->tone_low = gst_bin_get_by_name (GST_BIN (tone), "low");
  self->tone_high = gst_bin_get_by_name (GST_BIN (tone), "high");
  gst_object_unref (self->tone_low);
  gst_object_unref (self->tone_high);
  g_object_set (self->tone_low, "volume", TONE_VOLUME / 2, NULL);
  g_object_set (self->tone_high, "volume", TONE_VOLUME / 2, NULL);
  return tone;
}

void
ringer_play_tone (Ringer *self, char key)
{
  guint low, high;

  /* a keypad tone: only with "Keypad tones" on, and not in Vibrate or Mute */
  if (!dtmf_pair (key, &low, &high) || !g_settings_get_boolean (self->settings, "dtmf-tone-when-dialing")
      || ringer_mode (self) != RINGER_MODE_NORMAL || self->ringing)
    return;
  if (!self->tone && !(self->tone = make_tone (self)))
    return;
  g_clear_handle_id (&self->tone_idle_id, g_source_remove);
  g_object_set (self->tone_low, "freq", (double) low, NULL);
  g_object_set (self->tone_high, "freq", (double) high, NULL);
  gst_element_set_state (self->tone, GST_STATE_PLAYING);
}

void
ringer_stop_tone (Ringer *self)
{
  if (!self->tone)
    return;
  gst_element_set_state (self->tone, GST_STATE_PAUSED);
  g_clear_handle_id (&self->tone_idle_id, g_source_remove);
  self->tone_idle_id = g_timeout_add (TONE_IDLE_MS, tone_idle, self);
}

/* --- the vibration */

/* feedbackd's haptic pattern: (strength, ms) pairs, on and off in turn */
static void
vibrate (const guint *ms, guint n)
{
  g_autoptr(GDBusConnection) bus = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, NULL);
  GVariantBuilder pattern;

  if (!bus)
    return;
  g_variant_builder_init (&pattern, G_VARIANT_TYPE ("a(du)"));
  for (guint i = 0; i < n; i++)
    if (ms[i])
      g_variant_builder_add (&pattern, "(du)", i % 2 ? 0.0 : 1.0, ms[i]);
  g_dbus_connection_call (bus, "org.sigxcpu.Feedback", "/org/sigxcpu/Feedback", "org.sigxcpu.Feedback.Haptic",
                          "Vibrate", g_variant_new ("(sa(du))", "neo.Dialer", &pattern), NULL,
                          G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);
}

static gboolean
vibrate_step (gpointer data)
{
  const guint ms[] = { VIBRATE_ON_MS };

  vibrate (ms, 1);
  return G_SOURCE_CONTINUE;
}

/* --- start and stop */

void
ringer_start (Ringer *self)
{
  RingerMode mode = ringer_mode (self);

  if (self->ringing)
    return;
  self->ringing = TRUE;
  ringer_stop_tone (self);
  if (mode == RINGER_MODE_NORMAL && g_settings_get_double (self->settings, "ring-volume") > 0)
    play_ringtone (self);
  if (mode == RINGER_MODE_VIBRATE
      || (mode == RINGER_MODE_NORMAL && g_settings_get_boolean (self->settings, "vibrate-when-ringing")))
    {
      vibrate_step (self);
      self->vibrate_id = g_timeout_add (VIBRATE_PERIOD_MS, vibrate_step, self);
    }
  g_message ("ringer: ringing (%s)", mode == RINGER_MODE_NORMAL ? "ring" : mode == RINGER_MODE_VIBRATE
                                                                           ? "vibrate" : "mute");
}

void
ringer_vibrate (Ringer *self, guint on1, guint off, guint on2)
{
  const guint ms[] = { on1, off, on2 };

  if (ringer_mode (self) != RINGER_MODE_SILENT)
    vibrate (ms, on2 ? 3 : 1);
}

void
ringer_stop (Ringer *self)
{
  if (!self->ringing)
    return;
  self->ringing = FALSE;
  if (self->player)
    gst_element_set_state (self->player, GST_STATE_NULL);
  g_clear_handle_id (&self->vibrate_id, g_source_remove);
  sync_volume (self);
}

gboolean
ringer_is_ringing (Ringer *self)
{
  return self->ringing;
}

Ringer *
ringer_new (GSettings *settings)
{
  Ringer *self = g_new0 (Ringer, 1);
  GSettingsSchemaSource *schemas = g_settings_schema_source_get_default ();
  g_autoptr(GSettingsSchema) feedback = schemas ? g_settings_schema_source_lookup (schemas, "org.sigxcpu.feedbackd",
                                                                                   TRUE) : NULL;

  self->settings = g_object_ref (settings);
  if (feedback)
    self->feedback = g_settings_new ("org.sigxcpu.feedbackd");
  self->pa_loop = pa_glib_mainloop_new (NULL);
  return self;
}

void
ringer_free (Ringer *self)
{
  ringer_stop (self);
  g_clear_handle_id (&self->tone_idle_id, g_source_remove);
  if (self->tone)
    {
      gst_element_set_state (self->tone, GST_STATE_NULL);
      gst_object_unref (self->tone);
    }
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
  g_clear_object (&self->feedback);
  g_object_unref (self->settings);
  g_free (self->uri);
  g_free (self);
}
