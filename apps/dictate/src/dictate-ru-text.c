/* dictate-ru-text: the Russian front end's regular expressions and string helpers, and its Unicode clean-up (unify,
 * tidy: tiny-voca-ru's frontend/clean.py, see dictate-ru.h). GRegex is PCRE2 with Unicode classes, as Python's re is
 * for text: \w, \d, \s and \b know Cyrillic. */
#include "dictate-ru-private.h"

#include <string.h>

static GHashTable *compiled;    /* pattern → GRegex, kept for the life of the app */
G_LOCK_DEFINE_STATIC (compiled);

GRegex *
ru_rx (const char *pattern)
{
  GRegex *rx;

  G_LOCK (compiled);
  if (!compiled)
    compiled = g_hash_table_new (g_str_hash, g_str_equal);
  rx = g_hash_table_lookup (compiled, pattern);
  if (!rx)
    {
      g_autoptr (GError) error = NULL;
      rx = g_regex_new (pattern, G_REGEX_OPTIMIZE, 0, &error);
      if (!rx)
        {
          g_critical ("dictate-ru: a pattern does not compile: %s", error->message);
          rx = g_regex_new ("(?!)", 0, 0, NULL);
        }
      g_hash_table_insert (compiled, g_strdup (pattern), rx);
    }
  G_UNLOCK (compiled);
  return rx;
}

typedef struct {
  RuReplace fn;
  gpointer data;
} Eval;

static gboolean
eval (const GMatchInfo *m, GString *out, gpointer data)
{
  Eval *e = data;

  e->fn (m, out, e->data);
  return FALSE;
}

char *
ru_sub (const char *pattern, const char *text, RuReplace fn, gpointer data)
{
  Eval e = { fn, data };
  char *out = g_regex_replace_eval (ru_rx (pattern), text, -1, 0, 0, eval, &e, NULL);

  return out ? out : g_strdup (text);
}

char *
ru_subs (const char *pattern, const char *text, const char *replacement)
{
  char *out = g_regex_replace (ru_rx (pattern), text, -1, 0, replacement, 0, NULL);

  return out ? out : g_strdup (text);
}

void
ru_sub_in (char **text, const char *pattern, RuReplace fn, gpointer data)
{
  char *out = ru_sub (pattern, *text, fn, data);

  g_free (*text);
  *text = out;
}

void
ru_subs_in (char **text, const char *pattern, const char *replacement)
{
  char *out = ru_subs (pattern, *text, replacement);

  g_free (*text);
  *text = out;
}

gboolean
ru_search (const char *pattern, const char *text, gssize length, GMatchInfo **m)
{
  GMatchInfo *info = NULL;
  gboolean found = g_regex_match_full (ru_rx (pattern), text, length, 0, 0, &info, NULL);

  if (m && found)
    *m = info;
  else
    g_match_info_free (info);
  return found;
}

gboolean
ru_match_at (const char *pattern, const char *text, gsize at, GMatchInfo **m)
{
  GMatchInfo *info = NULL;
  gboolean found = g_regex_match_full (ru_rx (pattern), text, -1, at, G_REGEX_MATCH_ANCHORED, &info, NULL);

  if (m && found)
    *m = info;
  else
    g_match_info_free (info);
  return found;
}

gboolean
ru_group_pos (const GMatchInfo *m, const char *name, int *start, int *end)
{
  int s = -1, e = -1;

  if (!g_match_info_fetch_named_pos (m, name, &s, &e) || s < 0)
    return FALSE;
  if (start)
    *start = s;
  if (end)
    *end = e;
  return TRUE;
}

char *
ru_group (const GMatchInfo *m, const char *name)
{
  return ru_group_pos (m, name, NULL, NULL) ? g_match_info_fetch_named (m, name) : NULL;
}

char *
ru_group_n (const GMatchInfo *m, int n)
{
  int s = -1, e = -1;

  if (!g_match_info_fetch_pos (m, n, &s, &e) || s < 0)
    return NULL;
  return g_match_info_fetch (m, n);
}

char *
ru_lower (const char *s)
{
  return g_utf8_strdown (s, -1);
}

char *
ru_upper (const char *s)
{
  return g_utf8_strup (s, -1);
}

char *
ru_replace (const char *s, const char *from, const char *to)
{
  g_auto (GStrv) parts = g_strsplit (s, from, -1);

  return g_strjoinv (to, parts);
}

char *
ru_plain (const char *word)
{
  g_autofree char *bare = ru_replace (word, "+", "");

  return ru_lower (bare);
}

gsize
ru_chars (const char *s)
{
  return g_utf8_strlen (s, -1);
}

char *
ru_drop (const char *s, int n)
{
  glong length = g_utf8_strlen (s, -1);

  return g_utf8_substring (s, 0, MAX (0, length - n));
}

gboolean
ru_ends (const char *s, const char *suffix)
{
  return s && g_str_has_suffix (s, suffix);
}

gboolean
ru_starts (const char *s, const char *prefix)
{
  return s && g_str_has_prefix (s, prefix);
}

