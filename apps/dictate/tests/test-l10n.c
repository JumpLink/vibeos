/* dictate-l10n: a message, a message with a context and a plural looked up in a language's catalogue. The catalogues are
 * written by the test as .mo files (the format msgfmt makes) in the directory dictate-l10n.c reads them from. */
#include "dictate-l10n.h"

#include <glib/gstdio.h>
#include <string.h>

typedef struct {
  const char *key;              /* msgid, "context\004msgid" or "msgid\0msgid_plural" (given with its length) */
  gsize key_length;
  const char *value;            /* the translation, a plural's forms joined by NUL */
  gsize value_length;
} Pair;

#define PAIR(key, value) { key, sizeof (key) - 1, value, sizeof (value) - 1 }

static void
put (GByteArray *out, guint32 value, gboolean swapped)
{
  guint32 v = swapped ? GUINT32_SWAP_LE_BE (value) : value;

  g_byte_array_append (out, (const guint8 *) &v, 4);
}

/* LOCALEDIR/language/LC_MESSAGES/vibeos-dictate.mo holding `pairs` (already in the order of their keys) */
static void
write_catalogue (const char *language, const Pair *pairs, guint n, gboolean swapped)
{
  g_autofree char *dir = g_build_filename (LOCALEDIR, language, "LC_MESSAGES", NULL);
  g_autofree char *path = g_build_filename (dir, GETTEXT_PACKAGE ".mo", NULL);
  g_autoptr (GByteArray) out = g_byte_array_new ();
  guint32 strings = 28 + 16 * n;
  guint32 at = strings;

  g_assert_cmpint (g_mkdir_with_parents (dir, 0755), ==, 0);
  put (out, 0x950412deu, FALSE);
  if (swapped)
    {
      /* a catalogue of the other byte order: the magic reads backwards, the numbers are in that order */
      guint32 magic = 0xde120495u;

      memcpy (out->data, &magic, 4);
    }
  put (out, 0, swapped);
  put (out, n, swapped);
  put (out, 28, swapped);
  put (out, 28 + 8 * n, swapped);
  put (out, 0, swapped);
  put (out, 0, swapped);
  for (guint i = 0; i < n; i++)
    {
      put (out, pairs[i].key_length, swapped);
      put (out, at, swapped);
      at += pairs[i].key_length + 1;
    }
  for (guint i = 0; i < n; i++)
    {
      put (out, pairs[i].value_length, swapped);
      put (out, at, swapped);
      at += pairs[i].value_length + 1;
    }
  for (guint i = 0; i < n; i++)
    {
      g_byte_array_append (out, (const guint8 *) pairs[i].key, pairs[i].key_length + 1);
    }
  for (guint i = 0; i < n; i++)
    g_byte_array_append (out, (const guint8 *) pairs[i].value, pairs[i].value_length + 1);
  g_assert_true (g_file_set_contents (path, (const char *) out->data, out->len, NULL));
}

static void
test_plain_and_context (void)
{
  /* the header is an entry with an empty msgid, and is no message */
  const Pair de[] = {
    PAIR ("", "Language: de\n"),
    PAIR ("Hello", "Hallo"),
    PAIR ("Normal", "Normal (ohne Kontext)"),
    PAIR ("reply length\004Normal", "Normale"),
    PAIR ("speech speed\004Normal", "Normales"),
  };

  write_catalogue ("de", de, G_N_ELEMENTS (de), FALSE);
  g_assert_cmpstr (dictate_l10n ("de", "Hello"), ==, "Hallo");
  g_assert_cmpstr (dictate_l10n ("de", "Normal"), ==, "Normal (ohne Kontext)");
  g_assert_cmpstr (dictate_l10n_c ("de", "speech speed", "Normal"), ==, "Normales");
  g_assert_cmpstr (dictate_l10n_c ("de", "reply length", "Normal"), ==, "Normale");
  /* a context that has no entry is not the message without one */
  g_assert_cmpstr (dictate_l10n_c ("de", "signal strength", "Normal"), ==, "Normal");
  g_assert_cmpstr (dictate_l10n_c ("de", "speech speed", "Hello"), ==, "Hello");
  g_assert_cmpstr (dictate_l10n ("de", ""), ==, "");
  /* no catalogue, no language, nothing to look up */
  g_assert_cmpstr (dictate_l10n ("xx", "Hello"), ==, "Hello");
  g_assert_cmpstr (dictate_l10n_c ("xx", "speech speed", "Normal"), ==, "Normal");
  g_assert_cmpstr (dictate_l10n (NULL, "Hello"), ==, "Hello");
  g_assert_cmpstr (dictate_l10n ("", "Hello"), ==, "Hello");
  g_assert_null (dictate_l10n ("de", NULL));
  g_assert_cmpstr (dictate_l10n_c ("de", NULL, "Hello"), ==, "Hallo");
}

