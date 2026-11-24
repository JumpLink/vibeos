/* dictate-ru-normalize: written Russian to the words it is read as: numbers in the case and gender the words around
 * them ask for, dates, times, years, Roman numerals, units and currencies, phone numbers, abbreviations, acronyms,
 * Latin words and signs. A port of tiny-voca-ru's frontend/normalize.py (see dictate-ru.h); each pass is the
 * original's, in its order. Where the original fails (a number of 10^15 or more said as a cardinal), this reads the
 * digits one by one. */
#include "dictate-ru-private.h"

#include <string.h>

#define LETTER   "а-яёА-ЯЁ"
#define UPPER    "А-ЯЁ"
#define VOWELS   "аеёиоуыэюяАЕЁИОУЫЭЮЯ"
#define PUNCTUATION_CLASS ",\\.\\?!;:\"'\\(\\)—…"
#define NUMBER   "\\d+(?:,\\d+)?"
#define ALL_CASES "NGDAIP"
#define NEXT     "\\s+([" LETTER "+]+)"
#define MONTH_ANY "(?:январ|феврал|март|апрел|ма[йяюе]|июн|июл|август|сентябр|октябр|ноябр|декабр)[а-яё]*"

/* ---------------- numerals, with their stress marked ---------------- */

/* each word that has its stress marked (NUMERAL_STRESS) replaced by the marked one; takes `words` */
static char *
marked (char *words)
{
  g_auto (GStrv) parts = g_strsplit (words, " ", -1);

  g_free (words);
  for (char **p = parts; *p; p++)
    {
      const char *stressed = ru_numeral_stress (*p);
      if (stressed)
        {
          g_free (*p);
          *p = g_strdup (stressed);
        }
    }
  return g_strjoinv (" ", parts);
}

/* a number the numerals cannot name, digit by digit */
static char *
spelt (gint64 n)
{
  char text[24];

  g_snprintf (text, sizeof text, "%" G_GINT64_FORMAT, n < 0 ? (n == G_MININT64 ? G_MAXINT64 : -n) : n);
  return ru_digits (text);
}

static char *
cardinal (gint64 n, RuCase c, char gender, gboolean animate, gboolean plural)
{
  char *w = ru_cardinal (n, c, gender, animate, plural);

  return w ? marked (w) : spelt (n);
}

static char *
cardinal_nom (gint64 n)
{
  return cardinal (n, RU_NOM, 'm', FALSE, FALSE);
}

static char *
ordinal (gint64 n, RuCase c, char gender, gboolean plural, gboolean animate)
{
  char *w = ru_ordinal (n, c, gender, plural, animate);

  return w ? marked (w) : spelt (n);
}

/* ---------------- what the passes share ---------------- */

/* a number as written: Python's int, or the string of a decimal (with a comma) */
typedef struct {
  const char *text;
  gboolean decimal;
  gint64 n;                     /* G_MAXINT64 for a whole number too long to hold */
} Value;

static Value
value_of (const char *text)
{
  Value v = { text, strchr (text, ',') != NULL, 0 };

  if (!v.decimal && !ru_int (text, &v.n))
    v.n = G_MAXINT64;
  return v;
}

static gboolean
is_one (gint64 n)
{
  return n % 10 == 1 && n % 100 != 11;
}

static gboolean
is_few (gint64 n)
{
  return n % 10 >= 2 && n % 10 <= 4 && !(n % 100 >= 12 && n % 100 <= 14);
}

static gboolean
sentence_end (const char *text, gsize end)
{
  return ru_match_at ("\\s*$|\\s+[" UPPER "A-Z\"(—]", text, end, NULL);
}

/* the preposition just before `start`, past минус and плюс; "" for none */
static char *
preposition (const char *text, gsize start)
{
  gsize head = start;

  for (;;)
    {
      GMatchInfo *m = NULL;
      int at;
      if (!ru_search ("(?:^|(?<=[\\s(\"]))([^\\W\\d_][\\w+-]*)\\s+$", text, head, &m))
        return g_strdup ("");
      g_autofree char *found = g_match_info_fetch (m, 1);
      g_match_info_fetch_pos (m, 0, &at, NULL);
      g_match_info_free (m);
      char *word = ru_plain (found);
      if (!g_str_equal (word, "минус") && !g_str_equal (word, "плюс"))
        {
          if (ru_is_preposition (word))
            return word;
          g_free (word);
          return g_strdup ("");
        }
      g_free (word);
      head = at;
    }
}

/* the cases a preposition governs, as codes ("AP"), from `table` first; "N" when it is none */
static const char *
cases_of (const char *preposition, const char *table)
{
  return ru_cases (*preposition ? preposition : NULL, table);
}

static RuCase
first_case (const char *preposition)
{
  return ru_case_of (cases_of (preposition, NULL)[0]);
}

/* the word after `end`, plain; "" for none */
static char *
next_word (const char *text, gsize end)
{
  GMatchInfo *m = NULL;

  if (!ru_match_at (NEXT, text, end, &m))
    return g_strdup ("");
  g_autofree char *w = g_match_info_fetch (m, 1);
  g_match_info_free (m);
  return ru_plain (w);
}

/* `word` with the stress marks of `stressed` when they spell the same word */
static char *
restress (const char *word, const char *stressed)
{
  g_autofree char *plain = ru_plain (stressed), *lower = ru_lower (word);
  GString *out;
  const char *letter = word;

  if (strchr (word, '+') || !g_str_equal (plain, lower))
    return g_strdup (word);
  out = g_string_new (NULL);
  for (const char *p = stressed; *p; p = g_utf8_next_char (p))
    if (*p == '+')
      g_string_append_c (out, '+');
    else
      {
        const char *next = g_utf8_next_char (letter);
        g_string_append_len (out, letter, next - letter);
        letter = next;
      }
  return g_string_free (out, FALSE);
}

/* a noun's form after a count, in a case (noun_form ()) */
static const char *
noun_form (const char *lemma, const Value *count, RuCase c, gboolean stressed)
{
  const RuNoun *noun = ru_noun (lemma);
  const char *const *forms = stressed ? noun->stressed : (const char *const *) noun->plain;
  gint64 n = count->n;

  if (count->decimal)
    return noun->gender == 'p' ? forms[6 + RU_GEN] : forms[RU_GEN];
  if (n && n % 1000 == 0)
    return forms[6 + RU_GEN];                   /* тысяча рублей */
  if (noun->gender == 'p')
    return is_one (n) || (c != RU_NOM && c != RU_ACC) ? forms[6 + c] : forms[6 + RU_GEN];
  if (c != RU_NOM && c != RU_ACC)
    return is_one (n) ? forms[c] : forms[6 + c];
  if (is_one (n))
    return forms[c];
  if (is_few (n))
    return c == RU_ACC && noun->animate && n < 5 ? forms[6 + RU_GEN] : forms[12];
  return forms[6 + RU_GEN];
}

static const char *
noun_form_n (const char *lemma, gint64 n, RuCase c, gboolean stressed)
{
  Value v = { "", FALSE, n };

  return noun_form (lemma, &v, c, stressed);
}

/* a count said agreeing with its noun (_say ()) */
static char *
say (const Value *count, const char *lemma, RuCase c)
{
  const RuNoun *noun = ru_noun (lemma);

  if (count->decimal)
    {
      const char *comma = strchr (count->text, ',');
      g_autofree char *whole = g_strndup (count->text, comma - count->text);
      char *w = ru_decimal (whole, comma + 1, c);
      if (w)
        return marked (w);
      g_autofree char *a = ru_digits (whole), *b = ru_digits (comma + 1);
      return g_strconcat (a, " запятая ", b, NULL);
    }
  if (count->n == G_MAXINT64)
    return ru_digits (count->text);
  if (noun && noun->gender == 'p')
    return cardinal (count->n, c, 'm', noun->animate, TRUE);
  return cardinal (count->n, c, noun ? noun->gender : 'm', noun && noun->animate, FALSE);
}

/* the case in which the count's noun form is `word`, those in `cases` tried first; -1 for none */
static int
count_case (const Value *count, const char *lemma, const char *word, const char *cases)
{
  g_autofree char *all = g_strconcat (cases, ALL_CASES, NULL);

  for (const char *c = all; *c; c++)
    if (g_str_equal (noun_form (lemma, count, ru_case_of (*c), FALSE), word))
      return ru_case_of (*c);
  return -1;
}

