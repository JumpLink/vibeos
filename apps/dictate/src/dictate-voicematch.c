/* dictate-voicematch: see dictate-voicematch.h.
 *
 * The models: speaker embedding models (ONNX, an [1, frames, 80] filterbank in, an embedding out), the *.onnx of
 * the first of vibeos-dictate/voice-match-models under the user's data directory and the system's that has any
 * (or of $DICTATE_VOICE_MATCH_MODELS), at most MODELS_MAX, in the order of their names. With more than one, a voice's
 * closeness is the mean of their cosines (two models together reject the owner less often than either alone). The
 * models are loaded at the first check or enrollment and let go of after IDLE_S without one; loaded again when their
 * files change (an update).
 *
 * The sample: for each model (by its file's sha256), the mean of the embeddings of every CHUNK_S piece of the
 * enrollment's recordings, as a unit vector; in a key file. A model whose sha256 is not in it has no sample: the
 * check is not made then (an update that changes the models asks for the sample again).
 *
 * The score: the cosine against the needed one for the speech's length (a longer one needs a little more: other
 * voices come closer the longer they talk), threshold + slope · ln (seconds, 1 to 6): 75 % at it, 55 % `lenient`
 * under it and 95 % `strict` over it, in a straight line between (and on beyond, to 0 and 100). For the two CAM++
 * models together, on LibriSpeech's read English, the defaults let about 1 % of voices of the owner's gender through
 * at 75 % (voice-match-strictness's balanced), about 3 % at 55 % (lenient) and about 0.1 % at 95 % (strict). All four
 * numbers can be set for the models in their directory's voice-match.ini ([voice-match] threshold, slope, lenient,
 * strict). */
#include "dictate-voicematch.h"

#include <errno.h>
#include <glib/gi18n.h>
#include <glib/gstdio.h>
#include <math.h>
#include <string.h>
#include <onnxruntime/onnxruntime_c_api.h>

#include "dictate-fbank.h"

G_DEFINE_QUARK (dictate-voice-match-error-quark, dictate_voice_match_error)

#define MODELS_MAX    3
#define CHUNK_S       5       /* enrollment's pieces: as long as the models were trained on */
#define VERIFY_MAX_S  8       /* of a turn, at most this much is looked at */
#define SPEECH_MIN_S  1.0     /* a check needs this much speech; below it, it is not reliable */
#define PHRASE_MIN_S  1.5     /* an enrollment's recording this much */
#define QUIET_DB      (-40.0) /* the loud part of a recording under this (dBFS) is too quiet */
#define RANGE_DB      10.0    /* and less than this above its quiet part, too noisy */
#define CLIPPED       0.001   /* or this share of its samples at full scale */
#define IDLE_S        120
#define THRESHOLD     0.35
#define SLOPE         0.04
#define LENIENT       0.05
#define STRICT        0.08
#define SAMPLE_VERSION 1

static const OrtApi *ort;
static OrtEnv *ort_env;

typedef struct {
  char *file;              /* its name */
  char *id;                /* its sha256 */
  OrtSession *session;
  char *input, *output;
} Model;

/* the models, loaded: all of it under `lock` */
static struct {
  GMutex lock;
  Model models[MODELS_MAX];
  guint count;
  char *files;             /* the model files, their sizes and times, as loaded */
  double threshold, slope, lenient, strict;
  gint64 used;
  guint idle;
} engine;

static gboolean
ort_ok (OrtStatus *status, GError **error)
{
  if (!status)
    return TRUE;
  g_set_error (error, DICTATE_VOICE_MATCH_ERROR, DICTATE_VOICE_MATCH_ERROR_UNAVAILABLE, "onnxruntime: %s",
               ort->GetErrorMessage (status));
  ort->ReleaseStatus (status);
  return FALSE;
}

/* ---------------- where things are ---------------- */

static char *
sample_path (void)
{
  return g_build_filename (g_get_user_data_dir (), "vibeos-dictate", "voice-match", NULL);
}

