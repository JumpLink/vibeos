/* test-voicematch: Voice Match (dictate-voicematch.c) with the filterbank it feeds a model (dictate-fbank.c).
 *
 * The filterbank against Kaldi's (kaldi-native-fbank, set as torchaudio's kaldi.fbank, on the same made-up signal).
 * The rest with a stand-in model the test writes itself (ONNX, three operators: a band's peak over time, less the
 * mean of the peaks), on made-up voices: a low and a high tone in syllables, over a little noise. Nothing is
 * recorded and no real model is needed; the sample is made in the test's own data directory. */
#include <math.h>
#include <string.h>
#include <glib/gstdio.h>

#include "dictate-fbank.h"
#include "dictate-voicematch.h"

#define RATE 16000

/* ---------------- made-up sound ---------------- */

static double
noise (guint32 *state)
{
  *state = *state * 1664525u + 1013904223u;
  return (*state >> 8) / 16777216.0 * 2 - 1;
}

/* a voice: a tone at `pitch` Hz and its octave, in syllables (4 a second, a pause after each), over a little noise */
static GBytes *
voice (double pitch, double seconds, guint32 seed, double gain)
{
  gsize n = (gsize) (seconds * RATE);
  float *x = g_new (float, n);

  for (gsize i = 0; i < n; i++)
    {
      double t = (double) i / RATE, syllable = MAX (0, sin (2 * G_PI * 2 * t));
      x[i] = gain * (syllable * syllable * (0.3 * sin (2 * G_PI * pitch * t) + 0.1 * sin (2 * G_PI * 2 * pitch * t))
                     + 0.003 * noise (&seed));
    }
  return g_bytes_new_take (x, n * sizeof *x);
}

/* ---------------- the filterbank ---------------- */

static void
test_fbank (void)
{
  /* kaldi-native-fbank 1.23.3 (snip_edges, Povey's window, 20 Hz to 8 kHz, no dither) on this signal */
  static const struct { guint frame, band; double raw, normalised; } kaldi[] = {
    { 0, 0, -9.52802, 0.27617 },
    { 0, 9, -7.76350, -0.56673 },
    { 10, 40, -2.33727, -0.27721 },
    { 25, 79, 1.02149, -0.34406 },
    { 47, 9, -6.20580, 0.99097 },
    { 20, 14, 3.00923, -0.01503 },
  };
  float x[8000];
  guint32 state = 12345;
  gsize frames = 0;
  g_autofree float *raw = NULL, *normalised = NULL;

  for (guint i = 0; i < G_N_ELEMENTS (x); i++)
    x[i] = 0.25 * sin (2 * G_PI * 440 * i / RATE) + 0.05 * noise (&state);
  raw = dictate_fbank (x, G_N_ELEMENTS (x), FALSE, &frames);
  g_assert_cmpuint (frames, ==, 48);
  normalised = dictate_fbank (x, G_N_ELEMENTS (x), TRUE, &frames);
  for (guint i = 0; i < G_N_ELEMENTS (kaldi); i++)
    {
      g_assert_cmpfloat_with_epsilon (raw[kaldi[i].frame * DICTATE_FBANK_BANDS + kaldi[i].band], kaldi[i].raw, 1e-3);
      g_assert_cmpfloat_with_epsilon (normalised[kaldi[i].frame * DICTATE_FBANK_BANDS + kaldi[i].band],
                                      kaldi[i].normalised, 1e-3);
    }
  /* 440 Hz is band 14's */
  for (guint b = 0; b < DICTATE_FBANK_BANDS; b++)
    g_assert_cmpfloat (raw[20 * DICTATE_FBANK_BANDS + b], <=, raw[20 * DICTATE_FBANK_BANDS + 14]);
  g_assert_cmpuint (dictate_fbank_frames (399), ==, 0);
  g_assert_cmpuint (dictate_fbank_frames (400), ==, 1);
  g_assert_cmpuint (dictate_fbank_frames (559), ==, 1);
  g_assert_cmpuint (dictate_fbank_frames (560), ==, 2);
  g_assert_null (dictate_fbank (x, 399, TRUE, &frames));
  g_assert_cmpuint (frames, ==, 0);
}

/* ---------------- the stand-in model (protobuf, by hand) ---------------- */

static void
varint (GByteArray *b, guint64 v)
{
  do
    {
      guint8 byte = (v & 0x7f) | (v > 0x7f ? 0x80 : 0);
      g_byte_array_append (b, &byte, 1);
      v >>= 7;
    }
  while (v);
}

static void
field_int (GByteArray *b, guint field, gint64 v)
{
  varint (b, field << 3);
  varint (b, (guint64) v);
}

