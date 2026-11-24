/* navi-voice: the spoken directions.
 *
 * Spoken on the phone itself, never by a server. A Piper voice, Agent's (vibeos-dictate's dictate-voice.c: ONNX
 * Runtime, with espeak-ng's phonemes), from vibeos-dictate/voices/LANGUAGE in the user's data directory or the
 * system's: a voice downloaded for Agent speaks in Navi too. A language with no Piper voice is spoken through Speech
 * Dispatcher (eSpeak NG). Without either Navi is silent and Settings says so.
 *
 * A Piper prompt is made into sound on a thread of its own. navi_voice_prepare () makes it as soon as the app knows
 * it will be said (the next turn's instruction, its alert), so that navi_voice_say () only has to play it. The
 * prompts queue behind each other; none is cut off by the next. */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

typedef struct _NaviVoice NaviVoice;

NaviVoice *navi_voice_new (void);
void navi_voice_free (NaviVoice *self);
/* the voice for `language` (Valhalla's, "de-DE") loaded now, in the background: a Piper voice takes a second */
void navi_voice_warm (NaviVoice *self, const char *language);
/* the voice let go of (a Piper voice holds a hundred megabytes or more), and the prompts made with it: no drive
 * is near; the next navi_voice_warm () or prompt loads it again */
void navi_voice_rest (NaviVoice *self);
/* `text` made into sound now for a navi_voice_say () to come; the last few dozen are kept */
void navi_voice_prepare (NaviVoice *self, const char *text, const char *language);
/* says `text` in `language` */
void navi_voice_say (NaviVoice *self, const char *text, const char *language);
/* stops what is being said, and drops what is waiting */
void navi_voice_stop (NaviVoice *self);
/* the voices looked for again (one was just downloaded) */
void navi_voice_rescan (NaviVoice *self);
/* what speaks `language`, for Settings: "Piper voice Lessac", "eSpeak NG voices" or "No voice on this phone" */
const char *navi_voice_describe (NaviVoice *self, const char *language);
/* each Piper prompt's timings printed (--stats): how long it took to make, how soon it was heard; never its words */
void navi_voice_set_stats (NaviVoice *self, gboolean stats);

G_END_DECLS
