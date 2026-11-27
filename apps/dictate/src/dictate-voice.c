/* dictate-voice: see dictate-voice.h. */
#include "dictate-voice.h"
#include "dictate-phonemes.h"
#include "dictate-ru.h"

#include <gio/gio.h>
#include <json-glib/json-glib.h>
#include <onnxruntime/onnxruntime_c_api.h>
#include <math.h>
#include <string.h>

#define KOKORO_RATE     24000
#define KOKORO_STYLE    256     /* a style row's floats */
#define KOKORO_MAX      510     /* phonemes per run: the model's context less its two pads */
#define CLUSTER_MAX     4       /* phonemes in a Piper vowel cluster */
#define TINY_VOCA_MARGIN 12     /* tokens a tiny-voca utterance keeps free of its max_tokens, as its synthesizer does */
#define TINY_VOCA_PEAK  0.99    /* the loudest a tiny-voca text is let be: it is scaled down to it */

typedef enum { KOKORO, PIPER, TINY_VOCA } Kind;

/* Piper 1.5's vowel clusters: phonemes that follow each other (a, ɪ) read as one id, the id of their joined name
 * (aɪ) in the phoneme map, as Piper's phonemizer merges them (phonemize_espeak.py, _merge_known_vowel_clusters) */
typedef struct {
  gunichar phonemes[CLUSTER_MAX];
  guint length;
  gint64 id;
} Cluster;

struct _DictateVoice {
  Kind kind;
  OrtSession *session;
  OrtMemoryInfo *memory;
  char **inputs;                /* the model's input names, NULL-terminated */
  gsize n_inputs;
  char **outputs;               /* its output names, NULL-terminated */
  char *output;                 /* the first of them */
  char *language;               /* espeak-ng's voice for the phonemes */
  gboolean british;             /* Kokoro: misaki's British notation, not its American one */
  GHashTable *ids;              /* a phoneme (gunichar) → its id (gint64), as GINT_TO_POINTER (id + 1) */
  guint rate;
  /* Kokoro */
  float *styles;                /* rows of KOKORO_STYLE */
  gsize n_styles;
  /* Piper */
  gint64 pad, bos, eos;
  float noise_scale, length_scale, noise_w;
  gboolean has_sid;
  GArray *clusters;             /* Cluster, the longest first */
  /* tiny-voca */
  DictateRu *ru;                /* its Russian front end: the text in tokens and stress */
  gint64 male, female;          /* the speakers' indexes in its voices ("M", "F"), -1 where it has none */
  gint64 speaker;               /* the one speaking */
  guint max_tokens, max_frames;
  gboolean has_durations;       /* it says how long each token lasts: an utterance over max_frames is split */
};

static const OrtApi *ort;
static OrtEnv *ort_env;
G_LOCK_DEFINE_STATIC (engines);

static gboolean
ort_ok (OrtStatus *status, GError **error)
{
  if (!status)
    return TRUE;
  g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "onnxruntime: %s", ort->GetErrorMessage (status));
  ort->ReleaseStatus (status);
  return FALSE;
}

static gboolean
ort_init (GError **error)
{
  gboolean ok = TRUE;

  G_LOCK (engines);
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
        ok = ort_ok (ort->CreateEnv (ORT_LOGGING_LEVEL_WARNING, "dictate-voice", &ort_env), error);
    }
  G_UNLOCK (engines);
  return ok;
}

/* ---------------- loading ---------------- */

static char **
names (OrtSession *session, gboolean inputs, gsize *n, GError **error)
{
  OrtAllocator *allocator;
  size_t count = 0;
  char **list;

  if (!ort_ok (ort->GetAllocatorWithDefaultOptions (&allocator), error) ||
      !ort_ok (inputs ? ort->SessionGetInputCount (session, &count) : ort->SessionGetOutputCount (session, &count),
               error))
    return NULL;
  list = g_new0 (char *, count + 1);
  for (size_t i = 0; i < count; i++)
    {
      char *name = NULL;
      if (!ort_ok (inputs ? ort->SessionGetInputName (session, i, allocator, &name)
                          : ort->SessionGetOutputName (session, i, allocator, &name), error))
        {
          g_strfreev (list);
          return NULL;
        }
      list[i] = g_strdup (name);
      ort_ok (ort->AllocatorFree (allocator, name), NULL);
    }
  *n = count;
  return list;
}

static gboolean
has_input (DictateVoice *self, const char *name)
{
  return g_strv_contains ((const char *const *) self->inputs, name);
}

static gboolean
open_session (DictateVoice *self, const char *model, int threads, GError **error)
{
  OrtSessionOptions *options = NULL;
  g_auto (GStrv) outputs = NULL;
  gsize n_outputs = 0;
  gboolean ok;

  if (!ort_init (error) || !ort_ok (ort->CreateSessionOptions (&options), error))
    return FALSE;
  ok = (threads <= 0 || ort_ok (ort->SetIntraOpNumThreads (options, threads), error)) &&
       ort_ok (ort->SetInterOpNumThreads (options, 1), error) &&
       ort_ok (ort->SetSessionGraphOptimizationLevel (options, ORT_ENABLE_ALL), error) &&
       ort_ok (ort->CreateSession (ort_env, model, options, &self->session), error) &&
       ort_ok (ort->CreateCpuMemoryInfo (OrtArenaAllocator, OrtMemTypeDefault, &self->memory), error);
  ort->ReleaseSessionOptions (options);
  if (!ok)
    return FALSE;
  self->inputs = names (self->session, TRUE, &self->n_inputs, error);
  outputs = self->inputs ? names (self->session, FALSE, &n_outputs, error) : NULL;
  if (!outputs)
    return FALSE;
  if (n_outputs < 1)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "%s has no output", model);
      return FALSE;
    }
  self->outputs = g_steal_pointer (&outputs);
  self->output = self->outputs[0];
  return TRUE;
}