static char **
models_in (const char *dir)
{
  g_autoptr (GDir) d = g_dir_open (dir, 0, NULL);
  g_autoptr (GPtrArray) found = g_ptr_array_new ();
  const char *name;

  while (d && (name = g_dir_read_name (d)))
    if (g_str_has_suffix (name, ".onnx"))
      g_ptr_array_add (found, g_build_filename (dir, name, NULL));
  if (!found->len)
    return NULL;
  g_ptr_array_sort_values (found, (GCompareFunc) g_strcmp0);
  if (found->len > MODELS_MAX)
    {
      for (guint i = MODELS_MAX; i < found->len; i++)
        g_free (found->pdata[i]);
      g_ptr_array_set_size (found, MODELS_MAX);
    }
  g_ptr_array_add (found, NULL);
  return (char **) g_ptr_array_free (g_steal_pointer (&found), FALSE);
}

/* the model files, and the directory they are in */
static char **
models_find (char **dir)
{
  const char *set = g_getenv ("DICTATE_VOICE_MATCH_MODELS");
  const char *const *system = g_get_system_data_dirs ();
  char *at = set ? g_strdup (set) : g_build_filename (g_get_user_data_dir (), "vibeos-dictate", "voice-match-models",
                                                      NULL);
  char **found;

  for (guint i = 0; !(found = models_in (at)); i++)
    {
      g_free (at);
      if (set || !system[i])
        return NULL;
      at = g_build_filename (system[i], "vibeos-dictate", "voice-match-models", NULL);
    }
  if (dir)
    *dir = at;
  else
    g_free (at);
  return found;
}

/* ---------------- the models ---------------- */

static char *
file_sha256 (const char *path, GError **error)
{
  g_autoptr (GMappedFile) mapped = g_mapped_file_new (path, FALSE, error);

  if (!mapped)
    return NULL;
  return g_compute_checksum_for_data (G_CHECKSUM_SHA256, (const guchar *) g_mapped_file_get_contents (mapped),
                                      g_mapped_file_get_length (mapped));
}

static char *
session_name (OrtSession *session, gboolean input, GError **error)
{
  OrtAllocator *allocator;
  char *name = NULL, *copy;

  if (!ort_ok (ort->GetAllocatorWithDefaultOptions (&allocator), error) ||
      !ort_ok (input ? ort->SessionGetInputName (session, 0, allocator, &name)
                     : ort->SessionGetOutputName (session, 0, allocator, &name), error))
    return NULL;
  copy = g_strdup (name);
  ort_ok (ort->AllocatorFree (allocator, name), NULL);
  return copy;
}

static void
model_clear (Model *m)
{
  if (m->session)
    ort->ReleaseSession (m->session);
  g_free (m->file);
  g_free (m->id);
  g_free (m->input);
  g_free (m->output);
  memset (m, 0, sizeof *m);
}

static gboolean
model_open (Model *m, const char *path, GError **error)
{
  OrtSessionOptions *options = NULL;
  gboolean ok;

  m->file = g_path_get_basename (path);
  if (!(m->id = file_sha256 (path, error)))
    return FALSE;
  /* two threads: a check is about 40 ms of work a second of speech for each model, and the user is waiting */
  ok = ort_ok (ort->CreateSessionOptions (&options), error) &&
       ort_ok (ort->SetIntraOpNumThreads (options, 2), error) &&
       ort_ok (ort->SetInterOpNumThreads (options, 1), error) &&
       ort_ok (ort->SetSessionGraphOptimizationLevel (options, ORT_ENABLE_ALL), error) &&
       /* no arena: what a long recording needed is given back, not kept */
       ort_ok (ort->DisableCpuMemArena (options), error) &&
       ort_ok (ort->CreateSession (ort_env, path, options, &m->session), error) &&
       (m->input = session_name (m->session, TRUE, error)) &&
       (m->output = session_name (m->session, FALSE, error));
  if (options)
    ort->ReleaseSessionOptions (options);
  return ok;
}

