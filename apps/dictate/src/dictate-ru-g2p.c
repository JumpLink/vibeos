/* dictate-ru-g2p: stressed Russian words to their phones: the exceptions respelt (что: што, тест: тэст), the
 * consonant clusters simplified (солнце: сонце), then the letters to phones, each stressed host word with its
 * clitics as one phonetic word, the consonants assimilated (voicing, softness, г before к) and the unstressed vowels
 * reduced. A port of tiny-voca-ru's frontend/g2p.py (see dictate-ru.h). */
#include "dictate-ru-private.h"

#include <string.h>

#define G2P_VOWELS "аеёиоуыэюя"

static const RuPair VOWEL_PHONES[] = {
  { "а", "a" }, { "я", "a" }, { "о", "o" }, { "ё", "o" }, { "у", "u" }, { "ю", "u" }, { "э", "e" }, { "е", "e" },
  { "ы", "ɨ" }, { "и", "i" }, { NULL, NULL },
};
static const RuPair CONSONANT_PHONES[] = {
  { "б", "b" }, { "в", "v" }, { "г", "ɡ" }, { "д", "d" }, { "ж", "ʐ" }, { "з", "z" }, { "й", "j" }, { "к", "k" },
  { "л", "l" }, { "м", "m" }, { "н", "n" }, { "п", "p" }, { "р", "r" }, { "с", "s" }, { "т", "t" }, { "ф", "f" },
  { "х", "x" }, { "ц", "ts" }, { "ч", "tɕ" }, { "ш", "ʂ" }, { "щ", "ɕ" }, { NULL, NULL },
};
/* the paired consonants' soft partners (ж ш ц are always hard; ч щ й always soft) */
static const RuPair SOFT[] = {
  { "p", "pʲ" }, { "b", "bʲ" }, { "t", "tʲ" }, { "d", "dʲ" }, { "k", "kʲ" }, { "ɡ", "ɡʲ" }, { "f", "fʲ" },
  { "v", "vʲ" }, { "s", "sʲ" }, { "z", "zʲ" }, { "x", "xʲ" }, { "m", "mʲ" }, { "n", "nʲ" }, { "l", "lʲ" },
  { "r", "rʲ" }, { NULL, NULL },
};
static const char *const SOFTENING = "еёиюяь";
static const char *const IOTATED = "яюеё";
static const char *const HUSHING = "жшц";          /* always hard: и is ɨ, unstressed е ɨ */
static const char *const VOICELESS[] = {
  "p", "pʲ", "t", "tʲ", "k", "kʲ", "f", "fʲ", "s", "sʲ", "ʂ", "ɕ", "x", "xʲ", "ts", "tɕ", NULL,
};
/* х ц ч щ have no voiced partner and stay voiceless */
static const RuPair VOICED_PARTNER[] = {
  { "p", "b" }, { "pʲ", "bʲ" }, { "t", "d" }, { "tʲ", "dʲ" }, { "k", "ɡ" }, { "kʲ", "ɡʲ" }, { "f", "v" },
  { "fʲ", "vʲ" }, { "s", "z" }, { "sʲ", "zʲ" }, { "ʂ", "ʐ" }, { NULL, NULL },
};
static const char *const VOWEL_SET[] = { "a", "o", "e", "i", "u", "ɨ", "ɐ", "ə", NULL };

/* consonant clusters respelt, in order, the vowels' count kept */
static const RuPair CLUSTERS[] = {
  { "ть?ся$", "ца" },                       /* учится, учиться */
  { "вств", "ств" },                        /* чувство, здравствуй */
  { "стск", "ск" },                         /* туристский */
  { "н[тд](?=ск|ц)", "н" },                 /* гигантский, голландцы */
  { "нтг", "нг" },                          /* рентген */
  { "ндш", "нш" },                          /* ландшафт */
  { "стн", "сн" },                          /* честный */
  { "здн", "зн" },                          /* поздно */
  { "стл", "сл" },                          /* счастливый */
  { "лнц", "нц" },                          /* солнце */
  { "рдц", "рц" },                          /* сердце */
  { "[тд]ств", "цтв" },                     /* детство */
  { "[тд]ск(?=(ий|ая|ое|ие|ого|ому|ом|ой|ую|их|им|ими|а|у|е)?$)", "цк" },   /* детский, городской */
  { NULL, NULL },
};