static JsonNode *
read_json (const char *path, GError **error)
{
  g_autoptr (JsonParser) parser = json_parser_new ();

  if (!json_parser_load_from_file (parser, path, error))
    return NULL;
  return json_node_copy (json_parser_get_root (parser));
}

static void
add_id (DictateVoice *self, const char *symbol, gint64 id)
{
  /* one character each; a symbol of several (none in these models) is not ours to read */
  if (g_utf8_strlen (symbol, -1) == 1)
    g_hash_table_insert (self->ids, GUINT_TO_POINTER (g_utf8_get_char (symbol)), GINT_TO_POINTER ((int) id + 1));
}

static gboolean
load_kokoro (DictateVoice *self, const char *model, const char *voice, GError **error)
{
  g_autofree char *dir = g_path_get_dirname (model);
  g_autofree char *tokenizer = g_build_filename (dir, "tokenizer.json", NULL);
  g_autofree char *voice_file = NULL;
  g_autoptr (JsonNode) root = NULL;
  JsonObject *vocab;
  GList *members;
  char *bytes = NULL;
  gsize length = 0;

  if (!voice || !*voice)
    voice = "af_heart";
  if (strchr (voice, '/') || strchr (voice, '.'))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "not a voice name: %s", voice);
      return FALSE;
    }
  root = read_json (tokenizer, error);
  if (!root)
    return FALSE;
  vocab = JSON_NODE_HOLDS_OBJECT (root) ? json_object_get_object_member (json_node_get_object (root), "model") : NULL;
  vocab = vocab && json_object_has_member (vocab, "vocab") ? json_object_get_object_member (vocab, "vocab") : NULL;
  if (!vocab)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "%s has no model.vocab", tokenizer);
      return FALSE;
    }
  members = json_object_get_members (vocab);
  for (GList *l = members; l; l = l->next)
    add_id (self, l->data, json_object_get_int_member (vocab, l->data));
  g_list_free (members);

  voice_file = g_strdup_printf ("%s/voices/%s.bin", dir, voice);
  if (!g_file_get_contents (voice_file, &bytes, &length, error))
    return FALSE;
  if (length == 0 || length % (KOKORO_STYLE * sizeof (float)) != 0)
    {
      g_free (bytes);
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "%s is not rows of %d floats", voice_file,
                   KOKORO_STYLE);
      return FALSE;
    }
  self->styles = (float *) bytes;
  self->n_styles = length / (KOKORO_STYLE * sizeof (float));
  self->british = voice[0] == 'b';
  /* Kokoro's languages by the voice's first letter, as its own pipeline picks them */
  switch (voice[0])
    {
    case 'b': self->language = g_strdup ("en-gb"); break;
    case 'e': self->language = g_strdup ("es"); break;
    case 'f': self->language = g_strdup ("fr-fr"); break;
    case 'h': self->language = g_strdup ("hi"); break;
    case 'i': self->language = g_strdup ("it"); break;
    case 'p': self->language = g_strdup ("pt-br"); break;
    default:  self->language = g_strdup ("en-us"); break;
    }
  self->rate = KOKORO_RATE;
  if (!has_input (self, "input_ids") || !has_input (self, "style") || !has_input (self, "speed"))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "%s does not take input_ids, style and speed", model);
      return FALSE;
    }
  return TRUE;
}

static gint
longer_first (gconstpointer a, gconstpointer b)
{
  return (int) ((const Cluster *) b)->length - (int) ((const Cluster *) a)->length;
}

/* the config's vowel_clusters ([["a", "ɪ"], …]: one phoneme each), those whose joined name has an id */
static void
read_clusters (DictateVoice *self, JsonObject *config, JsonObject *map)
{
  JsonArray *list = json_object_has_member (config, "vowel_clusters")
                    ? json_object_get_array_member (config, "vowel_clusters") : NULL;

  for (guint i = 0; list && i < json_array_get_length (list); i++)
    {
      JsonNode *node = json_array_get_element (list, i);
      JsonArray *parts = JSON_NODE_HOLDS_ARRAY (node) ? json_node_get_array (node) : NULL;
      g_autoptr (GString) name = g_string_new (NULL);
      Cluster cluster = { 0 };
      JsonArray *ids;
      guint n = parts ? json_array_get_length (parts) : 0;

      if (n < 2 || n > CLUSTER_MAX)
        continue;
      for (guint j = 0; j < n; j++)
        {
          JsonNode *part = json_array_get_element (parts, j);
          const char *phoneme = JSON_NODE_HOLDS_VALUE (part) && json_node_get_value_type (part) == G_TYPE_STRING
                                ? json_node_get_string (part) : NULL;

          if (!phoneme || g_utf8_strlen (phoneme, -1) != 1)
            break;
          cluster.phonemes[cluster.length++] = g_utf8_get_char (phoneme);
          g_string_append (name, phoneme);
        }
      ids = cluster.length == n && json_object_has_member (map, name->str)
            ? json_object_get_array_member (map, name->str) : NULL;
      if (!ids || json_array_get_length (ids) == 0)
        continue;
      cluster.id = json_array_get_int_element (ids, 0);
      if (!self->clusters)
        self->clusters = g_array_new (FALSE, FALSE, sizeof (Cluster));
      g_array_append_val (self->clusters, cluster);
    }
  if (self->clusters)
    g_array_sort (self->clusters, longer_first);
}

