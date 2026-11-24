/* dictate-ru-numbers: Russian cardinals, ordinals, decimals and fractions in words, in any case and gender. A port of
 * tiny-voca-ru's frontend/numbers.py (see dictate-ru.h). */
#include "dictate-ru-private.h"

#include <string.h>

static const char *const ONE[4][6] = {
  { "один", "одного", "одному", "один", "одним", "одном" },
  { "одна", "одной", "одной", "одну", "одной", "одной" },
  { "одно", "одного", "одному", "одно", "одним", "одном" },
  { "одни", "одних", "одним", "одни", "одними", "одних" },
};
static const char *const FEW[5][6] = {
  [2] = { "два", "двух", "двум", "два", "двумя", "двух" },
  [3] = { "три", "трёх", "трём", "три", "тремя", "трёх" },
  [4] = { "четыре", "четырёх", "четырём", "четыре", "четырьмя", "четырёх" },
};
static const char *const COLLECTIVE[5][6] = {
  [2] = { "двое", "двоих", "двоим", "двое", "двоими", "двоих" },
  [3] = { "трое", "троих", "троим", "трое", "троими", "троих" },
  [4] = { "четверо", "четверых", "четверым", "четверо", "четверыми", "четверых" },
};
static const char *const UNITS[20] = {
  "ноль", "один", "два", "три", "четыре", "пять", "шесть", "семь", "восемь", "девять", "десять", "одиннадцать",
  "двенадцать", "тринадцать", "четырнадцать", "пятнадцать", "шестнадцать", "семнадцать", "восемнадцать",
  "девятнадцать",
};
static const char *const TENS[10] = {
  "", "", "двадцать", "тридцать", "сорок", "пятьдесят", "шестьдесят", "семьдесят", "восемьдесят", "девяносто",
};
static const char *const HUNDREDS[10] = {
  "", "сто", "двести", "триста", "четыреста", "пятьсот", "шестьсот", "семьсот", "восемьсот", "девятьсот",
};
static const char *const ZERO[6] = { "ноль", "ноля", "нолю", "ноль", "нолём", "ноле" };

/* the scales, the largest first; тысяча is feminine, the others masculine */
static const struct { gint64 size; const char *forms[2][6]; } SCALES[] = {
  { 1000000000000, { { "триллион", "триллиона", "триллиону", "триллион", "триллионом", "триллионе" },
                     { "триллионы", "триллионов", "триллионам", "триллионы", "триллионами", "триллионах" } } },
  { 1000000000, { { "миллиард", "миллиарда", "миллиарду", "миллиард", "миллиардом", "миллиарде" },
                  { "миллиарды", "миллиардов", "миллиардам", "миллиарды", "миллиардами", "миллиардах" } } },
  { 1000000, { { "миллион", "миллиона", "миллиону", "миллион", "миллионом", "миллионе" },
               { "миллионы", "миллионов", "миллионам", "миллионы", "миллионами", "миллионах" } } },
  { 1000, { { "тысяча", "тысячи", "тысяче", "тысячу", "тысячей", "тысяче" },
            { "тысячи", "тысяч", "тысячам", "тысячи", "тысячами", "тысячах" } } },
};
#define THOUSAND 3              /* SCALES' тысяча */
#define LIMIT    1000000000000000   /* the numerals name up to a thousand trillion less one */

static const char *const ORDINAL_UNITS[20] = {
  "нулев", "перв", "втор", "трет", "четвёрт", "пят", "шест", "седьм", "восьм", "девят", "десят", "одиннадцат",
  "двенадцат", "тринадцат", "четырнадцат", "пятнадцат", "шестнадцат", "семнадцат", "восемнадцат", "девятнадцат",
};
static const char *const ORDINAL_TENS[10] = {
  "", "", "двадцат", "тридцат", "сороков", "пятидесят", "шестидесят", "семидесят", "восьмидесят", "девяност",
};
/* stems stressed on the ending: -ой, not -ый */
static const char *const STRESSED[] = { "нулев", "втор", "шест", "седьм", "восьм", "сороков", NULL };
static const char *const HARD_ENDINGS[4][6] = {
  { "ый", "ого", "ому", "ый", "ым", "ом" },
  { "ая", "ой", "ой", "ую", "ой", "ой" },
  { "ое", "ого", "ому", "ое", "ым", "ом" },
  { "ые", "ых", "ым", "ые", "ыми", "ых" },
};
static const char *const SOFT_ENDINGS[4][6] = {
  { "ий", "ьего", "ьему", "ий", "ьим", "ьем" },
  { "ья", "ьей", "ьей", "ью", "ьей", "ьей" },
  { "ье", "ьего", "ьему", "ье", "ьим", "ьем" },
  { "ьи", "ьих", "ьим", "ьи", "ьими", "ьих" },
};