static gboolean
letter_in (const char *set, const char *letter)
{
  return *letter && strstr (set, letter) != NULL;
}

static gboolean
is_vowel (const char *phone)
{
  return ru_in (VOWEL_SET, phone);
}

static gboolean
is_soft_phone (const char *phone)
{
  for (const RuPair *s = SOFT; s->key; s++)
    if (g_str_equal (s->value, phone))
      return TRUE;
  return g_str_equal (phone, "tɕ") || g_str_equal (phone, "ɕ") || g_str_equal (phone, "j");
}

static const char *
voiceless_partner (const char *phone)
{
  for (const RuPair *v = VOICED_PARTNER; v->key; v++)
    if (g_str_equal (v->value, phone))
      return v->key;
  return NULL;
}

static gboolean
is_obstruent (const char *phone)
{
  return ru_in (VOICELESS, phone) || voiceless_partner (phone);
}

/* ---------------- respelling ---------------- */

static char *
stem_respelt (const char *plain)
{
  glong n = g_utf8_strlen (plain, -1);

  for (glong cut = n; cut > 0; cut--)
    {
      const char *at = g_utf8_offset_to_pointer (plain, cut);
      g_autofree char *stem = g_strndup (plain, at - plain);
      const char *noun = ru_lookup (ru_pairs ("LEX_NOUN_STEMS"), stem);
      if (noun && ru_in (ru_list ("LEX_NOUN_ENDINGS"), at))
        return g_strconcat (noun, at, NULL);
      const char *adjective = ru_lookup (ru_pairs ("LEX_ADJECTIVE_STEMS"), stem);
      if (adjective && ru_in (ru_list ("LEX_ADJECTIVE_ENDINGS"), at))
        return g_strconcat (adjective, at, NULL);
    }
  return NULL;
}

/* `target` with `source`'s stress marks, on the vowels of the same number */
static char *
restress (const char *source, const char *target)
{
  gboolean marks[256] = { FALSE }, mark = FALSE;
  int count = 0;
  GString *out = g_string_new (NULL);

  for (const char *p = source; *p; p = g_utf8_next_char (p))
    {
      char letter[8] = { 0 };
      g_unichar_to_utf8 (g_utf8_get_char (p), letter);
      if (*p == '+')
        mark = TRUE;
      else if (letter_in (G2P_VOWELS, letter))
        {
          if (mark && count < 256)
            marks[count] = TRUE;
          count++;
          mark = FALSE;
        }
    }
  count = 0;
  for (const char *p = target; *p; p = g_utf8_next_char (p))
    {
      char letter[8] = { 0 };
      g_unichar_to_utf8 (g_utf8_get_char (p), letter);
      if (letter_in (G2P_VOWELS, letter))
        {
          if (count < 256 && marks[count])
            g_string_append_c (out, '+');
          count++;
        }
      g_string_append (out, letter);
    }
  return g_string_free (out, FALSE);
}

static char *
respell (const char *word)
{
  g_autofree char *plain = ru_replace (word, "+", "");
  const char *exception = ru_lookup (ru_pairs ("LEX_WORDS"), plain);
  char *spelt = exception ? g_strdup (exception) : stem_respelt (plain);

  if (!spelt)
    spelt = g_strdup (plain);
  for (const RuPair *c = CLUSTERS; c->key; c++)
    ru_subs_in (&spelt, c->key, c->value);
  if ((ru_ends (spelt, "ого") || ru_ends (spelt, "его")) && !ru_in (ru_list ("LEX_KEEP_G"), plain))
    {
      g_autofree char *stem = ru_drop (spelt, 2);     /* нового, его: the genitive's г is в */
      g_free (spelt);
      spelt = g_strconcat (stem, "во", NULL);
    }
  if (g_str_equal (spelt, plain))
    {
      g_free (spelt);
      return g_strdup (word);
    }
  char *out = restress (word, spelt);
  g_free (spelt);
  return out;
}