static void
test_plural_n_ne_1 (void)
{
  const Pair de[] = {
    PAIR ("%u phrase\0%u phrases", "%u Satz\0%u Sätze"),
    PAIR ("%u tool\0%u tools", "%u Werkzeug"),
  };

  write_catalogue ("nl", de, G_N_ELEMENTS (de), FALSE);
  g_assert_cmpstr (dictate_l10n_n ("nl", "%u phrase", "%u phrases", 1), ==, "%u Satz");
  g_assert_cmpstr (dictate_l10n_n ("nl", "%u phrase", "%u phrases", 0), ==, "%u Sätze");
  g_assert_cmpstr (dictate_l10n_n ("nl", "%u phrase", "%u phrases", 4), ==, "%u Sätze");
  /* a catalogue with fewer forms than the language's rule asks for gives the last it has */
  g_assert_cmpstr (dictate_l10n_n ("nl", "%u tool", "%u tools", 3), ==, "%u Werkzeug");
  /* not in the catalogue, or no catalogue: the source's own forms */
  g_assert_cmpstr (dictate_l10n_n ("nl", "%u cat", "%u cats", 1), ==, "%u cat");
  g_assert_cmpstr (dictate_l10n_n ("nl", "%u cat", "%u cats", 2), ==, "%u cats");
  g_assert_cmpstr (dictate_l10n_n ("xx", "%u cat", "%u cats", 1), ==, "%u cat");
  g_assert_cmpstr (dictate_l10n_n (NULL, "%u cat", "%u cats", 5), ==, "%u cats");
}

static void
test_plural_french (void)
{
  const Pair fr[] = { PAIR ("%u phrase\0%u phrases", "%u phrase\0%u phrases (fr)") };

  write_catalogue ("fr", fr, G_N_ELEMENTS (fr), FALSE);
  /* French counts 0 and 1 as the singular */
  g_assert_cmpstr (dictate_l10n_n ("fr", "%u phrase", "%u phrases", 0), ==, "%u phrase");
  g_assert_cmpstr (dictate_l10n_n ("fr", "%u phrase", "%u phrases", 1), ==, "%u phrase");
  g_assert_cmpstr (dictate_l10n_n ("fr", "%u phrase", "%u phrases", 2), ==, "%u phrases (fr)");
}

static void
test_plural_russian (void)
{
  const Pair ru[] = { PAIR ("%u phrase\0%u phrases", "один\0несколько\0много") };
  /* count, then the form it takes */
  const struct { gulong n; const char *form; } cases[] = {
    { 0, "много" }, { 1, "один" }, { 2, "несколько" }, { 4, "несколько" }, { 5, "много" }, { 11, "много" },
    { 12, "много" }, { 14, "много" }, { 15, "много" }, { 20, "много" }, { 21, "один" }, { 22, "несколько" },
    { 25, "много" }, { 100, "много" }, { 101, "один" }, { 111, "много" }, { 112, "много" }, { 121, "один" },
  };

  write_catalogue ("ru", ru, G_N_ELEMENTS (ru), FALSE);
  for (guint i = 0; i < G_N_ELEMENTS (cases); i++)
    g_assert_cmpstr (dictate_l10n_n ("ru", "%u phrase", "%u phrases", cases[i].n), ==, cases[i].form);
}