static void
field_bytes (GByteArray *b, guint field, const void *data, gsize len)
{
  varint (b, field << 3 | 2);
  varint (b, len);
  g_byte_array_append (b, data, len);
}

static void
field_str (GByteArray *b, guint field, const char *s)
{
  field_bytes (b, field, s, strlen (s));
}

/* takes `sub` */
static void
field_msg (GByteArray *b, guint field, GByteArray *sub)
{
  field_bytes (b, field, sub->data, sub->len);
  g_byte_array_unref (sub);
}

static GByteArray *
attr_int (const char *name, gint64 v)
{
  GByteArray *a = g_byte_array_new ();
  field_str (a, 1, name);
  field_int (a, 3, v);
  field_int (a, 20, 2);  /* INT */
  return a;
}

static GByteArray *
attr_ints (const char *name, gint64 v)
{
  GByteArray *a = g_byte_array_new ();
  field_str (a, 1, name);
  field_int (a, 8, v);
  field_int (a, 20, 7);  /* INTS */
  return a;
}

static GByteArray *
node (const char *op, const char *in1, const char *in2, const char *out, gboolean axes, gint64 keep)
{
  GByteArray *n = g_byte_array_new ();
  field_str (n, 1, in1);
  if (in2)
    field_str (n, 1, in2);
  field_str (n, 2, out);
  field_str (n, 4, op);
  if (axes)
    {
      field_msg (n, 5, attr_ints ("axes", 1));
      field_msg (n, 5, attr_int ("keepdims", keep));
    }
  return n;
}

/* a float tensor's value info, of `dims` (a number, or a name for any) */
static GByteArray *
value (const char *name, const char *const *dims)
{
  GByteArray *v = g_byte_array_new (), *type = g_byte_array_new (), *tensor = g_byte_array_new ();
  GByteArray *shape = g_byte_array_new ();

  for (guint i = 0; dims && dims[i]; i++)
    {
      GByteArray *dim = g_byte_array_new ();
      if (g_ascii_isdigit (dims[i][0]))
        field_int (dim, 1, g_ascii_strtoll (dims[i], NULL, 10));
      else
        field_str (dim, 2, dims[i]);
      field_msg (shape, 1, dim);
    }
  field_int (tensor, 1, 1);  /* FLOAT */
  field_msg (tensor, 2, shape);
  field_msg (type, 1, tensor);
  field_str (v, 1, name);
  field_msg (v, 2, type);
  return v;
}

/* "x" [1, frames, 80] in, "embedding" [1, 80] out: each band's peak, less the peaks' mean; `name` only changes the
 * file (another model, to the engine) */
static void
write_model (const char *path, const char *name)
{
  g_autoptr (GByteArray) model = g_byte_array_new ();
  GByteArray *graph = g_byte_array_new (), *opset = g_byte_array_new ();
  static const char *const in_dims[] = { "1", "frames", "80", NULL }, *const out_dims[] = { "1", "80", NULL };
  g_autoptr (GError) error = NULL;

  field_msg (graph, 1, node ("ReduceMax", "x", NULL, "peak", TRUE, 0));
  field_msg (graph, 1, node ("ReduceMean", "peak", NULL, "mean", TRUE, 1));
  field_msg (graph, 1, node ("Sub", "peak", "mean", "embedding", FALSE, 0));
  field_str (graph, 2, name);
  field_msg (graph, 11, value ("x", in_dims));
  field_msg (graph, 12, value ("embedding", out_dims));
  field_int (model, 1, 7);  /* IR version */
  field_str (opset, 1, "");
  field_int (opset, 2, 13);
  field_msg (model, 8, opset);
  field_msg (model, 7, graph);
  g_assert_true (g_file_set_contents (path, (const char *) model->data, model->len, &error));
}

/* ---------------- the engine ---------------- */

static char *
models_dir (const char *name, const char *const *models, const char *ini)
{
  char *dir = g_build_filename (g_get_user_cache_dir (), name, NULL);

  g_assert_cmpint (g_mkdir_with_parents (dir, 0700), ==, 0);
  for (guint i = 0; models[i]; i++)
    {
      g_autofree char *file = g_strdup_printf ("%s.onnx", models[i]);
      g_autofree char *path = g_build_filename (dir, file, NULL);
      write_model (path, models[i]);
    }
  if (ini)
    {
      g_autofree char *path = g_build_filename (dir, "voice-match.ini", NULL);
      g_assert_true (g_file_set_contents (path, ini, -1, NULL));
    }
  return dir;
}

static void
on_result (GObject *source, GAsyncResult *result, gpointer data)
{
  *(GAsyncResult **) data = g_object_ref (result);
}