/* ---------------- letters to segments ---------------- */

typedef enum { CONTEXT_NONE, CONTEXT_SOFT, CONTEXT_HARD, CONTEXT_HUSH } Context;

typedef struct {
  const char *phone;            /* static strings */
  char letter[8];               /* the consonant's letter (в is transparent to voicing) */
  Context context;              /* a vowel's: what precedes it */
  gboolean stressed;
  gboolean full;                /* keeps its quality unstressed */
  gboolean last;                /* its written word's last vowel */
  gboolean initial;             /* its written word's first segment */
  gboolean loan;                /* its word has a hard е (э after a consonant) */
  int unit;
} Segment;

static const char *
soft_of (const char *phone)
{
  return ru_lookup (SOFT, phone);
}

static void
spell (const char *word, GArray *out)
{
  g_autoptr (GPtrArray) letters = g_ptr_array_new_with_free_func (g_free);
  g_autoptr (GArray) stressed = g_array_new (FALSE, TRUE, sizeof (gboolean));
  gboolean any = FALSE;
  guint first = out->len;

  for (const char *p = word; *p; p = g_utf8_next_char (p))
    {
      if (*p == '+')
        {
          g_array_set_size (stressed, letters->len + 1);
          g_array_index (stressed, gboolean, letters->len) = TRUE;
          any = TRUE;
          continue;
        }
      g_ptr_array_add (letters, g_strndup (p, g_utf8_next_char (p) - p));
    }
  g_array_set_size (stressed, letters->len + 1);
  if (!any)
    for (guint i = 0; i < letters->len; i++)
      if (g_str_equal (letters->pdata[i], "ё"))
        g_array_index (stressed, gboolean, i) = TRUE;
  for (guint i = 0; i < letters->len; i++)
    {
      const char *c = letters->pdata[i];
      const char *before = i ? letters->pdata[i - 1] : "";
      const char *after = i + 1 < letters->len ? letters->pdata[i + 1] : "";
      const char *consonant = ru_lookup (CONSONANT_PHONES, c);
      const char *vowel = ru_lookup (VOWEL_PHONES, c);
      Segment s = { 0 };
      if (consonant)
        {
          s.phone = consonant;
          if (soft_of (consonant) && letter_in (SOFTENING, after))
            s.phone = soft_of (consonant);
          g_strlcpy (s.letter, c, sizeof s.letter);
          g_array_append_val (out, s);
        }
      else if (vowel)
        {
          Context context;
          const char *quality = vowel;
          if (ru_lookup (CONSONANT_PHONES, before))
            {
              if (letter_in (HUSHING, before))
                {
                  context = g_str_equal (c, "э") ? CONTEXT_HARD : CONTEXT_HUSH;   /* э only in respelt loans */
                  if (g_str_equal (c, "и"))
                    quality = "ɨ";
                }
              else
                context = is_soft_phone (g_array_index (out, Segment, out->len - 1).phone) ? CONTEXT_SOFT
                                                                                          : CONTEXT_HARD;
            }
          else if (letter_in (IOTATED, c) ||
                   ((g_str_equal (before, "ь") || g_str_equal (before, "ъ")) &&
                    (g_str_equal (c, "и") || g_str_equal (c, "о"))))
            {
              Segment j = { .phone = "j" };
              g_array_append_val (out, j);
              context = CONTEXT_SOFT;
            }
          else
            context = CONTEXT_NONE;
          s.phone = quality;
          g_strlcpy (s.letter, c, sizeof s.letter);
          s.context = context;
          s.stressed = g_array_index (stressed, gboolean, i);
          s.full = g_str_equal (c, "ё") && !s.stressed;
          g_array_append_val (out, s);
        }
    }
  for (guint i = out->len; i > first; i--)
    if (is_vowel (g_array_index (out, Segment, i - 1).phone))
      {
        g_array_index (out, Segment, i - 1).last = TRUE;
        break;
      }
  if (out->len > first)
    g_array_index (out, Segment, first).initial = TRUE;
}