/* an amount with its noun (and its scale: 5 млн руб.) */
static char *
amount (const Value *v, const char *lemma, RuCase c, const char *scale)
{
  g_autofree char *said = say (v, scale ? scale : lemma, c);

  if (scale)
    {
      const char *scale_noun = noun_form (scale, v, c, TRUE);
      if (lemma)
        return g_strdup_printf ("%s %s %s", said, scale_noun, ru_noun (lemma)->stressed[6 + RU_GEN]);
      return g_strdup_printf ("%s %s", said, scale_noun);
    }
  if (!lemma)
    return g_steal_pointer (&said);
  return g_strdup_printf ("%s %s", said, noun_form (lemma, v, c, TRUE));
}

static char *
number_words (const char *value)
{
  gint64 n;

  if ((ru_chars (value) > 1 && value[0] == '0') || ru_chars (value) > 15 || !ru_int (value, &n))
    return ru_digits (value);
  return cardinal_nom (n);
}

static gsize
match_start (const GMatchInfo *m)
{
  int s;

  g_match_info_fetch_pos (m, 0, &s, NULL);
  return s;
}

static gsize
match_end (const GMatchInfo *m)
{
  int e;

  g_match_info_fetch_pos (m, 0, NULL, &e);
  return e;
}

static void
keep (const GMatchInfo *m, GString *out)
{
  g_autofree char *all = g_match_info_fetch (m, 0);

  g_string_append (out, all);
}

/* the last word before `start` (the letters before the spaces that end it), lowered; NULL for none */
static char *
word_before (const char *text, gsize start)
{
  GMatchInfo *m = NULL;

  if (!ru_search ("([" LETTER "]+)\\s+$", text, start, &m))
    return NULL;
  g_autofree char *w = g_match_info_fetch (m, 1);
  g_match_info_free (m);
  return ru_lower (w);
}

static gboolean
is_digit_string (const char *s)
{
  if (!*s)
    return FALSE;
  for (const char *p = s; *p; p = g_utf8_next_char (p))
    if (!g_unichar_isdigit (g_utf8_get_char (p)))
      return FALSE;
  return TRUE;
}

static char *
regex_escape (const char *s)
{
  GString *out = g_string_new (NULL);

  for (const char *p = s; *p; p++)
    {
      if ((guchar) *p < 0x80 && !g_ascii_isalnum (*p))
        g_string_append_c (out, '\\');
      g_string_append_c (out, *p);
    }
  return g_string_free (out, FALSE);
}

static int
longer_first (gconstpointer a, gconstpointer b)
{
  return (int) ru_chars (*(const char *const *) b) - (int) ru_chars (*(const char *const *) a);
}

/* the alternatives joined with |, the longest first */
static char *
alternatives (GPtrArray *words, gboolean escape)
{
  GString *out = g_string_new (NULL);

  g_ptr_array_sort (words, longer_first);
  for (guint i = 0; i < words->len; i++)
    {
      g_autofree char *w = escape ? regex_escape (words->pdata[i]) : g_strdup (words->pdata[i]);
      if (i)
        g_string_append_c (out, '|');
      g_string_append (out, w);
    }
  return g_string_free (out, FALSE);
}

/* a noun's distinct plain forms (`from`, `to`: 0 6 12), less `except`, as alternatives */
static char *
noun_forms (const char *lemma, const char *except)
{
  const RuNoun *noun = ru_noun (lemma);
  g_autoptr (GPtrArray) words = g_ptr_array_new ();

  for (int f = 0; f < 12; f++)
    if ((!except || !g_str_equal (noun->plain[f], except)) &&
        !g_ptr_array_find_with_equal_func (words, noun->plain[f], g_str_equal, NULL))
      g_ptr_array_add (words, noun->plain[f]);
  return alternatives (words, FALSE);
}

static gboolean
in_forms (const char *word, const RuNoun *noun, int from)
{
  for (int f = from; f < from + 6; f++)
    if (g_str_equal (noun->plain[f], word))
      return TRUE;
  return FALSE;
}

/* ---------------- phones ---------------- */

static void
phone (const GMatchInfo *m, GString *out, gpointer data)
{
  g_autofree char *all = g_match_info_fetch (m, 0);
  g_autoptr (GPtrArray) groups = g_ptr_array_new_with_free_func (g_free);
  g_autoptr (GPtrArray) words = g_ptr_array_new_with_free_func (g_free);
  GMatchInfo *d = NULL;

  g_regex_match (ru_rx ("\\d+"), all, 0, &d);
  while (g_match_info_matches (d))
    {
      g_ptr_array_add (groups, g_match_info_fetch (d, 0));
      g_match_info_next (d, NULL);
    }
  g_match_info_free (d);
  if (groups->len == 1)                         /* 89991234567 */
    {
      g_autofree char *whole = g_strdup (groups->pdata[0]);
      glong n = g_utf8_strlen (whole, -1);
      static const int CUTS[] = { 10, 7, 4, 2, 0 };
      glong from = 0;
      g_ptr_array_set_size (groups, 0);
      for (guint i = 0; i < G_N_ELEMENTS (CUTS); i++)
        {
          glong to = MAX (0, n - CUTS[i]);
          g_ptr_array_add (groups, g_utf8_substring (whole, MIN (from, to), to));
          from = MAX (from, to);
        }
    }
  if (all[0] == '+')
    g_ptr_array_add (words, g_strdup ("плюс"));
  for (guint i = 0; i < groups->len; i++)
    {
      const char *group = groups->pdata[i], *rest = group;
      gint64 n;
      while (*rest == '0')
        {
          g_ptr_array_add (words, g_strdup ("ноль"));
          rest++;
        }
      if (*rest)
        g_ptr_array_add (words, ru_int (rest, &n) ? cardinal_nom (n) : ru_digits (rest));
    }
  g_ptr_array_add (words, NULL);
  g_autofree char *said = g_strjoinv (" ", (char **) words->pdata);
  g_string_append (out, said);
}

static char *
phones (char *text)
{
#define SEPARATED "[\\s-]?\\(?\\d{3}\\)?[\\s-]?\\d{3}[\\s-]?\\d{2}[\\s-]?\\d{2}"
  ru_sub_in (&text, "(?<![\\d+])(?:\\+\\d{1,3}|8)" SEPARATED "(?!\\d)", phone, NULL);
  ru_sub_in (&text, "(?<![\\d+])(?:\\+7|8)\\d{10}(?!\\d)", phone, NULL);
  ru_sub_in (&text, "(?<![\\d.,:-])\\d{3}-\\d{2}-\\d{2}(?![\\d-])", phone, NULL);
  return text;
}

/* ---------------- groups ---------------- */

static void
without_spaces (const GMatchInfo *m, GString *out, gpointer data)
{
  g_autofree char *all = g_match_info_fetch (m, 0);
  g_autofree char *joined = ru_replace (all, data, "");

  g_string_append (out, joined);
}

static void
dotted (const GMatchInfo *m, GString *out, gpointer data)
{
  g_autofree char *all = g_match_info_fetch (m, 0);
  g_auto (GStrv) parts = g_strsplit (all, ".", -1);

  for (char **p = parts; *p; p++)
    {
      g_autofree char *w = number_words (*p);
      if (p != parts)
        g_string_append (out, " точка ");
      g_string_append (out, w);
    }
}

static char *
groups (char *text)
{
  ru_sub_in (&text, "(?<![\\d,.])\\d{1,3}(?: \\d{3})+(?![\\d,]|\\.\\d)", without_spaces, " ");
  ru_sub_in (&text, "(?<![\\d,.])\\d{1,3}(?:\\.\\d{3}){2,}(?![\\d,]|\\.\\d)", without_spaces, ".");
  ru_sub_in (&text, "(?<![\\d.])\\d+(?:\\.\\d+){2,}(?!\\d|\\.\\d)", dotted, NULL);
  ru_subs_in (&text, "(?<![\\d,.])(\\d+)\\.(\\d+)(?!\\d|\\.\\d)", "\\1,\\2");
  return text;
}