static void
engine_unload (void)
{
  for (guint i = 0; i < engine.count; i++)
    model_clear (&engine.models[i]);
  engine.count = 0;
  g_clear_pointer (&engine.files, g_free);
}

/* the files, each with its size and time: what is loaded is still what is there */
static char *
files_signature (char **files)
{
  GString *s = g_string_new (NULL);

  for (guint i = 0; files[i]; i++)
    {
      GStatBuf st;
      if (g_stat (files[i], &st) == 0)
        g_string_append_printf (s, "%s %" G_GINT64_FORMAT " %" G_GINT64_FORMAT "\n", files[i], (gint64) st.st_size,
                                (gint64) st.st_mtime);
    }
  return g_string_free (s, FALSE);
}

static gboolean
engine_idle (gpointer data)
{
  gboolean keep;

  g_mutex_lock (&engine.lock);
  keep = engine.count && g_get_monotonic_time () - engine.used < IDLE_S * G_USEC_PER_SEC;
  if (!keep)
    {
      engine_unload ();
      engine.idle = 0;
    }
  g_mutex_unlock (&engine.lock);
  return keep;
}

/* with the lock held: the models loaded (if they are not) */
static gboolean
engine_load (GError **error)
{
  g_autofree char *dir = NULL, *ini = NULL, *signature = NULL;
  g_auto (GStrv) files = NULL;
  g_autoptr (GKeyFile) settings = g_key_file_new ();
  gint64 start = g_get_monotonic_time ();

  engine.used = start;
  if (!engine.idle)
    engine.idle = g_timeout_add_seconds (IDLE_S / 4, engine_idle, NULL);
  files = models_find (&dir);
  signature = files ? files_signature (files) : NULL;
  if (engine.count && !g_strcmp0 (signature, engine.files))
    return TRUE;
  engine_unload ();
  if (!ort_env)
    {
      const OrtApiBase *base = OrtGetApiBase ();
      ort = base ? base->GetApi (ORT_API_VERSION) : NULL;
      if (!ort)
        {
          g_set_error (error, DICTATE_VOICE_MATCH_ERROR, DICTATE_VOICE_MATCH_ERROR_UNAVAILABLE,
                       "onnxruntime %s has no API version %d", base ? base->GetVersionString () : "?",
                       ORT_API_VERSION);
          return FALSE;
        }
      if (!ort_ok (ort->CreateEnv (ORT_LOGGING_LEVEL_WARNING, "dictate-voicematch", &ort_env), error))
        return FALSE;
    }
  if (!files)
    {
      g_set_error_literal (error, DICTATE_VOICE_MATCH_ERROR, DICTATE_VOICE_MATCH_ERROR_UNAVAILABLE,
                           _("Voice Match is not installed"));
      return FALSE;
    }
  for (guint i = 0; files[i]; i++)
    if (!model_open (&engine.models[engine.count++], files[i], error))
      {
        engine_unload ();
        return FALSE;
      }
  engine.files = g_steal_pointer (&signature);
  ini = g_build_filename (dir, "voice-match.ini", NULL);
  engine.threshold = THRESHOLD;
  engine.slope = SLOPE;
  engine.lenient = LENIENT;
  engine.strict = STRICT;
  if (g_key_file_load_from_file (settings, ini, G_KEY_FILE_NONE, NULL))
    {
      static const char *const keys[] = { "threshold", "slope", "lenient", "strict" };
      double *values[] = { &engine.threshold, &engine.slope, &engine.lenient, &engine.strict };

      for (guint i = 0; i < G_N_ELEMENTS (keys); i++)
        {
          g_autoptr (GError) none = NULL;
          double v = g_key_file_get_double (settings, "voice-match", keys[i], &none);

          /* the two spans are more than nothing, or the scale would have no slope */
          if (!none && (i < 2 || v > 0))
            *values[i] = v;
        }
    }
  g_message ("voice match: loaded, %u of its models, in %.0f ms", engine.count,
             (g_get_monotonic_time () - start) / 1000.0);
  return TRUE;
}

