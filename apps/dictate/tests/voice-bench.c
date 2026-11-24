/* voice-bench: how fast a voice (dictate-voice.h) speaks on this device.
 *
 *   voice-bench MODEL [-v VOICE] [-t THREADS] [-s SPEED] [-o DIR] [-r REPEATS] [TEXT…]
 *
 * Loads MODEL, says a warm-up sentence, then each TEXT (or a set of an assistant's typical answers): for each, the
 * phonemes' count, the time it took, the audio's length and their ratio (RTF: under 1 is faster than real time).
 * With -o, each one is written as DIR/NN.wav (16-bit mono) to listen to or to recognise. */
#include "dictate-voice.h"

#include <glib/gstdio.h>
#include <stdio.h>
#include <string.h>

static const char *const answers[] = {
  "Sure.",
  "It's ten past three in the afternoon.",
  "Good morning! You have two new messages, one from Sarah and one from the pharmacy.",
  "I set an alarm for six thirty tomorrow morning. Say undo to remove it.",
  "The weather today is mostly sunny, with a high of eighteen degrees and a light wind from the west, so a jacket "
  "should be enough if you go out this evening.",
  NULL,
};

static void
put16 (FILE *f, guint16 v)
{
  guint16 le = GUINT16_TO_LE (v);
  fwrite (&le, 2, 1, f);
}

static void
put32 (FILE *f, guint32 v)
{
  guint32 le = GUINT32_TO_LE (v);
  fwrite (&le, 4, 1, f);
}

static gboolean
write_wav (const char *path, const float *samples, gsize n, guint rate)
{
  FILE *f = g_fopen (path, "wb");
  if (!f)
    return FALSE;
  fwrite ("RIFF", 1, 4, f), put32 (f, 36 + n * 2), fwrite ("WAVEfmt ", 1, 8, f);
  put32 (f, 16), put16 (f, 1), put16 (f, 1), put32 (f, rate), put32 (f, rate * 2), put16 (f, 2), put16 (f, 16);
  fwrite ("data", 1, 4, f), put32 (f, n * 2);
  for (gsize i = 0; i < n; i++)
    put16 (f, (guint16) (gint16) (CLAMP (samples[i], -1.f, 1.f) * 32767.f));
  return fclose (f) == 0;
}

int
main (int argc, char **argv)
{
  g_autofree char *voice = NULL, *out = NULL;
  int threads = 0, repeats = 1;
  double speed = 1.0;
  g_autoptr (GError) error = NULL;
  g_autoptr (GOptionContext) context = g_option_context_new ("MODEL [TEXT…]");
  GOptionEntry entries[] = {
    { "voice", 'v', 0, G_OPTION_ARG_STRING, &voice, "Kokoro's voice", "NAME" },
    { "threads", 't', 0, G_OPTION_ARG_INT, &threads, "onnxruntime's threads (0: its choice)", "N" },
    { "speed", 's', 0, G_OPTION_ARG_DOUBLE, &speed, "speed (1 normal)", "X" },
    { "out", 'o', 0, G_OPTION_ARG_FILENAME, &out, "write the audio as DIR/NN.wav", "DIR" },
    { "repeats", 'r', 0, G_OPTION_ARG_INT, &repeats, "say each text this many times (the best time counts)", "N" },
    { NULL },
  };
  DictateVoice *v;
  gint64 t0;
  const char *const *texts;
  double total_audio = 0, total_time = 0;
  gsize n;
  float *samples;

  g_option_context_add_main_entries (context, entries, NULL);
  if (!g_option_context_parse (context, &argc, &argv, &error) || argc < 2)
    {
      g_printerr ("%s\n", error ? error->message : "a model, please");
      return 2;
    }
  t0 = g_get_monotonic_time ();
  v = dictate_voice_load (argv[1], voice, threads, &error);
  if (!v)
    {
      g_printerr ("load: %s\n", error->message);
      return 1;
    }
  printf ("load %.0f ms, rate %u Hz, threads %d\n", (g_get_monotonic_time () - t0) / 1000.0,
          dictate_voice_get_rate (v), threads);

  t0 = g_get_monotonic_time ();
  samples = dictate_voice_speak (v, "Hello there.", speed, &n, &error);
  if (!samples)
    {
      g_printerr ("speak: %s\n", error->message);
      return 1;
    }
  printf ("warm-up %.0f ms for %.2f s\n", (g_get_monotonic_time () - t0) / 1000.0,
          (double) n / dictate_voice_get_rate (v));
  g_free (samples);

  if (out)
    g_mkdir_with_parents (out, 0755);
  texts = argc > 2 ? (const char *const *) argv + 2 : answers;
  for (int i = 0; texts[i]; i++)
    {
      g_autofree char *phonemes = dictate_voice_phonemes (v, texts[i], NULL);
      double best = G_MAXDOUBLE, audio = 0;

      for (int r = 0; r < MAX (repeats, 1); r++)
        {
          double ms;
          t0 = g_get_monotonic_time ();
          samples = dictate_voice_speak (v, texts[i], speed, &n, &error);
          ms = (g_get_monotonic_time () - t0) / 1000.0;
          if (!samples)
            {
              g_printerr ("speak: %s\n", error->message);
              return 1;
            }
          best = MIN (best, ms);
          audio = (double) n / dictate_voice_get_rate (v);
          if (out && r == 0)
            {
              g_autofree char *path = g_strdup_printf ("%s/%02d.wav", out, i + 1);
              write_wav (path, samples, n, dictate_voice_get_rate (v));
            }
          g_free (samples);
        }
      total_audio += audio, total_time += best / 1000.0;
      printf ("%02d  %3ld phonemes  %6.0f ms  %5.2f s audio  RTF %.2f  | %s\n", i + 1,
              phonemes ? g_utf8_strlen (phonemes, -1) : -1, best, audio, best / 1000.0 / MAX (audio, 0.001),
              phonemes ? phonemes : "?");
    }
  printf ("total %.2f s for %.2f s of audio: RTF %.2f\n", total_time, total_audio, total_time / MAX (total_audio, 0.001));
  dictate_voice_free (v);
  return 0;
}
