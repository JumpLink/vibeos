/* dictate-text's casing of what the recogniser heard */
#include "dictate-text.h"

static void
check_in (const char *language, const char *line, gboolean start, const char *expected)
{
  g_autofree char *tidy = dictate_text_tidy (line, start, language);

  g_assert_cmpstr (tidy, ==, expected);
}

static void
check (const char *line, gboolean start, const char *expected)
{
  check_in ("en", line, start, expected);
}

static void
test_tidy (void)
{
  /* a line that goes on a sentence after a pause */
  check ("It was the age of foolishness,", FALSE, "it was the age of foolishness,");
  check ("That was the epoch of incredulity.", FALSE, "that was the epoch of incredulity.");
  /* ... and one that starts one */
  check ("It was the best of times. it was the worst of times.", TRUE,
         "It was the best of times. It was the worst of times.");
  check ("we had everything before us.", TRUE, "We had everything before us.");
  /* words that keep their capital */
  check ("I think so.", FALSE, "I think so.");
  check ("I'm here.", FALSE, "I'm here.");
  check ("NASA said so.", FALSE, "NASA said so.");
  check ("McDonald came.", FALSE, "McDonald came.");
  check ("Is it?", FALSE, "is it?");
  /* not every full stop ends a sentence */
  check ("about 3.5 times. then more", TRUE, "About 3.5 times. Then more");
  check ("this, e.g. that", TRUE, "This, e.g. that");
  /* the model's doubled comma */
  check ("In short,, the period", TRUE, "In short, the period");
  /* letters beyond ASCII */
  check ("Über alles, ölig", FALSE, "über alles, ölig");
  check ("ärger. öl", TRUE, "Ärger. Öl");
  check ("", TRUE, "");
}

static void
test_tidy_languages (void)
{
  /* German: a line going on a sentence keeps its capital, a noun's, unless the word is never one */
  check_in ("de", "So ergab sich's.", FALSE, "so ergab sich's.");
  check_in ("de", "Dass es keine Prinzessin war.", FALSE, "dass es keine Prinzessin war.");
  check_in ("de", "Prinzessinnen kamen genug.", FALSE, "Prinzessinnen kamen genug.");
  check_in ("de", "Sie kam.", FALSE, "Sie kam.");
  check_in ("de", "fast jeden Tag. kam eine", TRUE, "Fast jeden Tag. Kam eine");
  /* "I" is English's only */
  check_in ("it", "I ragazzi.", FALSE, "i ragazzi.");
  /* Vosk's lower case: a capital at the document's start only */
  check_in ("ru", "что взглянув на него", TRUE, "Что взглянув на него");
  check_in ("ru", "привет. как дела", TRUE, "Привет. Как дела");
  check_in ("fr", "c'était un fort galant homme", FALSE, "c'était un fort galant homme");
  check_in ("fr", "à l'un des plus grands", TRUE, "À l'un des plus grands");
  /* Spanish's opening question mark */
  check_in ("es", "¿qué clase de ave era?", TRUE, "¿Qué clase de ave era?");
}

static void
test_ends_sentence (void)
{
  g_assert_true (dictate_text_ends_sentence (""));
  g_assert_true (dictate_text_ends_sentence ("  "));
  g_assert_true (dictate_text_ends_sentence ("Done. "));
  g_assert_true (dictate_text_ends_sentence ("Really?"));
  g_assert_true (dictate_text_ends_sentence ("Wait…"));
  g_assert_true (dictate_text_ends_sentence ("A list,\n"));
  g_assert_false (dictate_text_ends_sentence ("of wisdom,"));
  g_assert_false (dictate_text_ends_sentence ("of wisdom, "));
  g_assert_false (dictate_text_ends_sentence ("the age"));
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/dictate/text/tidy", test_tidy);
  g_test_add_func ("/dictate/text/tidy-languages", test_tidy_languages);
  g_test_add_func ("/dictate/text/ends-sentence", test_ends_sentence);
  return g_test_run ();
}
