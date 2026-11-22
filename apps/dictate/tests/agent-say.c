/* agent-say: Agent's loop from typed text, the microphone left out: each TEXT is a turn the person said, all in one
 * conversation; the reply comes from Agent's model (the talk-brain setting, VibeOS's server by default) and is
 * spoken as the app speaks it, to the default output (-o: into PREFIX-N.wav instead).
 *
 *   agent-say [-l LANGUAGE] [-o PREFIX] [-w SECONDS] TEXT…
 *
 * The model and the voice are loaded first, as the window does (-w: how long that is given, 8 s), so that the first
 * reply is not timed with the load. For each turn: when the first of the reply was written, when the reply started
 * to sound and when the whole of it had been spoken, from the moment it was asked; then the reply. The exit status
 * is 0 when every turn was answered and spoken, 1 otherwise. Models and settings as talk-bench's. */
#include "dictate-talk.h"

#include <gst/gst.h>
#include <stdio.h>

#define TURN_LIMIT_S 180

typedef struct {
  GMainLoop *loop;
  DictateTalk *talk;
  const char *language;
  char **texts;
  guint next;
  /* this turn's */
  gint64 asked, written, sounded;
  char *reply;
  gboolean waiting, failed;
  guint limit;
  /* all turns' */
  guint answered;
} Say;

static void ask_next (Say *s);

static double
ms (gint64 from, gint64 to)
{
  return (to - from) / 1000.0;
}

static void
on_reply (DictateTalk *talk, const char *text, Say *s)
{
  if (!s->waiting)
    return;
  if (!s->written && text && *text)
    s->written = g_get_monotonic_time ();
  g_free (s->reply);
  s->reply = g_strdup (text);
}

static void
on_error (DictateTalk *talk, const char *message, Say *s)
{
  printf ("  error: %s\n", message);
  fflush (stdout);
  s->failed = TRUE;
}

static void
turn_done (Say *s)
{
  gint64 now = g_get_monotonic_time ();

  s->waiting = FALSE;
  g_clear_handle_id (&s->limit, g_source_remove);
  if (s->written)
    printf ("  first words written %.0f ms after the question\n", ms (s->asked, s->written));
  if (s->sounded)
    printf ("  speaking from %.0f ms, all spoken at %.0f ms\n", ms (s->asked, s->sounded), ms (s->asked, now));
  printf ("  reply: %s\n", s->reply && *s->reply ? s->reply : "(none)");
  fflush (stdout);
  if (!s->failed && s->sounded && s->reply && *s->reply)
    s->answered++;
  ask_next (s);
}

static void
on_state (DictateTalk *talk, Say *s)
{
  DictateTalkState state = dictate_talk_get_state (talk);

  if (!s->waiting)
    return;
  if (state == DICTATE_TALK_SPEAKING && !s->sounded)
    s->sounded = g_get_monotonic_time ();
  else if (state == DICTATE_TALK_IDLE)
    turn_done (s);
}

static gboolean
too_long (gpointer data)
{
  Say *s = data;

  s->limit = 0;
  printf ("  no end to the reply after %d s: stopped\n", TURN_LIMIT_S);
  s->failed = TRUE;
  dictate_talk_stop (s->talk);
  if (s->waiting)
    turn_done (s);
  return G_SOURCE_REMOVE;
}

static void
ask_next (Say *s)
{
  const char *text = s->texts[s->next];

  if (!text)
    {
      g_main_loop_quit (s->loop);
      return;
    }
  s->next++;
  s->written = s->sounded = 0;
  s->failed = FALSE;
  g_clear_pointer (&s->reply, g_free);
  printf ("\nsaid: %s\n", text);
  fflush (stdout);
  s->waiting = TRUE;
  s->asked = g_get_monotonic_time ();
  s->limit = g_timeout_add_seconds (TURN_LIMIT_S, too_long, s);
  dictate_talk_ask (s->talk, s->language, text);
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
  Say s = { 0 };
  const char *language = "en";
  char *prefix = NULL;
  int wait_s = 8;
  guint turns;
  g_autoptr (GError) error = NULL;
  g_autoptr (GOptionContext) options = g_option_context_new ("TEXT…");
  g_autofree char *reason = NULL;
  GOptionEntry entries[] = {
    { "language", 'l', 0, G_OPTION_ARG_STRING, &language, "the turns' language (en)", "CODE" },
    { "output", 'o', 0, G_OPTION_ARG_STRING, &prefix, "the replies into PREFIX-N.wav, not to the speaker", "PREFIX" },
    { "wait", 'w', 0, G_OPTION_ARG_INT, &wait_s, "seconds given to loading the model and the voice (8)", "S" },
    { NULL },
  };

  gst_init (&argc, &argv);
  g_option_context_add_main_entries (options, entries, NULL);
  if (!g_option_context_parse (options, &argc, &argv, &error) || argc < 2)
    {
      g_printerr ("%s\n", error ? error->message : "something to say, please");
      return 2;
    }
  if (prefix)
    g_setenv ("DICTATE_TEST_OUTPUT", prefix, TRUE);

  s.loop = g_main_loop_new (NULL, FALSE);
  s.talk = dictate_talk_new ();
  s.language = language;
  s.texts = argv + 1;
  turns = argc - 1;
  if (!dictate_talk_available (s.talk, language, &reason))
    {
      g_printerr ("%s\n", reason);
      return 1;
    }
  g_signal_connect (s.talk, "reply", G_CALLBACK (on_reply), &s);
  g_signal_connect (s.talk, "state-changed", G_CALLBACK (on_state), &s);
  g_signal_connect (s.talk, "error", G_CALLBACK (on_error), &s);
  printf ("loading the model and the voice (%d s)\n", wait_s);
  fflush (stdout);
  dictate_talk_prepare (s.talk, language);
  g_timeout_add_seconds (wait_s, start, &s);
  g_main_loop_run (s.loop);
  printf ("\n%u of %u turns answered and spoken\n", s.answered, turns);
  g_object_unref (s.talk);
  g_free (s.reply);
  return s.answered == turns ? 0 : 1;
}
