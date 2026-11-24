/* voicematch-bench: Voice Match on recordings, timed (not run by meson test: it needs the models and recordings).
 *
 *   voicematch-bench LIST        each line: "forget" (the sample removed), "enroll FILE", "save",
 *                                "check OWNER KIND SECONDS FILE"; prints a line for each, then the memory and CPU used
 *   voicematch-bench embed FILE REFERENCE...
 *                                the cosine of each model's embedding of all of FILE with its REFERENCE (a unit
 *                                vector, float32), for the filterbank and the model run against another's
 *
 * Recordings are raw float32, 16 kHz mono. The models come from $DICTATE_VOICE_MATCH_MODELS; the sample is made
 * under $XDG_DATA_HOME, so run it with a directory of its own. */
#include "dictate-voicematch.c"

#include <stdio.h>
#include <sys/resource.h>

static GBytes *
recording (const char *path)
{
  g_autoptr (GError) error = NULL;
  gchar *data = NULL;
  gsize len = 0;

  if (!g_file_get_contents (path, &data, &len, &error))
    g_error ("%s", error->message);
  return g_bytes_new_take (data, len - len % sizeof (float));
}

static double
ms_since (gint64 start)
{
  return (g_get_monotonic_time () - start) / 1000.0;
}

static void
memory (void)
{
  g_autofree char *status = NULL;
  struct rusage use;

  if (g_file_get_contents ("/proc/self/status", &status, NULL, NULL))
    {
      g_auto (GStrv) lines = g_strsplit (status, "\n", -1);

      for (guint i = 0; lines[i]; i++)
        if (g_str_has_prefix (lines[i], "VmHWM") || g_str_has_prefix (lines[i], "VmRSS"))
          printf ("# %s\n", lines[i]);
    }
  getrusage (RUSAGE_SELF, &use);
  printf ("# cpu user %.1f s, system %.1f s\n", use.ru_utime.tv_sec + use.ru_utime.tv_usec / 1e6,
          use.ru_stime.tv_sec + use.ru_stime.tv_usec / 1e6);
}

static int
embed_mode (int argc, char **argv)
{
  g_autoptr (GBytes) audio = recording (argv[2]);
  g_autoptr (GError) error = NULL;
  gsize n = g_bytes_get_size (audio) / sizeof (float), frames = 0;
  g_autofree float *feats = dictate_fbank (g_bytes_get_data (audio, NULL), n, TRUE, &frames);

  g_mutex_lock (&engine.lock);
  if (!engine_load (&error))
    g_error ("%s", error->message);
  for (guint m = 0; m < engine.count && 3 + m < (guint) argc; m++)
    {
      g_autoptr (GBytes) ref = recording (argv[3 + m]);
      const float *r = g_bytes_get_data (ref, NULL);
      gsize dim = 0;
      gint64 start = g_get_monotonic_time ();
      g_autofree float *v = embed (&engine.models[m], feats, frames, &dim, &error);
      double cos = 0;

      if (!v)
        g_error ("%s", error->message);
      if (dim != g_bytes_get_size (ref) / sizeof (float))
        g_error ("%s: %zu numbers, the reference %zu", engine.models[m].file, dim,
                 g_bytes_get_size (ref) / sizeof (float));
      for (gsize i = 0; i < dim; i++)
        cos += (double) v[i] * r[i];
      printf ("%s: %zu frames, dim %zu, cosine with the reference %.6f, %.0f ms\n", engine.models[m].file, frames, dim,
              cos, ms_since (start));
    }
  g_mutex_unlock (&engine.lock);
  return 0;
}

int
main (int argc, char **argv)
{
  g_autofree char *list = NULL;
  g_autoptr (DictateVoiceEnroll) e = NULL;
  g_auto (GStrv) lines = NULL;

  if (argc >= 4 && !strcmp (argv[1], "embed"))
    return embed_mode (argc, argv);
  if (argc != 2 || !g_file_get_contents (argv[1], &list, NULL, NULL))
    {
      fprintf (stderr, "usage: %s LIST | embed FILE REFERENCE...\n", argv[0]);
      return 2;
    }
  lines = g_strsplit (list, "\n", -1);
  for (guint i = 0; lines[i]; i++)
    {
      g_auto (GStrv) w = g_strsplit (lines[i], " ", -1);
      g_autoptr (GError) error = NULL;
      gint64 start = g_get_monotonic_time ();

      if (!w[0] || !*w[0])
        continue;
      if (!strcmp (w[0], "forget"))
        {
          dictate_voice_match_delete (NULL);
          g_clear_pointer (&e, dictate_voice_enroll_free);
          e = dictate_voice_enroll_new ();
        }
      else if (!strcmp (w[0], "enroll") && w[1])
        {
          g_autoptr (GBytes) audio = recording (w[1]);
          g_autoptr (GTask) task = NULL;
          Phrase *p;

          /* the worker's part on this thread, as the bench has no main loop */
          task = g_task_new (NULL, NULL, NULL, NULL);
          add_thread (task, NULL, audio, NULL);
          p = g_task_propagate_pointer (task, &error);
          if (!p)
            g_error ("%s", error->message);
          printf ("enroll\t%s\t%.1f s\t%s\t%.0f ms\n", w[1], p->seconds,
                  p->phrase == DICTATE_VOICE_PHRASE_OK ? "ok" : "refused", ms_since (start));
          if (p->phrase == DICTATE_VOICE_PHRASE_OK)
            {
              GTask *done = g_task_new (NULL, NULL, NULL, NULL);
              g_task_return_pointer (done, p, (GDestroyNotify) phrase_free);
              dictate_voice_enroll_add_finish (e, G_ASYNC_RESULT (done), NULL);
              g_object_unref (done);
            }
          else
            phrase_free (p);
        }
      else if (!strcmp (w[0], "save"))
        {
          if (!dictate_voice_enroll_save (e, &error))
            g_error ("%s", error->message);
          printf ("save\t%u recordings\n", dictate_voice_enroll_count (e));
        }
      else if (!strcmp (w[0], "check") && g_strv_length (w) == 5)
        {
          g_autoptr (GBytes) audio = recording (w[4]);
          double score = dictate_voice_match_verify (audio, &error);

          printf ("check\t%s\t%s\t%s\t%.1f\t%.0f ms\t%s\n", w[1], w[2], w[3], score, ms_since (start),
                  error ? error->message : "");
        }
      fflush (stdout);
    }
  memory ();
  return 0;
}
