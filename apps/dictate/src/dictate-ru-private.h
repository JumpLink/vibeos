/* dictate-ru-private: what the parts of the Russian text front end (dictate-ru.h) share: the numerals, the tables,
 * the regular expressions and the string helpers. A port to C of tiny-voca-ru's text front end (see dictate-ru.h).
 * Positions in strings are byte offsets into UTF-8 where the original counts characters; both name the same places. */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

/* the six cases, in this order everywhere */
typedef enum { RU_NOM, RU_GEN, RU_DAT, RU_ACC, RU_INS, RU_PREP, RU_CASES } RuCase;

/* ---------------- numerals (dictate-ru-numbers.c) ---------------- */

/* gender: 'm', 'f' or 'n'. NULL for a number the numerals cannot name: 10^15 and over */
char       *ru_cardinal  (gint64 n, RuCase c, char gender, gboolean animate, gboolean plural);
char       *ru_ordinal   (gint64 n, RuCase c, char gender, gboolean plural, gboolean animate);
/* whole and fraction are digit strings; fraction empty for a whole number */
char       *ru_decimal   (const char *whole, const char *fraction, RuCase c);
char       *ru_fraction  (gint64 numerator, gint64 denominator, RuCase c);
char       *ru_digits    (const char *digits);
/* the combining form of 0 < n < 1000 (двух in двухтысячный) */
char       *ru_compound_prefix (gint64 n);
/* of (1, 2-4, 5+) */
const char *ru_plural_form (gint64 n, const char *one, const char *few, const char *many);

/* ---------------- tables (dictate-ru-tables.c) ---------------- */

typedef struct {
  const char *lemma;            /* plain */
  char gender;                  /* m f n, p for plural only */
  gboolean animate;
  const char *stressed[13];     /* singular nominative..prepositional, plural, the form after 2-4; '+' marks */
  char *plain[13];              /* the same without the marks */
} RuNoun;

typedef struct { const char *key, *value; } RuPair;
typedef struct { const char *unit, *lemma, *tail; } RuUnit;
typedef struct { int c; const char *gender; } RuReading;
typedef struct { const char *ending; int n; RuReading readings[6]; } RuEnding;
typedef struct { const char *pattern, *lemma, *follows; } RuPlace;

const RuNoun *ru_noun          (const char *lemma);
/* the noun a plain form belongs to, NULL when it is none of theirs */
const char   *ru_form_lemma    (const char *form);
const char   *ru_numeral_stress (const char *word);
const RuUnit *ru_units         (gsize *n);
const RuUnit *ru_unit          (const char *unit);
/* the cases a preposition governs ("AP": accusative, prepositional), from `table` first when it has it, then from
 * the general one; "N" for none */
const char   *ru_cases         (const char *preposition, const char *table);
gboolean      ru_is_preposition (const char *word);
const RuEnding *ru_ordinal_ending (const char *ending);

/* a NULL-terminated list's lookups */
gboolean      ru_in            (const char *const *list, const char *word);
const char   *ru_lookup        (const RuPair *pairs, const char *key);
const char *const *ru_list     (const char *name);
const RuPair *ru_pairs         (const char *name);
const RuPlace *ru_places       (void);

/* ---------------- regular expressions and strings (dictate-ru.c) ---------------- */

/* the pattern compiled once and kept (Python's re semantics: UTF-8, Unicode classes) */
GRegex  *ru_rx       (const char *pattern);
/* `text` with each match of `pattern` replaced by what `fn` appends to `out`; fn's match info is over the whole of
 * `text`, as Python's m.string */
typedef void (*RuReplace) (const GMatchInfo *m, GString *out, gpointer data);
char    *ru_sub      (const char *pattern, const char *text, RuReplace fn, gpointer data);
/* the same with a fixed replacement (\1 refers to a group) */
char    *ru_subs     (const char *pattern, const char *text, const char *replacement);
/* in place: *text replaced by the result */
void     ru_sub_in   (char **text, const char *pattern, RuReplace fn, gpointer data);
void     ru_subs_in  (char **text, const char *pattern, const char *replacement);
gboolean ru_search   (const char *pattern, const char *text, gssize length, GMatchInfo **m);
/* the match anchored at `at` */
gboolean ru_match_at (const char *pattern, const char *text, gsize at, GMatchInfo **m);
/* a group of a match, NULL when it took no part in it */
char    *ru_group    (const GMatchInfo *m, const char *name);
char    *ru_group_n  (const GMatchInfo *m, int n);
gboolean ru_group_pos (const GMatchInfo *m, const char *name, int *start, int *end);

/* Python's str.lower () (and upper) */
char    *ru_lower    (const char *s);
char    *ru_upper    (const char *s);
/* without '+' and lowered */
char    *ru_plain    (const char *word);
/* `s` without its last `n` characters */
char    *ru_drop     (const char *s, int n);
gsize    ru_chars    (const char *s);
gboolean ru_ends     (const char *s, const char *suffix);
gboolean ru_starts   (const char *s, const char *prefix);
/* Python's int () of a digit string (Unicode digits too); FALSE beyond 18 digits */
gboolean ru_int      (const char *digits, gint64 *n);
/* str.isalnum () of any character */
gboolean ru_any_alnum (const char *s);
char    *ru_replace  (const char *s, const char *from, const char *to);
const char *ru_case_name (RuCase c);
/* "AP" → RU_ACC, … */
RuCase   ru_case_of  (char code);

/* ---------------- stress (dictate-ru-stress.c) and phones (dictate-ru-g2p.c) ---------------- */

typedef struct _RuStress RuStress;

RuStress *ru_stress_new        (const char *dir, gboolean homographs);
void      ru_stress_free       (RuStress *self);
/* normalised text with '+' before each stressed vowel, and ё put back */
char     *ru_stress_accentuate (RuStress *self, const char *text);

/* each phone of a phrase's stressed words in turn, with its stress (0, 1); a NULL phone starts a phonetic word */
typedef void (*RuPhoneFunc) (const char *phone, int stress, gpointer data);
void      ru_phrase_phones     (char **words, RuPhoneFunc fn, gpointer data);

/* ---------------- the passes ---------------- */

char *ru_unify    (const char *text);
char *ru_tidy     (const char *text);
/* the normaliser: written Russian to the words it is read as (normalize.py's rules ()) */
char *ru_normalize (const char *text);

G_END_DECLS