/* with the lock held: the model's embedding of `feats`, as a unit vector */
static float *
embed (Model *m, const float *feats, gsize frames, gsize *dim, GError **error)
{
  OrtMemoryInfo *memory = NULL;
  OrtValue *in = NULL, *out = NULL;
  OrtTensorTypeAndShapeInfo *info = NULL;
  const int64_t shape[3] = { 1, (int64_t) frames, DICTATE_FBANK_BANDS };
  const char *in_names[1] = { m->input }, *out_names[1] = { m->output };
  float *data = NULL, *v = NULL;
  size_t count = 0;

  if (ort_ok (ort->CreateCpuMemoryInfo (OrtDeviceAllocator, OrtMemTypeDefault, &memory), error) &&
      ort_ok (ort->CreateTensorWithDataAsOrtValue (memory, (void *) feats, frames * DICTATE_FBANK_BANDS * sizeof *feats,
                                                   shape, 3, ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &in), error) &&
      ort_ok (ort->Run (m->session, NULL, in_names, (const OrtValue *const *) &in, 1, out_names, 1, &out), error) &&
      ort_ok (ort->GetTensorTypeAndShape (out, &info), error) &&
      ort_ok (ort->GetTensorShapeElementCount (info, &count), error) &&
      ort_ok (ort->GetTensorMutableData (out, (void **) &data), error))
    {
      double norm = 0;

      for (size_t i = 0; i < count; i++)
        norm += (double) data[i] * data[i];
      norm = sqrt (norm);
      if (count && norm > 0)
        {
          v = g_new (float, count);
          for (size_t i = 0; i < count; i++)
            v[i] = data[i] / norm;
          *dim = count;
        }
      else
        g_set_error_literal (error, DICTATE_VOICE_MATCH_ERROR, DICTATE_VOICE_MATCH_ERROR_UNAVAILABLE,
                             "the model gave no embedding");
    }
  if (info)
    ort->ReleaseTensorTypeAndShapeInfo (info);
  if (out)
    ort->ReleaseValue (out);
  if (in)
    ort->ReleaseValue (in);
  if (memory)
    ort->ReleaseMemoryInfo (memory);
  return v;
}

/* ---------------- the speech in a recording ---------------- */

typedef struct {
  gsize start, end;   /* samples: the speech, from its first loud stretch to its last loud frame */
  double loud, quiet; /* dBFS: its frames' 90th and 10th percentile */
  double clipped;     /* the share of samples at full scale */
} Speech;

static int
by_value (gconstpointer a, gconstpointer b)
{
  double x = *(const double *) a, y = *(const double *) b;

  return x < y ? -1 : x > y;
}

/* an energy gate: 25 ms frames every 10 ms within 35 dB of the loudest (and above -60 dBFS); the speech starts at
 * the first three in a row (not at a click), and both ends are padded by 50 ms */
static Speech
speech_of (const float *x, gsize n)
{
  Speech s = { 0, n, -120, -120, 0 };
  gsize frames = dictate_fbank_frames (n), at = 0, on = G_MAXSIZE, last = 0;
  g_autofree double *db = NULL, *sorted = NULL;
  double top = -120, gate;
  guint run = 0;

  for (gsize i = 0; i < n; i++)
    at += fabsf (x[i]) >= 0.999f;
  s.clipped = n ? (double) at / n : 0;
  if (!frames)
    {
      s.end = 0;
      return s;
    }
  db = g_new (double, frames);
  for (gsize f = 0; f < frames; f++)
    {
      double e = 0;
      for (guint i = 0; i < DICTATE_FBANK_FRAME; i++)
        e += (double) x[f * DICTATE_FBANK_HOP + i] * x[f * DICTATE_FBANK_HOP + i];
      db[f] = 10 * log10 (e / DICTATE_FBANK_FRAME + 1e-12);
      top = MAX (top, db[f]);
    }
  gate = MAX (top - 35, -60);
  for (gsize f = 0; f < frames; f++)
    {
      run = db[f] > gate ? run + 1 : 0;
      if (run == 3 && on == G_MAXSIZE)
        on = f - 2;
      if (db[f] > gate)
        last = f;
    }
  sorted = g_memdup2 (db, frames * sizeof *db);
  qsort (sorted, frames, sizeof *sorted, by_value);
  s.loud = sorted[(gsize) (0.9 * (frames - 1))];
  s.quiet = sorted[(gsize) (0.1 * (frames - 1))];
  if (on == G_MAXSIZE)
    {
      s.end = 0;
      return s;
    }
  s.start = on * DICTATE_FBANK_HOP > DICTATE_FBANK_RATE / 20 ? on * DICTATE_FBANK_HOP - DICTATE_FBANK_RATE / 20 : 0;
  s.end = MIN (n, last * DICTATE_FBANK_HOP + DICTATE_FBANK_FRAME + DICTATE_FBANK_RATE / 20);
  return s;
}