gboolean
ru_int (const char *digits, gint64 *n)
{
  gint64 value = 0;

  if (!digits || !*digits)
    return FALSE;
  for (const char *p = digits; *p; p = g_utf8_next_char (p))
    {
      int d = g_unichar_digit_value (g_utf8_get_char (p));
      if (d < 0 || value > (G_MAXINT64 - d) / 10)
        return FALSE;
      value = value * 10 + d;
    }
  *n = value;
  return TRUE;
}

static gboolean
alnum (gunichar c)
{
  switch (g_unichar_type (c))
    {
    case G_UNICODE_LOWERCASE_LETTER: case G_UNICODE_UPPERCASE_LETTER: case G_UNICODE_TITLECASE_LETTER:
    case G_UNICODE_MODIFIER_LETTER: case G_UNICODE_OTHER_LETTER:
    case G_UNICODE_DECIMAL_NUMBER: case G_UNICODE_LETTER_NUMBER: case G_UNICODE_OTHER_NUMBER:
      return TRUE;
    default:
      return FALSE;
    }
}

gboolean
ru_any_alnum (const char *s)
{
  for (const char *p = s; *p; p = g_utf8_next_char (p))
    if (alnum (g_utf8_get_char (p)))
      return TRUE;
  return FALSE;
}

const char *
ru_case_name (RuCase c)
{
  static const char *const NAMES[RU_CASES] = { "nom", "gen", "dat", "acc", "ins", "prep" };

  return NAMES[c];
}

RuCase
ru_case_of (char code)
{
  const char *at = strchr ("NGDAIP", code);

  return at && code ? (RuCase) (at - "NGDAIP") : RU_NOM;
}

/* ---------------- clean.py ---------------- */

#define LETTER    "а-яёА-ЯЁ"
#define VOWELS    "аеёиоуыэюяАЕЁИОУЫЭЮЯ"
#define PUNCTUATION ",\\.\\?!;:\"'\\(\\)—…"

static const char *const QUOTES[][2] = {
  { "«", "\"" }, { "»", "\"" }, { "„", "\"" }, { "“", "\"" }, { "”", "\"" }, { "‟", "\"" }, { "‘", "'" },
  { "’", "'" }, { "‚", "'" }, { "‛", "'" }, { "`", "'" }, { "´", "'" }, { "[", "(" }, { "]", ")" }, { "{", "(" },
  { "}", ")" }, { "−", "-" }, { "‐", "-" }, { "‑", "-" }, { "‒", "–" }, { "―", "—" }, { " ", " " },
  { " ", " " }, { " ", " " }, { " ", " " }, { "℃", "°C" }, { "¼", "1/4" }, { "½", "1/2" },
  { "¾", "3/4" }, { "⅓", "1/3" }, { "⅔", "2/3" },
};

/* a Latin letter with a mark (café): its first character once decomposed */
static void
unmarked (const GMatchInfo *m, GString *out, gpointer data)
{
  g_autofree char *letter = g_match_info_fetch (m, 0);
  g_autofree char *parts = g_utf8_normalize (letter, -1, G_NORMALIZE_NFD);

  g_string_append_unichar (out, g_utf8_get_char (parts));
}

char *
ru_unify (const char *text)
{
  g_autofree char *nfc = g_utf8_normalize (text, -1, G_NORMALIZE_NFC);
  GString *quoted = g_string_new (NULL);
  char *out;

  for (const char *p = nfc ? nfc : ""; *p; p = g_utf8_next_char (p))
    {
      const char *next = g_utf8_next_char (p);
      const char *with = NULL;
      for (guint i = 0; i < G_N_ELEMENTS (QUOTES) && !with; i++)
        if ((gsize) (next - p) == strlen (QUOTES[i][0]) && !memcmp (p, QUOTES[i][0], next - p))
          with = QUOTES[i][1];
      if (with)
        g_string_append (quoted, with);
      else
        g_string_append_len (quoted, p, next - p);
    }
  out = g_string_free (quoted, FALSE);
  ru_subs_in (&out, "([" VOWELS "])[\\x{0301}\\x{0300}]", "+\\1");     /* за́мок: з+амок */
  ru_sub_in (&out, "[\\x{00c0}-\\x{024f}]", unmarked, NULL);
  ru_subs_in (&out, "[\\x{0300}-\\x{036f}]", "");
  ru_subs_in (&out, "\\.{3,}", "…");
  return out;
}

char *
ru_tidy (const char *text)
{
  char *out = ru_subs ("-{2,}|–", text, "—");

  ru_subs_in (&out, "(?<![" LETTER "+])-|-(?![" LETTER "+])", " — ");   /* a hyphen not in a word: a dash */
  ru_subs_in (&out, "[^\\s" LETTER "+\\-" PUNCTUATION "]", " ");
  ru_subs_in (&out, "(?<=\\S)—", " —");
  ru_subs_in (&out, "—(?=\\S)", "— ");
  ru_subs_in (&out, "(?<=[?!])\\.\\.(?!\\.)", "");
  ru_subs_in (&out, "(?<!\\.)\\.\\.(?!\\.)", ".");
  ru_subs_in (&out, "\\s+", " ");
  g_strstrip (out);
  return out;
}