/* ---------------- written units, phonetic words ---------------- */

typedef struct {
  char *text;                   /* with its marks */
  char *key;                    /* without */
  gboolean hyphen;              /* after a hyphen: кто-то, скажи-ка */
  gboolean stressed;
} Unit;

static void
unit_clear (gpointer data)
{
  Unit *u = data;

  g_free (u->text);
  g_free (u->key);
}

static void
units_of (const char *word, GArray *units)
{
  g_autofree char *lower = ru_lower (word);
  g_auto (GStrv) parts = g_strsplit (lower, "-", -1);

  for (guint i = 0; parts[i]; i++)
    {
      char *letters = ru_subs ("[^а-яё+]", parts[i], "");
      ru_subs_in (&letters, "\\+(?![аеёиоуыэюя])", "");
      char *key = ru_replace (letters, "+", "");
      if (!*key)
        {
          g_free (letters);
          g_free (key);
          continue;
        }
      Unit u = { letters, key, i > 0, strchr (letters, '+') || strstr (key, "ё") };
      g_array_append_val (units, u);
    }
}

/* the written units grouped into phonetic words, each stressed host with its clitics: [from, to) of `units` */
static GArray *
groups_of (GArray *units)
{
  GArray *groups = g_array_new (FALSE, FALSE, sizeof (int) * 2);
  int pending = -1;             /* the first unit waiting for its host, -1 for none */

  for (guint i = 0; i < units->len; i++)
    {
      Unit *u = &g_array_index (units, Unit, i);
      int *last = groups->len ? (int *) (groups->data + (groups->len - 1) * sizeof (int) * 2) : NULL;
      if (u->stressed)
        {
          int g[2] = { pending >= 0 ? pending : (int) i, i + 1 };
          g_array_append_val (groups, g);
          pending = -1;
        }
      else if (last && pending < 0)
        {
          Unit *host = &g_array_index (units, Unit, last[1] - 1);
          if (u->hyphen || ru_in (ru_list ("LEX_ENCLITICS"), u->key) ||
              (host->stressed && ru_in (ru_list ("LEX_PROCLITICS"), host->key)))
            last[1] = i + 1;
          else
            pending = i;
        }
      else if (pending < 0)
        pending = i;
    }
  if (pending >= 0)
    {
      if (groups->len)
        ((int *) (groups->data + (groups->len - 1) * sizeof (int) * 2))[1] = units->len;
      else
        {
          int g[2] = { pending, units->len };
          g_array_append_val (groups, g);
        }
    }
  return groups;
}

static GArray *
segments_of (GArray *units, int from, int to)
{
  GArray *segments = g_array_new (FALSE, FALSE, sizeof (Segment));

  for (int number = 0; number < to - from; number++)
    {
      Unit *u = &g_array_index (units, Unit, from + number);
      g_autofree char *text = respell (u->text);
      gboolean full = ru_in (ru_list ("LEX_NONREDUCING"), u->key) && !u->hyphen;
      gboolean loan = ru_search ("[бвгджзклмнпрстфхц]\\+?э", text, -1, NULL);
      guint first = segments->len;
      spell (text, segments);
      for (guint i = first; i < segments->len; i++)
        {
          Segment *s = &g_array_index (segments, Segment, i);
          s->unit = number;
          s->loan = loan;
          s->full = s->full || (full && g_str_equal (s->phone, "o"));
        }
      if (segments->len > first && first > 0)
        {
          Segment *s = &g_array_index (segments, Segment, first);
          Segment *previous = &g_array_index (segments, Segment, first - 1);
          if (is_vowel (s->phone) && !is_vowel (previous->phone) && !is_soft_phone (previous->phone))
            {
              s->context = CONTEXT_HARD;          /* к игре: kɨɡrʲe, в огороде */
              if (g_str_equal (s->phone, "i"))
                s->phone = "ɨ";
            }
        }
    }
  return segments;
}