static double
seconds (const Speech *s)
{
  return s->end > s->start ? (double) (s->end - s->start) / DICTATE_FBANK_RATE : 0;
}

/* ---------------- the sample ---------------- */

static GKeyFile *
sample_load (void)
{
  g_autofree char *path = sample_path ();
  GKeyFile *file = g_key_file_new ();

  if (!g_key_file_load_from_file (file, path, G_KEY_FILE_NONE, NULL)
      || g_key_file_get_integer (file, "sample", "version", NULL) != SAMPLE_VERSION)
    {
      g_key_file_unref (file);
      return NULL;
    }
  return file;
}

/* the model's vector in the sample, NULL for none (or one of another length) */
static float *
sample_vector (GKeyFile *file, const char *id, gsize dim)
{
  g_autofree char *text = g_key_file_get_string (file, id, "embedding", NULL);
  gsize len = 0;
  guchar *data;

  if (!text)
    return NULL;
  data = g_base64_decode (text, &len);
  if (len != dim * sizeof (float))
    {
      g_free (data);
      return NULL;
    }
  return (float *) data;
}

gboolean
dictate_voice_match_ready (void)
{
  g_auto (GStrv) files = models_find (NULL);
  const OrtApiBase *base = OrtGetApiBase ();

  return files && base && base->GetApi (ORT_API_VERSION);
}

gboolean
dictate_voice_match_enrolled (void)
{
  g_autoptr (GKeyFile) file = sample_load ();

  return file != NULL;
}

guint
dictate_voice_match_phrases (void)
{
  g_autoptr (GKeyFile) file = sample_load ();

  return file ? (guint) MAX (g_key_file_get_integer (file, "sample", "phrases", NULL), 0) : 0;
}

gint64
dictate_voice_match_recorded (void)
{
  g_autoptr (GKeyFile) file = sample_load ();

  return file ? g_key_file_get_int64 (file, "sample", "recorded", NULL) : 0;
}

gboolean
dictate_voice_match_delete (GError **error)
{
  g_autofree char *path = sample_path ();

  if (g_unlink (path) < 0 && errno != ENOENT)
    {
      int saved = errno;
      g_set_error (error, G_IO_ERROR, g_io_error_from_errno (saved), _("The voice sample could not be removed: %s"),
                   g_strerror (saved));
      return FALSE;
    }
  return TRUE;
}

double
dictate_voice_match_needed (int strictness)
{
  return 55 + 0.4 * CLAMP (strictness, 0, 100);
}

/* ---------------- the check ---------------- */

