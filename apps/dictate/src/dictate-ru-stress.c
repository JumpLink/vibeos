/* dictate-ru-stress: where the stress falls in each Russian word, marked with a '+' before its vowel (and ё put back):
 * from ruaccent's dictionary (stress.txt.gz), a word it lacks from ruaccent's character model (accent.onnx), and
 * a word with more than one reading (зАмок, замОк) from the words around it, by ruaccent's homograph model
 * (omograph.onnx, a BERT reading WordPieces). A port of tiny-voca-ru's frontend/stress.py and omograph.py (see
 * dictate-ru.h); the models and the dictionary are loaded when first needed.
 *
 * The dictionary is 400,000 lines in no order. It is sorted once, by its folded key, into a file beside it named by
 * the source's sum (stress-SUM.sorted: "KEY\tREADING READING\n"; in the user's cache directory when the dictionary's
 * is not writable), which is mapped and searched by halving: only the pages a search touches are read in. */
#include "dictate-ru.h"
#include "dictate-ru-private.h"

#include <gio/gio.h>
#include <onnxruntime/onnxruntime_c_api.h>
#include <math.h>
#include <string.h>
#include <unistd.h>

#define STRESS_VOWELS  "аеёиоуыэюя"
#define ACCENT_MAX     58       /* accent.onnx's positions (60) less [bos] and [eos] */
#define WORD_PATTERN   "(?i)[а-яё+]+(?:-[а-яё+]+)*"
#define SENTENCE_BREAK "(?<=[.!?…])(?:[\"')\\]»“])?\\s+|\\n+"

/* omograph.onnx's WordPieces */
#define PAD_ID         0
#define UNK_ID         1
#define CLS_ID         2
#define SEP_ID         3
#define W_OPEN_ID      30522
#define W_CLOSE_ID     30523
#define MAX_LENGTH     512
#define MAX_WORD_CHARS 100

/* accent.onnx's characters, in their ids' order */
static const char *const ACCENT_VOCAB[] = {
  "[pad]", "[unk]", "[bos]", "[eos]", "'", "-", ".", "?", "`", "c", "e", "́", "а", "б", "в", "г", "д", "е", "ж",
  "з", "и", "й", "к", "л", "м", "н", "о", "п", "р", "с", "т", "у", "ф", "х", "ц", "ч", "ш", "щ", "ъ", "ы", "ь", "э",
  "ю", "я", "ё",
};

/* ---------------- onnxruntime ---------------- */

static const OrtApi *ort;
static OrtEnv *ort_env;
G_LOCK_DEFINE_STATIC (ort);

static gboolean
ort_ok (OrtStatus *status, GError **error)
{
  if (!status)
    return TRUE;
  g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "onnxruntime: %s", ort->GetErrorMessage (status));
  ort->ReleaseStatus (status);
  return FALSE;
}

typedef struct {
  OrtSession *session;
  OrtMemoryInfo *memory;
} Model;

static gboolean
model_open (Model *model, const char *path, int threads, GError **error)
{
  OrtSessionOptions *options = NULL;
  gboolean ok = TRUE;

  G_LOCK (ort);
  if (!ort_env)
    {
      const OrtApiBase *base = OrtGetApiBase ();
      ort = base ? base->GetApi (ORT_API_VERSION) : NULL;
      if (!ort)
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, "onnxruntime %s has no API version %d",
                       base ? base->GetVersionString () : "?", ORT_API_VERSION);
          ok = FALSE;
        }
      else
        ok = ort_ok (ort->CreateEnv (ORT_LOGGING_LEVEL_WARNING, "dictate-ru", &ort_env), error);
    }
  G_UNLOCK (ort);
  if (!ok || !ort_ok (ort->CreateSessionOptions (&options), error))
    return FALSE;
  ok = ort_ok (ort->SetIntraOpNumThreads (options, threads), error) &&
       ort_ok (ort->SetInterOpNumThreads (options, 1), error) &&
       ort_ok (ort->SetSessionGraphOptimizationLevel (options, ORT_ENABLE_ALL), error) &&
       ort_ok (ort->CreateSession (ort_env, path, options, &model->session), error) &&
       ort_ok (ort->CreateCpuMemoryInfo (OrtArenaAllocator, OrtMemTypeDefault, &model->memory), error);
  ort->ReleaseSessionOptions (options);
  return ok;
}

static void
model_close (Model *model)
{
  if (model->session)
    ort->ReleaseSession (model->session);
  if (model->memory)
    ort->ReleaseMemoryInfo (model->memory);
  model->session = NULL;
  model->memory = NULL;
}

/* the model run on int64 rows of `n` (one per input name, the same length), its output `output` as floats; *count
 * its length */
