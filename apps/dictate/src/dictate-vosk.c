/* Vosk (Kaldi) as a DictateRecogniser (dictate-recogniser.h).
 *
 * libvosk.so is Vosk's own build, with Kaldi, OpenFST and OpenBLAS inside. Its recogniser decodes the audio as it is
 * given, and says when a pause ended an utterance, which is then finished; the utterance being spoken is its partial
 * result. Both come as JSON ({"text": ...}, {"partial": ...}), in lower case and without punctuation. Its small
 * models need 0.07 to 0.19 s of work per second of speech on the reference phones. */
#include "dictate-recogniser.h"

#include <dlfcn.h>
#include <json-glib/json-glib.h>
#include <string.h>

#define PIECE   1600    /* samples given to the recogniser at a time (100 ms), so a pause is found where it was */

typedef struct VoskModel VoskModel;
typedef struct VoskRecognizer VoskRecognizer;

static struct {
  void           *library;
  void           (*set_log_level)     (int level);
  VoskModel *    (*model_new)         (const char *path);
  void           (*model_free)        (VoskModel *model);
  VoskRecognizer *(*recognizer_new)   (VoskModel *model, float rate);
  void           (*recognizer_free)   (VoskRecognizer *recognizer);
  int            (*accept_waveform_f) (VoskRecognizer *recognizer, const float *samples, int n);
  const char *   (*result)            (VoskRecognizer *recognizer);
  const char *   (*partial_result)    (VoskRecognizer *recognizer);
  const char *   (*final_result)      (VoskRecognizer *recognizer);
} vosk;
G_LOCK_DEFINE_STATIC (vosk);

struct _DictateRecogniser {
  VoskModel *model;
};

struct _DictateStream {
  VoskRecognizer *recognizer;
  float *piece;         /* PIECE samples at the scale Kaldi takes */
};

static gboolean
installed (void)
{
  return dictate_library_installed ("libvosk.so");
}

static gboolean
load_library (char **error)
{
  g_autofree char *path = dictate_library_path ("libvosk.so");
  void *library;

  G_LOCK (vosk);
  if (vosk.library)
    {
      G_UNLOCK (vosk);
      return TRUE;
    }
  library = dlopen (path, RTLD_NOW | RTLD_LOCAL);
  if (!library)
    {
      *error = g_strdup (dlerror ());
      G_UNLOCK (vosk);
      return FALSE;
    }
#define SYMBOL(field, name) \
  if (!(*(void **) &vosk.field = dlsym (library, name))) { *error = g_strdup_printf ("no %s", name); goto fail; }
  SYMBOL (set_log_level, "vosk_set_log_level");
  SYMBOL (model_new, "vosk_model_new");
  SYMBOL (model_free, "vosk_model_free");
  SYMBOL (recognizer_new, "vosk_recognizer_new");
  SYMBOL (recognizer_free, "vosk_recognizer_free");
  SYMBOL (accept_waveform_f, "vosk_recognizer_accept_waveform_f");
  SYMBOL (result, "vosk_recognizer_result");
  SYMBOL (partial_result, "vosk_recognizer_partial_result");
  SYMBOL (final_result, "vosk_recognizer_final_result");
#undef SYMBOL
  /* Kaldi logs each model it loads and every utterance to stderr: only its errors */
  vosk.set_log_level (-1);
  vosk.library = library;
  G_UNLOCK (vosk);
  return TRUE;

fail:
  dlclose (library);
  memset (&vosk, 0, sizeof vosk);
  G_UNLOCK (vosk);
  return FALSE;
}

static DictateRecogniser *
load (const DictateModel *model, char **error)
{
  DictateRecogniser *recogniser;
  VoskModel *loaded;

  if (!load_library (error))
    return NULL;
  loaded = vosk.model_new (model->dir);
  if (!loaded)
    {
      *error = g_strdup_printf ("%s: not loaded", model->dir);
      return NULL;
    }
  recogniser = g_new (DictateRecogniser, 1);
  recogniser->model = loaded;
  return recogniser;
}

static void
unload (DictateRecogniser *recogniser)
{
  vosk.model_free (recogniser->model);
  g_free (recogniser);
}

static DictateStream *
begin (DictateRecogniser *recogniser, char **error)
{
  VoskRecognizer *recognizer = vosk.recognizer_new (recogniser->model, DICTATE_RATE);
  DictateStream *self;

  if (!recognizer)
    {
      *error = g_strdup ("no recogniser");
      return NULL;
    }
  self = g_new (DictateStream, 1);
  self->recognizer = recognizer;
  self->piece = g_new (float, PIECE);
  return self;
}

/* `json`'s string `member`, trimmed; "" if it has none */
static char *
member (const char *json, const char *name)
{
  g_autoptr (JsonParser) parser = json_parser_new ();
  JsonNode *root;

  if (!json || !json_parser_load_from_data (parser, json, -1, NULL))
    return g_strdup ("");
  root = json_parser_get_root (parser);
  if (!root || !JSON_NODE_HOLDS_OBJECT (root))
    return g_strdup ("");
  return g_strstrip (g_strdup (json_object_get_string_member_with_default (json_node_get_object (root), name, "")));
}

static void
add (GPtrArray *finished, char *sentence)
{
  if (*sentence)
    g_ptr_array_add (finished, sentence);
  else
    g_free (sentence);
}

static void
hear (DictateStream *self, const float *samples, guint n, GPtrArray *finished, char **partial)
{
  for (guint at = 0; at < n; at += PIECE)
    {
      guint len = MIN (PIECE, n - at);

      /* Kaldi takes samples as 16-bit ones would be, -32768..32767 */
      for (guint i = 0; i < len; i++)
        self->piece[i] = samples[at + i] * 32768.0f;
      if (vosk.accept_waveform_f (self->recognizer, self->piece, len) == 1)
        add (finished, member (vosk.result (self->recognizer), "text"));
    }
  *partial = member (vosk.partial_result (self->recognizer), "partial");
}

static void
end (DictateStream *self, GPtrArray *finished)
{
  add (finished, member (vosk.final_result (self->recognizer), "text"));
  vosk.recognizer_free (self->recognizer);
  g_free (self->piece);
  g_free (self);
}

const DictateRecogniserClass dictate_vosk = {
  .name = "Vosk",
  .package = "vibeos-dictate-vosk",
  .installed = installed,
  .load = load,
  .unload = unload,
  .begin = begin,
  .hear = hear,
  .end = end,
};