/* ---------------- dates ---------------- */

static RuCase
day_case (const GMatchInfo *m, const char *ending)
{
  const char *text = g_match_info_get_string (m);
  g_autofree char *prep = preposition (text, match_start (m));
  g_autofree char *before = word_before (text, match_start (m));
  const char *cases = *prep ? cases_of (prep, "DATE_PREPOSITIONS") : "G";
  const RuEnding *e = ru_ordinal_ending (ending ? ending : "");
  RuCase readings[6];
  int n = 0;

  if (!*prep && before && ru_in (ru_list ("TODAY"), before))
    return RU_NOM;                              /* сегодня семнадцатое ноября */
  for (int i = 0; e && i < e->n; i++)
    if (g_str_equal (e->readings[i].gender, "m") || g_str_equal (e->readings[i].gender, "n"))
      readings[n++] = e->readings[i].c;
  for (const char *c = cases; *c; c++)
    for (int i = 0; i < n; i++)
      if (readings[i] == ru_case_of (*c))
        return readings[i];
  return n ? readings[0] : ru_case_of (cases[0]);
}

static char *
year_words (const GMatchInfo *m)
{
  g_autofree char *y = ru_group (m, "y");
  g_autofree char *g = ru_group (m, "g");
  gint64 n;

  if (!y || !*y || !ru_int (y, &n))
    return g_strdup ("");
  gboolean dot = g && g_str_equal (g, "г.") && sentence_end (g_match_info_get_string (m), match_end (m));
  g_autofree char *w = ordinal (n, RU_GEN, 'm', FALSE, FALSE);
  return g_strdup_printf (" %s года%s", w, dot ? "." : "");
}

static const char *const MONTHS[] = {
  "января", "февраля", "марта", "апреля", "мая", "июня", "июля", "августа", "сентября", "октября", "ноября", "декабря",
};
#define MONTH_NAMES "января|февраля|марта|апреля|мая|июня|июля|августа|сентября|октября|ноября|декабря"

static void
numeric_date (const GMatchInfo *m, GString *out, gpointer data)
{
  g_autofree char *d = ru_group (m, "d"), *mo = ru_group (m, "m");
  gint64 day, month;

  if (!ru_int (d, &day) || !ru_int (mo, &month) || day < 1 || day > 31 || month < 1 || month > 12)
    {
      keep (m, out);
      return;
    }
  g_autofree char *w = ordinal (day, day_case (m, NULL), 'n', FALSE, FALSE);
  g_autofree char *year = year_words (m);
  g_string_append_printf (out, "%s %s%s", w, MONTHS[month - 1], year);
}

static void
from_day (const GMatchInfo *m, GString *out, gpointer data)
{
  g_autofree char *p = ru_group (m, "p"), *d = ru_group (m, "d");
  gint64 day;

  if (!ru_int (d, &day) || day < 1 || day > 31)
    {
      keep (m, out);
      return;
    }
  g_autofree char *w = ordinal (day, RU_GEN, 'n', FALSE, FALSE);
  g_string_append_printf (out, "%s %s", p, w);
}

static void
written_date (const GMatchInfo *m, GString *out, gpointer data)
{
  g_autofree char *s = ru_group (m, "s"), *d = ru_group (m, "d"), *d2 = ru_group (m, "d2");
  g_autofree char *month = ru_group (m, "month");
  RuCase c = day_case (m, s);
  const char *days[] = { d, d2 };
  gboolean first = TRUE;

  for (guint i = 0; i < G_N_ELEMENTS (days); i++)
    {
      gint64 n;
      if (!days[i] || !*days[i] || !ru_int (days[i], &n))
        continue;
      g_autofree char *w = ordinal (n, c, 'n', FALSE, FALSE);
      g_string_append_printf (out, "%s%s", first ? "" : " — ", w);
      first = FALSE;
    }
  g_autofree char *year = year_words (m);
  g_string_append_printf (out, " %s%s", month, year);
}

static void
month_year (const GMatchInfo *m, GString *out, gpointer data)
{
  g_autofree char *month = ru_group (m, "month");
  g_autofree char *year = year_words (m);

  g_string_append_printf (out, "%s%s", month, year);
}

static char *
dates (char *text)
{
#define YEAR   "(?P<y>\\d{4})(?!\\d)(?:\\s*(?P<g>г\\.|года\\b))?"
#define ENDING "(?:-(?P<s>го|е|ое|ого|му|ому|м|ом))?"
  ru_sub_in (&text, "(?<![\\d.])(?P<d>\\d{1,2})\\.(?P<m>\\d{1,2})\\." YEAR, numeric_date, NULL);
  ru_sub_in (&text, "(?<![\\d-])(?P<y>\\d{4})-(?P<m>\\d{2})-(?P<d>\\d{2})(?![\\d-])(?P<g>)", numeric_date, NULL);
  /* с 1 по 5 мая: с первого */
  ru_sub_in (&text, "\\b(?P<p>[сС]о?|[оО]т)\\s+(?P<d>\\d{1,2})(?=\\s+(?:по|до)\\s+\\d{1,2}(?:-[а-я]+)?\\s+"
             "(?:" MONTH_NAMES ")\\b)", from_day, NULL);
  ru_sub_in (&text, "(?<![\\d,.])(?P<d>\\d{1,2})" ENDING "(?:\\s?[-–—]\\s?(?P<d2>\\d{1,2})(?:-[а-я]+)?)?"
             "\\s+(?P<month>" MONTH_NAMES ")\\b(?:\\s+" YEAR ")?", written_date, NULL);
  ru_sub_in (&text, "(?P<month>\\b" MONTH_ANY ")\\s+" YEAR, month_year, NULL);
  return text;
}

/* ---------------- times ---------------- */

static void
time_of_day (const GMatchInfo *m, GString *out, gpointer data)
{
  g_autofree char *h = ru_group (m, "h"), *mi = ru_group (m, "m"), *s = ru_group (m, "s");
  gint64 hour, minute, second = 0;

  if (!ru_int (h, &hour) || !ru_int (mi, &minute) || (s && *s && !ru_int (s, &second)) ||
      hour > 24 || minute > 59 || second > 59)
    {
      keep (m, out);
      return;
    }
  g_autofree char *prep = preposition (g_match_info_get_string (m), match_start (m));
  RuCase c = first_case (prep);
  g_autofree char *hours = cardinal (hour, c, 'm', FALSE, FALSE);
  if (g_str_equal (mi, "00") && !(s && *s))
    {
      g_string_append_printf (out, "%s %s", hours, noun_form_n ("час", hour, c, TRUE));
      return;
    }
  g_string_append (out, hours);
  const char *parts[] = { mi, s };
  for (guint i = 0; i < G_N_ELEMENTS (parts); i++)
    {
      gint64 n;
      if (!parts[i] || !*parts[i] || !ru_int (parts[i], &n))
        continue;
      g_autofree char *w = cardinal (n, c == RU_ACC ? RU_NOM : c, 'f', FALSE, FALSE);   /* в 21:01: ноль одна */
      if (parts[i][0] == '0')
        g_string_append (out, " ноль");
      g_string_append_printf (out, " %s", w);
    }
}

static char *
times (char *text)
{
  ru_sub_in (&text, "(?<![\\d:.,])(?P<h>\\d{1,2}):(?P<m>\\d{2})(?::(?P<s>\\d{2}))?(?![\\d:])", time_of_day, NULL);
  return text;
}

/* ---------------- years ---------------- */

static gboolean
after_noun (const char *text, gsize start)
{
  GMatchInfo *m = NULL;

  if (!ru_search ("(?<![\\w+-])([" LETTER "+]{3,})[ \\t]+$", text, start, &m))
    return FALSE;
  g_autofree char *w = g_match_info_fetch (m, 1);
  g_match_info_free (m);
  g_autofree char *plain = ru_plain (w);
  if (ru_in (ru_list ("NOT_NOUNS"), plain))
    return FALSE;
  for (const char *const *e = ru_list ("VERB_ENDINGS"); *e; e++)
    if (ru_ends (plain, *e))
      return FALSE;
  return TRUE;
}

