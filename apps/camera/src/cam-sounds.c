/* cam-sounds: see cam-sounds.h. */
#include "cam-sounds.h"

#include <gio/gio.h>
#include <gst/gst.h>

static const char *const files[] = { "camera_click.ogg", "VideoRecord.ogg", "VideoStop.ogg" };
static GstElement *players[G_N_ELEMENTS (files)];

static gboolean
silenced (void)
{
  GSettingsSchemaSource *schemas = g_settings_schema_source_get_default ();
  g_autoptr(GSettingsSchema) schema = schemas ? g_settings_schema_source_lookup (schemas, "org.sigxcpu.feedbackd",
                                                                                 TRUE) : NULL;
  g_autoptr(GSettings) feedback = NULL;
  g_autofree char *profile = NULL;

  /* without feedbackd there is no silent mode to keep to */
  if (!schema)
    return FALSE;
  feedback = g_settings_new ("org.sigxcpu.feedbackd");
  profile = g_settings_get_string (feedback, "profile");
  return g_strcmp0 (profile, "full") != 0;
}

static gboolean
on_message (GstBus *bus, GstMessage *message, gpointer data)
{
  GstElement *player = data;

  if (GST_MESSAGE_TYPE (message) == GST_MESSAGE_ERROR)
    {
      g_autoptr(GError) error = NULL;

      gst_message_parse_error (message, &error, NULL);
      g_warning ("camera: a sound: %s", error->message);
    }
  /* done: the sound device is let go until the next one */
  if (GST_MESSAGE_TYPE (message) == GST_MESSAGE_EOS || GST_MESSAGE_TYPE (message) == GST_MESSAGE_ERROR)
    gst_element_set_state (player, GST_STATE_NULL);
  return G_SOURCE_CONTINUE;
}

static GstElement *
get_player (CamSound sound)
{
  g_autoptr(GstBus) bus = NULL;
  g_autofree char *path = NULL;
  g_autofree char *uri = NULL;
  GstElement *player, *sink;
  const char *dir = g_getenv ("VIBEOS_CAMERA_DATADIR");

  if (players[sound])
    return players[sound];
  player = gst_element_factory_make ("playbin", NULL);
  if (!player)
    return NULL;
  sink = gst_element_factory_make ("pulsesink", NULL);
  if (sink)
    {
      g_autoptr(GstStructure) props = gst_structure_new ("props", "media.role", G_TYPE_STRING, "event", NULL);

      g_object_set (sink, "stream-properties", props, NULL);
      g_object_set (player, "audio-sink", sink, NULL);
    }
  /* video and text are not wanted from a sound file */
  g_object_set (player, "flags", 0x2 /* GST_PLAY_FLAG_AUDIO */, NULL);
  path = g_build_filename (dir ? dir : DATADIR, "sounds", files[sound], NULL);
  uri = g_filename_to_uri (path, NULL, NULL);
  g_object_set (player, "uri", uri, NULL);
  if (g_strcmp0 (g_getenv ("VIBEOS_CAMERA_SOUND_MUTE"), "1") == 0)
    g_object_set (player, "mute", TRUE, "volume", 0.0, NULL);
  bus = gst_element_get_bus (player);
  gst_bus_add_watch (bus, on_message, player);
  players[sound] = player;
  return player;
}

gboolean
cam_sounds_play (CamSound sound, gboolean setting)
{
  gboolean muted = g_strcmp0 (g_getenv ("VIBEOS_CAMERA_SOUND_MUTE"), "1") == 0;
  GstElement *player;

  /* the muted test plays whatever the profile says */
  if (!muted && (!setting || silenced ()))
    return FALSE;
  if (muted && !setting)
    return FALSE;
  player = get_player (sound);
  if (!player)
    return FALSE;
  gst_element_set_state (player, GST_STATE_NULL);
  gst_element_set_state (player, GST_STATE_PLAYING);
  if (muted)
    g_message ("camera: sound %s (muted)", files[sound]);
  return TRUE;
}
