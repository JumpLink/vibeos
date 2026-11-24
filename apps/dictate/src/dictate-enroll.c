/* dictate-enroll: see dictate-enroll.h. */
#include "dictate-enroll.h"
#include "dictate-cues.h"
#include "dictate-engine.h"
#include "dictate-l10n.h"
#include "dictate-stage.h"
#include "dictate-voicematch.h"

#include <glib/gi18n.h>
#include <vibeos.h>

#define GOT_IT_MS 900           /* "Got it." stays this long before the next part */

/* the passage, in its four phrases of 6 to 8 seconds each: the last a few short commands, which the check is then
 * good at too */
static const char *const PASSAGE[] = {
  N_("Vibe, what time is it now? I'm teaching you how my voice sounds, so that you only listen to me."),
  N_("Read my new messages out loud, then tell me what is on my calendar for tomorrow morning."),
  N_("Call my sister on her mobile, and set an alarm for seven thirty tomorrow."),
  N_("What's the time? Call Anna. Set a timer."),
};
G_STATIC_ASSERT (G_N_ELEMENTS (PASSAGE) == DICTATE_VOICE_MATCH_PHRASES);

typedef enum {
  READY,                        /* a part on the screen, waiting for a tap */
  SAYING,                       /* Vibe says the part */
  LISTENING,
  CHECKING,                     /* the recording checked */
  GOT,                          /* it will do: the next part comes */
  FINISHED,                     /* the sample saved: a command can be said to test it */
  TESTING,                      /* listening for the test's command */
  TESTED,                       /* how close it was, on the screen */
  FAILED,                       /* nothing saved: it cannot be done now */
} Phase;

typedef struct {
  GtkWidget *stage, *dots, *corner;
  GSettings *settings;
  DictateTalk *talk;
  char *language;
  DictateEngine *engine;
  DictateVoiceEnroll *enroll;
  GCancellable *cancellable;
  guint part, next_id;
  Phase phase;
  gboolean over;                /* done has been told */
  gboolean test;                /* only the test: nothing is recorded */
  gboolean own_dots;            /* the dots are the phrases' */
  DictateEnrollDone done;
  gpointer data;
} Enroll;

static void say_part (Enroll *e);

/* Vibe's words, in the language it speaks */
static const char *
vibe (Enroll *e, const char *msgid)
{
  return dictate_l10n (e->language, msgid);
}

static DictateOrb *
orb (Enroll *e)
{
  return dictate_stage_get_orb (DICTATE_STAGE (e->stage));
}

static void
finish (Enroll *e, gboolean saved)
{
  if (e->over)
    return;
  e->over = TRUE;
  g_cancellable_cancel (e->cancellable);
  if (dictate_engine_get_state (e->engine) != DICTATE_IDLE)
    dictate_engine_stop (e->engine);
  if (e->talk && dictate_talk_get_state (e->talk) != DICTATE_TALK_IDLE)
    dictate_talk_stop (e->talk);
  /* told last: it may take the screen away */
  e->done (saved, e->data);
}

/* the phrase to read, as Vibe asks for it: `note` under it ("Phrase 2 of 4.", or what was wrong with the last try) */
static void
show_part (Enroll *e, const char *note, gboolean again)
{
  DictateStage *stage = DICTATE_STAGE (e->stage);
  /* TRANSLATORS: a phrase to read aloud, in quotation marks */
  g_autofree char *quoted = g_strdup_printf (vibe (e, N_("“%s”")), vibe (e, PASSAGE[e->part]));

  e->phase = READY;
  if (e->own_dots)
    dictate_dots_set (e->dots, e->part);
  dictate_orb_set_look (orb (e), again ? DICTATE_ORB_DIM : DICTATE_ORB_REST, NULL);
  dictate_stage_set_who (stage, vibe (e, N_("Vibe says · repeat after me")), FALSE);
  dictate_stage_set_text (stage, quoted, note);
  /* while Vibe says it, the microphone opens when it is done */
  dictate_stage_set_hint (stage, again ? _("Tap anywhere and read it again") : _("Tap anywhere, then read the phrase"));
}

