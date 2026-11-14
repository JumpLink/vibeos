/* DictateEngine: see dictate-engine.h.
 *
 * Three threads take part. GStreamer's streaming thread hands each piece of microphone audio to on_sample, which
 * only measures its loudness and queues it, so the microphone never waits for the recogniser. The worker (work)
 * gives the queued audio to Moonshine and asks it for the transcript every UPDATE_MS; a transcript update takes up
 * to 0.7 s on a POCO X3 with the tiny model, during which the audio just queues up. What it hears goes to the main
 * thread (post), where the signals are emitted.
 *
 * The library: libmoonshine.so (with the ONNX Runtime it brings, libonnxruntime.so.1, next to it) from MOONSHINE_DIR,
 * else from the system's library path. The models: a directory per model, named as Moonshine's downloads are
 * (tiny-streaming-en, small-streaming-de, ...), in neo-dictate/models under the user's data directory or one of the
 * system's. The one used is the smallest streaming model for the user's language, else for English: the tiny model
 * keeps up with speech on both reference phones (0.22 and 0.55 s of work per second of speech), the small one only
 * on the faster. */
#include "dictate-engine.h"
#include "moonshine-api.h"

#include <dlfcn.h>
#include <glib/gi18n.h>
#include <gst/app/gstappsink.h>
#include <gst/gst.h>
#include <math.h>
#include <string.h>

#define RATE            16000
#define UPDATE_MS       300     /* between transcript updates while listening */
#define QUIET_DB        -55.0   /* loudness: level 0 at or under this (a quiet room is -50 on the phones' mics) */
#define LOUD_DB         -20.0   /* ... 1 at or over this (speech close to the phone) */

/* the sizes in the order they are tried, with their architectures */
static const struct { const char *name; uint32_t arch; } MODELS[] = {
  { "tiny-streaming", MOONSHINE_MODEL_ARCH_TINY_STREAMING },
  { "small-streaming", MOONSHINE_MODEL_ARCH_SMALL_STREAMING },
  { "base-streaming", MOONSHINE_MODEL_ARCH_BASE_STREAMING },
  { "medium-streaming", MOONSHINE_MODEL_ARCH_MEDIUM_STREAMING },
};

typedef struct {
  void                     *library;
  MoonshineGetVersion       get_version;
  MoonshineErrorToString    error_to_string;
  MoonshineLoadFromFiles    load;
  MoonshineFreeTranscriber  free_transcriber;
  MoonshineCreateStream     create_stream;
  MoonshineStreamCall       free_stream, start_stream, stop_stream;
  MoonshineAddAudio         add_audio;
  MoonshineTranscribeStream transcribe;
} Moonshine;

struct _DictateEngine
{
  GObject parent;
  DictateState state;

  /* the worker's: loaded on the first start, kept from then on */
  Moonshine ms;
  int32_t transcriber;          /* < 0: not loaded */

  GstElement *pipeline;         /* while the microphone is open */
  guint bus_watch;
  GThread *worker;              /* from start until the worker posted the idle state */

  /* between the streaming thread, the worker and the main thread */
  GMutex lock;
  GCond wake;
  GArray *queued;               /* float samples the recogniser has not had */
  gboolean stop;                /* the worker finishes the stream and ends */
  int level;                    /* atomic: dictate_engine_get_level × 1e6 */
};

enum { SIGNAL_TEXT, SIGNAL_STATE_CHANGED, SIGNAL_ERROR, N_SIGNALS };
static guint signals[N_SIGNALS];

G_DEFINE_FINAL_TYPE (DictateEngine, dictate_engine, G_TYPE_OBJECT)

/* --- where the library and the model are */

static char *
library_path (void)
{
  char *path = g_build_filename (MOONSHINE_DIR, "libmoonshine.so", NULL);

  if (g_file_test (path, G_FILE_TEST_EXISTS))
    return path;
  g_free (path);
  return g_strdup ("libmoonshine.so");
}

static gboolean
library_installed (void)
{
  g_autofree char *path = library_path ();
  void *library;

  if (g_path_is_absolute (path))
    return TRUE;
  /* in the system's path, if anywhere: there is nothing to look for but loading it */
  library = dlopen (path, RTLD_LAZY | RTLD_LOCAL);
  if (library)
    dlclose (library);
  return library != NULL;
}

