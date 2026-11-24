/* brain-bench: a scripted conversation with a model (dictate-brain.h), timed the way talking to it feels.
 *
 *   brain-bench MODEL [-t THREADS] [-c CONTEXT] [TURN…]
 *
 * For each turn: the new prompt tokens read and how long that took, the time to the first written word and to the
 * first finished sentence (where a voice can start speaking), the tokens written and their rate, and the reply. */
#include "dictate-brain.h"

#include <stdio.h>
#include <string.h>

#define SYSTEM \
  "You are Vibe, a friendly voice assistant on a phone. The user speaks to you and hears your answers read aloud, " \
  "so answer the way people talk: one to three short sentences, no lists, no headings, no markdown, no emoji, no " \
  "links. You cannot do anything on the phone yet (no calls, messages, alarms or settings): if you are asked to, " \
  "say that you cannot do that yet. If you do not know something, say so."

static const char *const turns[] = {
  "Hi, my name is Alex. How are you?",
  "What's a good way to fall asleep faster?",
  "Can you say that again, but shorter?",
  "What's my name?",
  "Set an alarm for seven tomorrow morning.",
  "Tell me a fun fact about octopuses.",
  NULL,
};

typedef struct {
  gint64 start;
  double sentence_ms;
  GString *text;
} Turn;

static gboolean
on_piece (const char *piece, gpointer data)
{
  Turn *t = data;
  g_string_append (t->text, piece);
  if (t->sentence_ms < 0)
    {
      /* a sentence is finished at its mark followed by a space (the piece after it), or a newline */
      for (const char *p = t->text->str; *p; p++)
        if ((strchr (".!?", *p) && p[1] == ' ') || (*p == '\n' && p > t->text->str))
          {
            t->sentence_ms = (g_get_monotonic_time () - t->start) / 1000.0;
            break;
          }
    }
  return TRUE;
}

int
main (int argc, char **argv)
{
  int threads = 0, context = 0;
  g_autoptr (GError) error = NULL;
  g_autoptr (GOptionContext) options = g_option_context_new ("MODEL [TURN…]");
  GOptionEntry entries[] = {
    { "threads", 't', 0, G_OPTION_ARG_INT, &threads, "threads (0: the fastest cores)", "N" },
    { "context", 'c', 0, G_OPTION_ARG_INT, &context, "context tokens (0: 4096)", "N" },
    { NULL },
  };
  const char *const *script;
  DictateBrain *brain;
  gint64 t0;

  g_option_context_add_main_entries (options, entries, NULL);
  if (!g_option_context_parse (options, &argc, &argv, &error) || argc < 2)
    {
      g_printerr ("%s\n", error ? error->message : "a model, please");
      return 2;
    }
  t0 = g_get_monotonic_time ();
  brain = dictate_brain_load (argv[1], threads, context, &error);
  if (!brain)
    {
      g_printerr ("load: %s\n", error->message);
      return 1;
    }
  printf ("load %.0f ms\n", (g_get_monotonic_time () - t0) / 1000.0);
  dictate_brain_reset (brain, SYSTEM);
  script = argc > 2 ? (const char *const *) argv + 2 : turns;
  for (int i = 0; script[i]; i++)
    {
      Turn t = { g_get_monotonic_time (), -1, g_string_new (NULL) };
      g_autofree char *reply = dictate_brain_reply (brain, script[i], on_piece, &t, NULL, &error);
      int read, written;
      double read_ms, first_ms, total_ms;

      if (!reply)
        {
          g_printerr ("reply: %s\n", error->message);
          return 1;
        }
      if (t.sentence_ms < 0)
        t.sentence_ms = (g_get_monotonic_time () - t.start) / 1000.0;
      dictate_brain_last_timing (brain, &read, &written, &read_ms, &first_ms, &total_ms);
      printf ("\n> %s\n  read %d in %.0f ms | first word %.0f ms | first sentence %.0f ms | %d written in %.0f ms "
              "(%.1f/s)\n  %s\n", script[i], read, read_ms, first_ms, t.sentence_ms, written, total_ms,
              written / MAX ((total_ms - read_ms) / 1000.0, 0.001), g_strstrip (reply));
      g_string_free (t.text, TRUE);
    }
  dictate_brain_free (brain);
  return 0;
}