static void
listen (Enroll *e)
{
  DictateStage *stage = DICTATE_STAGE (e->stage);
  g_autofree char *reason = NULL;

  if (!dictate_engine_available (e->engine, &reason))
    {
      e->phase = FAILED;
      dictate_orb_set_look (orb (e), DICTATE_ORB_DIM, NULL);
      dictate_stage_set_who (stage, vibe (e, N_("Vibe says")), FALSE);
      dictate_stage_set_text (stage, vibe (e, N_("I can't listen right now.")), reason);
      dictate_stage_set_hint (stage, _("Tap anywhere to go back"));
      return;
    }
  dictate_orb_set_look (orb (e), DICTATE_ORB_LISTENING, NULL);
  if (e->phase == FINISHED || e->phase == TESTED)
    {
      /* the test: a command, checked and not carried out */
      e->phase = TESTING;
      dictate_stage_set_who (stage, vibe (e, N_("Listening")), FALSE);
      dictate_stage_set_text (stage, vibe (e, N_("Go ahead…")), vibe (e, N_("Say any command.")));
      dictate_stage_set_hint (stage, NULL);
    }
  else
    {
      /* TRANSLATORS: over the phrase being read: listening to phrase 2 of 4 */
      g_autofree char *who = g_strdup_printf (vibe (e, N_("Listening · phrase %u of %u")), e->part + 1,
                                              DICTATE_VOICE_MATCH_PHRASES);

      e->phase = LISTENING;
      dictate_stage_set_who (stage, who, FALSE);
      dictate_stage_set_hint (stage, _("Read the phrase at your normal pace"));
    }
  dictate_cue_play (e->settings, DICTATE_CUE_LISTENING);
  dictate_engine_set_language (e->engine, e->language);
  dictate_engine_set_turns (e->engine, TRUE);
  dictate_engine_start (e->engine);
}

/* the phrase said first, when Vibe speaks, then listened for; written only, it waits for a tap */
static void
say_part (Enroll *e)
{
  /* not while Vibe is answering something else */
  if (!e->talk || !g_settings_get_boolean (e->settings, "talk-speak")
      || dictate_talk_get_state (e->talk) != DICTATE_TALK_IDLE)
    {
      e->phase = READY;
      return;
    }
  e->phase = SAYING;
  dictate_stage_set_hint (DICTATE_STAGE (e->stage), _("Tap anywhere, or just start talking"));
  dictate_talk_announce (e->talk, e->language, vibe (e, PASSAGE[e->part]));
}

static void
on_talk_state (DictateTalk *talk, Enroll *e)
{
  if (e->phase == SAYING && dictate_talk_get_state (talk) == DICTATE_TALK_IDLE)
    listen (e);
}

static void
save (Enroll *e)
{
  DictateStage *stage = DICTATE_STAGE (e->stage);
  g_autoptr (GError) error = NULL;

  dictate_stage_set_hint (stage, NULL);
  if (!dictate_voice_enroll_save (e->enroll, &error))
    {
      e->phase = FAILED;
      g_message ("enroll: the sample was not saved: %s", error->message);
      dictate_orb_set_look (orb (e), DICTATE_ORB_DIM, NULL);
      dictate_stage_set_who (stage, vibe (e, N_("Vibe says")), FALSE);
      if (g_error_matches (error, DICTATE_VOICE_MATCH_ERROR, DICTATE_VOICE_MATCH_ERROR_UNAVAILABLE))
        dictate_stage_set_text (stage, vibe (e, N_("Voice Match is not ready yet.")),
                                vibe (e, N_("Your recordings were not kept, and nothing changed on the phone.")));
      else
        dictate_stage_set_text (stage, vibe (e, N_("I couldn't keep your voice sample.")), error->message);
      dictate_stage_set_hint (stage, _("Tap anywhere to go back"));
      dictate_cue_play (e->settings, DICTATE_CUE_FAILED);
      return;
    }
  e->phase = FINISHED;
  g_settings_set_boolean (e->settings, "voice-match", TRUE);
  if (e->own_dots)
    dictate_dots_set (e->dots, DICTATE_VOICE_MATCH_PHRASES);
  gtk_label_set_label (GTK_LABEL (vibeos_pressable_get_child (VIBEOS_PRESSABLE (e->corner))), _("Done"));
  dictate_orb_set_look (orb (e), DICTATE_ORB_DONE, NULL);
  dictate_stage_set_who (stage, vibe (e, N_("Vibe says")), FALSE);
  dictate_stage_set_text (stage, vibe (e, N_("That's all I need.")),
                          vibe (e, N_("Voice match is on. Say any command to test it. Nothing is carried out.")));
  dictate_stage_set_hint (stage, _("Tap anywhere, then speak"));
  dictate_cue_play (e->settings, DICTATE_CUE_DONE);
}

