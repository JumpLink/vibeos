/* talk-bench: Agent's whole loop on recorded questions, timed: the engine hears each sound file as if it were
 * the microphone (DICTATE_TEST_INPUT, in real time), finds the end of the speaker's turn (dictate-vad.h), and what
 * it heard goes to the model; the answer is spoken into a file (DICTATE_TEST_OUTPUT) instead of the speaker.
 *
 *   talk-bench [-l LANGUAGE] [-o PREFIX] [-p PAD] QUESTION.wav…
 *
 * Each file is the question followed by PAD seconds of silence (default 2): the speech ends PAD seconds before the
 * file does. For each question: when the turn's end was found after the speech ended, what was heard, and when the
 * first of the answer was ready to be heard after that, and the whole answer. Models are the app's own
 * (vibeos-dictate/brain, vibeos-dictate/voices/LANGUAGE, vibeos-dictate/vad under the data directories), settings
 * are vibeos.Dictate's (GSETTINGS_SCHEMA_DIR for a schema that is not installed). */
#include "dictate-engine.h"
#include "dictate-talk.h"

#include <gst/gst.h>
#include <stdio.h>
#include <string.h>

typedef struct {
  GMainLoop *loop;
  DictateEngine *engine;
  DictateTalk *talk;
  const char *language;
  char **files;
  guint next;
  double pad;
  /* this question's */
  GString *heard;
  char *partial;
  gint64 started, ended, first_audio;
  double speech_s;
  gboolean asked;
} Bench;

static void ask_next (Bench *b);

static double
ms (gint64 from, gint64 to)
{
  return (to - from) / 1000.0;
}

/* how long a sound file plays, from GStreamer's own reading of it */
static double
duration_of (const char *path)
{
  g_autofree char *uri = gst_filename_to_uri (path, NULL);
  g_autofree char *description = g_strdup_printf ("uridecodebin uri=\"%s\" ! fakesink", uri);
  GstElement *pipeline = gst_parse_launch (description, NULL);
  gint64 duration = 0;

  if (!pipeline)
    return 0;
  gst_element_set_state (pipeline, GST_STATE_PAUSED);
  gst_element_get_state (pipeline, NULL, NULL, 5 * GST_SECOND);
  gst_element_query_duration (pipeline, GST_FORMAT_TIME, &duration);
  gst_element_set_state (pipeline, GST_STATE_NULL);
  gst_object_unref (pipeline);
  return duration / (double) GST_SECOND;
}

static void
on_text (DictateEngine *engine, char **finished, const char *partial, Bench *b)
{
  for (guint i = 0; finished[i]; i++)
    {
      if (b->heard->len)
        g_string_append_c (b->heard, ' ');
      g_string_append (b->heard, finished[i]);
    }
  g_free (b->partial);
  b->partial = g_strdup (partial);
}

static void
on_engine_state (DictateEngine *engine, Bench *b)
{
  if (dictate_engine_get_state (engine) != DICTATE_IDLE || b->asked)
    return;
  b->ended = g_get_monotonic_time ();
  if (b->partial && *b->partial)
    g_string_append_printf (b->heard, "%s%s", b->heard->len ? " " : "", b->partial);
  printf ("  turn ended %.0f ms after the speech | heard: %s\n",
          ms (b->started, b->ended) - b->speech_s * 1000, b->heard->str);
  b->asked = TRUE;
  if (!b->heard->len)
    {
      printf ("  nothing heard\n");
      ask_next (b);
      return;
    }
  dictate_talk_ask (b->talk, b->language, b->heard->str);
}

static void
on_talk_state (DictateTalk *talk, Bench *b)
{
  DictateTalkState state = dictate_talk_get_state (talk);

  if (!b->asked)
    return;
  if (state == DICTATE_TALK_SPEAKING && !b->first_audio)
    {
      b->first_audio = g_get_monotonic_time ();
      printf ("  first audio %.0f ms after the turn ended (%.0f ms after the speech)\n", ms (b->ended, b->first_audio),
              ms (b->started, b->first_audio) - b->speech_s * 1000);
    }
  else if (state == DICTATE_TALK_IDLE)
    {
      printf ("  answer spoken %.0f ms after the turn ended\n", ms (b->ended, g_get_monotonic_time ()));
      ask_next (b);
    }
}