/* m f n pl → 0..3 */
static int
gender_index (char gender)
{
  return gender == 'f' ? 1 : gender == 'n' ? 2 : gender == 'p' ? 3 : 0;
}

static void
word (GString *out, const char *w)
{
  if (out->len && out->str[out->len - 1] != ' ')
    g_string_append_c (out, ' ');
  g_string_append (out, w);
}

const char *
ru_plural_form (gint64 n, const char *one, const char *few, const char *many)
{
  n = n < 0 ? -n : n;
  if (n % 10 == 1 && n % 100 != 11)
    return one;
  if (n % 10 >= 2 && n % 10 <= 4 && !(n % 100 >= 12 && n % 100 <= 14))
    return few;
  return many;
}

/* пять..двадцать, тридцать declined: -ью in the instrumental, -и in the genitive, dative and prepositional */
static char *
i_declension (const char *w, RuCase c)
{
  if (c == RU_NOM || c == RU_ACC)
    return g_strdup (w);
  if (g_str_equal (w, "восемь"))
    return g_strdup (c == RU_INS ? "восемью" : "восьми");
  g_autofree char *stem = ru_drop (w, 1);
  return g_strconcat (stem, c == RU_INS ? "ью" : "и", NULL);
}

static char *
unit (int n, RuCase c, char gender)
{
  if (n >= 2 && n <= 4)
    {
      if (gender == 'f' && (c == RU_NOM || c == RU_ACC))
        return g_strdup (n == 2 ? "две" : FEW[n][RU_NOM]);
      return g_strdup (FEW[n][c]);
    }
  return i_declension (UNITS[n], c);
}

static char *
tens (int t, RuCase c)
{
  if (c == RU_NOM || c == RU_ACC)
    return g_strdup (TENS[t]);
  if (t == 4)
    return g_strdup ("сорока");
  if (t == 9)
    return g_strdup ("девяноста");
  if (t == 2 || t == 3)
    return i_declension (TENS[t], c);
  g_autofree char *first = i_declension (UNITS[t], c);   /* пятьдесят: пятидесяти, пятьюдесятью */
  return g_strconcat (first, c == RU_INS ? "десятью" : "десяти", NULL);
}

static char *
hundreds (int h, RuCase c)
{
  static const char *const TAIL[RU_CASES] = { [RU_GEN] = "сот", [RU_DAT] = "стам", [RU_INS] = "стами",
                                               [RU_PREP] = "стах" };
  if (c == RU_NOM || c == RU_ACC)
    return g_strdup (HUNDREDS[h]);
  if (h == 1)
    return g_strdup ("ста");
  g_autofree char *u = unit (h, c, 'm');
  return g_strconcat (u, TAIL[c], NULL);
}

/* the last word of a group when its ones are 1-4, which agree differently (cardinal ()'s `last`) */
typedef struct {
  RuCase c;
  char gender;
  gboolean animate, plural, compound;
} Last;

static char *
last_word (const Last *l, int ones, gboolean compound)
{
  gboolean as_genitive = l->c == RU_ACC && l->animate;

  compound = compound || l->compound;
  if (ones == 1)
    {
      const char *const *form = ONE[l->plural ? 3 : gender_index (l->gender)];
      return g_strdup (as_genitive && (l->plural || l->gender == 'm') ? form[RU_GEN] : form[l->c]);
    }
  if (l->plural)
    return g_strdup (COLLECTIVE[ones][as_genitive ? RU_GEN : l->c]);
  if (as_genitive && l->gender != 'n' && !compound)
    return g_strdup (FEW[ones][RU_GEN]);
  return unit (ones, l->c == RU_ACC ? RU_NOM : l->c, l->gender);
}

/* 1..999 in words; FALSE for more */
static gboolean
group (GString *out, gint64 n, RuCase c, char gender, const Last *last)
{
  int h = n / 100, rest = n % 100;
  gboolean any = FALSE;

  if (h >= 10)
    return FALSE;
  if (h)
    {
      g_autofree char *w = hundreds (h, c);
      word (out, w);
      any = TRUE;
    }
  if (rest >= 20)
    {
      g_autofree char *w = tens (rest / 10, c);
      word (out, w);
      any = TRUE;
      rest %= 10;
    }
  if (rest)
    {
      g_autofree char *w = last && rest <= 4 ? last_word (last, rest, any)
                         : rest == 1 ? g_strdup (ONE[gender_index (gender)][c]) : unit (rest, c, gender);
      word (out, w);
    }
  return TRUE;
}