static gboolean
load_piper (DictateVoice *self, const char *model, const char *config, GError **error)
{
  g_autoptr (JsonNode) root = read_json (config, error);
  JsonObject *o, *map, *part;
  GList *members;

  if (!root)
    return FALSE;
  o = JSON_NODE_HOLDS_OBJECT (root) ? json_node_get_object (root) : NULL;
  map = o && json_object_has_member (o, "phoneme_id_map") ? json_object_get_object_member (o, "phoneme_id_map") : NULL;
  if (!map)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "%s has no phoneme_id_map", config);
      return FALSE;
    }
  members = json_object_get_members (map);
  for (GList *l = members; l; l = l->next)
    {
      JsonArray *ids = json_object_get_array_member (map, l->data);
      if (ids && json_array_get_length (ids) > 0)
        add_id (self, l->data, json_array_get_int_element (ids, 0));
    }
  g_list_free (members);
  self->pad = 0, self->bos = 1, self->eos = 2;
  if (g_hash_table_contains (self->ids, GUINT_TO_POINTER ('_')))
    self->pad = GPOINTER_TO_INT (g_hash_table_lookup (self->ids, GUINT_TO_POINTER ('_'))) - 1;
  if (g_hash_table_contains (self->ids, GUINT_TO_POINTER ('^')))
    self->bos = GPOINTER_TO_INT (g_hash_table_lookup (self->ids, GUINT_TO_POINTER ('^'))) - 1;
  if (g_hash_table_contains (self->ids, GUINT_TO_POINTER ('$')))
    self->eos = GPOINTER_TO_INT (g_hash_table_lookup (self->ids, GUINT_TO_POINTER ('$'))) - 1;

  part = json_object_has_member (o, "audio") ? json_object_get_object_member (o, "audio") : NULL;
  self->rate = part && json_object_has_member (part, "sample_rate") ? json_object_get_int_member (part, "sample_rate")
                                                                     : 22050;
  part = json_object_has_member (o, "espeak") ? json_object_get_object_member (o, "espeak") : NULL;
  self->language = g_strdup (part && json_object_has_member (part, "voice")
                             ? json_object_get_string_member (part, "voice") : "en-us");
  part = json_object_has_member (o, "inference") ? json_object_get_object_member (o, "inference") : NULL;
  self->noise_scale = part ? json_object_get_double_member_with_default (part, "noise_scale", 0.667) : 0.667;
  self->length_scale = part ? json_object_get_double_member_with_default (part, "length_scale", 1.0) : 1.0;
  self->noise_w = part ? json_object_get_double_member_with_default (part, "noise_w", 0.8) : 0.8;
  self->has_sid = has_input (self, "sid");
  read_clusters (self, o, map);
  if (!has_input (self, "input") || !has_input (self, "input_lengths") || !has_input (self, "scales"))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "%s does not take input, input_lengths and scales",
                   model);
      return FALSE;
    }
  return TRUE;
}

/* a string in a JSON array, NULL for anything else */
static const char *
string_at (JsonArray *array, guint i)
{
  JsonNode *node = json_array_get_element (array, i);

  return JSON_NODE_HOLDS_VALUE (node) && json_node_get_value_type (node) == G_TYPE_STRING
         ? json_node_get_string (node) : NULL;
}

