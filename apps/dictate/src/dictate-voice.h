/* dictate-voice: text to speech on the phone itself, for the conversation (dictate-talk.c).
 *
 * Three kinds of model, all ONNX, run by onnxruntime on the CPU. Two read phonemes that espeak-ng makes from the
 * text:
 *   Kokoro  a directory with model*.onnx, tokenizer.json (its phonemes' ids) and voices/NAME.bin (a voice's styles,
 *           510 rows of 256 floats: the row is picked by the phonemes' count). 24 kHz. Trained on misaki's phoneme
 *           notation, which espeak-ng's IPA is turned into (kokoro_phonemes ()).
 *   Piper   NAME.onnx with NAME.onnx.json beside it (rate, espeak voice, phoneme ids, scales). Usually 22.05 kHz.
 * The third reads its own tokens:
 *   tiny-voca  Russian, a man's voice and a woman's: model.onnx with model.json beside it (its symbols, voices, rate:
 *           24 kHz), and its front end's dictionary and stress models (dictate-ru.h). A text is read a sentence at a
 *           time, a sentence too long for the model in its clauses.
 *
 * Not thread-safe: one voice is used from one thread at a time (espeak-ng is shared, under a lock). */
#pragma once

#include <gio/gio.h>

G_BEGIN_DECLS

typedef struct _DictateVoice DictateVoice;

/* model: a Kokoro model file (its directory holds tokenizer.json and voices/), a Piper one (MODEL.json beside it) or
 * tiny-voca's (model.json beside it); voice: Kokoro's voice (e.g. "af_heart", whose first letter is its language:
 * a American English, b British), ignored for the others; threads: onnxruntime's, 0 for its own choice */
DictateVoice *dictate_voice_load     (const char *model, const char *voice, int threads, GError **error);
void          dictate_voice_free     (DictateVoice *self);
guint         dictate_voice_get_rate (DictateVoice *self);
/* the phonemes `text` is spoken with, as the model reads them (for the log and the tests) */
char         *dictate_voice_phonemes (DictateVoice *self, const char *text, GError **error);
/* `text` spoken, at `speed` (1 normal, 2 twice as fast): *n mono samples, -1..1, at the voice's rate */
float        *dictate_voice_speak    (DictateVoice *self, const char *text, double speed, gsize *n, GError **error);

/* who speaks: "male" or "female", from the next text on; FALSE (and no change) for a voice that has no such speaker
 * (only tiny-voca has both) */
gboolean      dictate_voice_set_gender (DictateVoice *self, const char *gender);
/* the voice for `language` (two letters) where the voices are kept, the downloaded ones (dictate-download.h) and the
 * packaged ones: vibeos-dictate/voices/LANGUAGE in the user's data directory, then in the system's. A language
 * tiny-voca speaks has only it, in tiny-voca/; another the first in each by name of a Piper voice (NAME.onnx with
 * NAME.onnx.json beside it) and a Kokoro directory. NULL when there is none */
char         *dictate_voice_find       (const char *language);
/* `language`'s voice speaks as a man or a woman (dictate_voice_set_gender), downloaded yet or not */
gboolean      dictate_voice_has_genders (const char *language);

/* Agent's voice for `language`, as its settings (vibeos.Dictate) choose it: talk-voice's file where it is there, else
 * dictate_voice_find (language). *name: Kokoro's speaker (talk-voice-name), *gender: who speaks where the voice has a
 * man and a woman (talk-voice-gender, for dictate_voice_set_gender); NULL for the voice's own, and either pointer may
 * be NULL. settings NULL: Agent's settings as they are by default. NULL when there is no voice: Agent does not speak
 * then. Another app that speaks with Agent's voices (Navi) asks this too, with dictate_voice_settings (), so that it
 * speaks as Agent does */
char         *dictate_voice_choose     (GSettings *settings, const char *language, char **name, char **gender);
/* Agent's settings, for dictate_voice_choose () in another app: NULL where vibeos-dictate is not installed */
GSettings    *dictate_voice_settings   (void);
/* `key` is one of Agent's settings dictate_voice_choose () reads (for a "changed" handler) */
gboolean      dictate_voice_is_choice_key (const char *key);

G_END_DECLS