/* an enrollment's recording added, waited for */
static DictateVoicePhrase
add (DictateVoiceEnroll *e, GBytes *audio)
{
  g_autoptr (GMainContext) context = g_main_context_new ();
  g_autoptr (GAsyncResult) result = NULL;
  g_autoptr (GError) error = NULL;
  DictateVoicePhrase phrase;

  g_main_context_push_thread_default (context);
  dictate_voice_enroll_add_async (e, audio, NULL, on_result, &result);
  while (!result)
    g_main_context_iteration (context, TRUE);
  phrase = dictate_voice_enroll_add_finish (e, result, &error);
  g_assert_no_error (error);
  g_main_context_pop_thread_default (context);
  return phrase;
}

static double
check (GBytes *audio, GError **error)
{
  g_autoptr (GMainContext) context = g_main_context_new ();
  g_autoptr (GAsyncResult) result = NULL;
  double score;

  g_main_context_push_thread_default (context);
  dictate_voice_match_verify_async (audio, NULL, on_result, &result);
  while (!result)
    g_main_context_iteration (context, TRUE);
  score = dictate_voice_match_verify_finish (result, error);
  g_main_context_pop_thread_default (context);
  return score;
}

/* a low voice and a high one: enrolled, checked, refused, the sample kept 0600 */
static void
test_enroll_and_check (void)
{
  static const char *const both[] = { "a-peaks", "b-peaks", NULL };
  g_autofree char *dir = models_dir ("models", both, "[voice-match]\nthreshold=0.5\nslope=0\n");
  g_autofree char *sample = g_build_filename (g_get_user_data_dir (), "vibeos-dictate", "voice-match", NULL);
  g_autoptr (DictateVoiceEnroll) e = NULL;
  g_autoptr (GError) error = NULL;
  GStatBuf st;
  double owner, other, again;

  g_setenv ("DICTATE_VOICE_MATCH_MODELS", dir, TRUE);
  g_assert_true (dictate_voice_match_ready ());
  g_assert_false (dictate_voice_match_enrolled ());
  g_assert_false (dictate_voice_match_stale ());
  g_assert_cmpuint (dictate_voice_match_phrases (), ==, 0);
  g_assert_cmpfloat (dictate_voice_match_verify (voice (220, 2, 1, 1), &error), ==, -1);
  g_assert_error (error, DICTATE_VOICE_MATCH_ERROR, DICTATE_VOICE_MATCH_ERROR_NOT_ENROLLED);
  g_clear_error (&error);

  e = dictate_voice_enroll_new ();
  g_assert_cmpint (add (e, voice (220, 1.0, 7, 1)), ==, DICTATE_VOICE_PHRASE_TOO_SHORT);
  g_assert_cmpint (add (e, voice (220, 3, 7, 0.002)), ==, DICTATE_VOICE_PHRASE_TOO_QUIET);
  g_assert_cmpint (add (e, voice (220, 3, 7, 6)), ==, DICTATE_VOICE_PHRASE_TOO_NOISY);
  g_assert_cmpuint (dictate_voice_enroll_count (e), ==, 0);
  for (guint32 seed = 1; seed <= DICTATE_VOICE_MATCH_PHRASES; seed++)
    {
      /* not enough of them yet */
      g_assert_false (dictate_voice_enroll_save (e, &error));
      g_assert_error (error, DICTATE_VOICE_MATCH_ERROR, DICTATE_VOICE_MATCH_ERROR_TOO_SHORT);
      g_clear_error (&error);
      g_assert_cmpint (add (e, voice (220, 2 + seed, seed * 101, 1)), ==, DICTATE_VOICE_PHRASE_OK);
    }
  g_assert_cmpuint (dictate_voice_enroll_count (e), ==, DICTATE_VOICE_MATCH_PHRASES);
  g_assert_true (dictate_voice_enroll_save (e, &error));
  g_assert_no_error (error);
  g_assert_true (dictate_voice_match_enrolled ());
  g_assert_false (dictate_voice_match_stale ());
  g_assert_cmpuint (dictate_voice_match_phrases (), ==, DICTATE_VOICE_MATCH_PHRASES);
  g_assert_cmpint (ABS (dictate_voice_match_recorded () - g_get_real_time () / G_USEC_PER_SEC), <=, 5);
  g_assert_cmpint (g_stat (sample, &st), ==, 0);
  g_assert_cmpint (st.st_mode & 0777, ==, 0600);
  {
    g_autofree char *parent = g_path_get_dirname (sample);
    g_assert_cmpint (g_stat (parent, &st), ==, 0);
    g_assert_cmpint (st.st_mode & 0777, ==, 0700);
  }

  owner = check (voice (220, 2, 999, 1), &error);
  g_assert_no_error (error);
  other = check (voice (1800, 2, 999, 1), &error);
  g_assert_no_error (error);
  again = dictate_voice_match_verify (voice (220, 1.5, 4242, 0.5), &error);
  g_assert_no_error (error);
  g_test_message ("owner %.0f %%, again %.0f %%, the other voice %.0f %%", owner, again, other);
  g_assert_cmpfloat (owner, >=, dictate_voice_match_needed (100));
  g_assert_cmpfloat (again, >=, dictate_voice_match_needed (50));
  g_assert_cmpfloat (other, <, dictate_voice_match_needed (0));

  /* too little to tell */
  g_assert_cmpfloat (dictate_voice_match_verify (voice (220, 0.6, 5, 1), &error), ==, -1);
  g_assert_error (error, DICTATE_VOICE_MATCH_ERROR, DICTATE_VOICE_MATCH_ERROR_TOO_SHORT);
  g_clear_error (&error);
  g_assert_cmpfloat (dictate_voice_match_verify (NULL, &error), ==, -1);
  g_assert_error (error, DICTATE_VOICE_MATCH_ERROR, DICTATE_VOICE_MATCH_ERROR_TOO_SHORT);
  g_clear_error (&error);

  /* another model in their place (an update), or none: stale, not checked, and the sample has to be made again */
  {
    static const char *const other_model[] = { "c-peaks", NULL };
    g_autofree char *changed = models_dir ("models-new", other_model, NULL);

    g_autofree char *none = g_build_filename (g_get_user_cache_dir (), "models-none", NULL);

    g_setenv ("DICTATE_VOICE_MATCH_MODELS", changed, TRUE);
    g_assert_true (dictate_voice_match_stale ());
    g_assert_cmpfloat (dictate_voice_match_verify (voice (220, 2, 999, 1), &error), ==, -1);
    g_assert_error (error, DICTATE_VOICE_MATCH_ERROR, DICTATE_VOICE_MATCH_ERROR_UNAVAILABLE);
    g_clear_error (&error);
    /* and none installed any more */
    g_assert_cmpint (g_mkdir_with_parents (none, 0700), ==, 0);
    g_setenv ("DICTATE_VOICE_MATCH_MODELS", none, TRUE);
    g_assert_true (dictate_voice_match_stale ());
    g_assert_cmpfloat (dictate_voice_match_verify (voice (220, 2, 999, 1), &error), ==, -1);
    g_assert_error (error, DICTATE_VOICE_MATCH_ERROR, DICTATE_VOICE_MATCH_ERROR_UNAVAILABLE);
    g_clear_error (&error);
    g_setenv ("DICTATE_VOICE_MATCH_MODELS", dir, TRUE);
    g_assert_false (dictate_voice_match_stale ());
  }
  g_assert_cmpfloat (dictate_voice_match_verify (voice (220, 2, 999, 1), &error), ==, owner);
  g_assert_no_error (error);

  /* removed */
  g_assert_true (dictate_voice_match_delete (&error));
  g_assert_false (dictate_voice_match_enrolled ());
  g_assert_false (dictate_voice_match_stale ());
  g_assert_false (g_file_test (sample, G_FILE_TEST_EXISTS));
  g_assert_true (dictate_voice_match_delete (&error));
  g_assert_no_error (error);
}