static gboolean
is_model (const char *dir)
{
  static const char *const FILES[] = { "streaming_config.json", "tokenizer.bin", "encoder.ort", "decoder_kv.ort" };

  for (guint i = 0; i < G_N_ELEMENTS (FILES); i++)
    {
      g_autofree char *path = g_build_filename (dir, FILES[i], NULL);

      if (!g_file_test (path, G_FILE_TEST_IS_REGULAR))
        return FALSE;
    }
  return TRUE;
}

/* the model's directory and its architecture, or NULL */
static char *
find_model (uint32_t *arch)
{
  const char *const *languages = g_get_language_names ();
  g_autoptr (GPtrArray) dirs = g_ptr_array_new ();
  char lang[3] = "en";

  /* the first of the user's languages with a code: "de_DE.UTF-8" is de */
  for (guint i = 0; languages[i]; i++)
    if (g_ascii_isalpha (languages[i][0]) && g_ascii_isalpha (languages[i][1]) &&
        (languages[i][2] == '\0' || languages[i][2] == '_' || languages[i][2] == '.' || languages[i][2] == '@'))
      {
        lang[0] = g_ascii_tolower (languages[i][0]);
        lang[1] = g_ascii_tolower (languages[i][1]);
        break;
      }

  g_ptr_array_add (dirs, (gpointer) g_get_user_data_dir ());
  for (const char *const *d = g_get_system_data_dirs (); *d; d++)
    g_ptr_array_add (dirs, (gpointer) *d);

  for (int pass = 0; pass < 2; pass++)
    {
      const char *code = pass == 0 ? lang : "en";

      if (pass == 1 && g_str_equal (lang, "en"))
        break;
      for (guint m = 0; m < G_N_ELEMENTS (MODELS); m++)
        for (guint d = 0; d < dirs->len; d++)
          {
            g_autofree char *name = g_strdup_printf ("%s-%s", MODELS[m].name, code);
            char *dir = g_build_filename (g_ptr_array_index (dirs, d), "neo-dictate", "models", name, NULL);

            if (is_model (dir))
              {
                *arch = MODELS[m].arch;
                return dir;
              }
            g_free (dir);
          }
    }
  return NULL;
}

gboolean
dictate_engine_available (DictateEngine *self, char **reason)
{
  uint32_t arch;
  g_autofree char *model = NULL;

  if (!library_installed ())
    {
      *reason = g_strdup (_("Speech recognition is not installed. Install the neo-dictate-moonshine package."));
      return FALSE;
    }
  model = find_model (&arch);
  if (!model)
    {
      *reason = g_strdup (_("There is no speech model. Install the neo-dictate-moonshine package."));
      return FALSE;
    }
  return TRUE;
}

/* --- to the main thread */

typedef enum { POST_TEXT, POST_STATE, POST_ERROR } PostKind;

typedef struct {
  DictateEngine *self;
  PostKind kind;
  char **finished;
  char *text;           /* the partial sentence, or the error */
  DictateState state;
} Post;

static void stop_capture (DictateEngine *self);

static void
post_free (gpointer data)
{
  Post *post = data;

  g_object_unref (post->self);
  g_strfreev (post->finished);
  g_free (post->text);
  g_free (post);
}

static gboolean
deliver (gpointer data)
{
  Post *post = data;
  DictateEngine *self = post->self;

  switch (post->kind)
    {
    case POST_TEXT:
      g_signal_emit (self, signals[SIGNAL_TEXT], 0, post->finished, post->text);
      break;
    case POST_ERROR:
      stop_capture (self);
      g_signal_emit (self, signals[SIGNAL_ERROR], 0, post->text);
      break;
    case POST_STATE:
      if (post->state == DICTATE_IDLE)
        {
          /* the worker's last word: it is ending */
          stop_capture (self);
          g_clear_pointer (&self->worker, g_thread_join);
        }
      else if (self->state == DICTATE_STOPPING)
        break;   /* listening, but stopped before the model was in: stopping it is */
      self->state = post->state;
      g_signal_emit (self, signals[SIGNAL_STATE_CHANGED], 0);
      break;
    }
  return G_SOURCE_REMOVE;
}

static void
post (DictateEngine *self, PostKind kind, char **finished, char *text, DictateState state)
{
  Post *post = g_new0 (Post, 1);

  post->self = g_object_ref (self);
  post->kind = kind;
  post->finished = finished;
  post->text = text;
  post->state = state;
  g_main_context_invoke_full (NULL, G_PRIORITY_DEFAULT, deliver, post, post_free);
}