static float *
model_run (Model *model, const char *const *inputs, gint64 *const *rows, gsize n_inputs, gsize n, const char *output,
           gsize *count, GError **error)
{
  OrtValue *in[4] = { NULL }, *result = NULL;
  const gint64 shape[] = { 1, n };
  float *values = NULL;
  gboolean ok = TRUE;

  for (gsize i = 0; i < n_inputs && ok; i++)
    ok = ort_ok (ort->CreateTensorWithDataAsOrtValue (model->memory, rows[i], n * sizeof (gint64), shape, 2,
                                                      ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64, &in[i]), error);
  if (ok)
    ok = ort_ok (ort->Run (model->session, NULL, inputs, (const OrtValue *const *) in, n_inputs, &output, 1,
                           &result), error);
  if (ok)
    {
      OrtTensorTypeAndShapeInfo *info = NULL;
      size_t elements = 0;
      float *data = NULL;
      ok = ort_ok (ort->GetTensorTypeAndShape (result, &info), error) &&
           ort_ok (ort->GetTensorShapeElementCount (info, &elements), error) &&
           ort_ok (ort->GetTensorMutableData (result, (void **) &data), error);
      if (info)
        ort->ReleaseTensorTypeAndShapeInfo (info);
      if (ok)
        {
          values = g_memdup2 (data, elements * sizeof (float));
          *count = elements;
        }
    }
  for (gsize i = 0; i < n_inputs; i++)
    if (in[i])
      ort->ReleaseValue (in[i]);
  if (result)
    ort->ReleaseValue (result);
  return values;
}

/* ---------------- the stress ---------------- */

struct _RuStress {
  char *dir;
  gboolean homographs;          /* the homograph model used ("full"), or the first reading taken ("lite") */
  GMutex lock;                  /* the loading below */
  GMappedFile *lexicon;         /* the sorted dictionary */
  gboolean lexicon_tried;
  Model accent, omograph;
  gboolean accent_tried, omograph_tried;
  GHashTable *vocab;            /* a WordPiece → its id + 1 */
  GHashTable *predicted;        /* a word → accent.onnx's stressed reading of it, "" for none */
};

static char *
fold (const char *word)
{
  g_autofree char *plain = ru_plain (word);

  return ru_replace (plain, "ё", "е");
}

static gboolean
has (const char *s, const char *what)
{
  return strstr (s, what) != NULL;
}

/* ---------------- the dictionary ---------------- */

static int
compare_lines (gconstpointer a, gconstpointer b)
{
  return strcmp (*(const char *const *) a, *(const char *const *) b);
}

/* the dictionary sorted by its keys, written to `sorted`: "KEY\tREADINGS\n" */
static gboolean
lexicon_sort (const char *source, const char *sorted, GError **error)
{
  g_autoptr (GFile) file = g_file_new_for_path (source);
  g_autoptr (GInputStream) raw = (GInputStream *) g_file_read (file, NULL, error);
  g_autoptr (GZlibDecompressor) gzip = NULL;
  g_autoptr (GInputStream) stream = NULL;
  g_autoptr (GDataInputStream) lines = NULL;
  g_autoptr (GPtrArray) entries = g_ptr_array_new_with_free_func (g_free);
  GString *out;
  char *line;
  gsize length;
  gboolean ok;

  if (!raw)
    return FALSE;
  gzip = g_zlib_decompressor_new (G_ZLIB_COMPRESSOR_FORMAT_GZIP);
  stream = g_converter_input_stream_new (raw, G_CONVERTER (gzip));
  lines = g_data_input_stream_new (stream);
  while ((line = g_data_input_stream_read_line_utf8 (lines, &length, NULL, error)))
    {
      if (*line)
        {
          const char *space = strchr (line, ' ');
          g_autofree char *first = space ? g_strndup (line, space - line) : g_strdup (line);
          g_autofree char *key = fold (first);
          g_ptr_array_add (entries, g_strconcat (key, "\t", line, NULL));
        }
      g_free (line);
    }
  if (error && *error)
    return FALSE;
  g_ptr_array_sort (entries, compare_lines);
  out = g_string_sized_new (16 * 1024 * 1024);
  for (guint i = 0; i < entries->len; i++)
    {
      g_string_append (out, entries->pdata[i]);
      g_string_append_c (out, '\n');
    }
  ok = g_file_set_contents_full (sorted, out->str, out->len, G_FILE_SET_CONTENTS_CONSISTENT, 0644, error);
  g_string_free (out, TRUE);
  return ok;
}

static GMappedFile *
lexicon_open (RuStress *self)
{
  g_mutex_lock (&self->lock);
  if (!self->lexicon_tried)
    {
      g_autoptr (GError) error = NULL;
      g_autofree char *source = g_build_filename (self->dir, "stress.txt.gz", NULL);
      g_autofree char *data = NULL;
      gsize size;
      self->lexicon_tried = TRUE;
      if (g_file_get_contents (source, &data, &size, &error))
        {
          g_autofree char *sum = g_compute_checksum_for_data (G_CHECKSUM_SHA256, (const guchar *) data, size);
          g_autofree char *name = g_strdup_printf ("stress-%.16s.sorted", sum);
          g_autofree char *sorted = g_build_filename (self->dir, name, NULL);
          if (!g_file_test (sorted, G_FILE_TEST_EXISTS) && access (self->dir, W_OK) != 0)
            {
              g_autofree char *cache = g_build_filename (g_get_user_cache_dir (), "vibeos-dictate", NULL);
              g_free (sorted);
              sorted = g_build_filename (cache, name, NULL);
              g_mkdir_with_parents (cache, 0700);
            }
          if (g_file_test (sorted, G_FILE_TEST_EXISTS) || lexicon_sort (source, sorted, &error))
            self->lexicon = g_mapped_file_new (sorted, FALSE, &error);
        }
      if (!self->lexicon)
        g_warning ("dictate-ru: no stress dictionary: %s", error ? error->message : "?");
    }
  g_mutex_unlock (&self->lock);
  return self->lexicon;
}