static gboolean
load_tiny_voca (DictateVoice *self, const char *model, const char *config, GError **error)
{
  static const char *const INPUTS[] = { "tokens", "stress", "voice", "speed", "pitch_shift", "pitch_range", NULL };
  g_autoptr (JsonNode) root = read_json (config, error);
  g_autofree char *dir = g_path_get_dirname (model);
  JsonObject *o;
  JsonArray *symbols, *voices;
  const char *const *known;
  guint n;

  if (!root)
    return FALSE;
  o = JSON_NODE_HOLDS_OBJECT (root) ? json_node_get_object (root) : NULL;
  symbols = o && json_object_has_member (o, "symbols") ? json_object_get_array_member (o, "symbols") : NULL;
  voices = o && json_object_has_member (o, "voices") ? json_object_get_array_member (o, "voices") : NULL;
  /* its ids are the front end's: the same symbols in the same order */
  known = dictate_ru_symbols (&n);
  for (guint i = 0; symbols && i < n; i++)
    if (json_array_get_length (symbols) != n || g_strcmp0 (string_at (symbols, i), known[i]) != 0)
      symbols = NULL;
  if (!symbols || !voices || json_array_get_length (voices) == 0)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "%s does not list the symbols and voices of "
                   "tiny-voca-ru", config);
      return FALSE;
    }
  self->male = self->female = -1;
  for (guint i = 0; i < json_array_get_length (voices); i++)
    {
      const char *name = string_at (voices, i);
      if (!g_strcmp0 (name, "M"))
        self->male = i;
      else if (!g_strcmp0 (name, "F"))
        self->female = i;
    }
  self->speaker = self->male >= 0 ? self->male : MAX (self->female, 0);
  self->rate = json_object_get_int_member_with_default (o, "sample_rate", 24000);
  self->max_tokens = json_object_get_int_member_with_default (o, "max_tokens", 512);
  self->max_frames = json_object_get_int_member_with_default (o, "max_frames", 3000);
  self->language = g_strdup ("ru");
  for (guint i = 0; INPUTS[i]; i++)
    if (!has_input (self, INPUTS[i]))
      {
        g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "%s does not take %s", model, INPUTS[i]);
        return FALSE;
      }
  if (self->max_tokens <= TINY_VOCA_MARGIN || self->rate == 0 || !g_str_equal (self->output, "audio"))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "%s does not give audio as tiny-voca-ru does",
                   model);
      return FALSE;
    }
  self->has_durations = g_strv_contains ((const char *const *) self->outputs, "durations");
  /* the front end's dictionary and stress models are beside it, and loaded when first needed */
  self->ru = dictate_ru_new (dir, TRUE);
  return TRUE;
}

DictateVoice *
dictate_voice_load (const char *model, const char *voice, int threads, GError **error)
{
  g_autofree char *config = g_strconcat (model, ".json", NULL);
  g_autofree char *dir = g_path_get_dirname (model);
  g_autofree char *tiny_voca = g_build_filename (dir, "model.json", NULL);
  DictateVoice *self = g_new0 (DictateVoice, 1);
  gboolean ok;

  self->ids = g_hash_table_new (NULL, NULL);
  /* MODEL.json beside it: Piper's; model.json: tiny-voca's; else Kokoro's directory */
  self->kind = g_file_test (config, G_FILE_TEST_EXISTS) ? PIPER
             : g_file_test (tiny_voca, G_FILE_TEST_EXISTS) ? TINY_VOCA : KOKORO;
  ok = open_session (self, model, threads, error) &&
       (self->kind == PIPER ? load_piper (self, model, config, error)
        : self->kind == TINY_VOCA ? load_tiny_voca (self, model, tiny_voca, error)
        : load_kokoro (self, model, voice, error));
  if (!ok)
    {
      dictate_voice_free (self);
      return NULL;
    }
  return self;
}

void
dictate_voice_free (DictateVoice *self)
{
  if (!self)
    return;
  if (self->session)
    ort->ReleaseSession (self->session);
  if (self->memory)
    ort->ReleaseMemoryInfo (self->memory);
  g_strfreev (self->inputs);
  g_strfreev (self->outputs);
  g_free (self->language);
  dictate_ru_free (self->ru);
  g_hash_table_unref (self->ids);
  g_free (self->styles);
  if (self->clusters)
    g_array_unref (self->clusters);
  g_free (self);
}

guint
dictate_voice_get_rate (DictateVoice *self)
{
  return self->rate;
}

/* ---------------- speaking ---------------- */

static char *tiny_voca_phonemes (DictateVoice *self, const char *text);
static float *tiny_voca_speak (DictateVoice *self, const char *text, double speed, gsize *n, GError **error);

char *
dictate_voice_phonemes (DictateVoice *self, const char *text, GError **error)
{
  if (self->kind == TINY_VOCA)
    return tiny_voca_phonemes (self, text);
  g_autofree char *ipa = dictate_espeak_phonemes (self->language, text, self->kind == KOKORO, error);
  g_autoptr (GString) known = NULL;
  g_autofree char *notation = NULL;

  if (!ipa)
    return NULL;
  notation = self->kind == KOKORO ? dictate_kokoro_phonemes (ipa, self->british) : g_strdup (ipa);
  /* what the model has no id for is left out, as its own tokenizer does */
  known = g_string_new (NULL);
  for (const char *p = notation; *p; p = g_utf8_next_char (p))
    {
      gunichar c = g_utf8_get_char (p);
      if (g_hash_table_contains (self->ids, GUINT_TO_POINTER (c)))
        g_string_append_unichar (known, c);
    }
  return g_strstrip (g_string_free (g_steal_pointer (&known), FALSE));
}

static gint64
id_of (DictateVoice *self, gunichar c)
{
  return GPOINTER_TO_INT (g_hash_table_lookup (self->ids, GUINT_TO_POINTER (c))) - 1;
}

static gboolean
tensor (DictateVoice *self, void *data, gsize bytes, const int64_t *shape, gsize dims, ONNXTensorElementDataType type,
        OrtValue **value, GError **error)
{
  return ort_ok (ort->CreateTensorWithDataAsOrtValue (self->memory, data, bytes, shape, dims, type, value), error);
}