/* the test's answer: how close the command's voice was, against what the strictness needs */
static void
on_verified (GObject *source, GAsyncResult *result, gpointer data)
{
  Enroll *e = data;
  DictateStage *stage;
  g_autoptr (GError) error = NULL;
  double score = dictate_voice_match_verify_finish (result, &error);
  double needed;
  g_autofree char *sub = NULL;

  if (e->over)
    return;
  stage = DICTATE_STAGE (e->stage);
  e->phase = TESTED;
  dictate_stage_set_who (stage, vibe (e, N_("Vibe says")), FALSE);
  dictate_stage_set_hint (stage, _("Tap anywhere to try again"));
  if (error)
    {
      dictate_orb_set_look (orb (e), DICTATE_ORB_DIM, NULL);
      if (g_error_matches (error, DICTATE_VOICE_MATCH_ERROR, DICTATE_VOICE_MATCH_ERROR_TOO_SHORT))
        dictate_stage_set_text (stage, vibe (e, N_("Say a bit more.")),
                                vibe (e, N_("That was too short to tell whose voice it is.")));
      else
        dictate_stage_set_text (stage, vibe (e, N_("I couldn't check that.")), error->message);
      return;
    }
  needed = dictate_voice_match_needed (g_settings_get_int (e->settings, "voice-match-strictness"));
  if (score >= needed)
    {
      /* TRANSLATORS: a voice check's result: 88% match, 75% needed */
      sub = g_strdup_printf (vibe (e, N_("%.0f%% match, %.0f%% needed. Vibe would carry out the command.")),
                             score, needed);
      dictate_orb_set_look (orb (e), DICTATE_ORB_DONE, NULL);
      dictate_stage_set_text (stage, vibe (e, N_("That's you.")), sub);
    }
  else
    {
      /* TRANSLATORS: a voice check's result: 41% match, 75% needed */
      sub = g_strdup_printf (vibe (e, N_("%.0f%% match, %.0f%% needed. Vibe would not carry out the command.")),
                             score, needed);
      dictate_orb_set_look (orb (e), DICTATE_ORB_REFUSED, NULL);
      dictate_stage_set_who (stage, vibe (e, N_("Vibe says")), TRUE);
      dictate_stage_set_text (stage, vibe (e, N_("That didn't sound like you.")), sub);
    }
}

static gboolean
on_next (gpointer data)
{
  Enroll *e = data;

  e->next_id = 0;
  if (++e->part >= DICTATE_VOICE_MATCH_PHRASES)
    {
      save (e);
      return G_SOURCE_REMOVE;
    }
  {
    /* TRANSLATORS: under a phrase to read aloud: phrase 2 of 4 */
    g_autofree char *note = g_strdup_printf (vibe (e, N_("Phrase %u of %u.")), e->part + 1,
                                             DICTATE_VOICE_MATCH_PHRASES);

    show_part (e, note, FALSE);
  }
  say_part (e);
  return G_SOURCE_REMOVE;
}