static void
from_year (const GMatchInfo *m, GString *out, gpointer data)
{
  g_autofree char *p = ru_group (m, "p"), *y = ru_group (m, "y");
  gint64 n = 0;

  ru_int (y, &n);
  g_autofree char *w = ordinal (n, RU_GEN, 'm', FALSE, FALSE);
  g_string_append_printf (out, "%s %s", p, w);
}

/* the case of a noun form among a noun's six forms, those of `cases` first: -1 when it is none of them */
static int
form_case (const char *word, const char *const *forms, const char *cases)
{
  int options[6], n = 0;

  for (int c = 0; c < RU_CASES; c++)
    if (g_str_equal (forms[c], word))
      options[n++] = c;
  for (const char *c = cases; *c; c++)
    for (int i = 0; i < n; i++)
      if (options[i] == (int) ru_case_of (*c))
        return options[i];
  return n ? options[0] : -1;
}

static void
year (const GMatchInfo *m, GString *out, gpointer data)
{
  const char *text = g_match_info_get_string (m);
  const RuNoun *god = ru_noun ("год");
  g_autofree char *noun = ru_group (m, "noun"), *suffix = ru_group (m, "suf");
  g_autofree char *y = ru_group (m, "y"), *y2 = ru_group (m, "y2");
  g_autofree char *prep = preposition (text, match_start (m));
  gboolean plural_noun = g_str_equal (noun, "гг.") || in_forms (noun, god, 6);
  const char *const *stressed = god->stressed + (plural_noun ? 6 : 0);
  const char *const *plain = (const char *const *) god->plain + (plural_noun ? 6 : 0);
  g_autofree char *word = NULL;
  RuCase c;

  if (g_str_equal (noun, "г.") || g_str_equal (noun, "гг."))
    {
      c = *prep ? ru_case_of (cases_of (prep, "YEAR_PREPOSITIONS")[0])
                : after_noun (text, match_start (m)) ? RU_GEN : RU_NOM;
      word = ru_replace (stressed[c], "л+ет", "год+ов");    /* до 1995 гг. */
      if (sentence_end (text, match_end (m)))
        {
          char *dot = g_strconcat (word, ".", NULL);
          g_free (word);
          word = dot;
        }
    }
  else
    {
      int found = form_case (noun, plain, cases_of (prep, "YEAR_PREPOSITIONS"));
      c = found < 0 ? RU_NOM : found;
      word = restress (noun, stressed[c]);
    }
  gboolean plural = suffix && *suffix && plural_noun;
  const char *years[] = { y, y2 };
  for (guint i = 0; i < G_N_ELEMENTS (years); i++)
    {
      gint64 n;
      if (!years[i] || !*years[i] || !ru_int (years[i], &n))
        continue;
      g_autofree char *w = ordinal (n, c, 'm', plural, FALSE);
      g_string_append_printf (out, "%s%s", i ? " — " : "", w);
    }
  g_string_append_printf (out, " %s", word);
}

static char *
years (char *text)
{
  g_autofree char *forms = noun_forms ("год", "лет");
  g_autofree char *from = g_strdup_printf ("\\b(?P<p>[сС]о?|[оО]т)\\s+(?P<y>\\d{3,4})(?=\\s+(?:по|до)\\s+\\d{3,4}"
                                           "\\s*(?:гг?\\.|(?:%s)\\b))", forms);
  g_autofree char *pattern = g_strdup_printf ("(?<![\\d,.])(?P<y>\\d{3,4})(?:\\s?[-–—]\\s?(?P<y2>\\d{3,4}))?"
                                              "(?P<suf>-[а-яё]+)?\\s*(?P<noun>гг\\.|г\\.|(?:%s)\\b)", forms);

  ru_sub_in (&text, from, from_year, NULL);
  ru_sub_in (&text, pattern, year, NULL);
  return text;
}

/* ---------------- Roman numerals ---------------- */

static char *
to_roman (int n)
{
  static const struct { int value; const char *symbol; } ROMAN[] = {
    { 1000, "M" }, { 900, "CM" }, { 500, "D" }, { 400, "CD" }, { 100, "C" }, { 90, "XC" }, { 50, "L" },
    { 40, "XL" }, { 10, "X" }, { 9, "IX" }, { 5, "V" }, { 4, "IV" }, { 1, "I" },
  };
  GString *out = g_string_new (NULL);

  for (guint i = 0; i < G_N_ELEMENTS (ROMAN); i++)
    while (n >= ROMAN[i].value)
      {
        g_string_append (out, ROMAN[i].symbol);
        n -= ROMAN[i].value;
      }
  return g_string_free (out, FALSE);
}

/* a Roman numeral's value (Cyrillic Х read as X); 0 when it is not one written the usual way */
static int
roman (const char *token)
{
  g_autofree char *latin = ru_replace (token, "Х", "X");
  int total = 0, previous = 0;

  for (int i = strlen (latin) - 1; i >= 0; i--)
    {
      const char *at = strchr ("IVXLCDM", latin[i]);
      static const int VALUES[] = { 1, 5, 10, 50, 100, 500, 1000 };
      if (!at || !latin[i])
        return 0;
      int value = VALUES[at - "IVXLCDM"];
      total = value < previous ? total - value : total + value;
      previous = MAX (previous, value);
    }
  if (total <= 0 || total >= 4000)
    return 0;
  g_autofree char *again = to_roman (total);
  return g_str_equal (again, latin) ? total : 0;
}

static void
century (const GMatchInfo *m, GString *out, gpointer data)
{
  const char *text = g_match_info_get_string (m);
  const RuNoun *vek = ru_noun ("век");
  g_autofree char *noun = ru_group (m, "noun"), *n1 = ru_group (m, "n"), *n2 = ru_group (m, "n2");
  g_autofree char *prep = preposition (text, match_start (m));
  g_autofree char *word = NULL;
  RuCase c;

  if (g_str_equal (noun, "в.") || g_str_equal (noun, "вв."))
    {
      c = ru_case_of (cases_of (prep, "YEAR_PREPOSITIONS")[0]);
      word = g_strconcat (vek->stressed[(g_str_equal (noun, "вв.") ? 6 : 0) + c],
                          sentence_end (text, match_end (m)) ? "." : "", NULL);
    }
  else
    {
      int from = in_forms (noun, vek, 0) ? 0 : 6;
      int found = form_case (noun, (const char *const *) vek->plain + from, cases_of (prep, "YEAR_PREPOSITIONS"));
      c = found < 0 ? RU_NOM : found;
      word = restress (noun, vek->stressed[from + c]);
    }
  const char *numerals[] = { n1, n2 };
  int values[2], n = 0;
  for (guint i = 0; i < G_N_ELEMENTS (numerals); i++)
    if (numerals[i] && *numerals[i])
      {
        values[n] = roman (numerals[i]);
        if (!values[n++])
          {
            keep (m, out);
            return;
          }
      }
  for (int i = 0; i < n; i++)
    {
      g_autofree char *w = ordinal (values[i], c, 'm', FALSE, FALSE);
      g_string_append_printf (out, "%s%s", i ? " — " : "", w);
    }
  g_string_append_printf (out, " %s", word);
}

static void
monarch (const GMatchInfo *m, GString *out, gpointer data)
{
  g_autofree char *word = ru_group (m, "name"), *numeral = ru_group (m, "n");
  g_autofree char *plain = ru_plain (word);
  const char *lemma = ru_form_lemma (plain);
  int value = roman (numeral);
  char gender;
  RuCase c = RU_NOM;

  if (!value || !(lemma || g_unichar_isupper (g_utf8_get_char (word))))
    {
      keep (m, out);
      return;
    }
  if (lemma)                                    /* глава IV, том II */
    {
      const RuNoun *noun = ru_noun (lemma);
      gender = noun->gender;
      for (int i = 0; i < RU_CASES; i++)
        if (g_str_equal (noun->plain[i], plain))
          {
            c = i;
            break;
          }
      char *again = restress (word, noun->stressed[c]);
      g_free (word);
      word = again;
    }
  else                                          /* Пётр I, при Екатерине II */
    {
      const char *by_ending = "N";
      gender = 'm';
      for (const char *const *q = ru_list ("QUEENS"); *q; q++)
        if (ru_starts (plain, *q))
          gender = 'f';
      for (const RuPair *e = ru_pairs (gender == 'f' ? "NAME_ENDINGS_F" : "NAME_ENDINGS_M"); e->key; e++)
        if (ru_ends (plain, e->key))
          {
            by_ending = e->value;
            break;
          }
      g_autofree char *prep = preposition (g_match_info_get_string (m), match_start (m));
      const char *by_preposition = cases_of (prep, NULL);
      c = ru_case_of (by_ending[0]);
      for (const char *p = by_preposition; *p; p++)
        if (strchr (by_ending, *p))
          {
            c = ru_case_of (*p);
            break;
          }
    }
  g_autofree char *w = ordinal (value, c, gender, FALSE, FALSE);
  g_string_append_printf (out, "%s %s", word, w);
}

