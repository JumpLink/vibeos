/* agent-eval-voice: Voice Match stood in for in the test set (dictate-voicematch.h): a sample is "enrolled" and a
 * voice is as close to it as the case says (agent_eval_voice_set ()); no audio is looked at and nothing is saved. */
#include "dictate-voicematch.h"

G_DEFINE_QUARK (dictate-voice-match-error-quark, dictate_voice_match_error)

static gboolean enrolled;
static double score = 100;

/* the case's voice: `close` in %, -1 for too little speech to tell, or -2 for one that cannot be checked (the
 * sample made for another model) */
void
agent_eval_voice_set (gboolean sample, double close)
{
  enrolled = sample;
  score = close;
}

struct _DictateVoiceEnroll {
  guint count;
};

gboolean
dictate_voice_match_ready (void)
{
  return TRUE;
}

gboolean
dictate_voice_match_stale (void)
{
  return enrolled && score == -2;
}

gboolean
dictate_voice_match_enrolled (void)
{
  return enrolled;
}

guint
dictate_voice_match_phrases (void)
{
  return enrolled ? DICTATE_VOICE_MATCH_PHRASES : 0;
}

gint64
dictate_voice_match_recorded (void)
{
  return 0;
}

gboolean
dictate_voice_match_delete (GError **error)
{
  enrolled = FALSE;
  return TRUE;
}

double
dictate_voice_match_needed (int strictness)
{
  return 55 + 0.4 * CLAMP (strictness, 0, 100);
}

double
dictate_voice_match_verify (GBytes *audio, GError **error)
{
  if (!enrolled)
    {
      g_set_error_literal (error, DICTATE_VOICE_MATCH_ERROR, DICTATE_VOICE_MATCH_ERROR_NOT_ENROLLED, "No voice sample");
      return -1;
    }
  if (score == -2)
    {
      g_set_error_literal (error, DICTATE_VOICE_MATCH_ERROR, DICTATE_VOICE_MATCH_ERROR_UNAVAILABLE,
                           "The voice sample was made for another Voice Match model: record it again");
      return -1;
    }
  if (score < 0)
    {
      g_set_error_literal (error, DICTATE_VOICE_MATCH_ERROR, DICTATE_VOICE_MATCH_ERROR_TOO_SHORT, "Too short");
      return -1;
    }
  return score;
}

void
dictate_voice_match_verify_async (GBytes *audio, GCancellable *cancellable, GAsyncReadyCallback callback,
                                  gpointer data)
{
  g_autoptr (GTask) task = g_task_new (NULL, cancellable, callback, data);
  GError *error = NULL;
  double close = dictate_voice_match_verify (audio, &error);

  if (error)
    g_task_return_error (task, error);
  else
    g_task_return_pointer (task, g_memdup2 (&close, sizeof close), g_free);
}

double
dictate_voice_match_verify_finish (GAsyncResult *result, GError **error)
{
  g_autofree double *close = g_task_propagate_pointer (G_TASK (result), error);

  return close ? *close : -1;
}

DictateVoiceEnroll *
dictate_voice_enroll_new (void)
{
  return g_new0 (DictateVoiceEnroll, 1);
}

void
dictate_voice_enroll_add_async (DictateVoiceEnroll *self, GBytes *audio, GCancellable *cancellable,
                                GAsyncReadyCallback callback, gpointer data)
{
  g_autoptr (GTask) task = g_task_new (NULL, cancellable, callback, data);

  g_task_return_int (task, DICTATE_VOICE_PHRASE_OK);
}

DictateVoicePhrase
dictate_voice_enroll_add_finish (DictateVoiceEnroll *self, GAsyncResult *result, GError **error)
{
  gssize phrase = g_task_propagate_int (G_TASK (result), error);

  if (phrase == DICTATE_VOICE_PHRASE_OK)
    self->count++;
  return phrase < 0 ? DICTATE_VOICE_PHRASE_TOO_SHORT : phrase;
}

guint
dictate_voice_enroll_count (DictateVoiceEnroll *self)
{
  return self->count;
}

gboolean
dictate_voice_enroll_save (DictateVoiceEnroll *self, GError **error)
{
  enrolled = TRUE;
  return TRUE;
}

void
dictate_voice_enroll_free (DictateVoiceEnroll *self)
{
  g_free (self);
}