static void
on_added (GObject *source, GAsyncResult *result, gpointer data)
{
  Enroll *e = data;
  g_autoptr (GError) error = NULL;
  DictateVoicePhrase phrase;

  if (g_cancellable_is_cancelled (e->cancellable) && e->over)
    return;
  phrase = dictate_voice_enroll_add_finish (e->enroll, result, &error);
  if (error)
    {
      e->phase = FAILED;
      g_message ("enroll: a recording could not be checked: %s", error->message);
      dictate_orb_set_look (orb (e), DICTATE_ORB_DIM, NULL);
      dictate_stage_set_who (DICTATE_STAGE (e->stage), vibe (e, N_("Vibe says")), FALSE);
      dictate_stage_set_text (DICTATE_STAGE (e->stage),
                              g_error_matches (error, DICTATE_VOICE_MATCH_ERROR,
                                               DICTATE_VOICE_MATCH_ERROR_UNAVAILABLE)
                              ? vibe (e, N_("Voice Match is not ready yet."))
                              : vibe (e, N_("I couldn't check that recording.")),
                              vibe (e, N_("Your recordings were not kept, and nothing changed on the phone.")));
      dictate_stage_set_hint (DICTATE_STAGE (e->stage), _("Tap anywhere to go back"));
      dictate_cue_play (e->settings, DICTATE_CUE_FAILED);
      return;
    }
  switch (phrase)
    {
    case DICTATE_VOICE_PHRASE_OK:
      e->phase = GOT;
      dictate_orb_set_look (orb (e), DICTATE_ORB_DONE, NULL);
      dictate_stage_set_who (DICTATE_STAGE (e->stage), vibe (e, N_("Vibe says")), FALSE);
      {
        g_autofree char *quoted = g_strdup_printf (vibe (e, N_("“%s”")), vibe (e, PASSAGE[e->part]));

        dictate_stage_set_text (DICTATE_STAGE (e->stage), quoted, vibe (e, N_("Got it.")));
      }
      dictate_stage_set_hint (DICTATE_STAGE (e->stage), NULL);
      e->next_id = g_timeout_add (GOT_IT_MS, on_next, e);
      return;
    case DICTATE_VOICE_PHRASE_TOO_SHORT:
      show_part (e, vibe (e, N_("That was too short. Please read all of the phrase.")), TRUE);
      break;
    case DICTATE_VOICE_PHRASE_TOO_QUIET:
      show_part (e, vibe (e, N_("That was too quiet. Read it again, a little closer to the phone.")), TRUE);
      break;
    case DICTATE_VOICE_PHRASE_TOO_NOISY:
      show_part (e, vibe (e, N_("There was too much noise. Read it again somewhere quieter.")), TRUE);
      break;
    }
  dictate_cue_play (e->settings, DICTATE_CUE_FAILED);
}

static void
on_engine_state (DictateEngine *engine, Enroll *e)
{
  g_autoptr (GBytes) audio = NULL;

  if ((e->phase != LISTENING && e->phase != TESTING) || dictate_engine_get_state (engine) != DICTATE_IDLE)
    return;
  audio = dictate_engine_take_turn_audio (engine);
  if (e->phase == TESTING)
    {
      if (!audio)
        {
          e->phase = TESTED;
          dictate_orb_set_look (orb (e), DICTATE_ORB_DIM, NULL);
          dictate_stage_set_who (DICTATE_STAGE (e->stage), vibe (e, N_("Vibe says")), FALSE);
          dictate_stage_set_text (DICTATE_STAGE (e->stage), vibe (e, N_("I didn't hear anything.")), NULL);
          dictate_stage_set_hint (DICTATE_STAGE (e->stage), _("Tap anywhere to try again"));
          return;
        }
      dictate_orb_set_look (orb (e), DICTATE_ORB_WORKING, NULL);
      dictate_voice_match_verify_async (audio, e->cancellable, on_verified, e);
      return;
    }
  if (!audio)
    {
      show_part (e, vibe (e, N_("I didn't hear anything.")), TRUE);
      return;
    }
  e->phase = CHECKING;
  dictate_orb_set_look (orb (e), DICTATE_ORB_WORKING, NULL);
  dictate_stage_set_hint (DICTATE_STAGE (e->stage), NULL);
  dictate_voice_enroll_add_async (e->enroll, audio, e->cancellable, on_added, e);
}

static void
on_engine_error (DictateEngine *engine, const char *message, Enroll *e)
{
  if (e->phase == TESTING)
    {
      e->phase = TESTED;
      dictate_orb_set_look (orb (e), DICTATE_ORB_DIM, NULL);
      dictate_stage_set_text (DICTATE_STAGE (e->stage), vibe (e, N_("I couldn't listen.")), message);
      dictate_stage_set_hint (DICTATE_STAGE (e->stage), _("Tap anywhere to try again"));
      return;
    }
  show_part (e, message, TRUE);
}

static void
on_tapped (DictateStage *stage, Enroll *e)
{
  switch (e->phase)
    {
    case READY:
    case FINISHED:
    case TESTED:
      listen (e);
      break;
    case SAYING:
      /* read now, without waiting for Vibe to finish saying it */
      e->phase = READY;
      dictate_talk_stop (e->talk);
      listen (e);
      break;
    case LISTENING:
    case TESTING:
      dictate_engine_stop (e->engine);
      break;
    case FAILED:
      finish (e, FALSE);
      break;
    case CHECKING:
    case GOT:
      break;
    }
}

static void
on_corner (VibeosPressable *button, Enroll *e)
{
  finish (e, !e->test && (e->phase == FINISHED || e->phase == TESTING || e->phase == TESTED));
}