double
dictate_voice_match_verify (GBytes *audio, GError **error)
{
  g_autoptr (GKeyFile) file = sample_load ();
  gsize n = audio ? g_bytes_get_size (audio) / sizeof (float) : 0, frames = 0, used = 0;
  const float *x = audio ? g_bytes_get_data (audio, NULL) : NULL;
  g_autofree float *feats = NULL;
  Speech speech;
  double close = 0, d, needed, span, score;
  gboolean ok = TRUE;

  if (!file)
    {
      g_set_error_literal (error, DICTATE_VOICE_MATCH_ERROR, DICTATE_VOICE_MATCH_ERROR_NOT_ENROLLED,
                           _("No voice sample"));
      return -1;
    }
  speech = speech_of (x, n);
  d = seconds (&speech);
  if (d < SPEECH_MIN_S)
    {
      g_set_error_literal (error, DICTATE_VOICE_MATCH_ERROR, DICTATE_VOICE_MATCH_ERROR_TOO_SHORT,
                           _("Too little speech to tell whose voice it is"));
      return -1;
    }
  speech.end = MIN (speech.end, speech.start + VERIFY_MAX_S * DICTATE_FBANK_RATE);
  d = seconds (&speech);
  feats = dictate_fbank (x + speech.start, speech.end - speech.start, TRUE, &frames);

  g_mutex_lock (&engine.lock);
  if (!engine_load (error))
    ok = FALSE;
  for (guint i = 0; ok && i < engine.count; i++)
    {
      gsize dim = 0;
      g_autofree float *v = embed (&engine.models[i], feats, frames, &dim, error);
      g_autofree float *owner = v ? sample_vector (file, engine.models[i].id, dim) : NULL;
      double cos = 0;

      if (!v)
        ok = FALSE;
      else if (!owner)
        {
          g_set_error_literal (error, DICTATE_VOICE_MATCH_ERROR, DICTATE_VOICE_MATCH_ERROR_UNAVAILABLE,
                               _("The voice sample was made for another Voice Match model: record it again"));
          ok = FALSE;
        }
      else
        {
          for (gsize k = 0; k < dim; k++)
            cos += (double) v[k] * owner[k];
          close += cos;
          used++;
        }
    }
  needed = engine.threshold + engine.slope * log (CLAMP (d, 1.0, 6.0));
  close = used ? close / used : 0;
  span = close < needed ? engine.lenient : engine.strict;
  g_mutex_unlock (&engine.lock);
  if (!ok || !used)
    return -1;
  score = 75 + 20 * (close - needed) / span;
  return CLAMP (score, 0, 100);
}

static void
verify_thread (GTask *task, gpointer source, gpointer data, GCancellable *cancellable)
{
  GError *error = NULL;
  double score = dictate_voice_match_verify (data, &error);

  if (error)
    g_task_return_error (task, error);
  else
    g_task_return_pointer (task, g_memdup2 (&score, sizeof score), g_free);
}

void
dictate_voice_match_verify_async (GBytes *audio, GCancellable *cancellable, GAsyncReadyCallback callback,
                                  gpointer data)
{
  g_autoptr (GTask) task = g_task_new (NULL, cancellable, callback, data);

  g_task_set_task_data (task, audio ? g_bytes_ref (audio) : g_bytes_new (NULL, 0), (GDestroyNotify) g_bytes_unref);
  g_task_run_in_thread (task, verify_thread);
}

double
dictate_voice_match_verify_finish (GAsyncResult *result, GError **error)
{
  g_autofree double *score = g_task_propagate_pointer (G_TASK (result), error);

  return score ? *score : -1;
}

/* ---------------- enrollment ---------------- */

/* what one model made of the recordings so far: the sum of their pieces' unit vectors */
typedef struct {
  char *id, *file;
  gsize dim;
  double *sum;
  guint pieces;
} Template;

struct _DictateVoiceEnroll {
  guint count;
  double seconds;
  Template models[MODELS_MAX];
  guint n_models;
};

/* one recording's result, made on a worker thread */
typedef struct {
  DictateVoicePhrase phrase;
  double seconds;
  guint n_models;
  char *id[MODELS_MAX], *file[MODELS_MAX];
  gsize dim[MODELS_MAX];
  double *sum[MODELS_MAX];
  guint pieces;
} Phrase;

static void
phrase_free (Phrase *p)
{
  for (guint i = 0; i < p->n_models; i++)
    {
      g_free (p->id[i]);
      g_free (p->file[i]);
      g_free (p->sum[i]);
    }
  g_free (p);
}