static const char *
scale_noun (gint64 count, int scale, RuCase c)
{
  const char *const *singular = SCALES[scale].forms[0], *const *plural = SCALES[scale].forms[1];

  if (c == RU_NOM || c == RU_ACC)
    return ru_plural_form (count, singular[c], singular[RU_GEN], plural[RU_GEN]);
  return count % 10 == 1 && count % 100 != 11 ? singular[c] : plural[c];
}

static gboolean
scale_words (GString *out, gint64 count, int scale, RuCase c, gboolean leading)
{
  if (!(count == 1 && leading) && !group (out, count, c, scale == THOUSAND ? 'f' : 'm', NULL))
    return FALSE;
  word (out, scale_noun (count, scale, c));
  return TRUE;
}

/* n split into its scales' counts and the rest: counts[i] for SCALES[i], -1 when absent */
static int
split (gint64 n, gint64 counts[G_N_ELEMENTS (SCALES)], gint64 *rest)
{
  int parts = 0;

  for (guint i = 0; i < G_N_ELEMENTS (SCALES); i++)
    {
      counts[i] = -1;
      if (n >= SCALES[i].size)
        {
          counts[i] = n / SCALES[i].size;
          n %= SCALES[i].size;
          parts++;
        }
    }
  *rest = n;
  return parts + (n ? 1 : 0);
}

/* cardinal () into `out` */
static gboolean
cardinal (GString *out, gint64 n, RuCase c, char gender, gboolean animate, gboolean plural)
{
  gint64 counts[G_N_ELEMENTS (SCALES)], rest;
  int parts;
  Last last = { c, gender, animate, plural, FALSE };

  if (n < 0)
    {
      word (out, "минус");
      return n != G_MININT64 && cardinal (out, -n, c, gender, animate, plural);
    }
  if (n >= LIMIT)
    return FALSE;
  if (n == 0)
    {
      word (out, ZERO[c]);
      return TRUE;
    }
  gsize start = out->len;
  parts = split (n, counts, &rest);
  last.compound = parts > 1;
  for (guint i = 0; i < G_N_ELEMENTS (SCALES); i++)
    if (counts[i] >= 0)
      scale_words (out, counts[i], i, c, out->len == start);
  if (rest)
    group (out, rest, c, gender, &last);
  return TRUE;
}

char *
ru_cardinal (gint64 n, RuCase c, char gender, gboolean animate, gboolean plural)
{
  GString *out = g_string_new (NULL);

  if (!cardinal (out, n, c, gender, animate, plural))
    {
      g_string_free (out, TRUE);
      return NULL;
    }
  return g_string_free (out, FALSE);
}

static void
adjective (GString *out, const char *stem, RuCase c, char gender, gboolean plural, gboolean animate)
{
  int key = plural ? 3 : gender_index (gender);
  const char *space = strrchr (stem, ' ');
  const char *last = space ? space + 1 : stem;
  const char *const *endings = g_str_equal (last, "трет") ? SOFT_ENDINGS[key] : HARD_ENDINGS[key];
  int i = c;
  const char *ending;

  if (c == RU_ACC && (key == 0 || key == 3))
    i = animate ? RU_GEN : RU_NOM;
  ending = endings[i];
  if (g_str_equal (ending, "ый") && ru_in (STRESSED, last))
    ending = "ой";
  g_autofree char *w = g_strconcat (stem, ending, NULL);
  word (out, w);
}

static void
compound_prefix (GString *out, gint64 n)
{
  int h = n / 100, rest = n % 100;

  if (h)
    {
      g_autofree char *w = h == 1 ? g_strdup ("сто") : hundreds (h, RU_GEN);
      g_string_append (out, w);
    }
  if (rest >= 20)
    {
      g_autofree char *w = rest / 10 == 9 ? g_strdup ("девяносто") : tens (rest / 10, RU_GEN);
      g_string_append (out, w);
      rest %= 10;
    }
  if (rest == 1)
    g_string_append (out, "одно");
  else if (rest)
    {
      g_autofree char *w = unit (rest, RU_GEN, 'm');
      g_string_append (out, w);
    }
}

char *
ru_compound_prefix (gint64 n)
{
  GString *out = g_string_new (NULL);

  compound_prefix (out, n);
  return g_string_free (out, FALSE);
}

