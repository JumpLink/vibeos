/* dictate-voicematch: see dictate-voicematch.h. Not built yet: there is never a sample, a check says so, and an
 * enrollment takes its recordings but cannot be saved. */
#include "dictate-voicematch.h"

#include <glib/gi18n.h>

G_DEFINE_QUARK (dictate-voice-match-error-quark, dictate_voice_match_error)

struct _DictateVoiceEnroll {
  guint count;
};

gboolean
dictate_voice_match_enrolled (void)
{
  return FALSE;
}

guint
dictate_voice_match_phrases (void)
{
  return 0;
}

gint64
dictate_voice_match_recorded (void)
{
  return 0;
}

gboolean
dictate_voice_match_delete (GError **error)
{
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
  g_set_error_literal (error, DICTATE_VOICE_MATCH_ERROR, DICTATE_VOICE_MATCH_ERROR_NOT_ENROLLED,
                       "No voice sample");
  return -1;
}

static void
verify_thread (GTask *task, gpointer source, gpointer data, GCancellable *cancellable)
{
  GError *error = NULL;
  double score = dictate_voice_match_verify (data, &error);

  if (error)
    g_task_return_error (task, error);
  else
    g_task_return_pointer (task, g_memdup2 (&score, sizeof score), g_free);
}

void
dictate_voice_match_verify_async (GBytes *audio, GCancellable *cancellable, GAsyncReadyCallback callback,
                                  gpointer data)
{
  g_autoptr (GTask) task = g_task_new (NULL, cancellable, callback, data);

  g_task_set_task_data (task, audio ? g_bytes_ref (audio) : g_bytes_new (NULL, 0), (GDestroyNotify) g_bytes_unref);
  g_task_run_in_thread (task, verify_thread);
}

double
dictate_voice_match_verify_finish (GAsyncResult *result, GError **error)
{
  g_autofree double *score = g_task_propagate_pointer (G_TASK (result), error);

  return score ? *score : -1;
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

  if (phrase < 0)
    return DICTATE_VOICE_PHRASE_TOO_SHORT;
  if (phrase == DICTATE_VOICE_PHRASE_OK)
    self->count++;
  return phrase;
}

guint
dictate_voice_enroll_count (DictateVoiceEnroll *self)
{
  return self->count;
}

gboolean
dictate_voice_enroll_save (DictateVoiceEnroll *self, GError **error)
{
  g_set_error_literal (error, DICTATE_VOICE_MATCH_ERROR, DICTATE_VOICE_MATCH_ERROR_UNAVAILABLE,
                       _("Voice Match is not ready yet"));
  return FALSE;
}

void
dictate_voice_enroll_free (DictateVoiceEnroll *self)
{
  g_free (self);
}
