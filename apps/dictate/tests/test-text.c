/* dictate-text's casing of what the recogniser heard */
#include "dictate-text.h"

static void
check (const char *line, gboolean start, const char *expected)
{
  g_autofree char *tidy = dictate_text_tidy (line, start);

  g_assert_cmpstr (tidy, ==, expected);
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
  g_test_add_func ("/dictate/text/ends-sentence", test_ends_sentence);
  return g_test_run ();
}
