/* dictate-voice: see dictate-voice.h. */
#include "dictate-voice.h"
#include "dictate-phonemes.h"

#include <gio/gio.h>
#include <json-glib/json-glib.h>
#include <onnxruntime/onnxruntime_c_api.h>
#include <math.h>
#include <string.h>

#define KOKORO_RATE     24000
#define KOKORO_STYLE    256     /* a style row's floats */
#define KOKORO_MAX      510     /* phonemes per run: the model's context less its two pads */

typedef enum { KOKORO, PIPER } Kind;

struct _DictateVoice {
  Kind kind;
  OrtSession *session;
  OrtMemoryInfo *memory;
  char **inputs;                /* the model's input names, NULL-terminated */
  gsize n_inputs;
  char *output;                 /* its (first) output's name */
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
  self->output = g_strdup (outputs[0]);
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
  if (!has_input (self, "input") || !has_input (self, "input_lengths") || !has_input (self, "scales"))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "%s does not take input, input_lengths and scales",
                   model);
      return FALSE;
    }
  return TRUE;
}

DictateVoice *
dictate_voice_load (const char *model, const char *voice, int threads, GError **error)
{
  g_autofree char *config = g_strconcat (model, ".json", NULL);
  DictateVoice *self = g_new0 (DictateVoice, 1);
  gboolean ok;

  self->ids = g_hash_table_new (NULL, NULL);
  self->kind = g_file_test (config, G_FILE_TEST_EXISTS) ? PIPER : KOKORO;
  ok = open_session (self, model, threads, error) &&
       (self->kind == PIPER ? load_piper (self, model, config, error) : load_kokoro (self, model, voice, error));
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
  g_free (self->output);
  g_free (self->language);
  g_hash_table_unref (self->ids);
  g_free (self->styles);
  g_free (self);
}

guint
dictate_voice_get_rate (DictateVoice *self)
{
  return self->rate;
}

/* ---------------- speaking ---------------- */

char *
dictate_voice_phonemes (DictateVoice *self, const char *text, GError **error)
{
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
      /* ^ _ p _ p _ … $ */
      g_array_append_val (ids, self->bos);
      g_array_append_val (ids, self->pad);
      for (gsize i = from; i < to; i++)
        {
          id = id_of (self, phonemes[i]);
          g_array_append_val (ids, id);
          g_array_append_val (ids, self->pad);
        }
      g_array_append_val (ids, self->eos);
    }
  return ids;
}

float *
dictate_voice_speak (DictateVoice *self, const char *text, double speed, gsize *n, GError **error)
{
  g_autofree char *phonemes = dictate_voice_phonemes (self, text, error);
  g_autofree gunichar *chars = NULL;
  glong count = 0;
  GArray *out;
  gsize from = 0;

  *n = 0;
  if (!phonemes)
    return NULL;
  chars = g_utf8_to_ucs4_fast (phonemes, -1, &count);
  out = g_array_new (FALSE, FALSE, sizeof (float));
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