/* one run of the model over `ids`; the samples appended to `out` */
static gboolean
run (DictateVoice *self, GArray *ids, double speed, GArray *out, GError **error)
{
  const char *in_names[4];
  OrtValue *in[4] = { NULL };
  OrtValue *result = NULL;
  gsize n_in = 0;
  int64_t shape[2] = { 1, ids->len };
  gboolean ok = FALSE;
  /* kept alive until the run is over: the tensors only point at them */
  float style[KOKORO_STYLE], speed_f = speed, scales[3];
  int64_t length = ids->len, sid = 0;

  if (self->kind == KOKORO)
    {
      /* the style for this many phonemes (the ids less their two pads) */
      gsize row = MIN ((gsize) ids->len - 2, self->n_styles - 1);
      int64_t style_shape[2] = { 1, KOKORO_STYLE }, one = 1;
      memcpy (style, self->styles + row * KOKORO_STYLE, sizeof style);
      in_names[0] = "input_ids", in_names[1] = "style", in_names[2] = "speed";
      ok = tensor (self, ids->data, ids->len * sizeof (gint64), shape, 2, ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64,
                   &in[0], error) &&
           tensor (self, style, sizeof style, style_shape, 2, ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &in[1], error) &&
           tensor (self, &speed_f, sizeof speed_f, &one, 1, ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &in[2], error);
      n_in = 3;
    }
  else
    {
      int64_t one = 1, three = 3;
      /* Piper's length scale is the inverse of a speed */
      scales[0] = self->noise_scale, scales[1] = self->length_scale / MAX (speed, 0.1), scales[2] = self->noise_w;
      in_names[0] = "input", in_names[1] = "input_lengths", in_names[2] = "scales";
      ok = tensor (self, ids->data, ids->len * sizeof (gint64), shape, 2, ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64,
                   &in[0], error) &&
           tensor (self, &length, sizeof length, &one, 1, ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64, &in[1], error) &&
           tensor (self, scales, sizeof scales, &three, 1, ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &in[2], error);
      n_in = 3;
      if (ok && self->has_sid)
        {
          in_names[3] = "sid";
          ok = tensor (self, &sid, sizeof sid, &one, 1, ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64, &in[3], error);
          n_in = 4;
        }
    }
  if (ok)
    ok = ort_ok (ort->Run (self->session, NULL, in_names, (const OrtValue *const *) in, n_in,
                           (const char *const *) &self->output, 1, &result), error);
  if (ok)
    {
      OrtTensorTypeAndShapeInfo *info = NULL;
      size_t count = 0;
      float *samples = NULL;
      ok = ort_ok (ort->GetTensorTypeAndShape (result, &info), error) &&
           ort_ok (ort->GetTensorShapeElementCount (info, &count), error) &&
           ort_ok (ort->GetTensorMutableData (result, (void **) &samples), error);
      if (info)
        ort->ReleaseTensorTypeAndShapeInfo (info);
      if (ok)
        g_array_append_vals (out, samples, count);
    }
  for (gsize i = 0; i < G_N_ELEMENTS (in); i++)
    if (in[i])
      ort->ReleaseValue (in[i]);
  if (result)
    ort->ReleaseValue (result);
  return ok;
}

/* the ids of `phonemes[from..to)` as the model reads them */
static GArray *
ids_for (DictateVoice *self, const gunichar *phonemes, gsize from, gsize to)
{
  GArray *ids = g_array_new (FALSE, FALSE, sizeof (gint64));
  gint64 id;

  if (self->kind == KOKORO)
    {
      id = 0;   /* the pad, both ends */
      g_array_append_val (ids, id);
      for (gsize i = from; i < to; i++)
        {
          id = id_of (self, phonemes[i]);
          g_array_append_val (ids, id);
        }
      id = 0;
      g_array_append_val (ids, id);
    }
  else
    {
      /* ^ _ p _ p _ … $, a vowel cluster one p */
      g_array_append_val (ids, self->bos);
      g_array_append_val (ids, self->pad);
      for (gsize i = from; i < to;)
        {
          guint length = 1;

          id = id_of (self, phonemes[i]);
          for (guint c = 0; self->clusters && c < self->clusters->len; c++)
            {
              const Cluster *cluster = &g_array_index (self->clusters, Cluster, c);
              guint k = 0;

              if (i + cluster->length > to)
                continue;
              while (k < cluster->length && phonemes[i + k] == cluster->phonemes[k])
                k++;
              if (k == cluster->length)
                {
                  id = cluster->id, length = cluster->length;
                  break;
                }
            }
          g_array_append_val (ids, id);
          g_array_append_val (ids, self->pad);
          i += length;
        }
      g_array_append_val (ids, self->eos);
    }
  return ids;
}