/* ---------------- the phonetic rules ---------------- */

/* regressive palatalisation: с сестрой, женщина, есть, песня */
static void
soften (GArray *segments)
{
  for (int i = (int) segments->len - 2; i >= 0; i--)
    {
      Segment *s = &g_array_index (segments, Segment, i), *following = &g_array_index (segments, Segment, i + 1);
      const char *soft = soft_of (s->phone);
      if (!soft)
        continue;
      if (g_str_equal (following->phone, soft) ||
          (s->unit == following->unit &&
           ((g_str_equal (s->phone, "n") && (g_str_equal (following->phone, "tɕ") ||
                                              g_str_equal (following->phone, "ɕ"))) ||
            ((g_str_equal (s->phone, "s") || g_str_equal (s->phone, "z")) &&
             (g_str_equal (following->phone, "tʲ") || g_str_equal (following->phone, "dʲ") ||
              g_str_equal (following->phone, "nʲ")) && !s->initial && !s->loan))))
        s->phone = soft;
    }
}

/* г, soft or hard, before к or ч is х: легко, мягкий */
static void
velar (GArray *segments)
{
  for (guint i = 0; i + 1 < segments->len; i++)
    {
      Segment *s = &g_array_index (segments, Segment, i), *following = &g_array_index (segments, Segment, i + 1);
      if ((g_str_equal (s->phone, "ɡ") || g_str_equal (s->phone, "ɡʲ")) &&
          (g_str_equal (following->phone, "k") || g_str_equal (following->phone, "kʲ") ||
           g_str_equal (following->phone, "tɕ")) && s->unit == following->unit)
        s->phone = g_str_equal (s->phone, "ɡ") ? "x" : "xʲ";
    }
}

/* regressive voicing and the word's last consonant devoiced (в is transparent) */
static void
voicing (GArray *segments)
{
  enum { NONE, VOICELESS_NEXT, VOICED_NEXT } state = VOICELESS_NEXT;

  for (int i = (int) segments->len - 1; i >= 0; i--)
    {
      Segment *s = &g_array_index (segments, Segment, i);
      if (!is_obstruent (s->phone))
        {
          state = NONE;
          continue;
        }
      if (state == VOICELESS_NEXT)
        {
          const char *partner = voiceless_partner (s->phone);
          if (partner)
            s->phone = partner;
        }
      else if (state == VOICED_NEXT)
        {
          const char *partner = ru_lookup (VOICED_PARTNER, s->phone);
          if (partner)
            s->phone = partner;
        }
      if (!g_str_equal (s->letter, "в"))
        state = ru_in (VOICELESS, s->phone) ? VOICELESS_NEXT : VOICED_NEXT;
    }
}

/* geminates and assimilated sibilants and affricates as one phone */
static GArray *
merge (GArray *segments)
{
  GArray *out = g_array_new (FALSE, FALSE, sizeof (Segment));

  for (guint i = 0; i < segments->len; i++)
    {
      Segment *s = &g_array_index (segments, Segment, i);
      Segment *last = out->len ? &g_array_index (out, Segment, out->len - 1) : NULL;
      const char *previous = last ? last->phone : NULL, *phone = s->phone;
      if (!previous || is_vowel (previous) || is_vowel (phone))
        g_array_append_val (out, *s);
      else if (g_str_equal (phone, previous))
        continue;
      else if (((g_str_equal (previous, "s") || g_str_equal (previous, "sʲ") || g_str_equal (previous, "ʂ")) &&
                (g_str_equal (phone, "tɕ") || g_str_equal (phone, "ɕ"))) ||
               ((g_str_equal (previous, "s") || g_str_equal (previous, "sʲ")) && g_str_equal (phone, "ʂ")) ||
               ((g_str_equal (previous, "z") || g_str_equal (previous, "zʲ")) && g_str_equal (phone, "ʐ")) ||
               ((g_str_equal (previous, "t") || g_str_equal (previous, "tʲ") || g_str_equal (previous, "d") ||
                 g_str_equal (previous, "dʲ")) && (g_str_equal (phone, "ts") || g_str_equal (phone, "tɕ"))))
        last->phone = g_str_equal (phone, "tɕ") && (g_str_equal (previous, "s") || g_str_equal (previous, "sʲ") ||
                                                     g_str_equal (previous, "ʂ")) ? "ɕ" : phone;
      else
        g_array_append_val (out, *s);
    }
  return out;
}