/* an ordinal's last group: the words before its last into `out`, the last one's stem returned */
static char *
ordinal_group (GString *out, int n)
{
  int h = n / 100, rest = n % 100;

  if (!rest)
    return h == 1 ? g_strdup ("сот") : hundreds (h, RU_GEN);
  if (h)
    word (out, HUNDREDS[h]);
  if (rest < 20)
    return g_strdup (ORDINAL_UNITS[rest]);
  if (!(rest % 10))
    return g_strdup (ORDINAL_TENS[rest / 10]);
  word (out, TENS[rest / 10]);
  return g_strdup (ORDINAL_UNITS[rest % 10]);
}

char *
ru_ordinal (gint64 n, RuCase c, char gender, gboolean plural, gboolean animate)
{
  gint64 counts[G_N_ELEMENTS (SCALES)], rest;
  int last_scale = -1;
  GString *out;

  if (n < 0 || n >= LIMIT)
    return NULL;
  out = g_string_new (NULL);
  if (n == 0)
    {
      adjective (out, "нулев", c, gender, plural, animate);
      return g_string_free (out, FALSE);
    }
  split (n, counts, &rest);
  if (!rest)
    for (guint i = 0; i < G_N_ELEMENTS (SCALES); i++)
      if (counts[i] >= 0)
        last_scale = i;
  for (guint i = 0; i < G_N_ELEMENTS (SCALES); i++)
    if (counts[i] >= 0 && (int) i != last_scale)
      scale_words (out, counts[i], i, RU_NOM, out->len == 0);
  if (last_scale >= 0)
    {
      GString *stem = g_string_new (NULL);
      if (counts[last_scale] != 1)
        compound_prefix (stem, counts[last_scale]);
      g_string_append (stem, last_scale == THOUSAND ? "тысяч" : SCALES[last_scale].forms[0][RU_NOM]);
      g_string_append (stem, "н");
      adjective (out, stem->str, c, gender, plural, animate);
      g_string_free (stem, TRUE);
    }
  else
    {
      g_autofree char *stem = ordinal_group (out, rest);
      adjective (out, stem, c, gender, plural, animate);
    }
  return g_string_free (out, FALSE);
}

/* a share's adjective: целых, сотых, четвёртая */
static void
share (GString *out, const char *stem, gint64 count, RuCase c)
{
  if (count % 10 == 1 && count % 100 != 11)
    adjective (out, stem, c, 'f', FALSE, FALSE);
  else
    adjective (out, stem, c == RU_NOM || c == RU_ACC ? RU_GEN : c, 'f', TRUE, TRUE);
}

/* ordinal (n) less its last two characters: the stem a share is made of */
static char *
share_stem (gint64 n)
{
  g_autofree char *w = ru_ordinal (n, RU_NOM, 'm', FALSE, FALSE);

  return w ? ru_drop (w, 2) : NULL;
}

char *
ru_decimal (const char *whole, const char *fraction, RuCase c)
{
  gint64 w, f, scale = 1;
  gsize places = ru_chars (fraction);
  g_autofree char *stem = NULL;
  GString *out;

  if (!ru_int (whole, &w))
    return NULL;
  if (!places)
    return ru_cardinal (w, c, 'm', FALSE, FALSE);
  if (!ru_int (fraction, &f) || places >= 15 || w >= LIMIT || f >= LIMIT)
    return NULL;
  for (gsize i = 0; i < places; i++)
    scale *= 10;
  stem = share_stem (scale);
  out = g_string_new (NULL);
  cardinal (out, w, c, 'f', FALSE, FALSE);
  share (out, "цел", w, c);
  cardinal (out, f, c, 'f', FALSE, FALSE);
  share (out, stem, f, c);
  return g_string_free (out, FALSE);
}

char *
ru_fraction (gint64 numerator, gint64 denominator, RuCase c)
{
  g_autofree char *stem = share_stem (denominator);
  GString *out;

  if (!stem || numerator < 0 || numerator >= LIMIT)
    return NULL;
  out = g_string_new (NULL);
  cardinal (out, numerator, c, 'f', FALSE, FALSE);
  share (out, stem, numerator, c);
  return g_string_free (out, FALSE);
}

char *
ru_digits (const char *digits)
{
  GString *out = g_string_new (NULL);

  for (const char *p = digits; *p; p = g_utf8_next_char (p))
    {
      int d = g_unichar_digit_value (g_utf8_get_char (p));
      word (out, UNITS[d < 0 ? 0 : d]);
    }
  return g_string_free (out, FALSE);
}
