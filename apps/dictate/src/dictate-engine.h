/* DictateEngine: speech to text from the microphone, on the phone itself.
 *
 * The microphone is the user's default PipeWire/PulseAudio source, read through GStreamer at 16 kHz. The recogniser
 * is the one for the chosen language's model (dictate-recogniser.h, dictate-models.h), run on a thread of its own:
 * it hears the speech as it comes, and every UPDATE_MS reports what it has so far. */
#pragma once

#include <glib-object.h>

G_BEGIN_DECLS

typedef enum {
  DICTATE_IDLE,
  DICTATE_STARTING,     /* the microphone opening, the model loading (the first time, or another language's) */
  DICTATE_LISTENING,
  DICTATE_STOPPING,     /* the microphone closed, the last words being recognised */
} DictateState;

#define DICTATE_TYPE_ENGINE (dictate_engine_get_type ())
G_DECLARE_FINAL_TYPE (DictateEngine, dictate_engine, DICTATE, ENGINE, GObject)

DictateEngine *dictate_engine_new          (void);
/* the language listened in, by its code (dictate-models.h): from the next start on */
void           dictate_engine_set_language (DictateEngine *self, const char *code);
const char    *dictate_engine_get_language (DictateEngine *self);
/* from the next start on: listening for one turn of the speaker's. The speech is cut out of the audio by voice
 * activity detection (dictate-vad.h) and only it is recognised; once the speaker has finished, or said nothing for
 * a while, the engine stops by itself (STOPPING is skipped: the state goes from listening to idle) */
void           dictate_engine_set_turns    (DictateEngine *self, gboolean turns);
/* FALSE, with the reason for the user, when the language's model or its recogniser is not installed */
gboolean       dictate_engine_available    (DictateEngine *self, char **reason);
void           dictate_engine_start        (DictateEngine *self);
/* the microphone closes at once; the words already heard still come, then the state goes back to idle */
void           dictate_engine_stop         (DictateEngine *self);
DictateState   dictate_engine_get_state    (DictateEngine *self);
/* how loud the microphone is just now: 0 (quiet room) .. 1 (speech up close) */
double         dictate_engine_get_level    (DictateEngine *self);

/* Signals, all on the main thread:
 *   "text" (GStrv finished, const char *partial): the sentences the speaker finished since the last "text", in
 *          order, and the one they are speaking now as heard so far ("" for none). A finished one never changes.
 *   "state-changed" ()
 *   "error" (const char *message): what went wrong, for the user; the engine stops */

G_END_DECLS