static void
add_thread (GTask *task, gpointer source, gpointer data, GCancellable *cancellable)
{
  GBytes *audio = data;
  gsize n = g_bytes_get_size (audio) / sizeof (float);
  const float *x = g_bytes_get_data (audio, NULL);
  Speech speech = speech_of (x, n);
  Phrase *p = g_new0 (Phrase, 1);
  GError *error = NULL;
  gsize pieces;

  p->seconds = seconds (&speech);
  /* quiet first: in a recording too quiet for the gate, no speech is found at all */
  if (speech.loud < QUIET_DB)
    p->phrase = DICTATE_VOICE_PHRASE_TOO_QUIET;
  else if (p->seconds < PHRASE_MIN_S)
    p->phrase = DICTATE_VOICE_PHRASE_TOO_SHORT;
  else if (speech.clipped > CLIPPED || speech.loud - speech.quiet < RANGE_DB)
    p->phrase = DICTATE_VOICE_PHRASE_TOO_NOISY;
  if (p->phrase != DICTATE_VOICE_PHRASE_OK)
    {
      g_task_return_pointer (task, p, (GDestroyNotify) phrase_free);
      return;
    }
  /* in pieces of about CHUNK_S, of equal length */
  pieces = (gsize) ceil (p->seconds / CHUNK_S);
  g_mutex_lock (&engine.lock);
  if (engine_load (&error))
    {
      p->n_models = engine.count;
      for (guint m = 0; m < engine.count && !error; m++)
        {
          p->id[m] = g_strdup (engine.models[m].id);
          p->file[m] = g_strdup (engine.models[m].file);
          for (gsize k = 0; k < pieces && !error; k++)
            {
              gsize length = speech.end - speech.start, a = speech.start + length * k / pieces;
              gsize b = speech.start + length * (k + 1) / pieces, frames = 0, dim = 0;
              g_autofree float *feats = dictate_fbank (x + a, b - a, TRUE, &frames);
              g_autofree float *v = feats ? embed (&engine.models[m], feats, frames, &dim, &error) : NULL;

              if (!v)
                break;
              if (!p->sum[m])
                {
                  p->sum[m] = g_new0 (double, dim);
                  p->dim[m] = dim;
                }
              for (gsize i = 0; i < MIN (dim, p->dim[m]); i++)
                p->sum[m][i] += v[i];
            }
        }
      p->pieces = pieces;
    }
  g_mutex_unlock (&engine.lock);
  if (error)
    {
      phrase_free (p);
      g_task_return_error (task, error);
      return;
    }
  g_task_return_pointer (task, p, (GDestroyNotify) phrase_free);
}

DictateVoiceEnroll *
dictate_voice_enroll_new (void)
{
  return g_new0 (DictateVoiceEnroll, 1);
}

void
dictate_voice_enroll_add_async (DictateVoiceEnroll *self, GBytes *audio, GCancellable *cancellable,
                                GAsyncReadyCallback callback, gpointer data)
{
  g_autoptr (GTask) task = g_task_new (NULL, cancellable, callback, data);

  g_task_set_task_data (task, audio ? g_bytes_ref (audio) : g_bytes_new (NULL, 0), (GDestroyNotify) g_bytes_unref);
  g_task_run_in_thread (task, add_thread);
}