/* the readings of a folded key, in the dictionary's order; NULL when it has none */
static char **
lexicon_get (RuStress *self, const char *key)
{
  GMappedFile *file = lexicon_open (self);
  const char *data;
  gsize lo = 0, hi, key_length = strlen (key);

  if (!file)
    return NULL;
  data = g_mapped_file_get_contents (file);
  hi = g_mapped_file_get_length (file);
  while (lo < hi)
    {
      gsize mid = lo + (hi - lo) / 2, start = mid, end;
      while (start > lo && data[start - 1] != '\n')
        start--;
      const char *tab = memchr (data + start, '\t', hi - start);
      const char *newline = memchr (data + start, '\n', g_mapped_file_get_length (file) - start);
      if (!tab || !newline || tab > newline)
        return NULL;
      end = newline - data + 1;
      gsize length = tab - (data + start);
      int order = memcmp (data + start, key, MIN (length, key_length));
      if (!order)
        order = length < key_length ? -1 : length > key_length ? 1 : 0;
      if (!order)
        {
          g_autofree char *readings = g_strndup (tab + 1, newline - tab - 1);
          return g_strsplit (readings, " ", -1);
        }
      if (order < 0)
        lo = end;
      else
        hi = start;
    }
  return NULL;
}

/* the dictionary's readings of `key` with the preferred one first, as the original orders them when it loads */
static char **
lexicon_readings (RuStress *self, const char *key)
{
  char **readings = lexicon_get (self, key);
  const char *preferred = ru_lookup (ru_pairs ("STRESS_PREFERRED"), key);

  if (!readings || !preferred)
    return readings;
  if (g_strv_contains ((const char *const *) readings, preferred))
    {
      GPtrArray *ordered = g_ptr_array_new ();
      g_ptr_array_add (ordered, g_strdup (preferred));
      for (char **r = readings; *r; r++)
        if (!g_str_equal (*r, preferred))
          g_ptr_array_add (ordered, g_strdup (*r));
      g_ptr_array_add (ordered, NULL);
      g_strfreev (readings);
      return (char **) g_ptr_array_free (ordered, FALSE);
    }
  if (ru_in (ru_list ("STRESS_PATRONYMICS"), key))
    {
      g_strfreev (readings);
      readings = g_new0 (char *, 2);
      readings[0] = g_strdup (preferred);
    }
  return readings;
}

/* `word` stressed by itself: with its ё, or its only vowel; NULL when it has more than one and no ё */
static char *
implied (const char *word)
{
  g_autofree char *lower = ru_lower (word);
  const char *vowel = NULL;

  if (has (lower, "ё"))
    return g_steal_pointer (&lower);
  for (const char *p = lower; *p; p = g_utf8_next_char (p))
    {
      char letter[8] = { 0 };
      g_unichar_to_utf8 (g_utf8_get_char (p), letter);
      if (strstr (STRESS_VOWELS, letter))
        {
          if (vowel)
            return NULL;
          vowel = p;
        }
    }
  if (!vowel)
    return NULL;
  return g_strdup_printf ("%.*s+%s", (int) (vowel - lower), lower, vowel);
}

/* the stressed readings of a word, the most frequent first; NULL for none */
static char **
readings (RuStress *self, const char *word)
{
  g_autofree char *plain = ru_plain (word);
  g_autofree char *key = NULL;
  char **found;

  if (ru_in (ru_list ("STRESS_CLITICS"), plain))
    return NULL;
  key = fold (plain);
  found = lexicon_readings (self, key);
  if (found && has (plain, "ё"))
    {
      GPtrArray *kept = g_ptr_array_new ();
      for (char **r = found; *r; r++)
        {
          g_autofree char *bare = ru_replace (*r, "+", "");
          if (g_str_equal (bare, plain))
            g_ptr_array_add (kept, g_strdup (*r));
        }
      g_ptr_array_add (kept, NULL);
      g_strfreev (found);
      found = (char **) g_ptr_array_free (kept, FALSE);
    }
  if (found && *found)
    return found;
  g_strfreev (found);
  char *stressed = implied (plain);
  if (!stressed)
    return NULL;
  found = g_new0 (char *, 2);
  found[0] = stressed;
  return found;
}