/* no models: not ready, nothing asked for; an enrollment's recording cannot be taken */
static void
test_not_installed (void)
{
  g_autofree char *dir = g_build_filename (g_get_user_cache_dir (), "no-models", NULL);
  g_autoptr (DictateVoiceEnroll) e = dictate_voice_enroll_new ();
  g_autoptr (GMainContext) context = g_main_context_new ();
  g_autoptr (GAsyncResult) result = NULL;
  g_autoptr (GError) error = NULL;

  g_assert_cmpint (g_mkdir_with_parents (dir, 0700), ==, 0);
  g_setenv ("DICTATE_VOICE_MATCH_MODELS", dir, TRUE);
  g_assert_false (dictate_voice_match_ready ());
  g_main_context_push_thread_default (context);
  dictate_voice_enroll_add_async (e, voice (220, 3, 1, 1), NULL, on_result, &result);
  while (!result)
    g_main_context_iteration (context, TRUE);
  dictate_voice_enroll_add_finish (e, result, &error);
  g_assert_error (error, DICTATE_VOICE_MATCH_ERROR, DICTATE_VOICE_MATCH_ERROR_UNAVAILABLE);
  g_main_context_pop_thread_default (context);
  g_assert_cmpuint (dictate_voice_enroll_count (e), ==, 0);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, G_TEST_OPTION_ISOLATE_DIRS, NULL);
  g_test_add_func ("/voicematch/fbank", test_fbank);
  g_test_add_func ("/voicematch/enroll-and-check", test_enroll_and_check);
  g_test_add_func ("/voicematch/not-installed", test_not_installed);
  return g_test_run ();
}
