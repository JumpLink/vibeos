/* DictateEngine: speech to text from the microphone, on the phone itself.
 *
 * The microphone is the user's default PipeWire/PulseAudio source, read through GStreamer at 16 kHz. The recogniser
 * is Moonshine's streaming transcriber with one of its small models, run on a thread of its own: it hears the
 * speech as it comes, and every UPDATE_MS reports what it has so far. Moonshine is not part of GNOME Phone; it is
 * loaded at run time from MOONSHINE_DIR, its models found under neo-dictate/models in the data directories (see
 * dictate-engine.c). */
#pragma once

#include <glib-object.h>

G_BEGIN_DECLS

typedef enum {
  DICTATE_IDLE,
  DICTATE_STARTING,     /* the microphone opening, the model loading (the first time) */
  DICTATE_LISTENING,
  DICTATE_STOPPING,     /* the microphone closed, the last words being recognised */
} DictateState;

#define DICTATE_TYPE_ENGINE (dictate_engine_get_type ())
G_DECLARE_FINAL_TYPE (DictateEngine, dictate_engine, DICTATE, ENGINE, GObject)

DictateEngine *dictate_engine_new       (void);
/* FALSE, with the reason for the user, when the recogniser or a model for it is not installed */
gboolean       dictate_engine_available (DictateEngine *self, char **reason);
void           dictate_engine_start     (DictateEngine *self);
/* the microphone closes at once; the words already heard still come, then the state goes back to idle */
void           dictate_engine_stop      (DictateEngine *self);
DictateState   dictate_engine_get_state (DictateEngine *self);
/* how loud the microphone is just now: 0 (quiet room) .. 1 (speech up close) */
double         dictate_engine_get_level (DictateEngine *self);

/* Signals, all on the main thread:
 *   "text" (GStrv finished, const char *partial): the sentences the speaker finished since the last "text", in
 *          order, and the one they are speaking now as heard so far ("" for none). A finished one never changes.
 *   "state-changed" ()
 *   "error" (const char *message): what went wrong, for the user; the engine stops */

G_END_DECLS