/* the readings of a homograph that keep its spelling, when there are more than one; NULL otherwise */
static char **
homograph_readings (RuStress *self, const char *word)
{
  g_autofree char *lower = ru_plain (word);
  g_auto (GStrv) all = readings (self, lower);
  GPtrArray *candidates;

  if (!all)
    return NULL;
  candidates = g_ptr_array_new ();
  for (char **r = all; *r; r++)
    {
      g_autofree char *bare = ru_replace (*r, "+", "");
      if (has (*r, "+") && g_str_equal (bare, lower))
        g_ptr_array_add (candidates, g_strdup (*r));
    }
  g_ptr_array_add (candidates, NULL);
  if (candidates->len < 3)
    {
      g_ptr_array_set_free_func (candidates, g_free);
      g_ptr_array_free (candidates, TRUE);
      return NULL;
    }
  return (char **) g_ptr_array_free (candidates, FALSE);
}

/* a word's hyphenated parts, lowered, with "" for a clitic part (кто-то: кто, "") */
static char **
parts_of (const char *word)
{
  g_autofree char *lower = ru_lower (word);
  char **parts = g_strsplit (lower, "-", -1);
  guint n = g_strv_length (parts);

  for (guint i = 0; i < n; i++)
    if (ru_in (ru_list ("STRESS_CLITICS"), parts[i]) || (i && ru_in (ru_list ("STRESS_ENCLITICS"), parts[i])) ||
        (i + 1 < n && ru_in (ru_list ("STRESS_PROCLITICS"), parts[i])))
      {
        g_free (parts[i]);
        parts[i] = g_strdup ("");
      }
  return parts;
}

/* the stress marks and ё of the lowercase `reading` on `word`, its capitals kept */
static char *
apply (const char *word, const char *reading)
{
  GString *out = g_string_new (NULL);
  const char *letter = word;

  for (const char *p = reading; *p; p = g_utf8_next_char (p))
    {
      gunichar c = g_utf8_get_char (p);
      if (c == '+')
        {
          g_string_append_c (out, '+');
          continue;
        }
      gboolean upper = *letter && g_unichar_isupper (g_utf8_get_char (letter));
      if (*letter)
        letter = g_utf8_next_char (letter);
      g_string_append_unichar (out, upper ? g_unichar_toupper (c) : c);
    }
  return g_string_free (out, FALSE);
}

/* ---------------- accent.onnx: a word the dictionary lacks ---------------- */

static char *
predict (RuStress *self, const char *word)
{
  g_autofree char *lower = ru_lower (word);
  glong n = g_utf8_strlen (lower, -1);
  gboolean vowel = FALSE;
  char *cached;

  for (const char *p = lower; *p && !vowel; p = g_utf8_next_char (p))
    {
      char letter[8] = { 0 };
      g_unichar_to_utf8 (g_utf8_get_char (p), letter);
      vowel = strstr (STRESS_VOWELS, letter) != NULL;
    }
  if (n > ACCENT_MAX || !vowel)
    return NULL;
  g_mutex_lock (&self->lock);
  cached = g_hash_table_lookup (self->predicted, word);
  if (cached)
    {
      g_mutex_unlock (&self->lock);
      return *cached ? g_strdup (cached) : NULL;
    }
  if (!self->accent_tried)
    {
      g_autoptr (GError) error = NULL;
      g_autofree char *path = g_build_filename (self->dir, "accent.onnx", NULL);
      self->accent_tried = TRUE;
      if (!model_open (&self->accent, path, 1, &error))
        {
          g_warning ("dictate-ru: no accent model: %s", error->message);
          model_close (&self->accent);
        }
    }
  g_mutex_unlock (&self->lock);
  if (!self->accent.session)
    return NULL;

  gsize length = n + 2, count = 0;
  g_autofree gint64 *ids = g_new0 (gint64, length), *mask = g_new0 (gint64, length), *types = g_new0 (gint64, length);
  ids[0] = 2;
  ids[length - 1] = 3;
  glong i = 1;
  for (const char *p = lower; *p; p = g_utf8_next_char (p), i++)
    {
      const char *next = g_utf8_next_char (p);
      ids[i] = 1;
      for (guint v = 0; v < G_N_ELEMENTS (ACCENT_VOCAB); v++)
        if (strlen (ACCENT_VOCAB[v]) == (gsize) (next - p) && !memcmp (ACCENT_VOCAB[v], p, next - p))
          ids[i] = v;
    }
  for (gsize k = 0; k < length; k++)
    mask[k] = 1;
  static const char *const INPUTS[] = { "input_ids", "attention_mask", "token_type_ids" };
  gint64 *rows[] = { ids, mask, types };
  g_autoptr (GError) error = NULL;
  g_mutex_lock (&self->lock);
  g_autofree float *logits = model_run (&self->accent, INPUTS, rows, 3, length, "logits", &count, &error);
  g_mutex_unlock (&self->lock);
  if (!logits || count % length)
    {
      g_warning ("dictate-ru: the accent model failed: %s", error ? error->message : "its output's size");
      return NULL;
    }
  gsize classes = count / length;
  glong best = -1;
  double best_p = -1.0;
  i = 0;
  for (const char *p = lower; *p; p = g_utf8_next_char (p), i++)
    {
      char letter[8] = { 0 };
      g_unichar_to_utf8 (g_utf8_get_char (p), letter);
      if (!strstr (STRESS_VOWELS, letter))
        continue;
      const float *row = logits + (i + 1) * classes;
      double sum = 0;
      for (gsize k = 0; k < classes; k++)
        sum += exp ((double) row[k] - (double) row[1]);
      double prob = 1.0 / sum;
      if (prob > best_p)
        {
          best = i;
          best_p = prob;
        }
    }
  char *stressed = NULL;
  if (best >= 0)
    {
      const char *at = g_utf8_offset_to_pointer (word, best);
      stressed = g_strdup_printf ("%.*s+%s", (int) (at - word), word, at);
    }
  g_mutex_lock (&self->lock);
  g_hash_table_insert (self->predicted, g_strdup (word), g_strdup (stressed ? stressed : ""));
  g_mutex_unlock (&self->lock);
  return stressed;
}