static char *
romans (char *text)
{
  g_autofree char *forms = noun_forms ("век", NULL);
  g_autofree char *pattern = g_strdup_printf ("(?<![\\w-])(?P<n>[IVXLCХ]+)(?:\\s?[-–—]\\s?(?P<n2>[IVXLCХ]+))?\\s+"
                                              "(?P<noun>вв\\.|в\\.|(?:%s)\\b)", forms);

  ru_sub_in (&text, pattern, century, NULL);
  ru_sub_in (&text, "(?P<name>\\b[" UPPER "]?[а-яё]{2,})[ \\t]+(?P<n>[IVX]+)\\b(?![\\w-])", monarch, NULL);
  return text;
}

/* ---------------- places and abbreviations ---------------- */

static void
place (const GMatchInfo *m, GString *out, gpointer data)
{
  const char *lemma = data, *text = g_match_info_get_string (m);

  if (ru_search ("\\d\\s*$", text, match_start (m), NULL))     /* 5 ч. 30 мин. is a unit, not часть */
    {
      keep (m, out);
      return;
    }
  g_autofree char *prep = preposition (text, match_start (m));
  g_string_append (out, ru_noun (lemma)->stressed[ru_case_of (cases_of (prep, "LOCATIVE_PREPOSITIONS")[0])]);
}

static char *
places (char *text)
{
  for (const RuPlace *p = ru_places (); p->pattern; p++)
    {
      const char *after = g_str_equal (p->follows, "name") ? "(?=\\s*[" UPPER "])"
                        : g_str_equal (p->follows, "number") ? "(?=\\s*\\d)" : "(?=\\s*\\S)";
      g_autofree char *pattern = g_strdup_printf ("(?<![\\w.-])%s%s", p->pattern, after);
      ru_sub_in (&text, pattern, place, (gpointer) p->lemma);
    }
  return text;
}

static void
expand (const GMatchInfo *m, GString *out, gpointer data)
{
  const char *words = data, *text = g_match_info_get_string (m);

  if (ru_search ("\\d\\s?$", text, match_start (m), NULL))     /* 75 см. is a unit, not смотри */
    {
      keep (m, out);
      return;
    }
  g_string_append (out, words);
  if (ru_in (ru_list ("SENTENCE_FINAL"), words) && sentence_end (text, match_end (m)))
    g_string_append_c (out, '.');
}

static char *
abbreviations (char *text)
{
  for (const RuPair *a = ru_pairs ("ABBREVIATIONS"); a->key; a++)
    {
      const char *rest = g_utf8_next_char (a->key);
      g_autofree char *first = g_strndup (a->key, rest - a->key);
      g_autofree char *upper = ru_upper (first);                 /* См. рис. 3, Т.е. */
      g_autofree char *pattern = g_strdup_printf ("(?<![\\w.])(?:%s|%s%s)", a->key, upper, rest);
      ru_sub_in (&text, pattern, expand, (gpointer) a->value);
    }
  return text;
}

/* ---------------- fractions, signs, powers ---------------- */

static void
fraction (const GMatchInfo *m, GString *out, gpointer data)
{
  const char *text = g_match_info_get_string (m);
  g_autofree char *a = g_match_info_fetch (m, 1), *b = g_match_info_fetch (m, 2);
  g_autofree char *before = word_before (text, match_start (m));
  g_autofree char *plain = before ? ru_plain (before) : NULL;
  gint64 numerator, denominator;

  if (ru_int (a, &numerator) && ru_int (b, &denominator) && 0 < numerator && numerator < denominator &&
      denominator <= 100 && !(plain && ru_form_lemma (plain)))
    {
      g_autofree char *prep = preposition (text, match_start (m));
      char *w = ru_fraction (numerator, denominator, first_case (prep));
      if (w)
        {
          g_autofree char *said = marked (w);
          g_string_append (out, said);
          return;
        }
    }
  g_string_append_printf (out, "%s дробь %s", a, b);
}

static char *
fractions (char *text)
{
  ru_sub_in (&text, "(?<![\\d/,])(\\d+)/(\\d+)(?![\\d/])", fraction, NULL);
  return text;
}

static void
currency_last (const GMatchInfo *m, GString *out, gpointer data)
{
  g_autofree char *c = ru_group (m, "c"), *n = ru_group (m, "n"), *s = ru_group (m, "s");

  g_string_append (out, n);
  if (s && *s)
    g_string_append_printf (out, " %s", s);
  g_string_append_printf (out, " %s", c);
}

static char *
signs (char *text)
{
  ru_subs_in (&text, "(?:(?<=[\\s(\"])|^)[-–](?=\\d)", "минус ");
  ru_subs_in (&text, "(?:(?<=[\\s(\"])|^)\\+(?=\\d)", "плюс ");
  ru_subs_in (&text, "[~≈]\\s?(?=\\d)", "около ");
  ru_subs_in (&text, "(?<=\\d)\\s?[×xхX*]\\s?(?=\\d)", " на ");
  ru_sub_in (&text, "(?P<c>[$€£₽¥₴])\\s?(?P<n>" NUMBER ")(?:\\s?(?P<s>тыс\\.|млн|млрд|трлн))?", currency_last, NULL);
  return text;
}

static char *
powers (char *text)
{
  ru_subs_in (&text, "(?<![\\d,])(\\d+)\\s?\\^\\s?(\\d+)(?!\\d)", "\\1 в степени \\2");
  return text;
}

/* ---------------- quantities ---------------- */

/* what is said, and the noun after it with its stress, NULL when it is left as it is */
typedef struct {
  char *said;
  char *noun;
} Said;

static RuCase
counting_case (const char *prep, const Value *v)
{
  if (g_str_equal (prep, "по"))                 /* по одному рублю (dat), по пять рублей (acc) */
    return !v->decimal && is_one (v->n) ? RU_DAT : RU_ACC;
  return first_case (prep);
}

static Said
ordinal_agreeing (gint64 n, const char *lemma, const char *word, const char *cases)
{
  const RuNoun *noun = ru_noun (lemma);
  g_autofree char *all = g_strconcat (cases, ALL_CASES, NULL);

  for (int pass = 0; pass < 2; pass++)
    {
      gboolean plural = pass == 1 || noun->gender == 'p';
      int from = pass * 6;
      for (const char *c = all; *c; c++)
        if (g_str_equal (noun->plain[from + ru_case_of (*c)], word))
          return (Said) { ordinal (n, ru_case_of (*c), plural ? 'm' : noun->gender, plural, noun->animate),
                          g_strdup (noun->stressed[from + ru_case_of (*c)]) };
    }
  return (Said) { ordinal (n, ru_case_of (cases[0]), 'm', FALSE, FALSE), NULL };
}

static gboolean
cardinal_ending (gint64 n, const char *ending, const char *prep, const char *word)
{
  if (g_str_equal (ending, "х"))
    return is_few (n);
  if (g_str_equal (ending, "м"))
    return g_str_equal (prep, "к") || g_str_equal (prep, "ко");
  if (g_str_equal (ending, "ю"))
    return g_str_equal (prep, "с") || g_str_equal (prep, "со") || g_str_equal (prep, "между") ||
           g_str_equal (prep, "над") || g_str_equal (prep, "перед") || ru_ends (word, "ами") ||
           ru_ends (word, "ями") || ru_ends (word, "ьми");
  return TRUE;
}

