/* dictate-cues: see dictate-cues.h */
#include "dictate-cues.h"

#define APP_ID "vibeos.Agent"

typedef struct {
  const char *name;             /* the sound's file, agent-NAME.wav, and feedbackd's event */
  /* the vibration: [magnitude (feedbackd's, 0..1), ms] steps, ending at a 0 ms step. The owner wants the phone's
   * haptics at about twice Android's default strength: 1.0 for 12 ms is the shell's DOT_TICK (haptics.js), Android's
   * click at twice its strength */
  struct { double magnitude; guint ms; } steps[5];
} Cue;

static const Cue CUES[] = {
  [DICTATE_CUE_LISTENING] = { "listening", { { 1.0, 12 }, { 0, 0 } } },
  [DICTATE_CUE_WORKING] = { "working", { { 0.6, 8 }, { 0, 0 } } },
  [DICTATE_CUE_DONE] = { "done", { { 1.0, 12 }, { 0.0, 70 }, { 1.0, 12 }, { 0, 0 } } },
  [DICTATE_CUE_FAILED] = { "failed", { { 1.0, 45 }, { 0.0, 80 }, { 1.0, 45 }, { 0, 0 } } },
};

/* agent-NAME.wav in vibeos-dictate/sounds under the data directories, or NULL */
static char *
sound_file (const char *name)
{
  g_autofree char *file = g_strdup_printf ("agent-%s.wav", name);
  g_autofree char *mine = g_build_filename (g_get_user_data_dir (), "vibeos-dictate", "sounds", file, NULL);
  const char *const *dirs = g_get_system_data_dirs ();

  if (g_file_test (mine, G_FILE_TEST_IS_REGULAR))
    return g_steal_pointer (&mine);
  for (guint i = 0; dirs[i]; i++)
    {
      char *path = g_build_filename (dirs[i], "vibeos-dictate", "sounds", file, NULL);
      if (g_file_test (path, G_FILE_TEST_IS_REGULAR))
        return path;
      g_free (path);
    }
  return NULL;
}

static void
call (GDBusConnection *bus, const char *interface, const char *method, GVariant *params)
{
  /* feedbackd is the shell's to start: an Agent that finds none plays nothing */
  g_dbus_connection_call (bus, "org.sigxcpu.Feedback", "/org/sigxcpu/Feedback", interface, method, params, NULL,
                          G_DBUS_CALL_FLAGS_NO_AUTO_START, 2000, NULL, NULL, NULL);
}

void
dictate_cue_play (GSettings *settings, DictateCue cue)
{
  g_autoptr (GDBusConnection) bus = NULL;
  const Cue *c;

  g_return_if_fail (cue < G_N_ELEMENTS (CUES));
  c = &CUES[cue];
  if (!g_settings_get_boolean (settings, "cue-sounds") && !g_settings_get_boolean (settings, "cue-haptics"))
    return;
  if (!(bus = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, NULL)))
    return;
  if (g_settings_get_boolean (settings, "cue-sounds"))
    {
      g_autofree char *file = sound_file (c->name);

      if (file)
        {
          /* an event of Agent's own, which the sound theme has nothing for: only this sound, and feedbackd plays
           * a custom sound only when the phone's sound is on */
          g_autofree char *event = g_strdup_printf ("agent-%s", c->name);
          GVariantBuilder hints;

          g_variant_builder_init (&hints, G_VARIANT_TYPE_VARDICT);
          g_variant_builder_add (&hints, "{sv}", "sound-file", g_variant_new_string (file));
          call (bus, "org.sigxcpu.Feedback", "TriggerFeedback",
                g_variant_new ("(ssa{sv}i)", APP_ID, event, &hints, -1));
        }
    }
  if (g_settings_get_boolean (settings, "cue-haptics"))
    {
      GVariantBuilder steps;

      g_variant_builder_init (&steps, G_VARIANT_TYPE ("a(du)"));
      for (guint i = 0; i < G_N_ELEMENTS (c->steps) && c->steps[i].ms; i++)
        g_variant_builder_add (&steps, "(du)", c->steps[i].magnitude, c->steps[i].ms);
      call (bus, "org.sigxcpu.Feedback.Haptic", "Vibrate", g_variant_new ("(sa(du))", APP_ID, &steps));
    }
}