typedef enum { POST, PRE1, PRE } Position;

static const char *
reduced (const Segment *s, Position position, gboolean before_soft)
{
  const char *quality = s->phone;

  if (g_str_equal (quality, "u") || g_str_equal (quality, "i") || g_str_equal (quality, "ɨ"))
    return quality;
  if (s->context == CONTEXT_SOFT)       /* ə in the last syllable (тётя, вишням), i before a soft one (память) */
    return g_str_equal (quality, "a") && position == POST && s->last && !before_soft ? "ə" : "i";
  if (g_str_equal (quality, "e"))       /* a loan's hard е stays e (интернет) */
    return s->context == CONTEXT_NONE ? "i" : s->context == CONTEXT_HARD ? "e" : "ɨ";
  if (position == PRE1 || (position == PRE && s->context == CONTEXT_NONE))
    return "ɐ";
  return "ə";
}

/* the unstressed vowels reduced, by where they are from the stressed one */
static void
reduce (GArray *segments)
{
  g_autoptr (GArray) vowels = g_array_new (FALSE, FALSE, sizeof (int));
  int stress = -1;

  for (guint i = 0; i < segments->len; i++)
    if (is_vowel (g_array_index (segments, Segment, i).phone))
      {
        int at = i;
        g_array_append_val (vowels, at);
        if (stress < 0 && g_array_index (segments, Segment, i).stressed)
          stress = i;
      }
  if (stress < 0)
    return;                     /* clitics only */
  for (guint k = 0; k < vowels->len; k++)
    {
      int i = g_array_index (vowels, int, k);
      Segment *s = &g_array_index (segments, Segment, i);
      Position position;
      if (s->stressed || s->full)
        continue;
      if (i > stress)
        position = POST;
      else if (k + 1 < vowels->len && g_array_index (vowels, int, k + 1) == stress)
        position = PRE1;
      else
        position = PRE;
      gboolean before_soft = (guint) i + 1 < segments->len &&
                             is_soft_phone (g_array_index (segments, Segment, i + 1).phone);
      s->phone = reduced (s, position, before_soft);
    }
}

void
ru_phrase_phones (char **words, RuPhoneFunc fn, gpointer data)
{
  g_autoptr (GArray) units = g_array_new (FALSE, FALSE, sizeof (Unit));
  g_autoptr (GArray) groups = NULL;

  g_array_set_clear_func (units, unit_clear);
  for (char **w = words; *w; w++)
    units_of (*w, units);
  groups = groups_of (units);
  for (guint g = 0; g < groups->len; g++)
    {
      int *range = (int *) (groups->data + g * sizeof (int) * 2);
      g_autoptr (GArray) segments = segments_of (units, range[0], range[1]);
      velar (segments);
      voicing (segments);
      soften (segments);
      g_autoptr (GArray) merged = merge (segments);
      reduce (merged);
      if (!merged->len)
        continue;
      fn (NULL, 0, data);       /* a phonetic word starts */
      for (guint i = 0; i < merged->len; i++)
        {
          Segment *s = &g_array_index (merged, Segment, i);
          fn (s->phone, s->stressed, data);
        }
    }
}
