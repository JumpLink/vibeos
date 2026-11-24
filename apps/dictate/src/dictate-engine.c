/* DictateEngine: see dictate-engine.h.
 *
 * Three threads take part. GStreamer's streaming thread hands each piece of microphone audio to on_sample, which
 * only measures its loudness and queues it, so the microphone never waits for the recogniser. The worker (work)
 * gives the queued audio to the recogniser every UPDATE_MS, which reports what it heard; meanwhile the audio just
 * queues up. What it heard goes to the main thread (post), where the signals are emitted.
 *
 * The worker loads the language's model the first time and keeps it: starting again in the same language is at
 * once, and another language's model takes the place of the one before (0.15 s for Moonshine's tiny models, up to
 * 0.9 s for Vosk's small ones on the reference phones).
 *
 * Taking turns (dictate_engine_set_turns), the worker wakes every TURN_WAKE_MS and cuts the speech out of the audio
 * with the voice activity detector (dictate-vad.h; the loudness when its model is not installed): only speech goes
 * to the recogniser, from TURN_PREROLL_MS before it starts, and once the speaker has been quiet for TURN_END_MS it
 * ends the turn by itself, as if stopped. Held (dictate_engine_set_held), it does not: neither a pause nor a long
 * wait for the first word ends the turn until it is let go. */
#include "dictate-engine.h"
#include "dictate-recogniser.h"
#include "dictate-vad.h"

#include <dlfcn.h>
#include <glib/gi18n.h>
#include <gst/app/gstappsink.h>
#include <gst/gst.h>
#include <math.h>
#include <string.h>

#define UPDATE_MS       300     /* between updates while listening */
#define QUIET_DB        -55.0   /* loudness: level 0 at or under this (a quiet room is -50 on the phones' mics) */
#define LOUD_DB         -20.0   /* ... 1 at or over this (speech close to the phone) */

#define TURN_WAKE_MS    64      /* taking turns: between looks at the audio */
#define TURN_START      0.5     /* a frame this likely speech starts the speech */
#define TURN_GOING      0.35    /* in speech, a frame under this is a quiet one */
#define TURN_END_MS     700     /* this long quiet after speech, the turn is over */
#define TURN_MIN_MS     250     /* shorter speech (a cough, a knock) with no words heard in it does not count */
#define TURN_PREROLL_MS 300     /* the audio before the speech's start that goes with it: its first sound */
#define TURN_WAIT_MS    8000    /* nothing said for this long, the turn is over */
#define TURN_KEEP_S     30      /* a turn's speech kept for dictate_engine_take_turn_audio: at most this long */
#define TURN_LOUD       0.3     /* without the detector: a frame this loud (as the level) is speech */
#define FRAME_MS        (DICTATE_VAD_FRAME * 1000 / DICTATE_RATE)

struct _DictateEngine
{
  GObject parent;
  DictateState state;
  char *language;

  /* the worker's: the model loaded last, kept from then on */
  DictateModel *loaded;
  DictateRecogniser *recogniser;
  /* the model to listen with, from start until the worker took it */
  DictateModel *wanted;

  GstElement *pipeline;         /* while the microphone is open */
  guint bus_watch;
  GThread *worker;              /* from start until the worker posted the idle state */

  /* between the streaming thread, the worker and the main thread */
  GMutex lock;
  GCond wake;
  GArray *queued;               /* float samples the recogniser has not had */
  gboolean stop;                /* the worker finishes the stream and ends */
  gboolean turns;               /* taking turns, for the next start; the worker's copy is its own */
  gboolean held;                /* taking turns: the turn does not end by itself (read by the worker as it goes) */
  int level;                    /* atomic: dictate_engine_get_level × 1e6 */
  /* the worker's: the voice activity detector, loaded the first time turns are taken, kept */
  DictateVad *vad;
  gboolean vad_tried;
  GBytes *turn_audio;           /* under the lock: the last turn's speech, until taken or the next start */
};

enum { SIGNAL_TEXT, SIGNAL_STATE_CHANGED, SIGNAL_ERROR, N_SIGNALS };
static guint signals[N_SIGNALS];