/* Polish: 1; 2-4, 22-24... but not 12-14; the rest. Ukrainian: Russian's. Hebrew: 1; 2; the rest */
static void
test_plural_polish_ukrainian_hebrew (void)
{
  const Pair forms[] = { PAIR ("%u phrase\0%u phrases", "one\0few\0many") };
  const struct { const char *language; gulong n; const char *form; } cases[] = {
    { "pl", 0, "many" }, { "pl", 1, "one" }, { "pl", 2, "few" }, { "pl", 4, "few" }, { "pl", 5, "many" },
    { "pl", 12, "many" }, { "pl", 14, "many" }, { "pl", 21, "many" }, { "pl", 22, "few" }, { "pl", 25, "many" },
    { "pl", 101, "many" }, { "pl", 102, "few" }, { "pl", 112, "many" },
    { "uk", 0, "many" }, { "uk", 1, "one" }, { "uk", 2, "few" }, { "uk", 11, "many" }, { "uk", 21, "one" },
    { "uk", 22, "few" }, { "uk", 111, "many" },
    { "he", 0, "many" }, { "he", 1, "one" }, { "he", 2, "few" }, { "he", 3, "many" }, { "he", 10, "many" },
    { "he", 20, "many" }, { "he", 102, "many" },
  };

  write_catalogue ("pl", forms, G_N_ELEMENTS (forms), FALSE);
  write_catalogue ("uk", forms, G_N_ELEMENTS (forms), FALSE);
  write_catalogue ("he", forms, G_N_ELEMENTS (forms), FALSE);
  for (guint i = 0; i < G_N_ELEMENTS (cases); i++)
    g_assert_cmpstr (dictate_l10n_n (cases[i].language, "%u phrase", "%u phrases", cases[i].n), ==, cases[i].form);
}

static void
test_other_byte_order (void)
{
  const Pair es[] = {
    PAIR ("Hello", "Hola"),
    PAIR ("%u phrase\0%u phrases", "%u frase\0%u frases"),
    PAIR ("speech speed\004Normal", "Normal"),
  };

  write_catalogue ("es", es, G_N_ELEMENTS (es), TRUE);
  g_assert_cmpstr (dictate_l10n ("es", "Hello"), ==, "Hola");
  g_assert_cmpstr (dictate_l10n_n ("es", "%u phrase", "%u phrases", 4), ==, "%u frases");
  g_assert_cmpstr (dictate_l10n_c ("es", "speech speed", "Normal"), ==, "Normal");
}

static void
test_broken_file (void)
{
  g_autofree char *dir = g_build_filename (LOCALEDIR, "it", "LC_MESSAGES", NULL);
  g_autofree char *path = g_build_filename (dir, GETTEXT_PACKAGE ".mo", NULL);

  g_assert_cmpint (g_mkdir_with_parents (dir, 0755), ==, 0);
  g_assert_true (g_file_set_contents (path, "not a catalogue, but longer than a header is", -1, NULL));
  g_assert_cmpstr (dictate_l10n ("it", "Hello"), ==, "Hello");
  g_assert_cmpstr (dictate_l10n_n ("it", "%u cat", "%u cats", 2), ==, "%u cats");
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/l10n/plain-and-context", test_plain_and_context);
  g_test_add_func ("/l10n/plural", test_plural_n_ne_1);
  g_test_add_func ("/l10n/plural-french", test_plural_french);
  g_test_add_func ("/l10n/plural-russian", test_plural_russian);
  g_test_add_func ("/l10n/plural-polish-ukrainian-hebrew", test_plural_polish_ukrainian_hebrew);
  g_test_add_func ("/l10n/other-byte-order", test_other_byte_order);
  g_test_add_func ("/l10n/broken-file", test_broken_file);
  return g_test_run ();
}