/* a number with an ending (5-летний, 2-хкомнатная, 5-й, 10-ти) */
static Said
suffixed (gint64 n, const char *ending, const char *lemma, const char *word, const char *prep)
{
  const char *cases = cases_of (prep, NULL);
  const RuEnding *readings = ru_ordinal_ending (ending);
  gboolean cardinal_end = ru_in (ru_list ("CARDINAL_ENDINGS"), ending);
  const RuNoun *noun = ru_noun (lemma);

  if (!readings && !cardinal_end)
    {
      char *prefix;
      if (0 < n && n < 1000)
        prefix = ru_compound_prefix (n);
      else
        {
          g_autofree char *w = ru_cardinal (n, RU_GEN, 'm', FALSE, FALSE);
          g_autofree char *num = w ? g_steal_pointer (&w) : spelt (n);
          prefix = g_strconcat (num, " ", NULL);
        }
      /* the ending's start that the prefix already ends with is not written twice */
      glong length = g_utf8_strlen (ending, -1), overlap = 0;
      for (glong k = 0; k <= length; k++)
        {
          g_autofree char *head = g_utf8_substring (ending, 0, k);
          if (g_str_has_suffix (prefix, head))
            overlap = k;
        }
      char *said = g_strconcat (prefix, g_utf8_offset_to_pointer (ending, overlap), NULL);
      g_free (prefix);
      return (Said) { said, NULL };
    }
  char gender = noun ? (noun->gender == 'p' ? 'm' : noun->gender) : 'm';
  gboolean animate = noun && noun->animate;
  if (cardinal_end && cardinal_ending (n, ending, prep, word))
    {
      char noun_cases[RU_CASES + 1] = "";
      int k = 0;
      for (int c = 0; c < RU_CASES && noun; c++)
        if (g_str_equal (noun_form_n (lemma, n, c, FALSE), word))
          noun_cases[k++] = ALL_CASES[c];
      noun_cases[k] = 0;
      g_autofree char *order = g_strconcat (noun_cases, cases, "GDPI", NULL);
      for (const char *c = order; *c; c++)
        {
          RuCase rc = ru_case_of (*c);
          if (rc == RU_NOM || rc == RU_ACC)
            continue;
          g_autofree char *plain = ru_cardinal (n, rc, gender, FALSE, FALSE);
          if (plain && ru_ends (plain, ending))
            return (Said) { cardinal (n, rc, gender, animate, FALSE),
                            noun && strchr (noun_cases, *c) ? g_strdup (noun_form_n (lemma, n, rc, TRUE)) : NULL };
        }
    }
  if (!readings)
    return (Said) { cardinal_nom (n), NULL };
  if (noun)
    for (int r = 0; r < readings->n; r++)
      {
        RuCase c = readings->readings[r].c;
        const char *reading = readings->readings[r].gender;
        gboolean starred = ru_ends (reading, "*");
        g_autofree char *bare = g_strndup (reading, strcspn (reading, "*"));
        for (int pass = 0; pass < 2; pass++)
          {
            gboolean plural = pass == 1;
            char own_g[2] = { noun->gender, 0 };
            const char *own = plural ? "pl" : own_g;
            gboolean fits = g_str_equal (bare, own) ||
                            (g_str_equal (reading, "m") && g_str_equal (own, "n") && c != RU_NOM && c != RU_ACC);
            if (fits && g_str_equal (noun->plain[pass * 6 + c], word) && (noun->animate || !starred))
              return (Said) { ordinal (n, c, plural ? 'm' : noun->gender, plural, noun->animate),
                              g_strdup (noun->stressed[pass * 6 + c]) };
          }
      }
  const RuReading *filtered[6];
  int nf = 0;
  for (int r = 0; r < readings->n; r++)
    if (!ru_ends (readings->readings[r].gender, "*"))
      filtered[nf++] = &readings->readings[r];
  if ((g_str_equal (ending, "е") || g_str_equal (ending, "ое") || g_str_equal (ending, "ье")) && !noun &&
      n % 10 == 0 && n >= 20)
    {
      int kept = 0;
      for (int r = 0; r < nf; r++)              /* 90-е, 2000-е */
        if (g_str_equal (filtered[r]->gender, "pl"))
          filtered[kept++] = filtered[r];
      nf = kept;
    }
  if (!nf)
    return (Said) { cardinal_nom (n), NULL };
  const RuReading *chosen = filtered[0];
  gboolean found = FALSE;
  for (const char *c = cases; *c && !found; c++)
    for (int r = 0; r < nf && !found; r++)
      if (filtered[r]->c == (int) ru_case_of (*c))
        {
          chosen = filtered[r];
          found = TRUE;
        }
  gboolean plural = g_str_equal (chosen->gender, "pl");
  return (Said) { ordinal (n, chosen->c, plural ? 'm' : chosen->gender[0], plural, FALSE), NULL };
}

/* a noun the tables do not know, guessed from its ending: its case (-1 for none) and gender */
static int
unknown_noun (const char *word, const Value *count, const char *cases, char *gender)
{
  gint64 n = count->n;

  *gender = 'm';
  if (ru_chars (word) < 4 || count->decimal)
    return -1;
  if (is_one (n))
    {
      if (strchr (cases, 'I'))
        for (const RuPair *e = ru_pairs ("SINGULAR_INSTRUMENTAL"); e->key; e++)
          if (ru_ends (word, e->key))
            {
              *gender = e->value[0];
              return RU_INS;
            }
      *gender = ru_ends (word, "о") ? 'n' : 'm';
      return -1;
    }
  for (const RuPair *e = ru_pairs ("OBLIQUE_PLURAL"); e->key; e++)
    if (ru_ends (word, e->key))
      return ru_case_of (e->value[0]);
  if (is_few (n))
    {
      if (ru_ends (word, "ов") || ru_ends (word, "ев") || ru_ends (word, "ёв"))
        return RU_GEN;
      if (ru_ends (word, "ы") || ru_ends (word, "и"))
        *gender = 'f';
    }
  return -1;
}

static Said
bare_number (const char *text, const char *value, const Value *values, int n_values, const char *prep, gsize end)
{
  g_autofree char *word = next_word (text, end);
  const char *lemma = ru_form_lemma (word);
  const Value *last = &values[n_values - 1];
  const char *cases = cases_of (prep, NULL);
  GString *out;

  if (!lemma)
    {
      gboolean single = n_values == 1 && is_digit_string (value);
      char gender;
      int c;
      if ((g_str_equal (prep, "в") || g_str_equal (prep, "во")) && single && !last->decimal && last->n >= 1800 &&
          last->n <= 2099 && !ru_match_at ("\\s*\\d", text, end, NULL))
        return (Said) { ordinal (last->n, RU_PREP, 'm', FALSE, FALSE), NULL };  /* в 1999 (году) */
      if (single && (ru_chars (value) > 15 || (ru_chars (value) > 1 && value[0] == '0')))
        return (Said) { ru_digits (value), NULL };
      c = unknown_noun (word, last, cases, &gender);
      out = g_string_new (NULL);
      for (int i = 0; i < n_values; i++)
        {
          const Value *v = &values[i];
          g_autofree char *w = c >= 0 && !v->decimal ? cardinal (v->n, c, gender, FALSE, FALSE)
                             : gender == 'm' || v->decimal ? say (v, NULL, counting_case (prep, v))
                             : cardinal (v->n, counting_case (prep, v), gender, FALSE, FALSE);
          g_string_append_printf (out, "%s%s", i ? " — " : "", w);
        }
      return (Said) { g_string_free (out, FALSE), NULL };
    }
  if ((g_str_equal (lemma, "век") || g_str_equal (lemma, "число")) && !last->decimal)
    {
      Said said = { NULL, NULL };
      out = g_string_new (NULL);
      for (int i = 0; i < n_values; i++)
        if (!values[i].decimal)
          {
            g_free (said.said);
            g_free (said.noun);
            said = ordinal_agreeing (values[i].n, lemma, word, cases);
            g_string_append_printf (out, "%s%s", out->len ? " — " : "", said.said);
          }
      g_free (said.said);
      return (Said) { g_string_free (out, FALSE), said.noun };
    }
  g_autofree char *order = g_strdup_printf ("%c%s", ALL_CASES[counting_case (prep, last)], cases);
  int c = count_case (last, lemma, word, order);
  if (c < 0 && !last->decimal && n_values == 1)    /* на 5 этаже: на пятом этаже */
    return ordinal_agreeing (last->n, lemma, word, cases);
  out = g_string_new (NULL);
  for (int i = 0; i < n_values; i++)
    {
      g_autofree char *w = say (&values[i], lemma, c < 0 ? ru_case_of (cases[0]) : (RuCase) c);
      g_string_append_printf (out, "%s%s", i ? " — " : "", w);
    }
  return (Said) { g_string_free (out, FALSE), c < 0 ? NULL : g_strdup (noun_form (lemma, last, c, TRUE)) };
}