DictateVoicePhrase
dictate_voice_enroll_add_finish (DictateVoiceEnroll *self, GAsyncResult *result, GError **error)
{
  Phrase *p = g_task_propagate_pointer (G_TASK (result), error);
  DictateVoicePhrase phrase;
  gboolean same = TRUE;

  if (!p)
    return DICTATE_VOICE_PHRASE_TOO_SHORT;
  phrase = p->phrase;
  if (phrase != DICTATE_VOICE_PHRASE_OK)
    {
      phrase_free (p);
      return phrase;
    }
  /* the models are the ones the recordings before were made with (they could change between two, with an update) */
  if (self->count)
    {
      same = p->n_models == self->n_models;
      for (guint m = 0; same && m < p->n_models; m++)
        same = !g_strcmp0 (p->id[m], self->models[m].id) && p->dim[m] == self->models[m].dim;
    }
  if (!same)
    {
      for (guint m = 0; m < self->n_models; m++)
        {
          g_free (self->models[m].id);
          g_free (self->models[m].file);
          g_free (self->models[m].sum);
        }
      memset (self->models, 0, sizeof self->models);
      self->count = 0;
      self->seconds = 0;
    }
  if (!self->count)
    {
      self->n_models = p->n_models;
      for (guint m = 0; m < p->n_models; m++)
        {
          self->models[m].id = g_strdup (p->id[m]);
          self->models[m].file = g_strdup (p->file[m]);
          self->models[m].dim = p->dim[m];
          self->models[m].sum = g_new0 (double, p->dim[m]);
        }
    }
  for (guint m = 0; m < p->n_models; m++)
    {
      for (gsize i = 0; i < p->dim[m]; i++)
        self->models[m].sum[i] += p->sum[m][i];
      self->models[m].pieces += p->pieces;
    }
  self->count++;
  self->seconds += p->seconds;
  phrase_free (p);
  return DICTATE_VOICE_PHRASE_OK;
}

guint
dictate_voice_enroll_count (DictateVoiceEnroll *self)
{
  return self->count;
}

gboolean
dictate_voice_enroll_save (DictateVoiceEnroll *self, GError **error)
{
  g_autoptr (GKeyFile) file = g_key_file_new ();
  g_autofree char *path = sample_path (), *dir = g_path_get_dirname (path), *text = NULL;
  g_autoptr (GPtrArray) ids = g_ptr_array_new ();
  gsize len = 0;

  if (self->count < DICTATE_VOICE_MATCH_PHRASES || !self->n_models)
    {
      g_set_error (error, DICTATE_VOICE_MATCH_ERROR, DICTATE_VOICE_MATCH_ERROR_TOO_SHORT,
                   _("A voice sample needs %d recordings"), DICTATE_VOICE_MATCH_PHRASES);
      return FALSE;
    }
  g_key_file_set_integer (file, "sample", "version", SAMPLE_VERSION);
  g_key_file_set_int64 (file, "sample", "recorded", g_get_real_time () / G_USEC_PER_SEC);
  g_key_file_set_integer (file, "sample", "phrases", self->count);
  g_key_file_set_double (file, "sample", "seconds", round (self->seconds * 10) / 10);
  for (guint m = 0; m < self->n_models; m++)
    {
      Template *t = &self->models[m];
      g_autofree float *v = g_new (float, t->dim);
      g_autofree char *encoded = NULL;
      double norm = 0;

      for (gsize i = 0; i < t->dim; i++)
        norm += t->sum[i] * t->sum[i];
      norm = sqrt (norm);
      for (gsize i = 0; i < t->dim; i++)
        v[i] = norm > 0 ? t->sum[i] / norm : 0;
      encoded = g_base64_encode ((const guchar *) v, t->dim * sizeof *v);
      g_key_file_set_string (file, t->id, "file", t->file);
      g_key_file_set_integer (file, t->id, "pieces", t->pieces);
      g_key_file_set_string (file, t->id, "embedding", encoded);
      g_ptr_array_add (ids, t->id);
    }
  g_key_file_set_string_list (file, "sample", "models", (const char *const *) ids->pdata, ids->len);
  text = g_key_file_to_data (file, &len, NULL);
  if (g_mkdir_with_parents (dir, 0700) < 0)
    {
      int saved = errno;
      g_set_error (error, G_IO_ERROR, g_io_error_from_errno (saved), _("The voice sample could not be saved: %s"),
                   g_strerror (saved));
      return FALSE;
    }
  return g_file_set_contents_full (path, text, len, G_FILE_SET_CONTENTS_CONSISTENT | G_FILE_SET_CONTENTS_DURABLE,
                                   0600, error);
}

void
dictate_voice_enroll_free (DictateVoiceEnroll *self)
{
  if (!self)
    return;
  for (guint m = 0; m < self->n_models; m++)
    {
      g_free (self->models[m].id);
      g_free (self->models[m].file);
      g_free (self->models[m].sum);
    }
  g_free (self);
}