float *
dictate_voice_speak (DictateVoice *self, const char *text, double speed, gsize *n, GError **error)
{
  g_autofree char *phonemes = self->kind == TINY_VOCA ? NULL : dictate_voice_phonemes (self, text, error);
  g_autofree gunichar *chars = NULL;
  glong count = 0;
  GArray *out;
  gsize from = 0;

  *n = 0;
  if (self->kind == TINY_VOCA)
    return tiny_voca_speak (self, text, speed, n, error);
  if (!phonemes)
    return NULL;
  chars = g_utf8_to_ucs4_fast (phonemes, -1, &count);
  /* room made at once: a text with nothing to say gives no samples, but not NULL, which is a failure */
  out = g_array_sized_new (FALSE, FALSE, sizeof (float), self->rate);
  /* Kokoro reads at most KOKORO_MAX phonemes a run: a longer text goes in pieces, cut after a sentence's or a
   * clause's mark, else at a space */
  while (from < (gsize) count)
    {
      gsize to = count;
      g_autoptr (GArray) ids = NULL;

      if (self->kind == KOKORO && to - from > KOKORO_MAX)
        {
          gsize cut = 0;
          for (gsize i = from + KOKORO_MAX; i > from && !cut; i--)
            if (strchr (".!?;:,", (int) chars[i - 1]) && chars[i - 1] < 128)
              cut = i;
          for (gsize i = from + KOKORO_MAX; i > from && !cut; i--)
            if (chars[i - 1] == ' ')
              cut = i;
          to = cut ? cut : from + KOKORO_MAX;
        }
      ids = ids_for (self, chars, from, to);
      if (!run (self, ids, speed, out, error))
        {
          g_array_free (out, TRUE);
          return NULL;
        }
      from = to;
      while (from < (gsize) count && chars[from] == ' ')
        from++;
    }
  *n = out->len;
  return (float *) g_array_free (out, FALSE);
}

/* ---------------- tiny-voca ---------------- */

/* the tokens of `text`'s sentences written out, for the log and the tests: a stressed vowel after ˈ, <sp> a space */
static char *
tiny_voca_phonemes (DictateVoice *self, const char *text)
{
  g_auto (GStrv) sentences = dictate_ru_sentences (text);
  GString *out = g_string_new (NULL);
  guint n;
  const char *const *symbols = dictate_ru_symbols (&n);

  for (char **sentence = sentences; *sentence; sentence++)
    {
      g_autoptr (GArray) ids = g_array_new (FALSE, FALSE, sizeof (gint64));
      g_autoptr (GArray) stress = g_array_new (FALSE, FALSE, sizeof (gint64));

      if (!dictate_ru_phonemize (self->ru, *sentence, ids, stress))
        continue;
      if (out->len)
        g_string_append_c (out, ' ');
      /* past <bos>, before <eos> */
      for (guint i = 1; i + 1 < ids->len; i++)
        {
          gint64 id = g_array_index (ids, gint64, i);
          const char *symbol = id >= 0 && id < n ? symbols[id] : "<unk>";

          if (g_str_equal (symbol, "<sp>"))
            g_string_append_c (out, ' ');
          else
            g_string_append_printf (out, "%s%s", g_array_index (stress, gint64, i) ? "ˈ" : "", symbol);
        }
    }
  return g_string_free (out, FALSE);
}

/* the frames an utterance lasts: the sum of its tokens' durations */
static gboolean
tiny_voca_frames (OrtValue *durations, gint64 *total, GError **error)
{
  OrtTensorTypeAndShapeInfo *info = NULL;
  ONNXTensorElementDataType type = ONNX_TENSOR_ELEMENT_DATA_TYPE_UNDEFINED;
  size_t count = 0;
  void *data = NULL;
  gboolean ok = ort_ok (ort->GetTensorTypeAndShape (durations, &info), error) &&
                ort_ok (ort->GetTensorElementType (info, &type), error) &&
                ort_ok (ort->GetTensorShapeElementCount (info, &count), error) &&
                ort_ok (ort->GetTensorMutableData (durations, &data), error);

  if (info)
    ort->ReleaseTensorTypeAndShapeInfo (info);
  *total = 0;
  for (size_t i = 0; ok && i < count; i++)
    switch (type)
      {
      case ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64: *total += ((const int64_t *) data)[i]; break;
      case ONNX_TENSOR_ELEMENT_DATA_TYPE_INT32: *total += ((const int32_t *) data)[i]; break;
      case ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT: *total += (gint64) ((const float *) data)[i]; break;
      default: ok = FALSE; break;
      }
  if (!ok && error && !*error)
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "the voice's durations are not numbers");
  return ok;
}

/* one utterance through tiny-voca, its samples appended to `out`; FALSE without an error (*too_long) when it would
 * last longer than the model's max_frames, and is to be read in parts */