/* what is said, followed by the noun after it with its stress; *end moved past that noun */
static char *
with_noun (const char *text, gsize *end, Said said)
{
  GMatchInfo *m = NULL;
  int word_start, word_end;

  if (!said.noun || !ru_match_at (NEXT, text, *end, &m))
    {
      g_free (said.noun);
      return said.said;
    }
  g_match_info_fetch_pos (m, 1, &word_start, &word_end);
  g_autofree char *w = g_match_info_fetch (m, 1);
  g_autofree char *stressed = restress (w, said.noun);
  g_autofree char *between = g_strndup (text + *end, word_start - *end);
  char *out = g_strconcat (said.said, between, stressed, NULL);
  *end = word_end;
  g_match_info_free (m);
  g_free (said.said);
  g_free (said.noun);
  return out;
}

static char *
quantity (const GMatchInfo *m, gsize *end)
{
  const char *src = g_match_info_get_string (m);
  g_autofree char *n1 = ru_group (m, "n"), *n2 = ru_group (m, "n2"), *suf = ru_group (m, "suf");
  g_autofree char *scale_word = ru_group (m, "scale"), *unit_word = ru_group (m, "unit");
  g_autofree char *bare = ru_group (m, "bare"), *dot_mark = ru_group (m, "dot");
  const char *unit = unit_word ? unit_word : bare;
  const char *scale = scale_word ? ru_lookup (ru_pairs ("SCALES"), scale_word) : NULL;
  Value values[2] = { value_of (n1) };
  int n_values = 1;
  g_autofree char *prep = preposition (src, match_start (m));

  *end = match_end (m);
  if (n2)
    values[n_values++] = value_of (n2);
  if (suf && !unit && !scale)
    {
      GString *out;
      Said said = { NULL, NULL };
      for (int i = 0; i < n_values; i++)
        if (values[i].decimal)
          return g_match_info_fetch (m, 0);
      g_autofree char *word = next_word (src, *end);
      out = g_string_new (NULL);
      for (int i = 0; i < n_values; i++)
        {
          g_free (said.noun);
          said = suffixed (values[i].n, suf, ru_form_lemma (word), word, prep);
          g_string_append_printf (out, "%s%s", i ? " — " : "", said.said);
          g_free (said.said);
        }
      return with_noun (src, end, (Said) { g_string_free (out, FALSE), said.noun });
    }
  if (!unit && !scale)
    return with_noun (src, end, bare_number (src, n1, values, n_values, prep, *end));

  const RuUnit *u = unit ? ru_unit (unit) : NULL;
  const char *lemma = u ? u->lemma : NULL, *tail = u ? u->tail : "";
  RuCase c = scale ? first_case (prep) : counting_case (prep, &values[n_values - 1]);
  if ((g_str_equal (prep, "в") || g_str_equal (prep, "во")) && lemma &&
      (((g_str_equal (lemma, "километр") || g_str_equal (lemma, "метр")) && ru_match_at ("\\s+от\\b", src, *end, NULL))
       || (g_str_equal (lemma, "процент") && ru_match_at ("\\s+случа", src, *end, NULL))))
    c = RU_PREP;                                /* в 5 км от города, в 15% случаев */
  const char *dot = dot_mark && *dot_mark && (!ru_in (ru_list ("DOTTED"), unit) || sentence_end (src, *end)) ? "." : "";
  const char *cents = lemma ? ru_lookup (ru_pairs ("CENTS"), lemma) : NULL;
  GString *out = g_string_new (NULL);

  if (cents && !scale && n_values == 1 && values[0].decimal)
    {
      const char *comma = strchr (values[0].text, ',');
      g_autofree char *whole_text = g_strndup (values[0].text, comma - values[0].text);
      gint64 whole, part;
      if (ru_chars (comma + 1) == 2 && ru_int (whole_text, &whole) && ru_int (comma + 1, &part))  /* 12,50 руб. */
        {
          Value w = { whole_text, FALSE, whole }, p = { comma + 1, FALSE, part };
          g_autofree char *words = amount (&p, cents, c, NULL);
          g_autofree char *main_words = amount (&w, lemma, c, NULL);
          if (whole)
            g_string_append_printf (out, "%s%s%s %s", main_words, *tail ? " " : "", tail, words);
          else
            g_string_append (out, words);
          g_string_append (out, dot);
          return g_string_free (out, FALSE);
        }
    }
  for (int i = 0; i < n_values - 1; i++)
    {
      g_autofree char *w = say (&values[i], scale ? scale : lemma, c);
      g_string_append (out, w);
      if (scale)
        g_string_append_printf (out, " %s", noun_form (scale, &values[i], c, TRUE));
      g_string_append (out, " — ");
    }
  g_autofree char *last = amount (&values[n_values - 1], lemma, c, scale);
  g_string_append (out, last);
  if (*tail)
    g_string_append_printf (out, " %s", tail);
  g_string_append (out, dot);
  return g_string_free (out, FALSE);
}

static char *
quantities (char *text)
{
  static char *pattern;
  gsize done = 0;
  GMatchInfo *m = NULL;
  GString *out;

  if (g_once_init_enter_pointer (&pattern))
    {
      gsize n;
      const RuUnit *units = ru_units (&n);
      g_autoptr (GPtrArray) words = g_ptr_array_new ();
      for (gsize i = 0; i < n; i++)
        if (!ru_in (ru_list ("BARE"), units[i].unit))
          g_ptr_array_add (words, (gpointer) units[i].unit);
      g_autofree char *unit_alternatives = alternatives (words, TRUE);
      g_autofree char *bare = g_strjoinv ("|", (char **) ru_list ("BARE"));
      GString *scales = g_string_new (NULL);
      for (const RuPair *s = ru_pairs ("SCALES"); s->key; s++)
        g_string_append_printf (scales, "%s%s", scales->len ? "|" : "", s->key);
      char *built = g_strdup_printf ("(?<![\\d,])(?P<n>" NUMBER ")(?:\\s?[-–—]\\s?(?P<n2>" NUMBER "))?"
                                     "(?:-(?P<suf>[а-яё]+))?(?:\\s?(?P<scale>%s)\\.?(?![" LETTER "]))?"
                                     "(?:\\s?(?P<unit>%s)(?P<dot>\\.)?(?![" LETTER "A-Za-z])"
                                     "|\\s(?P<bare>%s)(?=\\s*(?:[" PUNCTUATION_CLASS "]|$)))?",
                                     scales->str, unit_alternatives, bare);
      g_string_free (scales, TRUE);
      g_once_init_leave_pointer (&pattern, built);
    }
  out = g_string_new (NULL);
  g_regex_match (ru_rx (pattern), text, 0, &m);
  while (g_match_info_matches (m))
    {
      gsize end, start = match_start (m);
      g_autofree char *said = quantity (m, &end);
      if (start > done)
        g_string_append_len (out, text + done, start - done);
      g_string_append (out, said);
      done = end;
      g_match_info_next (m, NULL);
    }
  g_match_info_free (m);
  if (done < strlen (text))
    g_string_append (out, text + done);
  g_free (text);
  return g_string_free (out, FALSE);
}

/* ---------------- standalone units, acronyms, Latin, symbols ---------------- */

static void
standalone_unit (const GMatchInfo *m, GString *out, gpointer data)
{
  const char *const *pair = data;

  g_string_append (out, pair[1]);
  if (ru_ends (pair[0], ".") && sentence_end (g_match_info_get_string (m), match_end (m)))
    g_string_append_c (out, '.');
}