/* --- the worker */

static gboolean
load_library (Moonshine *ms, char **error)
{
  g_autofree char *path = library_path ();
  int32_t version;

  ms->library = dlopen (path, RTLD_NOW | RTLD_LOCAL);
  if (!ms->library)
    {
      *error = g_strdup (dlerror ());
      return FALSE;
    }
#define SYMBOL(field, name) \
  if (!(*(void **) &ms->field = dlsym (ms->library, name))) { *error = g_strdup_printf ("no %s", name); goto fail; }
  SYMBOL (get_version, "moonshine_get_version");
  SYMBOL (error_to_string, "moonshine_error_to_string");
  SYMBOL (load, "moonshine_load_transcriber_from_files");
  SYMBOL (free_transcriber, "moonshine_free_transcriber");
  SYMBOL (create_stream, "moonshine_create_stream");
  SYMBOL (free_stream, "moonshine_free_stream");
  SYMBOL (start_stream, "moonshine_start_stream");
  SYMBOL (stop_stream, "moonshine_stop_stream");
  SYMBOL (add_audio, "moonshine_transcribe_add_audio_to_stream");
  SYMBOL (transcribe, "moonshine_transcribe_stream");
#undef SYMBOL
  /* moonshine-api.h's structures are those of version 3 */
  version = ms->get_version ();
  if (version < 30000 || version >= 40000)
    {
      *error = g_strdup_printf ("version %d, not 3", version);
      goto fail;
    }
  return TRUE;

fail:
  dlclose (ms->library);
  memset (ms, 0, sizeof *ms);
  return FALSE;
}

static gboolean
load (DictateEngine *self)
{
  g_autofree char *model = NULL, *error = NULL;
  uint32_t arch;

  if (self->transcriber >= 0)
    return TRUE;
  if (!self->ms.library && !load_library (&self->ms, &error))
    {
      g_warning ("Moonshine: %s", error);
      post (self, POST_ERROR, NULL, g_strdup (_("Speech recognition could not be started.")), 0);
      return FALSE;
    }
  model = find_model (&arch);
  if (model)
    self->transcriber = self->ms.load (model, arch, NULL, 0, MOONSHINE_HEADER_VERSION);
  if (!model || self->transcriber < 0)
    {
      g_warning ("Moonshine: %s: %s", model ? model : "no model",
                 model ? self->ms.error_to_string (self->transcriber) : "");
      post (self, POST_ERROR, NULL, g_strdup (_("The speech model could not be loaded.")), 0);
      return FALSE;
    }
  return TRUE;
}

/* the queued audio to the recogniser: TRUE if there was any */
static gboolean
feed (DictateEngine *self, int32_t stream)
{
  g_autoptr (GArray) audio = NULL;

  g_mutex_lock (&self->lock);
  audio = self->queued;
  self->queued = g_array_new (FALSE, FALSE, sizeof (float));
  g_mutex_unlock (&self->lock);
  if (audio->len)
    self->ms.add_audio (self->transcriber, stream, (const float *) audio->data, audio->len, RATE, 0);
  return audio->len > 0;
}

/* What is new in the transcript since the last time, to the main thread. `done` counts the lines already passed on as
 * finished; `partial` is the unfinished one as last passed on. */
static void
report (DictateEngine *self, int32_t stream, uint64_t *done, char **partial)
{
  struct transcript_t *transcript = NULL;
  g_autoptr (GPtrArray) finished = g_ptr_array_new_with_free_func (g_free);
  const char *heard = "";
  char *now;

  if (self->ms.transcribe (self->transcriber, stream, 0, &transcript) != 0 || !transcript)
    return;
  for (uint64_t i = *done; i < transcript->line_count; i++)
    {
      const struct transcript_line_t *line = &transcript->lines[i];
      const char *text = line->text ? line->text : "";

      if (line->is_complete)
        {
          char *sentence = g_strstrip (g_strdup (text));

          /* speech the recogniser made nothing of is an empty line */
          if (*sentence)
            g_ptr_array_add (finished, sentence);
          else
            g_free (sentence);
          *done = i + 1;
        }
      else
        heard = text;
    }
  now = g_strstrip (g_strdup (heard));
  if (finished->len == 0 && g_str_equal (now, *partial))
    {
      g_free (now);
      return;
    }
  g_free (*partial);
  *partial = now;
  g_ptr_array_add (finished, NULL);
  post (self, POST_TEXT, (char **) g_ptr_array_steal (finished, NULL), g_strdup (now), 0);
}

