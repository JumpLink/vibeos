/* The sound as a message goes (msg-sound.h). */
#include "msg-sound.h"

#include <gio/gio.h>
#include <gst/gst.h>

#define SENT_SOUND SOUNDS_DIR "/message_sent.wav"

static GstElement *player;      /* one playbin, kept: a message after a message plays from the start again */

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
  if (GST_MESSAGE_TYPE (message) == GST_MESSAGE_ERROR)
    {
      g_autoptr(GError) error = NULL;

      gst_message_parse_error (message, &error, NULL);
      g_warning ("messaging: the sent sound: %s", error->message);
    }
  /* done (or failed): the sound device is let go until the next message */
  if (GST_MESSAGE_TYPE (message) == GST_MESSAGE_EOS || GST_MESSAGE_TYPE (message) == GST_MESSAGE_ERROR)
    gst_element_set_state (player, GST_STATE_NULL);
  return G_SOURCE_CONTINUE;
}

static GstElement *
get_player (void)
{
  g_autoptr(GstBus) bus = NULL;
  g_autofree char *uri = NULL;
  GstElement *sink;

  if (player)
    return player;
  gst_init (NULL, NULL);
  player = gst_element_factory_make ("playbin", "sent-sound");
  if (!player)
    return NULL;
  /* PulseAudio's "event" role: the notification stream's place in the mixer */
  sink = gst_element_factory_make ("pulsesink", NULL);
  if (sink)
    {
      g_autoptr(GstStructure) props = gst_structure_new ("props", "media.role", G_TYPE_STRING, "event", NULL);

      g_object_set (sink, "stream-properties", props, NULL);
      g_object_set (player, "audio-sink", sink, NULL);
    }
  uri = g_filename_to_uri (SENT_SOUND, NULL, NULL);
  g_object_set (player, "uri", uri, NULL);
  bus = gst_element_get_bus (player);
  gst_bus_add_watch (bus, on_message, NULL);
  return player;
}

void
msg_sound_play_sent (void)
{
  g_autoptr(GSettings) settings = g_settings_new ("vibeos.Messaging");
  GstElement *sound;

  if (!g_settings_get_boolean (settings, "send-sound") || silenced ())
    return;
  sound = get_player ();
  if (!sound)
    return;
  gst_element_set_state (sound, GST_STATE_NULL);
  gst_element_set_state (sound, GST_STATE_PLAYING);
}
