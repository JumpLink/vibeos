/* dictate-vad: see dictate-vad.h.
 *
 * Silero VAD (v5 and later) at 16 kHz reads 512 new samples with the 64 before them, its recurrent state (2×1×128)
 * and the rate, and gives the speech probability and the next state. */
#include "dictate-vad.h"

#include <gio/gio.h>
#include <onnxruntime/onnxruntime_c_api.h>
#include <string.h>

#define CONTEXT 64
#define STATE   (2 * 1 * 128)
#define RATE    16000

struct _DictateVad {
  OrtSession *session;
  OrtMemoryInfo *memory;
  float input[CONTEXT + DICTATE_VAD_FRAME];
  float state[STATE];
};

static const OrtApi *ort;
static OrtEnv *ort_env;
G_LOCK_DEFINE_STATIC (ort);

static gboolean
ok (OrtStatus *status, GError **error)
{
  if (!status)
    return TRUE;
  g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "onnxruntime: %s", ort->GetErrorMessage (status));
  ort->ReleaseStatus (status);
  return FALSE;
}

char *
dictate_vad_find (void)
{
  const char *const *system = g_get_system_data_dirs ();
  char *path = g_build_filename (g_get_user_data_dir (), "vibeos-dictate", "vad", "silero_vad.onnx", NULL);

  for (guint i = 0; !g_file_test (path, G_FILE_TEST_IS_REGULAR); i++)
    {
      g_free (path);
      if (!system[i])
        return NULL;
      path = g_build_filename (system[i], "vibeos-dictate", "vad", "silero_vad.onnx", NULL);
    }
  return path;
}

DictateVad *
dictate_vad_load (const char *model, GError **error)
{
  DictateVad *self;
  OrtSessionOptions *options = NULL;
  gboolean loaded;

  G_LOCK (ort);
  if (!ort_env)
    {
      const OrtApiBase *base = OrtGetApiBase ();
      ort = base ? base->GetApi (ORT_API_VERSION) : NULL;
      if (!ort || !ok (ort->CreateEnv (ORT_LOGGING_LEVEL_WARNING, "dictate-vad", &ort_env), error))
        {
          if (!ort)
            g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, "onnxruntime is too old");
          G_UNLOCK (ort);
          return NULL;
        }
    }
  G_UNLOCK (ort);

  self = g_new0 (DictateVad, 1);
  /* one thread: a frame is a few hundred microseconds of work, and the recogniser wants the others */
  loaded = ok (ort->CreateSessionOptions (&options), error) &&
           ok (ort->SetIntraOpNumThreads (options, 1), error) &&
           ok (ort->SetInterOpNumThreads (options, 1), error) &&
           ok (ort->SetSessionGraphOptimizationLevel (options, ORT_ENABLE_ALL), error) &&
           ok (ort->CreateSession (ort_env, model, options, &self->session), error) &&
           ok (ort->CreateCpuMemoryInfo (OrtArenaAllocator, OrtMemTypeDefault, &self->memory), error);
  if (options)
    ort->ReleaseSessionOptions (options);
  if (!loaded)
    {
      dictate_vad_free (self);
      return NULL;
    }
  return self;
}

void
dictate_vad_free (DictateVad *self)
{
  if (!self)
    return;
  if (self->session)
    ort->ReleaseSession (self->session);
  if (self->memory)
    ort->ReleaseMemoryInfo (self->memory);
  g_free (self);
}

void
dictate_vad_reset (DictateVad *self)
{
  memset (self->input, 0, sizeof self->input);
  memset (self->state, 0, sizeof self->state);
}

float
dictate_vad_frame (DictateVad *self, const float *frame)
{
  static const char *const in_names[] = { "input", "state", "sr" };
  static const char *const out_names[] = { "output", "stateN" };
  const int64_t input_shape[2] = { 1, CONTEXT + DICTATE_VAD_FRAME }, state_shape[3] = { 2, 1, 128 };
  int64_t rate = RATE;
  OrtValue *in[3] = { NULL }, *out[2] = { NULL };
  float probability = -1;
  g_autoptr (GError) error = NULL;

  /* the last frame's end is this one's context */
  memmove (self->input, self->input + DICTATE_VAD_FRAME, CONTEXT * sizeof (float));
  memcpy (self->input + CONTEXT, frame, DICTATE_VAD_FRAME * sizeof (float));
  if (ok (ort->CreateTensorWithDataAsOrtValue (self->memory, self->input, sizeof self->input, input_shape, 2,
                                               ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &in[0]), &error) &&
      ok (ort->CreateTensorWithDataAsOrtValue (self->memory, self->state, sizeof self->state, state_shape, 3,
                                               ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &in[1]), &error) &&
      ok (ort->CreateTensorWithDataAsOrtValue (self->memory, &rate, sizeof rate, NULL, 0,
                                               ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64, &in[2]), &error) &&
      ok (ort->Run (self->session, NULL, in_names, (const OrtValue *const *) in, 3, out_names, 2, out), &error))
    {
      float *p = NULL, *next = NULL;
      if (ok (ort->GetTensorMutableData (out[0], (void **) &p), &error) &&
          ok (ort->GetTensorMutableData (out[1], (void **) &next), &error))
        {
          probability = p[0];
          memcpy (self->state, next, sizeof self->state);
        }
    }
  if (error)
    g_warning ("vad: %s", error->message);
  for (guint i = 0; i < G_N_ELEMENTS (in); i++)
    if (in[i])
      ort->ReleaseValue (in[i]);
  for (guint i = 0; i < G_N_ELEMENTS (out); i++)
    if (out[i])
      ort->ReleaseValue (out[i]);
  return probability;
}