static char *
standalone (char *text)
{
  static const char *const SHORT[][2] = {
    { "тыс.", "тысяча" }, { "млн", "миллион" }, { "млрд", "миллиард" }, { "трлн", "триллион" }, { "руб.", "рубль" },
    { "коп.", "копейка" }, { "км", "километр" }, { "кг", "килограмм" },
  };

  for (guint i = 0; i < G_N_ELEMENTS (SHORT); i++)
    {
      const char *pair[2] = { SHORT[i][0], ru_noun (SHORT[i][1])->stressed[6 + RU_GEN] };
      g_autofree char *escaped = regex_escape (SHORT[i][0]);
      g_autofree char *pattern = g_strdup_printf ("(?<![\\w.])%s(?![\\w])", escaped);
      ru_sub_in (&text, pattern, standalone_unit, pair);
    }
  return text;
}

static int
vowel_count (const char *token, glong *length)
{
  int vowels = 0;

  *length = 0;
  for (const char *p = token; *p; p = g_utf8_next_char (p), (*length)++)
    {
      gunichar c = g_utf8_get_char (p);
      char buffer[8] = { 0 };
      g_unichar_to_utf8 (c, buffer);
      if (strstr (VOWELS, buffer))
        vowels++;
    }
  return vowels;
}

static gboolean
word_like (const char *token)
{
  glong length;
  int vowels = vowel_count (token, &length);

  if (length >= 5)
    return (double) vowels / length >= 0.25;
  return length == 4 && vowels > 0 && !ru_search ("[^" VOWELS "]{3}", token, -1, NULL);
}

static gboolean
shouting_word (GMatchInfo *n)
{
  if (!n)
    return FALSE;
  g_autofree char *w = g_match_info_fetch (n, 1);
  g_autofree char *lower = ru_lower (w);
  return word_like (w) && !ru_in (ru_list ("ACRONYM_WORDS"), lower);
}

static void
acronym (const GMatchInfo *m, GString *out, gpointer data)
{
  const char *text = g_match_info_get_string (m);
  g_autofree char *token = ru_group (m, "a"), *s = ru_group (m, "s");
  const char *suffix = s ? s : "";
  g_autofree char *key = ru_lower (token);
  GMatchInfo *before = NULL, *after = NULL;
  gboolean shouting;

  if (!ru_search ("(?<![\\w+])([" UPPER "]+)\\s+$", text, match_start (m), &before))
    before = NULL;
  if (!ru_match_at ("\\s+([" UPPER "]+)(?![\\w+])", text, match_end (m), &after))
    after = NULL;
  shouting = shouting_word (before) || shouting_word (after);
  g_match_info_free (before);
  g_match_info_free (after);
  if (ru_in (ru_list ("ACRONYM_WORDS"), key) || word_like (token) || (shouting && !*suffix))   /* ЭТО ОЧЕНЬ ВАЖНО */
    {
      g_string_append_printf (out, "%s%s", key, suffix);
      return;
    }
  for (const char *p = key; *p; p = g_utf8_next_char (p))
    {
      g_autofree char *letter = g_strndup (p, g_utf8_next_char (p) - p);
      const char *name = ru_lookup (ru_pairs ("LETTERS"), letter);
      g_string_append_printf (out, "%s%s", p == key ? "" : " ", name ? name : letter);
    }
  if (*suffix)
    g_string_append_printf (out, " %s", suffix);
}

static char *
acronyms (char *text)
{
  ru_sub_in (&text, "(?<![\\w+])(?P<a>[" UPPER "]{2,})(?P<s>[а-яё]+)?(?![\\w+])", acronym, NULL);
  return text;
}

/* a lowercase Latin word in Cyrillic, by rule */
static char *
transliterate (const char *word)
{
  g_autofree char *w = g_strdup (word);
  gsize n = strlen (w), i = 0;
  GString *out = g_string_new (NULL);

  if (n > 4 && w[n - 1] == 'e' && !strchr ("aeiouy", w[n - 2]))
    w[--n] = 0;
  while (i < n)
    {
      if (w[i] == 'c' && i + 1 < n && strchr ("eiy", w[i + 1]))
        {
          g_string_append (out, "с");
          i++;
          continue;
        }
      if (w[i] == 'y' && i == 0 && n > 1 && strchr ("aeiou", w[1]))
        {
          g_string_append (out, "й");
          i++;
          continue;
        }
      if (w[i] == 'e' && i == 0)
        {
          g_string_append (out, "э");
          i++;
          continue;
        }
      const RuPair *rule;
      for (rule = ru_pairs ("LATIN_RULES"); rule->key; rule++)
        if (!strncmp (w + i, rule->key, strlen (rule->key)))
          break;
      if (rule->key)
        {
          g_string_append (out, rule->value);
          i += strlen (rule->key);
        }
      else
        i++;
    }
  return g_string_free (out, FALSE);
}

static char *
latin_word (const char *token)
{
  g_autofree char *key = g_ascii_strdown (token, -1);
  const char *known = ru_lookup (ru_pairs ("LATIN_WORDS"), key);
  gboolean upper = *token != 0;

  if (known)
    return g_strdup (known);
  for (const char *p = token; *p; p++)
    if (g_ascii_islower (*p))
      upper = FALSE;
  if (upper && strlen (token) <= 5)
    {
      GString *out = g_string_new (NULL);
      for (const char *p = key; *p; p++)
        {
          char letter[2] = { *p, 0 };
          const char *name = ru_lookup (ru_pairs ("LATIN_LETTERS"), letter);
          g_string_append_printf (out, "%s%s", p == key ? "" : " ", name ? name : letter);
        }
      return g_string_free (out, FALSE);
    }
  return transliterate (key);
}

static void
latin_words (const GMatchInfo *m, GString *out, gpointer data)
{
  g_autofree char *all = g_match_info_fetch (m, 0);
  g_autofree char *lower = g_ascii_strdown (all, -1);
  const char *known = ru_lookup (ru_pairs ("LATIN_WORDS"), lower);

  if (known)
    {
      g_string_append (out, known);
      return;
    }
  g_auto (GStrv) parts = g_strsplit (all, "-", -1);
  for (char **p = parts; *p; p++)
    {
      g_autofree char *bare = ru_replace (*p, "'", "");
      g_autofree char *w = latin_word (bare);
      g_string_append_printf (out, "%s%s", p == parts ? "" : " ", w);
    }
}

static char *
latin (char *text)
{
  ru_subs_in (&text, "(?<=[A-Za-z])\\.(?=[A-Za-z])", " точка ");       /* example.com */
  ru_sub_in (&text, "(?<![\\w])[A-Za-z]+(?:[-'][A-Za-z]+)*(?![\\w])", latin_words, NULL);
  return text;
}

static char *
symbols (char *text)
{
  /* a stress mark is kept from the plus sign's words: set aside as \x01 meanwhile */
  ru_subs_in (&text, "\\+(?=[" VOWELS "])", "\x01");
  for (const RuPair *s = ru_pairs ("SYMBOLS"); s->key; s++)
    {
      char *again = ru_replace (text, s->key, s->value);
      g_free (text);
      text = again;
    }
  char *again = ru_replace (text, "\x01", "+");
  g_free (text);
  return again;
}

/* ---------------- all of them ---------------- */

char *
ru_normalize (const char *text)
{
  typedef char *(*Pass) (char *text);
  static const Pass PASSES[] = {
    phones, dates, times, groups, years, romans, abbreviations, places, signs, fractions, powers, quantities,
    standalone, acronyms, latin, symbols,
  };
  g_autofree char *fractions_apart = ru_subs ("(?<=\\d)(?=[½¼¾⅓⅔])", text, " и ");
  char *out = ru_unify (fractions_apart);

  ru_subs_in (&out, "(?<=[A-Za-z])(?=\\d)|(?<=\\d)(?=[A-Za-z])", " ");                    /* MP3, 4K */
  ru_subs_in (&out, "(?<=[" LETTER "A-Za-z])-(?=\\d)|(?<=\\d)-(?=[A-Za-z])", " ");         /* Ту-154, COVID-19 */
  ru_subs_in (&out, "[ \\t]+", " ");
  for (guint i = 0; i < G_N_ELEMENTS (PASSES); i++)
    out = PASSES[i] (out);
  char *tidy = ru_tidy (out);
  g_free (out);
  return tidy;
}