/* a word part's stress: the dictionary's first reading, else the model's guess; NULL for none */
static char *
known_or_predicted (RuStress *self, const char *part)
{
  g_auto (GStrv) found = readings (self, part);

  if (found && *found)
    return g_strdup (found[0]);
  return predict (self, part);
}

/* ---------------- omograph.onnx: a homograph from its sentence ---------------- */

static gboolean
vocab_load (RuStress *self)
{
  g_mutex_lock (&self->lock);
  if (!self->omograph_tried)
    {
      g_autoptr (GError) error = NULL;
      g_autofree char *model = g_build_filename (self->dir, "omograph.onnx", NULL);
      g_autofree char *vocab = g_build_filename (self->dir, "omograph_vocab.txt.gz", NULL);
      g_autoptr (GFile) file = g_file_new_for_path (vocab);
      g_autoptr (GInputStream) raw = (GInputStream *) g_file_read (file, NULL, &error);
      self->omograph_tried = TRUE;
      if (raw)
        {
          g_autoptr (GZlibDecompressor) gzip = g_zlib_decompressor_new (G_ZLIB_COMPRESSOR_FORMAT_GZIP);
          g_autoptr (GInputStream) stream = g_converter_input_stream_new (raw, G_CONVERTER (gzip));
          g_autoptr (GDataInputStream) lines = g_data_input_stream_new (stream);
          char *line;
          gsize length;
          guint id = 0;
          self->vocab = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
          while ((line = g_data_input_stream_read_line_utf8 (lines, &length, NULL, &error)))
            {
              /* the original's dict: a later line with the same piece replaces the earlier */
              g_hash_table_insert (self->vocab, line, GUINT_TO_POINTER (++id));
            }
        }
      if (!self->vocab || error || !model_open (&self->omograph, model, 1, &error))
        {
          g_warning ("dictate-ru: no homograph model: %s", error ? error->message : "?");
          model_close (&self->omograph);
          g_clear_pointer (&self->vocab, g_hash_table_unref);
        }
    }
  g_mutex_unlock (&self->lock);
  return self->omograph.session != NULL;
}

static gboolean
bert_whitespace (gunichar c)
{
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || g_unichar_type (c) == G_UNICODE_SPACE_SEPARATOR;
}

static gboolean
bert_control (gunichar c)
{
  if (c == '\t' || c == '\n' || c == '\r')
    return FALSE;
  switch (g_unichar_type (c))
    {
    case G_UNICODE_CONTROL: case G_UNICODE_FORMAT: case G_UNICODE_UNASSIGNED: case G_UNICODE_PRIVATE_USE:
    case G_UNICODE_SURROGATE:
      return TRUE;
    default:
      return FALSE;
    }
}

static gboolean
bert_punctuation (gunichar c)
{
  if ((c >= 33 && c <= 47) || (c >= 58 && c <= 64) || (c >= 91 && c <= 96) || (c >= 123 && c <= 126))
    return TRUE;
  switch (g_unichar_type (c))
    {
    case G_UNICODE_CONNECT_PUNCTUATION: case G_UNICODE_DASH_PUNCTUATION: case G_UNICODE_CLOSE_PUNCTUATION:
    case G_UNICODE_FINAL_PUNCTUATION: case G_UNICODE_INITIAL_PUNCTUATION: case G_UNICODE_OTHER_PUNCTUATION:
    case G_UNICODE_OPEN_PUNCTUATION:
      return TRUE;
    default:
      return FALSE;
    }
}

static gboolean
bert_chinese (gunichar c)
{
  return (c >= 0x4E00 && c <= 0x9FFF) || (c >= 0x3400 && c <= 0x4DBF) || (c >= 0x20000 && c <= 0x2A6DF) ||
         (c >= 0x2A700 && c <= 0x2B73F) || (c >= 0x2B740 && c <= 0x2B81F) || (c >= 0x2B820 && c <= 0x2CEAF) ||
         (c >= 0xF900 && c <= 0xFAFF) || (c >= 0x2F800 && c <= 0x2FA1F);
}

/* Python's str.isspace (), which str.split () splits at */
static gboolean
python_space (gunichar c)
{
  return (c >= 0x09 && c <= 0x0d) || (c >= 0x1c && c <= 0x1f) || c == 0x85 || g_unichar_type (c) ==
         G_UNICODE_SPACE_SEPARATOR || g_unichar_type (c) == G_UNICODE_LINE_SEPARATOR ||
         g_unichar_type (c) == G_UNICODE_PARAGRAPH_SEPARATOR;
}