static void
on_talk_error (DictateTalk *talk, const char *message, Bench *b)
{
  printf ("  error: %s\n", message);
}

static void
on_last_reply (DictateTalk *talk, const char *text, gpointer data)
{
  g_free (g_object_get_data (G_OBJECT (talk), "last"));
  g_object_set_data (G_OBJECT (talk), "last", g_strdup (text));
}

static void
ask_next (Bench *b)
{
  const char *file;
  char *last = g_object_steal_data (G_OBJECT (b->talk), "last");

  if (last)
    {
      printf ("  answer: %s\n", last);
      g_free (last);
    }
  file = b->files[b->next];
  if (!file)
    {
      g_main_loop_quit (b->loop);
      return;
    }
  b->next++;
  g_string_truncate (b->heard, 0);
  g_clear_pointer (&b->partial, g_free);
  b->asked = FALSE;
  b->first_audio = 0;
  b->speech_s = duration_of (file) - b->pad;
  printf ("\n%s (%.1f s of speech)\n", file, b->speech_s);
  g_setenv ("DICTATE_TEST_INPUT", file, TRUE);
  b->started = g_get_monotonic_time ();
  dictate_engine_start (b->engine);
}

static gboolean
start (gpointer data)
{
  ask_next (data);
  return G_SOURCE_REMOVE;
}

int
main (int argc, char **argv)
{
  Bench b = { 0 };
  const char *language = "en", *prefix = "talk-bench-answer";
  double pad = 2.0;
  g_autoptr (GError) error = NULL;
  g_autoptr (GOptionContext) options = g_option_context_new ("QUESTION.wav…");
  g_autofree char *reason = NULL;
  GOptionEntry entries[] = {
    { "language", 'l', 0, G_OPTION_ARG_STRING, &language, "the questions' language (en)", "CODE" },
    { "output", 'o', 0, G_OPTION_ARG_STRING, &prefix, "where the answers go: PREFIX-N.wav", "PREFIX" },
    { "pad", 'p', 0, G_OPTION_ARG_DOUBLE, &pad, "seconds of silence after each question (2)", "S" },
    { NULL },
  };

  gst_init (&argc, &argv);
  g_option_context_add_main_entries (options, entries, NULL);
  if (!g_option_context_parse (options, &argc, &argv, &error) || argc < 2)
    {
      g_printerr ("%s\n", error ? error->message : "a question, please");
      return 2;
    }
  /* as the app runs Moonshine (dictate-main.c) */
  g_setenv ("MOONSHINE_ORT_SINGLE_THREAD", "1", FALSE);
  g_setenv ("DICTATE_TEST_OUTPUT", prefix, TRUE);

  b.loop = g_main_loop_new (NULL, FALSE);
  b.engine = dictate_engine_new ();
  b.talk = dictate_talk_new ();
  b.language = language;
  b.files = argv + 1;
  b.pad = pad;
  b.heard = g_string_new (NULL);
  dictate_engine_set_language (b.engine, language);
  dictate_engine_set_turns (b.engine, TRUE);
  if (!dictate_engine_available (b.engine, &reason) || !dictate_talk_available (b.talk, language, &reason))
    {
      g_printerr ("%s\n", reason);
      return 1;
    }
  g_signal_connect (b.engine, "text", G_CALLBACK (on_text), &b);
  g_signal_connect (b.engine, "state-changed", G_CALLBACK (on_engine_state), &b);
  g_signal_connect (b.talk, "reply", G_CALLBACK (on_last_reply), NULL);
  g_signal_connect (b.talk, "state-changed", G_CALLBACK (on_talk_state), &b);
  g_signal_connect (b.talk, "error", G_CALLBACK (on_talk_error), &b);

  /* the model and the voice loaded first, as the window does: the first answer is not the load's */
  {
    gint64 t0 = g_get_monotonic_time ();
    dictate_talk_prepare (b.talk, language);
    printf ("preparing (the model and the voice load in the background, %.0f ms so far)\n",
            ms (t0, g_get_monotonic_time ()));
  }
  g_timeout_add_seconds (8, start, &b);
  g_main_loop_run (b.loop);
  g_object_unref (b.talk);
  g_object_unref (b.engine);
  return 0;
}
