/* dictate-ru: see dictate-ru.h. The tokens and the sentences: tiny-voca-ru's frontend/tokenize.py and symbols.py. */
#include "dictate-ru.h"
#include "dictate-ru-private.h"

#include <string.h>

/* the model's 61 symbols, in their ids' order: specials, punctuation, consonants, vowels */
static const char *const SYMBOLS[] = {
  "<pad>", "<bos>", "<eos>", "<sp>", "<unk>",
  ",", ".", "?", "!", ";", ":", "\"", "'", "(", ")", "—", "…",
  "p", "pʲ", "b", "bʲ", "t", "tʲ", "d", "dʲ", "k", "kʲ", "ɡ", "ɡʲ", "f", "fʲ", "v", "vʲ", "s", "sʲ", "z", "zʲ", "ʂ",
  "ʐ", "ɕ", "x", "xʲ", "ts", "tɕ", "m", "mʲ", "n", "nʲ", "l", "lʲ", "r", "rʲ", "j",
  "a", "o", "e", "i", "u", "ɨ", "ɐ", "ə",
};
#define BOS        1
#define EOS        2
#define SP         3
#define UNK        4
#define FIRST_PUNCTUATION 5
#define FIRST_PHONE 17

#define PUNCTUATION_CLASS ",\\.\\?!;:\"'\\(\\)—…"
#define SPACE      "[\\s\\x{feff}]+"
#define SENTENCE_END "(?:(?<=[.!?…])|(?<=[.!?…][\"')\\]»“]))\\s+(?=[\"'(\\[«„]?[A-ZА-ЯЁ0-9])"
#define NOT_AN_END "(?i)(?:\\b(?:тыс|млн|млрд|руб|коп|долл|ул|пр|просп|пер|наб|пл|обл|кв|корп|стр|рис|табл|см|ср|" \
                   "напр|проф|акад|доц|им|св|гр|тел|др|т\\.е|т\\.д|т\\.п|т\\.к|т\\.н)|\\b[A-ZА-ЯЁ])\\.$"
#define CLAUSE_END "(?<=[,;:—])\\s+"
#define CLOSING    "\"'”’)]»"

struct _DictateRu {
  RuStress *stress;
};

DictateRu *
dictate_ru_new (const char *dir, gboolean homographs)
{
  DictateRu *self = g_new0 (DictateRu, 1);

  self->stress = ru_stress_new (dir, homographs);
  return self;
}

void
dictate_ru_free (DictateRu *self)
{
  if (!self)
    return;
  ru_stress_free (self->stress);
  g_free (self);
}

char *
dictate_ru_normalize (const char *text)
{
  return ru_normalize (text);
}

char *
dictate_ru_accentuate (DictateRu *self, const char *normalized)
{
  return ru_stress_accentuate (self->stress, normalized);
}

const char *const *
dictate_ru_symbols (guint *n)
{
  *n = G_N_ELEMENTS (SYMBOLS);
  return SYMBOLS;
}

static gint64
symbol_id (const char *symbol)
{
  for (guint i = 0; i < G_N_ELEMENTS (SYMBOLS); i++)
    if (g_str_equal (SYMBOLS[i], symbol))
      return i;
  return -1;
}

/* the tokens as they are made: the last one, for <sp> and the repeated punctuation */
typedef struct {
  GArray *ids, *stress;
  gboolean spoken;              /* a phone (or <unk>) among them */
} Tokens;

static gint64
last_id (Tokens *t)
{
  return g_array_index (t->ids, gint64, t->ids->len - 1);
}

static void
add (Tokens *t, gint64 id, gint64 level)
{
  g_array_append_val (t->ids, id);
  g_array_append_val (t->stress, level);
}

static void
phone (const char *phone, int stress, gpointer data)
{
  Tokens *t = data;
  gint64 id;

  if (!phone)                   /* a phonetic word: <sp> after a phone */
    {
      if (last_id (t) >= FIRST_PHONE || last_id (t) == UNK)
        add (t, SP, 0);
      return;
    }
  id = symbol_id (phone);
  if (id < FIRST_PHONE)
    id = UNK;
  add (t, id, stress);
  t->spoken = TRUE;
}