static void
wordpiece (RuStress *self, const char *token, GArray *ids)
{
  glong n = g_utf8_strlen (token, -1), start = 0;
  guint from = ids->len;

  if (n > MAX_WORD_CHARS)
    {
      g_array_append_val (ids, (gint64) { UNK_ID });
      return;
    }
  while (start < n)
    {
      glong end = n;
      guint found = 0;
      for (; start < end; end--)
        {
          const char *a = g_utf8_offset_to_pointer (token, start), *b = g_utf8_offset_to_pointer (token, end);
          g_autofree char *piece = g_strdup_printf ("%s%.*s", start ? "##" : "", (int) (b - a), a);
          found = GPOINTER_TO_UINT (g_hash_table_lookup (self->vocab, piece));
          if (found)
            break;
        }
      if (!found)
        {
          g_array_set_size (ids, from);
          g_array_append_val (ids, (gint64) { UNK_ID });
          return;
        }
      gint64 id = found - 1;
      g_array_append_val (ids, id);
      start = end;
    }
}

/* BERT's basic tokens of `text` (BertNormalizer without lowering, BertPreTokenizer), each in WordPieces */
static GArray *
wordpiece_ids (RuStress *self, const char *text)
{
  GArray *ids = g_array_new (FALSE, FALSE, sizeof (gint64));
  GString *cleaned = g_string_new (NULL), *buffer = g_string_new (NULL);

  for (const char *p = text; *p; p = g_utf8_next_char (p))
    {
      gunichar c = g_utf8_get_char (p);
      if (c == 0xFFFD || bert_control (c))
        continue;
      if (bert_whitespace (c))
        g_string_append_c (cleaned, ' ');
      else if (bert_chinese (c))
        {
          g_string_append_c (cleaned, ' ');
          g_string_append_unichar (cleaned, c);
          g_string_append_c (cleaned, ' ');
        }
      else
        g_string_append_unichar (cleaned, c);
    }
  g_string_append_c (cleaned, ' ');
  for (const char *p = cleaned->str; *p; p = g_utf8_next_char (p))
    {
      gunichar c = g_utf8_get_char (p);
      if (python_space (c) || bert_punctuation (c))
        {
          if (buffer->len)
            wordpiece (self, buffer->str, ids);
          g_string_truncate (buffer, 0);
          if (!python_space (c))
            {
              char piece[8] = { 0 };
              g_unichar_to_utf8 (c, piece);
              wordpiece (self, piece, ids);
            }
        }
      else
        g_string_append_unichar (buffer, c);
    }
  g_string_free (cleaned, TRUE);
  g_string_free (buffer, TRUE);
  return ids;
}

/* the ids of `text`'s part from `start` to `end`, '+' dropped and lowered */
static GArray *
span_ids (RuStress *self, const char *text, gsize start, gsize end)
{
  g_autofree char *span = g_strndup (text + start, end - start);
  g_autofree char *plain = ru_plain (span);

  return wordpiece_ids (self, plain);
}

static void
append_ids (GArray *to, const GArray *from, guint first, guint count)
{
  if (count)
    g_array_append_vals (to, &g_array_index (from, gint64, first), count);
}

/* each candidate's score (logit 1 less logit 0) for the homograph from `start` to `end` of `text`; the index of the
 * best, 0 when they cannot be scored */
