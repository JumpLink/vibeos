/* dictate-phonemes: see dictate-phonemes.h */
#include "dictate-phonemes.h"

#include <espeak-ng/speak_lib.h>
#include <string.h>

G_LOCK_DEFINE_STATIC (espeak);
static gboolean espeak_ready;
static char *espeak_language;

/* espeak-ng's clause terminators (translate.h), as piper-phonemize reads them */
#define CLAUSE_INTONATION_FULL_STOP   0x00000000
#define CLAUSE_INTONATION_COMMA       0x00001000
#define CLAUSE_INTONATION_QUESTION    0x00002000
#define CLAUSE_INTONATION_EXCLAMATION 0x00003000
#define CLAUSE_TYPE_CLAUSE            0x00040000
#define CLAUSE_TYPE_SENTENCE          0x00080000
#define CLAUSE_PERIOD      (40 | CLAUSE_INTONATION_FULL_STOP | CLAUSE_TYPE_SENTENCE)
#define CLAUSE_COMMA       (20 | CLAUSE_INTONATION_COMMA | CLAUSE_TYPE_CLAUSE)
#define CLAUSE_QUESTION    (40 | CLAUSE_INTONATION_QUESTION | CLAUSE_TYPE_SENTENCE)
#define CLAUSE_EXCLAMATION (45 | CLAUSE_INTONATION_EXCLAMATION | CLAUSE_TYPE_SENTENCE)
#define CLAUSE_COLON       (30 | CLAUSE_INTONATION_FULL_STOP | CLAUSE_TYPE_CLAUSE)
#define CLAUSE_SEMICOLON   (30 | CLAUSE_INTONATION_COMMA | CLAUSE_TYPE_CLAUSE)

static char
terminator_mark (int terminator)
{
  switch (terminator & 0x000fffff)
    {
    case CLAUSE_PERIOD:      return '.';
    case CLAUSE_QUESTION:    return '?';
    case CLAUSE_EXCLAMATION: return '!';
    case CLAUSE_COMMA:       return ',';
    case CLAUSE_COLON:       return ':';
    case CLAUSE_SEMICOLON:   return ';';
    default:                 return 0;
    }
}

/* espeak-ng's IPA for `text`, clause by clause, each followed by its punctuation; `tie`: multi-letter phonemes
 * joined by '^' (a^ɪ), as misaki's conversion expects */
char *
dictate_espeak_phonemes (const char *language, const char *text, gboolean tie, GError **error)
{
  g_autoptr (GString) out = g_string_new (NULL);
  int mode = espeakPHONEMES_IPA | (tie ? espeakPHONEMES_TIE | ('^' << 8) : 0);
  const void *at = text;

  G_LOCK (espeak);
  if (!espeak_ready)
    {
      if (espeak_Initialize (AUDIO_OUTPUT_SYNCHRONOUS, 0, NULL, espeakINITIALIZE_DONT_EXIT) < 0)
        {
          G_UNLOCK (espeak);
          g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED, "espeak-ng did not start");
          return NULL;
        }
      espeak_ready = TRUE;
    }
  if (g_strcmp0 (espeak_language, language) != 0)
    {
      /* a voice's name ("en-us"), else a language ("en-gb" is the voice "en"'s) */
      espeak_VOICE wanted = { .languages = language };
      if (espeak_SetVoiceByName (language) != EE_OK && espeak_SetVoiceByProperties (&wanted) != EE_OK)
        {
          G_UNLOCK (espeak);
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "espeak-ng has no voice %s", language);
          return NULL;
        }
      g_free (espeak_language);
      espeak_language = g_strdup (language);
    }
  while (at && *(const char *) at)
    {
      int terminator = 0;
      const char *clause = espeak_TextToPhonemesWithTerminator (&at, espeakCHARS_UTF8, mode, &terminator);
      g_autofree char *trimmed = g_strstrip (g_strdup (clause ? clause : ""));
      char mark = terminator_mark (terminator);

      if (!*trimmed && !mark)
        continue;
      if (out->len)
        g_string_append_c (out, ' ');
      g_string_append (out, trimmed);
      if (mark)
        g_string_append_c (out, mark);
    }
  G_UNLOCK (espeak);
  return g_string_free (g_steal_pointer (&out), FALSE);
}

static void
replace_all (GString *s, const char *const *pairs)
{
  for (; pairs[0]; pairs += 2)
    g_string_replace (s, pairs[0], pairs[1], 0);
}

/* espeak-ng's IPA (with '^' ties) in misaki's notation, which Kokoro was trained on (misaki/espeak.py's
 * EspeakFallback): diphthongs and affricates are one letter (aɪ → I, oʊ → O, tʃ → ʧ), the American flap is T, and
 * American vowels carry no length mark. Longest first. */
char *
dictate_kokoro_phonemes (const char *ipa, gboolean british)
{
  static const char *const common[] = {
    "ʔˌn" "\xcc\xa9", "tᵊn", "ʔn" "\xcc\xa9", "tᵊn", "ʔn", "tᵊn", "ʔ", "t",
    "a^ɪ", "I", "a^ʊ", "W", "d^ʒ", "ʤ", "e^ɪ", "A", "t^ʃ", "ʧ", "ɔ^ɪ", "Y", "ə^l", "ᵊl",
    "e", "A", "r", "ɹ", "x", "k", "ç", "k", "ɐ", "ə", "ɚ", "əɹ", "ɬ", "l", "ʲ", "",
    NULL,
  };
  static const char *const american[] = {
    "o^ʊ", "O", "ɜːɹ", "ɜɹ", "ɜː", "ɜɹ", "ɪ^ə", "iə", "ɾ", "T", NULL,
  };
  static const char *const british_[] = {
    "ə^ʊ", "Q", "e^ə", "ɛː", "i^ə", "ɪə", NULL,
  };
  g_autoptr (GString) s = g_string_new (ipa);

  /* "e" is a substring of the diphthongs' left halves only through ties: those go first */
  replace_all (s, british ? british_ : american);
  replace_all (s, common);
  g_string_replace (s, "^", "", 0);
  if (!british)
    g_string_replace (s, "ː", "", 0);
  /* nasalisation (a combining tilde) dropped, the vowel kept */
  g_string_replace (s, "\xcc\x83", "", 0);
  return g_string_free (g_steal_pointer (&s), FALSE);
}