static void
enroll_free (gpointer data)
{
  Enroll *e = data;

  g_cancellable_cancel (e->cancellable);
  g_clear_object (&e->cancellable);
  g_clear_handle_id (&e->next_id, g_source_remove);
  g_signal_handlers_disconnect_by_data (e->engine, e);
  dictate_engine_stop (e->engine);
  g_clear_object (&e->engine);
  if (e->talk)
    {
      g_signal_handlers_disconnect_by_data (e->talk, e);
      g_clear_object (&e->talk);
    }
  /* not saved: the recordings go, the old sample stays */
  g_clear_pointer (&e->enroll, dictate_voice_enroll_free);
  g_clear_object (&e->settings);
  g_free (e->language);
  g_free (e);
}

static Enroll *
enroll_new (GSettings *settings, DictateTalk *talk, DictateEngine *engine, const char *language, const char *corner,
            DictateEnrollDone done, gpointer data)
{
  Enroll *e = g_new0 (Enroll, 1);

  e->settings = g_object_ref (settings);
  e->talk = talk ? g_object_ref (talk) : NULL;
  e->language = g_strdup (language);
  e->engine = engine ? g_object_ref (engine) : dictate_engine_new ();
  e->enroll = dictate_voice_enroll_new ();
  e->cancellable = g_cancellable_new ();
  e->done = done;
  e->data = data;
  e->stage = dictate_stage_new (e->engine);
  e->dots = dictate_dots_new (DICTATE_VOICE_MATCH_PHRASES);
  e->own_dots = TRUE;
  dictate_stage_set_header_start (DICTATE_STAGE (e->stage), e->dots);
  e->corner = vibeos_text_button_new (corner);
  g_signal_connect (e->corner, "clicked", G_CALLBACK (on_corner), e);
  dictate_stage_set_header_end (DICTATE_STAGE (e->stage), e->corner);
  g_signal_connect (e->stage, "tapped", G_CALLBACK (on_tapped), e);
  g_signal_connect (e->engine, "state-changed", G_CALLBACK (on_engine_state), e);
  g_signal_connect (e->engine, "error", G_CALLBACK (on_engine_error), e);
  if (e->talk)
    g_signal_connect (e->talk, "state-changed", G_CALLBACK (on_talk_state), e);
  g_object_set_data_full (G_OBJECT (e->stage), "dictate-enroll", e, enroll_free);
  return e;
}

GtkWidget *
dictate_enroll_new (GSettings *settings, DictateTalk *talk, DictateEngine *engine, const char *language,
                    const char *corner, DictateEnrollDone done, gpointer data)
{
  Enroll *e = enroll_new (settings, talk, engine, language, corner, done, data);

  show_part (e, vibe (e, N_("I'll learn your voice from 4 short phrases, so I only take commands from you. "
                             "The sample stays on this phone.")), FALSE);
  return e->stage;
}

GtkWidget *
dictate_enroll_test_new (GSettings *settings, const char *language, DictateEnrollDone done, gpointer data)
{
  Enroll *e = enroll_new (settings, NULL, NULL, language, _("Done"), done, data);
  DictateStage *stage = DICTATE_STAGE (e->stage);

  e->test = TRUE;
  e->phase = FINISHED;
  gtk_widget_set_visible (e->dots, FALSE);
  dictate_stage_set_title (stage, _("Test voice match"));
  dictate_stage_set_header_start (stage, NULL);
  dictate_stage_set_who (stage, vibe (e, N_("Vibe says")), FALSE);
  dictate_stage_set_text (stage, vibe (e, N_("Say any command.")),
                          vibe (e, N_("For example “Vibe, what's the weather?” Nothing is carried out during a test.")));
  dictate_stage_set_hint (stage, _("Tap anywhere, then speak"));
  return e->stage;
}

void
dictate_enroll_set_dots (GtkWidget *enroll, GtkWidget *dots)
{
  Enroll *e = g_object_get_data (G_OBJECT (enroll), "dictate-enroll");

  e->dots = dots;
  e->own_dots = FALSE;
  dictate_stage_set_header_start (DICTATE_STAGE (e->stage), dots);
}

void
dictate_enroll_start (GtkWidget *enroll)
{
  Enroll *e = g_object_get_data (G_OBJECT (enroll), "dictate-enroll");

  if (e && e->phase == READY && e->part == 0)
    say_part (e);
}
