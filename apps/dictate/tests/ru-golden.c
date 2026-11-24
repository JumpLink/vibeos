/* ru-golden: the Russian front end (dictate-ru.h) against tiny-voca-ru's own, in Python, on the lines of a corpus:
 * the normalised text, the stress (full and lite), and each utterance's piece, tokens and stress, as the voice would
 * read them. The reference is tests/ru-golden/golden.tsv, made by tests/ru-golden/make-golden.py.
 *
 *   ru-golden GOLDEN.tsv DATA_DIR [SHOWN]
 *
 * DATA_DIR holds the front end's data, as the voice is downloaded (stress.txt.gz, accent.onnx, omograph.onnx,
 * omograph_vocab.txt.gz); SHOWN differences are printed (20). The exit status is 0 when every line matches.
 *
 * The homograph model is a network: on another onnxruntime than the reference's a near tie can fall the other way
 * (with 1.21 and 1.23 one line of the corpus does: "вечера вечером дня."; with the reference's 1.32 none). */
#include "dictate-ru.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LIMIT (512 - 12)        /* the synthesizer's: max_tokens less 12 (dictate-voice.c) */

static char *
joined (GArray *values)
{
  GString *s = g_string_new (NULL);

  for (guint i = 0; i < values->len; i++)
    g_string_append_printf (s, "%s%" G_GINT64_FORMAT, i ? "," : "", g_array_index (values, gint64, i));
  return g_string_free (s, FALSE);
}

/* a piece of a sentence as the voice reads it: PIECE\x1fIDS\x1fSTRESS for each utterance, appended to `out` */
static void
read_piece (DictateRu *ru, const char *piece, GPtrArray *out)
{
  g_autoptr (GArray) ids = g_array_new (FALSE, FALSE, sizeof (gint64));
  g_autoptr (GArray) stress = g_array_new (FALSE, FALSE, sizeof (gint64));
  g_auto (GStrv) parts = NULL;

  if (!dictate_ru_phonemize (ru, piece, ids, stress))
    return;
  if (ids->len <= LIMIT)
    {
      g_autofree char *i = joined (ids), *s = joined (stress);
      g_ptr_array_add (out, g_strjoin ("\x1f", piece, i, s, NULL));
      return;
    }
  parts = dictate_ru_split_clause (piece);
  for (char **part = parts; part && *part; part++)
    read_piece (ru, *part, out);
}

/* TRUE when they differ, and then both added to `report` */
static gboolean
differs (GString *report, const char *what, const char *want, const char *got)
{
  g_autofree char *wanted = NULL, *given = NULL;

  if (!g_strcmp0 (want, got))
    return FALSE;
  /* an utterance's piece, ids and stress, apart */
  wanted = g_strdelimit (g_strdup (want), "\x1f", ' '), given = g_strdelimit (g_strdup (got), "\x1f", ' ');
  g_string_append_printf (report, "  %s want %s\n  %s got  %s\n", what, wanted, what, given);
  return TRUE;
}

int
main (int argc, char **argv)
{
  g_autoptr (GError) error = NULL;
  g_autofree char *contents = NULL;
  g_auto (GStrv) lines = NULL;
  DictateRu *full, *lite;
  guint shown = 0, limit = argc > 3 ? (guint) atoi (argv[3]) : 20, n = 0, normalised = 0, stressed = 0, light = 0,
        spoken = 0;

  if (argc < 3 || !g_file_get_contents (argv[1], &contents, NULL, &error))
    {
      g_printerr ("%s\n", error ? error->message : "ru-golden GOLDEN.tsv DATA_DIR [SHOWN]");
      return 2;
    }
  full = dictate_ru_new (argv[2], TRUE);
  lite = dictate_ru_new (argv[2], FALSE);
  lines = g_strsplit (contents, "\n", -1);
  for (char **line = lines; *line && **line; line++, n++)
    {
      g_auto (GStrv) fields = g_strsplit (*line, "\t", -1);
      g_autoptr (GPtrArray) utterances = g_ptr_array_new_with_free_func (g_free);
      g_auto (GStrv) sentences = NULL;
      g_autofree char *norm = NULL, *by_full = NULL, *by_lite = NULL, *want = NULL, *got = NULL;
      g_autoptr (GString) report = g_string_new (NULL);

      if (g_strv_length (fields) < 4)
        {
          g_printerr ("line %u: not TEXT, NORMALISED, FULL, LITE[, UTTERANCE…]\n", n + 1);
          return 2;
        }
      norm = dictate_ru_normalize (fields[0]);
      by_full = dictate_ru_accentuate (full, fields[1]);
      by_lite = dictate_ru_accentuate (lite, fields[1]);
      sentences = dictate_ru_sentences (fields[0]);
      for (char **sentence = sentences; *sentence; sentence++)
        read_piece (full, *sentence, utterances);
      g_ptr_array_add (utterances, NULL);
      want = g_strjoinv ("\n", fields + 4);
      got = g_strjoinv ("\n", (char **) utterances->pdata);
      normalised += differs (report, "normalised", fields[1], norm);
      stressed += differs (report, "full", fields[2], by_full);
      light += differs (report, "lite", fields[3], by_lite);
      spoken += differs (report, "utterances", want, got);
      if (report->len && shown++ < limit)
        printf ("TEXT %s\n%s", fields[0], report->str);
    }
  printf ("%u lines: normalised %u differ, full stress %u, lite stress %u, utterances (tokens and stress) %u\n", n,
          normalised, stressed, light, spoken);
  dictate_ru_free (full);
  dictate_ru_free (lite);
  return normalised || stressed || light || spoken;
}