static gboolean
tiny_voca_run (DictateVoice *self, GArray *ids, GArray *stress, double speed, GArray *out, gboolean *too_long,
               GError **error)
{
  static const char *const IN[] = { "tokens", "stress", "voice", "speed", "pitch_shift", "pitch_range" };
  static const char *const OUT[] = { "audio", "durations" };
  OrtValue *in[G_N_ELEMENTS (IN)] = { NULL }, *result[G_N_ELEMENTS (OUT)] = { NULL };
  int64_t shape[2] = { 1, ids->len }, one = 1;
  /* kept alive until the run is over: the tensors only point at them */
  int64_t voice = self->speaker;
  float speed_f = CLAMP (speed, 0.25, 4.0), pitch_shift = 0.0f, pitch_range = 1.0f;
  gsize n_out = self->has_durations ? 2 : 1;
  gint64 frames = 0;
  gboolean ok;

  *too_long = FALSE;
  ok = tensor (self, ids->data, ids->len * sizeof (gint64), shape, 2, ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64, &in[0],
               error) &&
       tensor (self, stress->data, stress->len * sizeof (gint64), shape, 2, ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64,
               &in[1], error) &&
       tensor (self, &voice, sizeof voice, &one, 1, ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64, &in[2], error) &&
       tensor (self, &speed_f, sizeof speed_f, &one, 1, ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &in[3], error) &&
       tensor (self, &pitch_shift, sizeof pitch_shift, &one, 1, ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &in[4], error) &&
       tensor (self, &pitch_range, sizeof pitch_range, &one, 1, ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &in[5], error) &&
       ort_ok (ort->Run (self->session, NULL, IN, (const OrtValue *const *) in, G_N_ELEMENTS (IN), OUT, n_out, result),
               error);
  if (ok && n_out == 2 && (ok = tiny_voca_frames (result[1], &frames, error)) && frames > self->max_frames)
    *too_long = TRUE, ok = FALSE;
  if (ok)
    {
      OrtTensorTypeAndShapeInfo *info = NULL;
      size_t count = 0;
      float *samples = NULL;

      ok = ort_ok (ort->GetTensorTypeAndShape (result[0], &info), error) &&
           ort_ok (ort->GetTensorShapeElementCount (info, &count), error) &&
           ort_ok (ort->GetTensorMutableData (result[0], (void **) &samples), error);
      if (info)
        ort->ReleaseTensorTypeAndShapeInfo (info);
      for (size_t i = 0; ok && i < count; i++)
        if (!isfinite (samples[i]))
          {
            g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "the voice gave samples that are not numbers");
            ok = FALSE;
          }
      if (ok)
        g_array_append_vals (out, samples, count);
    }
  for (gsize i = 0; i < G_N_ELEMENTS (in); i++)
    if (in[i])
      ort->ReleaseValue (in[i]);
  for (gsize i = 0; i < G_N_ELEMENTS (result); i++)
    if (result[i])
      ort->ReleaseValue (result[i]);
  return ok;
}

/* a sentence (or a part of one) spoken: in one run if the model reads it at once, else in its clauses or halves.
 * What has nothing to say is left out; one word too long for the model is too (tiny-voca's synthesizer stops there) */
static gboolean
tiny_voca_read (DictateVoice *self, const char *piece, double speed, GArray *out, GError **error)
{
  g_autoptr (GArray) ids = g_array_new (FALSE, FALSE, sizeof (gint64));
  g_autoptr (GArray) stress = g_array_new (FALSE, FALSE, sizeof (gint64));
  g_auto (GStrv) parts = NULL;
  gboolean too_long = FALSE;

  if (!dictate_ru_phonemize (self->ru, piece, ids, stress))
    return TRUE;
  if (ids->len <= self->max_tokens - TINY_VOCA_MARGIN)
    {
      if (tiny_voca_run (self, ids, stress, speed, out, &too_long, error))
        return TRUE;
      if (!too_long)
        return FALSE;
    }
  parts = dictate_ru_split_clause (piece);
  if (!parts)
    {
      g_debug ("voice: a word of %u tokens is too long to say", ids->len);
      return TRUE;
    }
  for (char **part = parts; *part; part++)
    if (!tiny_voca_read (self, *part, speed, out, error))
      return FALSE;
  return TRUE;
}

static float *
tiny_voca_speak (DictateVoice *self, const char *text, double speed, gsize *n, GError **error)
{
  g_auto (GStrv) sentences = dictate_ru_sentences (text);
  GArray *out = g_array_sized_new (FALSE, FALSE, sizeof (float), self->rate);
  float loudest = 0.0f;

  for (char **sentence = sentences; *sentence; sentence++)
    if (!tiny_voca_read (self, *sentence, speed, out, error))
      {
        g_array_free (out, TRUE);
        return NULL;
      }
  /* too loud, the whole text is scaled down to TINY_VOCA_PEAK, as by tiny-voca's limit_peak () */
  for (guint i = 0; i < out->len; i++)
    loudest = MAX (loudest, fabsf (g_array_index (out, float, i)));
  if (loudest > TINY_VOCA_PEAK)
    {
      float scale = (float) (TINY_VOCA_PEAK / loudest);
      for (guint i = 0; i < out->len; i++)
        g_array_index (out, float, i) *= scale;
    }
  *n = out->len;
  return (float *) g_array_free (out, FALSE);
}

gboolean
dictate_voice_set_gender (DictateVoice *self, const char *gender)
{
  gint64 speaker = self->kind != TINY_VOCA ? -1
                 : !g_strcmp0 (gender, "female") ? self->female : !g_strcmp0 (gender, "male") ? self->male : -1;

  if (speaker < 0)
    return FALSE;
  self->speaker = speaker;
  return TRUE;
}

/* ---------------- finding a voice ---------------- */

/* the languages tiny-voca speaks, in vibeos-dictate/voices/LANGUAGE/TINY_VOCA_DIR: a Piper voice also there is not
 * used for them */
static const char *const TINY_VOCA_LANGUAGES[] = { "ru", NULL };
#define TINY_VOCA_DIR   "tiny-voca"

gboolean
dictate_voice_has_genders (const char *language)
{
  return language && g_strv_contains (TINY_VOCA_LANGUAGES, language);
}

