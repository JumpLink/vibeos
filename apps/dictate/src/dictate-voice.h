/* dictate-voice: text to speech on the phone itself, for the conversation (dictate-talk.c).
 *
 * Two kinds of model, both ONNX, run by onnxruntime on the CPU, both reading phonemes that espeak-ng makes from the
 * text:
 *   Kokoro  a directory with model*.onnx, tokenizer.json (its phonemes' ids) and voices/NAME.bin (a voice's styles,
 *           510 rows of 256 floats: the row is picked by the phonemes' count). 24 kHz. Trained on misaki's phoneme
 *           notation, which espeak-ng's IPA is turned into (kokoro_phonemes ()).
 *   Piper   NAME.onnx with NAME.onnx.json beside it (rate, espeak voice, phoneme ids, scales). Usually 22.05 kHz.
 *
 * Not thread-safe: one voice is used from one thread at a time (espeak-ng is shared, under a lock). */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

typedef struct _DictateVoice DictateVoice;

/* model: a Kokoro model file (its directory holds tokenizer.json and voices/) or a Piper one (MODEL.json beside it);
 * voice: Kokoro's voice (e.g. "af_heart", whose first letter is its language: a American English, b British),
 * ignored for Piper; threads: onnxruntime's, 0 for its own choice */
DictateVoice *dictate_voice_load     (const char *model, const char *voice, int threads, GError **error);
void          dictate_voice_free     (DictateVoice *self);
guint         dictate_voice_get_rate (DictateVoice *self);
/* the phonemes `text` is spoken with, as the model reads them (for the log and the tests) */
char         *dictate_voice_phonemes (DictateVoice *self, const char *text, GError **error);
/* `text` spoken, at `speed` (1 normal, 2 twice as fast): *n mono samples, -1..1, at the voice's rate */
float        *dictate_voice_speak    (DictateVoice *self, const char *text, double speed, gsize *n, GError **error);

G_END_DECLS