static guint
choose (RuStress *self, const char *text, gsize start, gsize end, char **candidates)
{
  gsize sent_start = 0, sent_end = strlen (text);
  GMatchInfo *m = NULL;
  guint best = 0;
  double best_score = 0;

  g_regex_match_full (ru_rx (SENTENCE_BREAK), text, start, 0, 0, &m, NULL);
  while (g_match_info_matches (m))
    {
      int e;
      g_match_info_fetch_pos (m, 0, NULL, &e);
      sent_start = e;
      g_match_info_next (m, NULL);
    }
  g_match_info_free (m);
  if (g_regex_match_full (ru_rx (SENTENCE_BREAK), text, -1, end, 0, &m, NULL))
    {
      int s;
      g_match_info_fetch_pos (m, 0, &s, NULL);
      sent_end = s;
    }
  g_match_info_free (m);

  g_autoptr (GArray) left = span_ids (self, text, sent_start, start);
  g_autoptr (GArray) word = span_ids (self, text, start, end);
  g_autoptr (GArray) right = span_ids (self, text, end, MAX (end, sent_end));
  for (guint i = 0; candidates[i]; i++)
    {
      g_autofree char *lower = ru_lower (candidates[i]);
      g_autoptr (GArray) cand = wordpiece_ids (self, lower);
      g_autoptr (GArray) seq = g_array_new (FALSE, FALSE, sizeof (gint64));
      int budget = MAX_LENGTH - (int) word->len - (int) cand->len - 5;   /* [CLS] <w> </w> [SEP] [SEP] */
      guint left_first = 0, left_n = 0, right_n = 0, word_n = word->len, cand_n = cand->len;
      if (budget >= 0)
        {
          int half = budget / 2;
          left_n = left->len;
          right_n = right->len;
          if ((int) (left->len + right->len) > budget)
            {
              if ((int) left->len <= half)
                right_n = budget - left->len;
              else if ((int) right->len <= budget - half)
                left_n = budget - right->len;
              else
                {
                  left_n = half;
                  right_n = budget - half;
                }
            }
          left_first = left->len - left_n;
        }
      else
        {
          int cap = MAX_LENGTH - 5, half = cap / 2;
          word_n = MIN ((int) word->len, half);
          cand_n = MIN ((int) cand->len, cap - (int) word_n);
        }
      g_array_append_val (seq, (gint64) { CLS_ID });
      append_ids (seq, left, left_first, left_n);
      g_array_append_val (seq, (gint64) { W_OPEN_ID });
      append_ids (seq, word, 0, word_n);
      g_array_append_val (seq, (gint64) { W_CLOSE_ID });
      append_ids (seq, right, 0, right_n);
      g_array_append_val (seq, (gint64) { SEP_ID });
      append_ids (seq, cand, 0, cand_n);
      g_array_append_val (seq, (gint64) { SEP_ID });

      g_autofree gint64 *mask = g_new (gint64, seq->len);
      for (guint k = 0; k < seq->len; k++)
        mask[k] = 1;
      static const char *const INPUTS[] = { "input_ids", "attention_mask" };
      gint64 *rows[] = { (gint64 *) seq->data, mask };
      g_autoptr (GError) error = NULL;
      gsize count = 0;
      g_mutex_lock (&self->lock);
      g_autofree float *logits = model_run (&self->omograph, INPUTS, rows, 2, seq->len, "logits", &count, &error);
      g_mutex_unlock (&self->lock);
      if (!logits || count < 2)
        {
          g_warning ("dictate-ru: the homograph model failed: %s", error ? error->message : "its output's size");
          return 0;
        }
      double score = (double) logits[1] - (double) logits[0];
      if (i == 0 || score > best_score)
        {
          best = i;
          best_score = score;
        }
    }
  return best;
}

/* ---------------- a text's stress ---------------- */

typedef struct {
  gsize start, end;
  char *reading;
} Override;

static const char *
override_at (GArray *overrides, gsize start, gsize end)
{
  for (guint i = 0; overrides && i < overrides->len; i++)
    {
      Override *o = &g_array_index (overrides, Override, i);
      if (o->start == start && o->end == end)
        return o->reading;
    }
  return NULL;
}

/* each homograph of `text` resolved, as (start, end, reading) */
static GArray *
resolve (RuStress *self, const char *text)
{
  GArray *overrides = g_array_new (FALSE, FALSE, sizeof (Override));
  GMatchInfo *m = NULL;

  g_regex_match (ru_rx (WORD_PATTERN), text, 0, &m);
  while (g_match_info_matches (m))
    {
      int start, end;
      g_autofree char *word = g_match_info_fetch (m, 0);
      g_match_info_fetch_pos (m, 0, &start, &end);
      if (!has (word, "+"))
        {
          g_autofree char *key = fold (word);
          g_auto (GStrv) whole = has (word, "-") ? lexicon_readings (self, key) : NULL;
          if (whole && *whole)
            {
              g_auto (GStrv) candidates = homograph_readings (self, word);
              if (candidates && vocab_load (self))
                {
                  Override o = { start, end, g_strdup (candidates[choose (self, text, start, end, candidates)]) };
                  g_array_append_val (overrides, o);
                }
            }
          else
            {
              g_auto (GStrv) parts = g_strsplit (word, "-", -1), lowers = parts_of (word);
              gsize pos = start;
              for (guint i = 0; parts[i]; i++)
                {
                  if (*lowers[i])
                    {
                      g_auto (GStrv) candidates = homograph_readings (self, lowers[i]);
                      if (candidates && vocab_load (self))
                        {
                          gsize part_end = pos + strlen (parts[i]);
                          Override o = { pos, part_end,
                                         g_strdup (candidates[choose (self, text, pos, part_end, candidates)]) };
                          g_array_append_val (overrides, o);
                        }
                    }
                  pos += strlen (parts[i]) + 1;
                }
            }
        }
      g_match_info_next (m, NULL);
    }
  g_match_info_free (m);
  return overrides;
}

