/* dictate-voicematch: Voice Match, on the phone only. The owner's voice sample (what enrollment makes of their
 * recordings) is kept 0600 in vibeos-dictate/voice-match under the user's data directory; it is never sent
 * anywhere, never logged, and never in the debug log. Voice Match answers how close a turn's voice is to it; it never
 * unlocks the phone.
 *
 * Audio, everywhere here: 16 kHz mono float32 samples (-1..1), the speech part of one turn as the engine cut it
 * (dictate_engine_take_turn_audio ()).
 *
 * The voice check of a turn is made by DictateTalk (dictate_talk_ask_full ()), from the settings voice-match (on),
 * voice-match-strictness and voice-match-everyday, -personal and -actions (dictate_tools_risk ()), and only while a
 * sample is enrolled. */
#pragma once

#include <gio/gio.h>

G_BEGIN_DECLS

#define DICTATE_VOICE_MATCH_PHRASES 4   /* phrases an enrollment asks for */

typedef enum {
  DICTATE_VOICE_PHRASE_OK,
  DICTATE_VOICE_PHRASE_TOO_SHORT,       /* too little speech in it */
  DICTATE_VOICE_PHRASE_TOO_QUIET,
  DICTATE_VOICE_PHRASE_TOO_NOISY,       /* the noise, or the speech clipped */
} DictateVoicePhrase;

#define DICTATE_VOICE_MATCH_ERROR (dictate_voice_match_error_quark ())
typedef enum {
  DICTATE_VOICE_MATCH_ERROR_NOT_ENROLLED,
  DICTATE_VOICE_MATCH_ERROR_TOO_SHORT,  /* too little speech to tell whose voice it is */
  DICTATE_VOICE_MATCH_ERROR_UNAVAILABLE, /* the model is not installed, or could not run */
} DictateVoiceMatchError;
GQuark   dictate_voice_match_error_quark (void);

/* whether a sample can be made on this phone (the model is installed and runs): until then, nothing asks for one */
gboolean dictate_voice_match_ready     (void);
gboolean dictate_voice_match_enrolled  (void);
/* a sample, and it cannot be checked against: Voice Match is not installed any more, or its model changed (an update)
 * since the sample was made. It has to be recorded again (or removed); until then Personal and Actions are not run
 * by voice (DictateTalk's "refused", DICTATE_REFUSED_UNCHECKED). The first call after a model changed reads it
 * (a fraction of a second); call it off the main thread, or when the settings page opens, not per frame */
gboolean dictate_voice_match_stale     (void);
/* the recordings in the saved sample, 0 for none */
guint    dictate_voice_match_phrases   (void);
/* when the sample was saved, Unix seconds, 0 for none */
gint64   dictate_voice_match_recorded  (void);
/* the sample removed */
gboolean dictate_voice_match_delete    (GError **error);
/* the match needed, in %, for voice-match-strictness (0 lenient .. 100 strict; 50, balanced, needs 75 %) */
double   dictate_voice_match_needed    (int strictness);

/* how close `audio` is to the sample: 0..100 (%), or -1 when there is no sample (with the error, as for too
 * little speech) */
void     dictate_voice_match_verify_async  (GBytes *audio, GCancellable *cancellable, GAsyncReadyCallback callback,
                                            gpointer data);
double   dictate_voice_match_verify_finish (GAsyncResult *result, GError **error);
/* the same, on the calling thread (the brain's) */
double   dictate_voice_match_verify        (GBytes *audio, GError **error);

/* an enrollment: recordings added one by one, each checked; saved, it replaces the old sample (only then) */
typedef struct _DictateVoiceEnroll DictateVoiceEnroll;

DictateVoiceEnroll *dictate_voice_enroll_new        (void);
void                dictate_voice_enroll_add_async  (DictateVoiceEnroll *self, GBytes *audio,
                                                     GCancellable *cancellable, GAsyncReadyCallback callback,
                                                     gpointer data);
/* OK: the recording is kept for the sample; otherwise what was wrong with it (and it is not kept) */
DictateVoicePhrase  dictate_voice_enroll_add_finish (DictateVoiceEnroll *self, GAsyncResult *result,
                                                     GError **error);
/* the recordings kept so far */
guint               dictate_voice_enroll_count      (DictateVoiceEnroll *self);
gboolean            dictate_voice_enroll_save       (DictateVoiceEnroll *self, GError **error);
/* not saved: its recordings are dropped, and the old sample stays */
void                dictate_voice_enroll_free       (DictateVoiceEnroll *self);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (DictateVoiceEnroll, dictate_voice_enroll_free)

G_END_DECLS
