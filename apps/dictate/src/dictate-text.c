/* dictate-text: see dictate-text.h. */
#include "dictate-text.h"

#include <string.h>

static gboolean
is_terminal (gunichar c)
{
  return c == '.' || c == '!' || c == '?' || c == 0x2026;   /* … */
}

/* German words, other than nouns, that a line starting mid-sentence capitalises: the conjunctions, articles,
 * pronouns, prepositions and verbs most often heard there. "Sie" is not one: it is also the polite "you". */
static const char *const GERMAN_LOWER[] = {
  "aber", "als", "also", "am", "an", "auch", "auf", "aus", "bei", "bevor", "bin", "bis", "bist", "da", "damit", "dann",
  "das", "dass", "daß", "dein", "deine", "dem", "den", "denn", "der", "des", "dich", "die", "dir", "doch", "dort",
  "du", "durch", "ehe", "ein", "eine", "einem", "einen", "einer", "eines", "er", "es", "für", "gegen", "hat", "hatte",
  "hier", "ich", "ihm", "ihn", "im", "in", "indem", "ist", "ja", "jedoch", "jetzt", "kein", "keine", "man", "mein",
  "meine", "mich", "mir", "mit", "nach", "nachdem", "nein", "nicht", "noch", "nun", "nur", "ob", "obwohl", "oder",
  "ohne", "sagte", "schon", "sehr", "sein", "seine", "sich", "sind", "so", "sodass", "sondern", "sowie", "trotzdem",
  "um", "und", "uns", "unter", "vom", "von", "vor", "während", "war", "waren", "was", "weil", "wenn", "wer", "wie",
  "wieder", "wir", "wird", "wo", "zu", "zum", "zur",
};

static gboolean
german_lower (const char *word, gsize len)
{
  g_autofree char *lower = g_utf8_strdown (word, len);

  for (guint i = 0; i < G_N_ELEMENTS (GERMAN_LOWER); i++)
    if (g_str_equal (lower, GERMAN_LOWER[i]))
      return TRUE;
  return FALSE;
}

/* the word at `p` is capitalised only as a sentence's start would be: one capital, then lower case letters (and
 * apostrophes); in English not "I" or "I'm", "I'll", ...; in German only one of GERMAN_LOWER */
static gboolean
plain_capitalised (const char *p, const char *language)
{
  const char *start = p;
  gunichar c = g_utf8_get_char (p);

  if (!g_unichar_isupper (c))
    return FALSE;
  if (c == 'I' && g_strcmp0 (language, "en") == 0)
    {
      gunichar next = g_utf8_get_char (g_utf8_next_char (p));

      if (!g_unichar_isalpha (next))
        return FALSE;   /* "I", "I'm" */
    }
  for (p = g_utf8_next_char (p); *p; p = g_utf8_next_char (p))
    {
      c = g_utf8_get_char (p);
      if (c == '\'' || c == 0x2019)
        continue;
      if (!g_unichar_isalpha (c))
        break;
      if (g_unichar_isupper (c))
        return FALSE;
    }
  if (g_strcmp0 (language, "de") == 0)
    return german_lower (start, p - start);
  return TRUE;
}

char *
dictate_text_tidy (const char *line, gboolean sentence_start, const char *language)
{
  GString *out = g_string_new (NULL);
  gboolean capital = sentence_start;    /* the next letter starts a sentence */
  gboolean first = TRUE;                /* the next letter starts the line's first word */
  guint word = 0;                       /* letters in the word right before this character (0: none) */

  for (const char *p = line; *p; p = g_utf8_next_char (p))
    {
      gunichar c = g_utf8_get_char (p);

      if (c == ',' && out->len && out->str[out->len - 1] == ',')
        continue;
      if (g_unichar_isalpha (c))
        {
          if (capital)
            c = g_unichar_toupper (c);
          else if (first && plain_capitalised (p, language))
            c = g_unichar_tolower (c);
          capital = first = FALSE;
          /* the rest of the word as it is */
          g_string_append_unichar (out, c);
          word = 1;
          for (const char *q = g_utf8_next_char (p); *q; q = g_utf8_next_char (q))
            {
              gunichar d = g_utf8_get_char (q);

              if (!g_unichar_isalpha (d) && d != '\'' && d != 0x2019)
                break;
              g_string_append_unichar (out, d);
              word++;
              p = q;
            }
          continue;
        }
      if (g_unichar_isdigit (c))
        capital = first = FALSE;
      else if (is_terminal (c))
        {
          /* a sentence's end is followed by a space (3.5 is not one), and a full stop after a single letter is an
           * abbreviation's (e.g.) */
          gunichar next = g_utf8_get_char (g_utf8_next_char (p));

          if ((next == 0 || g_unichar_isspace (next)) && !(c == '.' && word == 1))
            capital = TRUE;
        }
      g_string_append_unichar (out, c);
      word = 0;
    }
  return g_string_free (out, FALSE);
}

gboolean
dictate_text_ends_sentence (const char *before)
{
  const char *end = before + strlen (before);

  while (end > before)
    {
      const char *prev = g_utf8_prev_char (end);
      gunichar c = g_utf8_get_char (prev);

      if (c == '\n')
        return TRUE;
      if (!g_unichar_isspace (c))
        return is_terminal (c);
      end = prev;
    }
  return TRUE;
}