/* a tiny-voca voice whole in `dir`: the model, its symbols and its front end's dictionary and models */
static gboolean
tiny_voca_whole (const char *dir)
{
  static const char *const FILES[] = { "model.onnx", "model.json", "stress.txt.gz", "accent.onnx", "omograph.onnx",
                                       "omograph_vocab.txt.gz", NULL };

  for (guint i = 0; FILES[i]; i++)
    {
      g_autofree char *path = g_build_filename (dir, FILES[i], NULL);
      if (!g_file_test (path, G_FILE_TEST_IS_REGULAR))
        return FALSE;
    }
  return TRUE;
}

static gint
compare_names (gconstpointer a, gconstpointer b)
{
  return g_strcmp0 (*(char *const *) a, *(char *const *) b);
}

char *
dictate_voice_find (const char *language)
{
  g_autoptr (GPtrArray) dirs = g_ptr_array_new_with_free_func (g_free);
  const char *const *system = g_get_system_data_dirs ();

  if (!language || !*language || strchr (language, '/') || strstr (language, ".."))
    return NULL;
  g_ptr_array_add (dirs, g_build_filename (g_get_user_data_dir (), "vibeos-dictate", "voices", language, NULL));
  for (guint i = 0; system[i]; i++)
    g_ptr_array_add (dirs, g_build_filename (system[i], "vibeos-dictate", "voices", language, NULL));
  for (guint i = 0; i < dirs->len; i++)
    {
      g_autoptr (GDir) dir = NULL;
      g_autoptr (GPtrArray) names = g_ptr_array_new_with_free_func (g_free);
      const char *name;

      if (dictate_voice_has_genders (language))
        {
          g_autofree char *tiny_voca = g_build_filename (dirs->pdata[i], TINY_VOCA_DIR, NULL);
          if (tiny_voca_whole (tiny_voca))
            return g_build_filename (tiny_voca, "model.onnx", NULL);
          continue;
        }
      dir = g_dir_open (dirs->pdata[i], 0, NULL);
      while (dir && (name = g_dir_read_name (dir)))
        g_ptr_array_add (names, g_strdup (name));
      g_ptr_array_sort (names, compare_names);
      for (guint j = 0; j < names->len; j++)
        {
          g_autofree char *path = g_build_filename (dirs->pdata[i], names->pdata[j], NULL);
          g_autofree char *config = g_strconcat (path, ".json", NULL);
          g_autofree char *tokenizer = g_build_filename (path, "tokenizer.json", NULL);

          if (g_str_has_suffix (path, ".onnx") && g_file_test (path, G_FILE_TEST_IS_REGULAR) &&
              g_file_test (config, G_FILE_TEST_IS_REGULAR))
            return g_steal_pointer (&path);
          if (g_file_test (tokenizer, G_FILE_TEST_IS_REGULAR))
            {
              /* the smallest of its precisions that is there: the 8-bit one, else the full one */
              static const char *const files[] = { "model_quantized.onnx", "model.onnx", NULL };
              for (guint k = 0; files[k]; k++)
                {
                  g_autofree char *model = g_build_filename (path, files[k], NULL);
                  if (g_file_test (model, G_FILE_TEST_IS_REGULAR))
                    return g_steal_pointer (&model);
                }
            }
        }
    }
  return NULL;
}

/* ---------------- Agent's choice ---------------- */

#define SETTINGS_SCHEMA "vibeos.Dictate"

/* the settings dictate_voice_choose () reads */
static const char *const CHOICE_KEYS[] = { "talk-voice", "talk-voice-name", "talk-voice-gender", NULL };

/* `key`'s value, NULL where it is empty or the settings have no such key (an older vibeos-dictate's) */
static char *
setting (GSettings *settings, const char *key)
{
  g_autoptr (GSettingsSchema) schema = NULL;
  char *value;

  if (!settings)
    return NULL;
  g_object_get (settings, "settings-schema", &schema, NULL);
  if (!schema || !g_settings_schema_has_key (schema, key))
    return NULL;
  value = g_settings_get_string (settings, key);
  if (!*value)
    g_clear_pointer (&value, g_free);
  return value;
}

char *
dictate_voice_choose (GSettings *settings, const char *language, char **name, char **gender)
{
  g_autofree char *chosen = setting (settings, CHOICE_KEYS[0]);

  if (name)
    *name = setting (settings, CHOICE_KEYS[1]);
  if (gender)
    *gender = setting (settings, CHOICE_KEYS[2]);
  if (chosen && g_file_test (chosen, G_FILE_TEST_IS_REGULAR))
    return g_steal_pointer (&chosen);
  return dictate_voice_find (language);
}

GSettings *
dictate_voice_settings (void)
{
  GSettingsSchemaSource *source = g_settings_schema_source_get_default ();
  g_autoptr (GSettingsSchema) schema = source ? g_settings_schema_source_lookup (source, SETTINGS_SCHEMA, TRUE) : NULL;

  return schema ? g_settings_new_full (schema, NULL, NULL) : NULL;
}

gboolean
dictate_voice_is_choice_key (const char *key)
{
  return key && g_strv_contains (CHOICE_KEYS, key);
}
