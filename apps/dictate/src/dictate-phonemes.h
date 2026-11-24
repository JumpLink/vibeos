/* dictate-phonemes: the voices' text front end (dictate-voice.c): espeak-ng's IPA, as piper-phonemize makes it, and
 * that IPA in misaki's notation for Kokoro. espeak-ng is used from one thread at a time (a lock). */
#pragma once

#include <gio/gio.h>

G_BEGIN_DECLS

/* espeak-ng's IPA for `text` in the voice `language` ("en-us"), clause by clause, each followed by its punctuation;
 * `tie`: multi-letter phonemes joined by '^' (a^ɪ), as dictate_kokoro_phonemes () expects */
char *dictate_espeak_phonemes (const char *language, const char *text, gboolean tie, GError **error);
/* tied IPA in misaki's notation, which Kokoro was trained on */
char *dictate_kokoro_phonemes (const char *ipa, gboolean british);

G_END_DECLS