static gpointer
work (gpointer data)
{
  DictateEngine *self = data;
  int32_t stream = -1;
  uint64_t done = 0;
  g_autofree char *partial = g_strdup ("");
  gint64 last = 0;

  if (!load (self))
    goto out;
  stream = self->ms.create_stream (self->transcriber, 0);
  if (stream < 0 || self->ms.start_stream (self->transcriber, stream) != 0)
    {
      g_warning ("Moonshine: no stream: %s", self->ms.error_to_string (stream < 0 ? stream : -1));
      post (self, POST_ERROR, NULL, g_strdup (_("Speech recognition could not be started.")), 0);
      goto out;
    }
  post (self, POST_STATE, NULL, NULL, DICTATE_LISTENING);

  for (;;)
    {
      gboolean stop;
      gint64 next = last + UPDATE_MS * G_TIME_SPAN_MILLISECOND;

      g_mutex_lock (&self->lock);
      while (!self->stop && g_get_monotonic_time () < next)
        g_cond_wait_until (&self->wake, &self->lock, next);
      stop = self->stop;
      g_mutex_unlock (&self->lock);
      if (stop)
        break;
      last = g_get_monotonic_time ();
      if (feed (self, stream))
        report (self, stream, &done, &partial);
    }

  /* the microphone is closed: what it heard last, and the end of the stream, which finishes the last sentence */
  feed (self, stream);
  self->ms.stop_stream (self->transcriber, stream);
  report (self, stream, &done, &partial);

out:
  if (stream >= 0)
    self->ms.free_stream (self->transcriber, stream);
  post (self, POST_STATE, NULL, NULL, DICTATE_IDLE);
  g_object_unref (self);
  return NULL;
}

/* --- the microphone */

static GstFlowReturn
on_sample (GstAppSink *sink, gpointer data)
{
  DictateEngine *self = data;
  GstSample *sample = gst_app_sink_pull_sample (sink);
  GstBuffer *buffer = sample ? gst_sample_get_buffer (sample) : NULL;
  GstMapInfo map;

  if (buffer && gst_buffer_map (buffer, &map, GST_MAP_READ))
    {
      const float *samples = (const float *) map.data;
      guint n = map.size / sizeof (float);
      double sum = 0, db;

      for (guint i = 0; i < n; i++)
        sum += samples[i] * samples[i];
      db = n ? 10 * log10 (sum / n + 1e-12) : QUIET_DB;
      g_atomic_int_set (&self->level, (int) (CLAMP ((db - QUIET_DB) / (LOUD_DB - QUIET_DB), 0, 1) * 1e6));
      g_mutex_lock (&self->lock);
      g_array_append_vals (self->queued, samples, n);
      g_mutex_unlock (&self->lock);
      gst_buffer_unmap (buffer, &map);
    }
  if (sample)
    gst_sample_unref (sample);
  return GST_FLOW_OK;
}

static gboolean
on_bus (GstBus *bus, GstMessage *message, gpointer data)
{
  DictateEngine *self = data;
  g_autoptr (GError) error = NULL;
  g_autofree char *debug = NULL;

  if (GST_MESSAGE_TYPE (message) != GST_MESSAGE_ERROR)
    return G_SOURCE_CONTINUE;
  gst_message_parse_error (message, &error, &debug);
  g_warning ("microphone: %s (%s)", error->message, debug ? debug : "");
  self->bus_watch = 0;
  dictate_engine_stop (self);
  g_signal_emit (self, signals[SIGNAL_ERROR], 0, _("The microphone could not be opened."));
  return G_SOURCE_REMOVE;
}