gboolean
dictate_ru_phonemize (DictateRu *self, const char *text, GArray *ids, GArray *stress)
{
  g_autofree char *normalized = ru_normalize (text);
  g_autofree char *stressed = ru_stress_accentuate (self->stress, normalized);
  g_autoptr (GPtrArray) words = g_ptr_array_new_with_free_func (g_free);
  guint ids_from = ids->len, stress_from = stress->len;
  Tokens t = { ids, stress, FALSE };
  GMatchInfo *m = NULL;

  add (&t, BOS, 0);
  g_regex_match (ru_rx ("[" PUNCTUATION_CLASS "]|[^\\s" PUNCTUATION_CLASS "]+"), stressed, 0, &m);
  while (TRUE)
    {
      gboolean more = g_match_info_matches (m);
      g_autofree char *token = more ? g_match_info_fetch (m, 0) : NULL;
      gint64 punctuation = token ? symbol_id (token) : -1;
      gboolean is_punctuation = punctuation >= FIRST_PUNCTUATION && punctuation < FIRST_PHONE;
      if ((!more || is_punctuation) && words->len)          /* the phrase so far, in phones */
        {
          g_ptr_array_add (words, NULL);
          ru_phrase_phones ((char **) words->pdata, phone, &t);
          g_ptr_array_set_size (words, 0);
        }
      if (!more)
        break;
      if (is_punctuation)
        {
          if (last_id (&t) != punctuation)    /* !! read as one */
            add (&t, punctuation, 0);
        }
      else
        g_ptr_array_add (words, g_steal_pointer (&token));
      g_match_info_next (m, NULL);
    }
  g_match_info_free (m);
  add (&t, EOS, 0);
  if (!t.spoken)
    {
      g_array_set_size (ids, ids_from);
      g_array_set_size (stress, stress_from);
    }
  return t.spoken;
}

static gboolean
char_in (gunichar c, const char *set)
{
  for (const char *p = set; *p; p = g_utf8_next_char (p))
    if (g_utf8_get_char (p) == c)
      return TRUE;
  return FALSE;
}

/* `text` with a full stop when it ends without its punctuation (ensure_terminal_punctuation ()) */
static char *
terminated (const char *text)
{
  g_autofree char *body = ru_subs (SPACE "$", text, "");
  g_auto (GStrv) words = NULL;
  const char *last, *end;
  gboolean marked = FALSE;

  if (!*body)
    return g_strdup (text);
  words = g_regex_split (ru_rx (SPACE), body, 0);
  last = words[0] ? words[g_strv_length (words) - 1] : "";
  end = last + strlen (last);
  if (ru_any_alnum (last))
    {
      while (end > last && char_in (g_utf8_get_char (g_utf8_prev_char (end)), CLOSING))
        end = g_utf8_prev_char (end);
      marked = end > last && char_in (g_utf8_get_char (g_utf8_prev_char (end)), ".!?…,;:");
    }
  else
    {
      for (const char *p = last; *p; p = g_utf8_next_char (p))
        marked = marked || char_in (g_utf8_get_char (p), ".!?…");
      marked = marked || g_str_equal (last, ",") || g_str_equal (last, ";") || g_str_equal (last, ":");
    }
  return marked ? g_strdup (text) : g_strconcat (body, ".", NULL);
}

char **
dictate_ru_sentences (const char *text)
{
  g_autofree char *ended = terminated (text);
  g_autofree char *spaced = NULL;
  g_auto (GStrv) pieces = NULL;
  GPtrArray *sentences = g_ptr_array_new ();
  GString *current = g_string_new (NULL);

  spaced = ru_subs (SPACE, ended, " ");
  g_strstrip (spaced);
  pieces = g_regex_split (ru_rx (SENTENCE_END), spaced, 0);
  for (char **piece = pieces; *piece; piece++)
    {
      if (current->len)
        g_string_append_c (current, ' ');
      g_string_append (current, *piece);
      if (!ru_search (NOT_AN_END, current->str, -1, NULL))
        {
          g_ptr_array_add (sentences, g_strdup (current->str));
          g_string_truncate (current, 0);
        }
    }
  if (current->len)
    g_ptr_array_add (sentences, g_strdup (current->str));
  g_string_free (current, TRUE);
  for (guint i = sentences->len; i > 0; i--)
    if (!ru_any_alnum (sentences->pdata[i - 1]))
      g_ptr_array_remove_index (sentences, i - 1);
  g_ptr_array_add (sentences, NULL);
  return (char **) g_ptr_array_free (sentences, FALSE);
}

char **
dictate_ru_split_clause (const char *text)
{
  char **clauses = g_regex_split (ru_rx (CLAUSE_END), text, 0);
  char **words, **halves;
  guint n, half;

  if (g_strv_length (clauses) > 1)
    return clauses;
  g_strfreev (clauses);
  words = g_strsplit (text, " ", -1);
  n = g_strv_length (words);
  if (n < 2)
    {
      g_strfreev (words);
      return NULL;
    }
  half = (n + 1) / 2;
  halves = g_new0 (char *, 3);
  char *cut = words[half];
  words[half] = NULL;
  halves[0] = g_strjoinv (" ", words);
  words[half] = cut;
  halves[1] = g_strjoinv (" ", words + half);
  g_strfreev (words);
  return halves;
}