/* one word (from `start` in the text) stressed */
static char *
stress_span (RuStress *self, gsize start, const char *word, GArray *overrides)
{
  GString *out;
  gsize pos = start;

  if (has (word, "+"))
    return g_strdup (word);
  if (has (word, "-"))
    {
      g_autofree char *key = fold (word);
      g_auto (GStrv) full = lexicon_readings (self, key);
      if (full && *full)
        {
          const char *chosen = override_at (overrides, start, start + strlen (word));
          return apply (word, chosen ? chosen : full[0]);
        }
    }
  g_auto (GStrv) parts = g_strsplit (word, "-", -1), lowers = parts_of (word);
  out = g_string_new (NULL);
  for (guint i = 0; parts[i]; i++)
    {
      g_autofree char *stressed = NULL;
      if (*lowers[i])
        {
          const char *chosen = override_at (overrides, pos, pos + strlen (parts[i]));
          stressed = chosen ? g_strdup (chosen) : known_or_predicted (self, lowers[i]);
        }
      if (i)
        g_string_append_c (out, '-');
      g_autofree char *a = stressed ? fold (stressed) : NULL, *b = stressed ? fold (parts[i]) : NULL;
      if (stressed && g_str_equal (a, b))
        {
          g_autofree char *applied = apply (parts[i], stressed);
          g_string_append (out, applied);
        }
      else
        g_string_append (out, parts[i]);
      pos += strlen (parts[i]) + 1;
    }
  return g_string_free (out, FALSE);
}

/* the clitics' marks dropped and none left before ё */
static char *
finish (const char *word)
{
  g_autofree char *bare = ru_replace (word, "+", "");
  g_auto (GStrv) parts = g_strsplit (word, "-", -1), lowers = parts_of (bare);
  GString *out = g_string_new (NULL);

  for (guint i = 0; parts[i]; i++)
    {
      g_autofree char *part = *lowers[i] ? ru_replace (parts[i], "+ё", "ё") : ru_replace (parts[i], "+", "");
      g_autofree char *canonical = ru_replace (part, "+Ё", "Ё");
      g_string_append_printf (out, "%s%s", i ? "-" : "", *lowers[i] ? canonical : part);
    }
  return g_string_free (out, FALSE);
}

static gboolean
all_space (const char *s, gsize n)
{
  if (!n)
    return FALSE;
  for (const char *p = s; p < s + n; p = g_utf8_next_char (p))
    if (!g_unichar_isspace (g_utf8_get_char (p)))
      return FALSE;
  return TRUE;
}

char *
ru_stress_accentuate (RuStress *self, const char *text)
{
  g_autoptr (GArray) overrides = self->homographs ? resolve (self, text) : NULL;
  g_autoptr (GArray) starts = g_array_new (FALSE, FALSE, sizeof (int));
  g_autoptr (GArray) ends = g_array_new (FALSE, FALSE, sizeof (int));
  g_autoptr (GPtrArray) words = g_ptr_array_new_with_free_func (g_free);
  GMatchInfo *m = NULL;
  GString *out;
  gsize done = 0;

  g_regex_match (ru_rx (WORD_PATTERN), text, 0, &m);
  while (g_match_info_matches (m))
    {
      int start, end;
      g_autofree char *word = g_match_info_fetch (m, 0);
      g_match_info_fetch_pos (m, 0, &start, &end);
      g_autofree char *stressed = stress_span (self, start, word, overrides);
      g_ptr_array_add (words, finish (stressed));
      g_array_append_val (starts, start);
      g_array_append_val (ends, end);
      g_match_info_next (m, NULL);
    }
  g_match_info_free (m);
  /* не был: н+е был */
  for (guint i = 1; i < words->len; i++)
    {
      int s0 = g_array_index (starts, int, i - 1), e0 = g_array_index (ends, int, i - 1);
      int s1 = g_array_index (starts, int, i), e1 = g_array_index (ends, int, i);
      g_autofree char *before = fold (words->pdata[i - 1]), *after = fold (words->pdata[i]);
      if (g_str_equal (before, "не") && ru_in (ru_list ("STRESS_NE_STRESSED"), after) &&
          all_space (text + e0, s1 - e0) && !memchr (text + s0, '+', e0 - s0) && !memchr (text + s1, '+', e1 - s1))
        {
          g_autofree char *ne = g_strndup (text + s0, e0 - s0);
          g_free (words->pdata[i - 1]);
          words->pdata[i - 1] = apply (ne, "н+е");
          char *bare = ru_replace (words->pdata[i], "+", "");
          g_free (words->pdata[i]);
          words->pdata[i] = bare;
        }
    }
  out = g_string_new (NULL);
  for (guint i = 0; i < words->len; i++)
    {
      int start = g_array_index (starts, int, i);
      g_string_append_len (out, text + done, start - done);
      g_string_append (out, words->pdata[i]);
      done = g_array_index (ends, int, i);
    }
  g_string_append (out, text + done);
  for (guint i = 0; overrides && i < overrides->len; i++)
    g_free (g_array_index (overrides, Override, i).reading);
  return g_string_free (out, FALSE);
}

RuStress *
ru_stress_new (const char *dir, gboolean homographs)
{
  RuStress *self = g_new0 (RuStress, 1);

  self->dir = g_strdup (dir);
  self->homographs = homographs;
  g_mutex_init (&self->lock);
  self->predicted = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
  return self;
}

void
ru_stress_free (RuStress *self)
{
  if (!self)
    return;
  model_close (&self->accent);
  model_close (&self->omograph);
  g_clear_pointer (&self->lexicon, g_mapped_file_unref);
  g_clear_pointer (&self->vocab, g_hash_table_unref);
  g_hash_table_unref (self->predicted);
  g_mutex_clear (&self->lock);
  g_free (self->dir);
  g_free (self);
}