static gboolean
start_capture (DictateEngine *self)
{
  /* the user's default source, through PipeWire's PulseAudio server; GStreamer's choice where there is none */
  g_autoptr (GstElementFactory) pulse = gst_element_factory_find ("pulsesrc");
  g_autofree char *description = g_strdup_printf (
    "%s ! audioconvert ! audioresample ! audio/x-raw,format=F32LE,rate=%d,channels=1,layout=interleaved "
    "! appsink name=sink sync=false", pulse ? "pulsesrc client-name=Dictate" : "autoaudiosrc", RATE);
  g_autoptr (GError) error = NULL;
  GstAppSinkCallbacks callbacks = { .new_sample = on_sample };
  GstElement *sink;
  GstBus *bus;

  self->pipeline = gst_parse_launch (description, &error);
  if (!self->pipeline)
    {
      g_warning ("microphone: %s", error->message);
      return FALSE;
    }
  sink = gst_bin_get_by_name (GST_BIN (self->pipeline), "sink");
  gst_app_sink_set_callbacks (GST_APP_SINK (sink), &callbacks, self, NULL);
  gst_object_unref (sink);
  bus = gst_element_get_bus (self->pipeline);
  self->bus_watch = gst_bus_add_watch (bus, on_bus, self);
  gst_object_unref (bus);
  if (gst_element_set_state (self->pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE)
    {
      stop_capture (self);
      return FALSE;
    }
  return TRUE;
}

/* once this returns, on_sample does not run again */
static void
stop_capture (DictateEngine *self)
{
  g_clear_handle_id (&self->bus_watch, g_source_remove);
  if (self->pipeline)
    {
      gst_element_set_state (self->pipeline, GST_STATE_NULL);
      g_clear_object (&self->pipeline);
    }
  g_atomic_int_set (&self->level, 0);
}

/* --- the engine */

void
dictate_engine_start (DictateEngine *self)
{
  if (self->state != DICTATE_IDLE)
    return;
  g_mutex_lock (&self->lock);
  g_array_set_size (self->queued, 0);
  self->stop = FALSE;
  g_mutex_unlock (&self->lock);
  /* the microphone first: what is said while the model loads is heard too */
  if (!start_capture (self))
    {
      g_signal_emit (self, signals[SIGNAL_ERROR], 0, _("The microphone could not be opened."));
      return;
    }
  self->state = DICTATE_STARTING;
  self->worker = g_thread_new ("dictate", work, g_object_ref (self));
  g_signal_emit (self, signals[SIGNAL_STATE_CHANGED], 0);
}

void
dictate_engine_stop (DictateEngine *self)
{
  if (self->state != DICTATE_STARTING && self->state != DICTATE_LISTENING)
    return;
  stop_capture (self);
  g_mutex_lock (&self->lock);
  self->stop = TRUE;
  g_cond_signal (&self->wake);
  g_mutex_unlock (&self->lock);
  self->state = DICTATE_STOPPING;
  g_signal_emit (self, signals[SIGNAL_STATE_CHANGED], 0);
}

DictateState
dictate_engine_get_state (DictateEngine *self)
{
  return self->state;
}

double
dictate_engine_get_level (DictateEngine *self)
{
  return g_atomic_int_get (&self->level) / 1e6;
}

static void
dictate_engine_finalize (GObject *object)
{
  DictateEngine *self = DICTATE_ENGINE (object);

  /* the worker holds a reference while it runs: it is over by now */
  stop_capture (self);
  if (self->transcriber >= 0)
    self->ms.free_transcriber (self->transcriber);
  if (self->ms.library)
    dlclose (self->ms.library);
  g_array_unref (self->queued);
  g_mutex_clear (&self->lock);
  g_cond_clear (&self->wake);
  G_OBJECT_CLASS (dictate_engine_parent_class)->finalize (object);
}

static void
dictate_engine_class_init (DictateEngineClass *klass)
{
  G_OBJECT_CLASS (klass)->finalize = dictate_engine_finalize;
  signals[SIGNAL_TEXT] = g_signal_new ("text", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                                       G_TYPE_NONE, 2, G_TYPE_STRV, G_TYPE_STRING);
  signals[SIGNAL_STATE_CHANGED] = g_signal_new ("state-changed", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0,
                                                NULL, NULL, NULL, G_TYPE_NONE, 0);
  signals[SIGNAL_ERROR] = g_signal_new ("error", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                                        G_TYPE_NONE, 1, G_TYPE_STRING);
}

static void
dictate_engine_init (DictateEngine *self)
{
  self->transcriber = -1;
  self->queued = g_array_new (FALSE, FALSE, sizeof (float));
  g_mutex_init (&self->lock);
  g_cond_init (&self->wake);
}

DictateEngine *
dictate_engine_new (void)
{
  return g_object_new (DICTATE_TYPE_ENGINE, NULL);
}