G_DEFINE_FINAL_TYPE (DictateEngine, dictate_engine, G_TYPE_OBJECT)

/* --- the recognisers' libraries */

char *
dictate_library_path (const char *library)
{
  char *path = g_build_filename (LIBRARY_DIR, library, NULL);

  if (g_file_test (path, G_FILE_TEST_EXISTS))
    return path;
  g_free (path);
  return g_strdup (library);
}

gboolean
dictate_library_installed (const char *library)
{
  g_autofree char *path = dictate_library_path (library);
  void *handle;

  if (g_path_is_absolute (path))
    return TRUE;
  /* in the system's path, if anywhere: there is nothing to look for but loading it */
  handle = dlopen (path, RTLD_LAZY | RTLD_LOCAL);
  if (handle)
    dlclose (handle);
  return handle != NULL;
}

gboolean
dictate_engine_available (DictateEngine *self, char **reason)
{
  g_autoptr (DictateModel) model = self->language ? dictate_model_find (self->language) : NULL;
  const DictateLanguage *language = dictate_language_lookup (self->language);
  const DictateRecogniserClass *recogniser;

  if (!model)
    {
      g_autofree char *package = language ? g_strconcat ("vibeos-dictate-", language->code, NULL) : NULL;

      if (!reason)
        return FALSE;
      if (language)
        /* TRANSLATORS: the first %s is the language's name for itself (Deutsch), the second the package's name
         * (vibeos-dictate-de) */
        *reason = g_strdup_printf (_("There is no speech model for %s. Install the %s package."), language->name,
                                   package);
      else
        *reason = g_strdup (_("There is no speech model. Install a vibeos-dictate language package, such as "
                              "vibeos-dictate-en."));
      return FALSE;
    }
  recogniser = dictate_recogniser_class (model->kind);
  if (!recogniser->installed ())
    {
      if (reason)
        /* TRANSLATORS: %s is the package's name (vibeos-dictate-moonshine) */
        *reason = g_strdup_printf (_("Speech recognition is not installed. Install the %s package."),
                                   recogniser->package);
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

/* the model the start asked for, loaded */
static gboolean
load (DictateEngine *self, DictateModel *model)
{
  g_autofree char *error = NULL;
  const DictateRecogniserClass *recogniser = dictate_recogniser_class (model->kind);

  if (self->recogniser && g_str_equal (self->loaded->dir, model->dir))
    {
      dictate_model_free (model);
      return TRUE;
    }
  if (self->recogniser)
    {
      dictate_recogniser_class (self->loaded->kind)->unload (self->recogniser);
      self->recogniser = NULL;
    }
  g_clear_pointer (&self->loaded, dictate_model_free);
  self->recogniser = recogniser->load (model, &error);
  if (!self->recogniser)
    {
      g_warning ("%s: %s", recogniser->name, error);
      dictate_model_free (model);
      post (self, POST_ERROR, NULL, g_strdup (_("The speech model could not be loaded.")), 0);
      return FALSE;
    }
  self->loaded = model;
  {
    /* which model listens now, for the log: its directory's name ("tiny-streaming-de") */
    g_autofree char *dir = g_path_get_basename (model->dir);
    g_message ("engine: %s model %s loaded", recogniser->name, dir);
  }
  return TRUE;
}

static GArray *
take_queued (DictateEngine *self)
{
  GArray *audio;

  g_mutex_lock (&self->lock);
  audio = self->queued;
  self->queued = g_array_new (FALSE, FALSE, sizeof (float));
  g_mutex_unlock (&self->lock);
  return audio;
}

/* `audio` to the recogniser, and what it heard to the main thread if that is news. `partial` is the unfinished
 * sentence as last passed on. TRUE when it heard words: a sentence finished, or one being spoken. */
static gboolean
hear_audio (DictateEngine *self, DictateStream *stream, GArray *audio, char **partial)
{
  const DictateRecogniserClass *recogniser = dictate_recogniser_class (self->loaded->kind);
  g_autoptr (GPtrArray) finished = g_ptr_array_new_with_free_func (g_free);
  char *now = NULL;
  gboolean words;

  if (audio->len == 0)
    return FALSE;
  recogniser->hear (stream, (const float *) audio->data, audio->len, finished, &now);
  words = finished->len > 0 || *now;
  if (finished->len == 0 && g_str_equal (now, *partial))
    {
      g_free (now);
      return words;
    }
  g_free (*partial);
  *partial = now;
  g_ptr_array_add (finished, NULL);
  post (self, POST_TEXT, (char **) g_ptr_array_steal (finished, NULL), g_strdup (now), 0);
  return words;
}

/* the queued audio to the recogniser */
static void
hear (DictateEngine *self, DictateStream *stream, char **partial)
{
  g_autoptr (GArray) audio = take_queued (self);
  hear_audio (self, stream, audio, partial);
}

/* --- taking turns */

typedef struct {
  GArray *pending;              /* samples short of a whole frame */
  GArray *preroll;              /* the last TURN_PREROLL_MS before the speech */
  GArray *speech;               /* for the recogniser */
  gboolean speaking;            /* in speech now */
  guint frames, speech_frames, quiet_frames;
  gboolean over;                /* the turn has ended */
} Turn;

static void
turn_init (Turn *t)
{
  memset (t, 0, sizeof *t);
  t->pending = g_array_new (FALSE, FALSE, sizeof (float));
  t->preroll = g_array_new (FALSE, FALSE, sizeof (float));
  t->speech = g_array_new (FALSE, FALSE, sizeof (float));
}

static void
turn_clear (Turn *t)
{
  g_array_unref (t->pending);
  g_array_unref (t->preroll);
  g_array_unref (t->speech);
}

/* how likely `frame` is speech: the detector's answer, else its loudness against TURN_LOUD */
static float
speech_likelihood (DictateEngine *self, const float *frame)
{
  double sum = 0, db;
  float p;

  if (self->vad && (p = dictate_vad_frame (self->vad, frame)) >= 0)
    return p;
  for (guint i = 0; i < DICTATE_VAD_FRAME; i++)
    sum += frame[i] * frame[i];
  db = 10 * log10 (sum / DICTATE_VAD_FRAME + 1e-12);
  return (db - QUIET_DB) / (LOUD_DB - QUIET_DB) >= TURN_LOUD ? 1 : 0;
}

/* the new audio frame by frame: speech onto t->speech (with the preroll before it), the turn's end found.
 * `heard`: the recogniser has words for this turn already; `held`: the turn does not end now, whatever is heard */
static void
turn_feed (DictateEngine *self, Turn *t, GArray *audio, gboolean heard, gboolean held)
{
  guint preroll = TURN_PREROLL_MS * DICTATE_RATE / 1000;

  g_array_append_vals (t->pending, audio->data, audio->len);
  while (t->pending->len >= DICTATE_VAD_FRAME && !t->over)
    {
      const float *frame = (const float *) t->pending->data;
      float p = speech_likelihood (self, frame);

      t->frames++;
      if (!t->speaking)
        {
          if (p >= TURN_START)
            {
              /* speech: what came just before it goes too, for its first sound */
              t->speaking = TRUE;
              t->quiet_frames = 0;
              g_array_append_vals (t->speech, t->preroll->data, t->preroll->len);
              g_array_set_size (t->preroll, 0);
              g_array_append_vals (t->speech, frame, DICTATE_VAD_FRAME);
              t->speech_frames++;
            }
          else
            {
              g_array_append_vals (t->preroll, frame, DICTATE_VAD_FRAME);
              if (t->preroll->len > preroll)
                g_array_remove_range (t->preroll, 0, t->preroll->len - preroll);
              if (t->speech_frames == 0 && t->frames * FRAME_MS >= TURN_WAIT_MS && !held)
                t->over = TRUE;
            }
        }
      else
        {
          /* the quiet within the speech goes on to the recogniser too: it ends its sentences on it */
          g_array_append_vals (t->speech, frame, DICTATE_VAD_FRAME);
          t->speech_frames++;
          t->quiet_frames = p < TURN_GOING ? t->quiet_frames + 1 : 0;
          /* held, a pause is only a pause: let go after one, the turn is over at once */
          if (t->quiet_frames * FRAME_MS >= TURN_END_MS && !held)
            {
              t->speaking = FALSE;
              /* a blip with no words in it is not a turn: listening goes on */
              if ((t->speech_frames - t->quiet_frames) * FRAME_MS >= TURN_MIN_MS || heard)
                t->over = TRUE;
              else
                t->speech_frames = 0, t->frames = 0;
            }
        }
      g_array_remove_range (t->pending, 0, DICTATE_VAD_FRAME);
    }
}

/* the speech sent to the recogniser, kept too: up to TURN_KEEP_S of it */
static void
keep (GArray *kept, GArray *speech)
{
  guint room = TURN_KEEP_S * DICTATE_RATE - MIN (kept->len, TURN_KEEP_S * DICTATE_RATE);

  g_array_append_vals (kept, speech->data, MIN (speech->len, room));
}

static gpointer
work (gpointer data)
{
  DictateEngine *self = data;
  DictateModel *model;
  const DictateRecogniserClass *recogniser;
  DictateStream *stream = NULL;
  g_autofree char *partial = g_strdup ("");
  g_autofree char *error = NULL;
  gint64 last = 0;
  gboolean turns, heard = FALSE, was_held = FALSE;
  Turn turn;
  g_autoptr (GArray) kept = g_array_new (FALSE, FALSE, sizeof (float));

  g_mutex_lock (&self->lock);
  model = g_steal_pointer (&self->wanted);
  turns = self->turns;
  g_mutex_unlock (&self->lock);
  turn_init (&turn);
  if (!load (self, model))
    goto out;
  if (turns && !self->vad_tried)
    {
      g_autofree char *path = dictate_vad_find ();
      g_autoptr (GError) vad_error = NULL;

      self->vad_tried = TRUE;
      self->vad = path ? dictate_vad_load (path, &vad_error) : NULL;
      if (!self->vad)
        g_message ("turns are cut by loudness: %s", path ? vad_error->message : "no voice activity model");
    }
  if (turns && self->vad)
    dictate_vad_reset (self->vad);
  recogniser = dictate_recogniser_class (self->loaded->kind);
  stream = recogniser->begin (self->recogniser, &error);
  if (!stream)
    {
      g_warning ("%s: %s", recogniser->name, error);
      post (self, POST_ERROR, NULL, g_strdup (_("Speech recognition could not be started.")), 0);
      goto out;
    }
  post (self, POST_STATE, NULL, NULL, DICTATE_LISTENING);

  for (;;)
    {
      gboolean stop, held;
      gint64 next = last + (turns ? TURN_WAKE_MS : UPDATE_MS) * G_TIME_SPAN_MILLISECOND;

      g_mutex_lock (&self->lock);
      while (!self->stop && g_get_monotonic_time () < next)
        g_cond_wait_until (&self->wake, &self->lock, next);
      stop = self->stop;
      held = self->held;
      g_mutex_unlock (&self->lock);
      if (stop)
        break;
      last = g_get_monotonic_time ();
      if (turns)
        {
          g_autoptr (GArray) audio = take_queued (self);

          /* let go with nothing said yet: the wait for the first word starts now */
          if (was_held && !held && !turn.speaking && turn.speech_frames == 0)
            turn.frames = 0;
          was_held = held;
          turn_feed (self, &turn, audio, heard, held);
          keep (kept, turn.speech);
          if (hear_audio (self, stream, turn.speech, &partial))
            heard = TRUE;
          g_array_set_size (turn.speech, 0);
          /* the speaker has finished: the microphone closes as if stopped (on the main thread, at the idle state) */
          if (turn.over)
            break;
        }
      else
        hear (self, stream, &partial);
    }

  /* the microphone is closed: what it heard last, then the end of the stream, which finishes the last sentence.
   * Taking turns, only the speech goes: what came after the turn's end is not the speaker's */
  if (!turns)
    hear (self, stream, &partial);
  else if (!turn.over)
    {
      g_autoptr (GArray) audio = take_queued (self);
      turn_feed (self, &turn, audio, heard, was_held);
      keep (kept, turn.speech);
      hear_audio (self, stream, turn.speech, &partial);
    }
  /* the turn's speech, for the voice match (taken on the main thread once the state is idle) */
  if (turns && kept->len)
    {
      g_mutex_lock (&self->lock);
      g_clear_pointer (&self->turn_audio, g_bytes_unref);
      self->turn_audio = g_bytes_new (kept->data, kept->len * sizeof (float));
      g_mutex_unlock (&self->lock);
    }
  {
    g_autoptr (GPtrArray) finished = g_ptr_array_new_with_free_func (g_free);

    recogniser->end (stream, finished);
    if (finished->len || *partial)
      {
        g_ptr_array_add (finished, NULL);
        post (self, POST_TEXT, (char **) g_ptr_array_steal (finished, NULL), g_strdup (""), 0);
      }
  }

out:
  turn_clear (&turn);
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
  /* for tests (tests/talk-bench.c): a sound file instead of the microphone, played in real time */
  const char *test = g_getenv ("DICTATE_TEST_INPUT");
  g_autofree char *quoted = test ? g_strescape (test, NULL) : NULL;
  g_autofree char *source = test ? g_strdup_printf ("filesrc location=\"%s\" ! decodebin", quoted)
                                 : g_strdup (pulse ? "pulsesrc client-name=Dictate" : "autoaudiosrc");
  g_autofree char *description = g_strdup_printf (
    "%s ! audioconvert ! audioresample ! audio/x-raw,format=F32LE,rate=%d,channels=1,layout=interleaved "
    "! appsink name=sink sync=%s", source, DICTATE_RATE, test ? "true" : "false");
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
dictate_engine_set_language (DictateEngine *self, const char *code)
{
  g_free (self->language);
  self->language = g_strdup (code);
}

const char *
dictate_engine_get_language (DictateEngine *self)
{
  return self->language;
}

void
dictate_engine_set_turns (DictateEngine *self, gboolean turns)
{
  g_mutex_lock (&self->lock);
  self->turns = turns;
  g_mutex_unlock (&self->lock);
}

void
dictate_engine_set_held (DictateEngine *self, gboolean held)
{
  g_mutex_lock (&self->lock);
  self->held = held;
  g_mutex_unlock (&self->lock);
}

void
dictate_engine_start (DictateEngine *self)
{
  DictateModel *model;

  if (self->state != DICTATE_IDLE)
    return;
  model = self->language ? dictate_model_find (self->language) : NULL;
  if (!model)
    {
      g_signal_emit (self, signals[SIGNAL_ERROR], 0, _("The speech model could not be loaded."));
      return;
    }
  g_mutex_lock (&self->lock);
  g_array_set_size (self->queued, 0);
  g_clear_pointer (&self->turn_audio, g_bytes_unref);
  self->stop = FALSE;
  g_clear_pointer (&self->wanted, dictate_model_free);
  self->wanted = model;
  g_mutex_unlock (&self->lock);
  /* the microphone first: what is said while the model loads is heard too */
  if (!start_capture (self))
    {
      g_clear_pointer (&self->wanted, dictate_model_free);
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

GBytes *
dictate_engine_take_turn_audio (DictateEngine *self)
{
  GBytes *audio;

  g_mutex_lock (&self->lock);
  audio = g_steal_pointer (&self->turn_audio);
  g_mutex_unlock (&self->lock);
  return audio;
}

static void
dictate_engine_finalize (GObject *object)
{
  DictateEngine *self = DICTATE_ENGINE (object);

  /* the worker holds a reference while it runs: it is over by now */
  stop_capture (self);
  if (self->recogniser)
    dictate_recogniser_class (self->loaded->kind)->unload (self->recogniser);
  dictate_model_free (self->loaded);
  dictate_model_free (self->wanted);
  dictate_vad_free (self->vad);
  g_clear_pointer (&self->turn_audio, g_bytes_unref);
  g_free (self->language);
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
  self->queued = g_array_new (FALSE, FALSE, sizeof (float));
  g_mutex_init (&self->lock);
  g_cond_init (&self->wake);
}

DictateEngine *
dictate_engine_new (void)
{
  return g_object_new (DICTATE_TYPE_ENGINE, NULL);
}
