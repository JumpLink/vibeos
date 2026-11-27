/* navi-voice: see navi-voice.h.
 *
 * One thread of its own does what is slow: loading a Piper voice, making a prompt's sound, talking to Speech
 * Dispatcher (opening it can start the daemon). The main thread keeps the prompts made, by language and words, and
 * the queue of those to say, and plays them one after the other, each through a GStreamer pipeline of its own to the
 * session's sound server, as Agent does. */
#include "navi-voice.h"

#include <gio/gio.h>
#include <glib/gi18n.h>
#include <string.h>
#ifdef HAVE_PIPER
#include <gst/app/gstappsrc.h>
#include <gst/gst.h>

#include "dictate-voice.h"
#endif
#ifdef HAVE_SPEECHD
#include <libspeechd.h>
#endif

#define KEEP_CLIPS    32 /* prompts kept made */
#define PIPER_THREADS 2  /* ONNX Runtime's, as Agent's: the other cores stay the map's */

typedef enum {
  JOB_WARM,     /* load the voice, and make one word: a model's first run is its slowest */
  JOB_MAKE,     /* a prompt's sound */
  JOB_REST,     /* the voice let go of */
  JOB_SPD_SAY,
  JOB_SPD_STOP,
} JobKind;

typedef struct {
  JobKind kind;
  char *model;  /* a Piper voice's .onnx (or a Kokoro or tiny-voca model) */
  char *name;   /* Kokoro's speaker, or NULL */
  char *gender; /* who speaks where the voice has a man and a woman, or NULL */
  char *language;
  char *text;
  char *key;
} Job;

/* Agent's voice for a language (dictate_voice_choose): no Navi setting of its own picks one */
typedef struct {
  char *model; /* NULL: none */
  char *name, *gender;
} Choice;

#ifdef HAVE_PIPER
/* a prompt, made or being made */
typedef struct {
  float *samples;
  gsize n;
  guint rate;
  gboolean ready, failed;
  gint64 make_us; /* how long the making took */
} Clip;

/* a prompt to say */
typedef struct {
  char *key;
  gint64 due;          /* when it was asked to be said */
  gint64 started;      /* when its playing started: when it was due, or when the one before it ended */
  gboolean made_ahead; /* it had been asked for before */
} Turn;
#endif

struct _NaviVoice {
  GThreadPool *pool;
  gboolean dead; /* freed: what the thread hands back after that is dropped */
  gboolean stats;
  GHashTable *models; /* the main thread's: language ("de") -> Agent's voice for it, a Choice */
  char *description;
#ifdef HAVE_PIPER
  GSettings *agent; /* Agent's settings (dictate_voice_settings), or NULL: its voice choice follows them */
#endif
  /* the thread's */
#ifdef HAVE_PIPER
  DictateVoice *voice;
  char *voice_model, *voice_name;
  char *failed_model, *failed_name;
#endif
#ifdef HAVE_SPEECHD
  SPDConnection *conn;
  char *spd_language;
  gboolean spd_failed;
#endif
  /* the main thread's */
#ifdef HAVE_PIPER
  GHashTable *clips; /* key -> Clip */
  GQueue *order;     /* the clips' keys, oldest first */
  GQueue *turns;     /* Turn */
  GstElement *pipeline, *spare; /* the prompt's; the next one's, READY */
  guint bus_watch;
  gboolean warm;                /* a drive is near: keep a spare */
  gboolean making_spare;        /* on a thread: connecting to the sound server takes 40 to 100 ms */
  Turn *playing;
  gpointer heard; /* when its first sound reached the output, a gint64 (the sink's thread writes it) */
  gint64 last_end; /* when the one before ended */
#endif
};

static void
job_free (Job *job)
{
  g_free (job->model);
  g_free (job->name);
  g_free (job->gender);
  g_free (job->language);
  g_free (job->text);
  g_free (job->key);
  g_free (job);
}

static NaviVoice *
voice_ref (NaviVoice *self)
{
  return g_rc_box_acquire (self);
}

static void
voice_clear (gpointer data)
{
  NaviVoice *self = data;
  g_clear_pointer (&self->models, g_hash_table_unref);
  g_free (self->description);
#ifdef HAVE_PIPER
  g_clear_pointer (&self->clips, g_hash_table_unref);
  if (self->order)
    g_queue_free_full (self->order, g_free);
#endif
}

static void
voice_unref (NaviVoice *self)
{
  g_rc_box_release_full (self, voice_clear);
}

/* "de-DE" -> "de" */
static char *
short_language (const char *language)
{
  return g_ascii_strdown (language && strlen (language) >= 2 ? language : "en", 2);
}

/* ---- the thread ---- */

#ifdef HAVE_PIPER
typedef struct {
  NaviVoice *self;
  char *key;
  float *samples;
  gsize n;
  guint rate;
  gint64 make_us;
} Made;

static gboolean made (gpointer data);

/* the job's voice loaded, unless it is already */
static gboolean
open_voice (NaviVoice *self, const Job *job)
{
  if (self->voice && !g_strcmp0 (self->voice_model, job->model) && !g_strcmp0 (self->voice_name, job->name))
    return TRUE;
  if (!g_strcmp0 (self->failed_model, job->model) && !g_strcmp0 (self->failed_name, job->name))
    return FALSE;
  g_clear_pointer (&self->voice, dictate_voice_free);
  g_clear_pointer (&self->voice_model, g_free);
  g_clear_pointer (&self->voice_name, g_free);
  g_autoptr (GError) error = NULL;
  gint64 t0 = g_get_monotonic_time ();
  self->voice = dictate_voice_load (job->model, job->name, PIPER_THREADS, &error);
  if (!self->voice) {
    g_message ("voice: %s", error->message);
    g_free (self->failed_model);
    g_free (self->failed_name);
    self->failed_model = g_strdup (job->model);
    self->failed_name = g_strdup (job->name);
    return FALSE;
  }
  self->voice_model = g_strdup (job->model);
  self->voice_name = g_strdup (job->name);
  if (self->stats)
    g_print ("navi: voice: Piper voice loaded in %.0f ms\n", (g_get_monotonic_time () - t0) / 1000.0);
  return TRUE;
}

/* the job's voice loaded, and who speaks in it set: Agent's man or woman (Russian's tiny-voca has both), as Agent
 * sets it before each sentence */
static gboolean
load (NaviVoice *self, const Job *job)
{
  if (!open_voice (self, job))
    return FALSE;
  if (job->gender)
    dictate_voice_set_gender (self->voice, job->gender);
  return TRUE;
}
#endif

static void
work (gpointer data, gpointer user_data)
{
  Job *job = data;
  NaviVoice *self = user_data;
  switch (job->kind) {
#ifdef HAVE_PIPER
  case JOB_WARM:
    gst_init (NULL, NULL);
    if (load (self, job)) {
      gsize n = 0;
      g_free (dictate_voice_speak (self->voice, "OK.", 1.0, &n, NULL));
    }
    break;
  case JOB_REST:
    g_clear_pointer (&self->voice, dictate_voice_free);
    g_clear_pointer (&self->voice_model, g_free);
    g_clear_pointer (&self->voice_name, g_free);
    break;
  case JOB_MAKE: {
    Made *m = g_new0 (Made, 1);
    m->self = voice_ref (self);
    m->key = g_strdup (job->key);
    gint64 t0 = g_get_monotonic_time ();
    if (load (self, job)) {
      g_autoptr (GError) error = NULL;
      m->samples = dictate_voice_speak (self->voice, job->text, 1.0, &m->n, &error);
      m->rate = dictate_voice_get_rate (self->voice);
      if (!m->samples)
        g_message ("voice: %s", error->message);
    }
    m->make_us = g_get_monotonic_time () - t0;
    g_idle_add_full (G_PRIORITY_DEFAULT, made, m, NULL);
    break;
  }
#endif
#ifdef HAVE_SPEECHD
  case JOB_SPD_SAY:
  case JOB_SPD_STOP:
    if (!self->conn && !self->spd_failed && job->kind == JOB_SPD_SAY) {
      char *error = NULL;
      self->conn = spd_open2 ("vibeos-navi", "directions", NULL, SPD_MODE_SINGLE, NULL, TRUE, &error);
      if (!self->conn) {
        g_message ("voice: no Speech Dispatcher: %s", error ? error : "?");
        free (error);
        self->spd_failed = TRUE;
      }
    }
    if (!self->conn)
      break;
    if (job->kind == JOB_SPD_STOP) {
      spd_cancel (self->conn);
      break;
    }
    if (g_strcmp0 (job->language, self->spd_language)) {
      spd_set_language (self->conn, job->language);
      g_free (self->spd_language);
      self->spd_language = g_strdup (job->language);
    }
    spd_set_voice_rate (self->conn, -10);
    spd_say (self->conn, SPD_MESSAGE, job->text);
    break;
#endif
  default:
    break;
  }
  job_free (job);
}

static void
push (NaviVoice *self, JobKind kind, const Choice *voice, const char *language, const char *text, const char *key)
{
  Job *job = g_new0 (Job, 1);
  job->kind = kind;
  if (voice) {
    job->model = g_strdup (voice->model);
    job->name = g_strdup (voice->name);
    job->gender = g_strdup (voice->gender);
  }
  job->language = g_strdup (language);
  job->text = g_strdup (text);
  job->key = g_strdup (key);
  g_thread_pool_push (self->pool, job, NULL);
}

/* ---- the voices on the phone ---- */

static void
choice_free (gpointer data)
{
  Choice *v = data;
  g_free (v->model);
  g_free (v->name);
  g_free (v->gender);
  g_free (v);
}

/* the voice Agent speaks `language` (short) with: Agent's own choice (dictate_voice_choose, from its settings), among
 * its voices, downloaded in either app or packaged; NULL for none. Navi has no voice setting of its own. The answers
 * are kept until navi_voice_rescan (), which a change to Agent's choice makes too. */
static const Choice *
find_voice (NaviVoice *self, const char *language)
{
#ifdef HAVE_PIPER
  Choice *known = g_hash_table_lookup (self->models, language);
  if (known)
    return known->model ? known : NULL;
  Choice *v = g_new0 (Choice, 1);
  v->model = dictate_voice_choose (self->agent, language, &v->name, &v->gender);
  g_autofree char *file = v->model ? g_path_get_basename (v->model) : NULL;
  g_debug ("voice: %s: %s", language, file ? file : "no voice of Agent's");
  g_hash_table_insert (self->models, g_strdup (language), v);
  return v->model ? v : NULL;
#else
  return NULL;
#endif
}

void
navi_voice_rescan (NaviVoice *self)
{
  g_hash_table_remove_all (self->models);
}

#ifdef HAVE_PIPER
/* Agent's voice, or who speaks in it, changed in its settings: from the next prompt on, Navi's too */
static void
agent_changed (GSettings *settings, const char *key, NaviVoice *self)
{
  if (dictate_voice_is_choice_key (key))
    navi_voice_rescan (self);
}

/* a prompt's clip, by what makes its sound: the voice and who speaks in it, and the words */
static char *
clip_key (const char *language, const Choice *v, const char *text)
{
  return g_strjoin ("\n", language, v->model, v->name ? v->name : "", v->gender ? v->gender : "", text, NULL);
}
#endif

/* ---- Piper's prompts, played ---- */

#ifdef HAVE_PIPER
static void play_next (NaviVoice *self);

static void
clip_free (gpointer data)
{
  Clip *c = data;
  g_free (c->samples);
  g_free (c);
}

static void
turn_free (Turn *t)
{
  g_free (t->key);
  g_free (t);
}

static gboolean
key_waiting (NaviVoice *self, const char *key)
{
  if (self->playing && !strcmp (self->playing->key, key))
    return TRUE;
  for (GList *l = self->turns->head; l; l = l->next)
    if (!strcmp (((Turn *) l->data)->key, key))
      return TRUE;
  return FALSE;
}

/* the prompt's clip, asked to be made if it is not there yet; *had: it was */
static Clip *
ensure_clip (NaviVoice *self, const Choice *voice, const char *language, const char *text, const char *key,
             gboolean *had)
{
  Clip *c = g_hash_table_lookup (self->clips, key);
  *had = c != NULL;
  if (c)
    return c;
  c = g_new0 (Clip, 1);
  g_hash_table_insert (self->clips, g_strdup (key), c);
  g_queue_push_tail (self->order, g_strdup (key));
  push (self, JOB_MAKE, voice, language, text, key);
  /* the oldest made ones go, but none still to be said */
  for (GList *l = self->order->head; l && g_hash_table_size (self->clips) > KEEP_CLIPS;) {
    GList *next = l->next;
    Clip *old = g_hash_table_lookup (self->clips, l->data);
    if (old && old->ready && !key_waiting (self, l->data)) {
      g_hash_table_remove (self->clips, l->data);
      g_free (l->data);
      g_queue_delete_link (self->order, l);
    }
    l = next;
  }
  return c;
}

static gboolean
made (gpointer data)
{
  Made *m = data;
  NaviVoice *self = m->self;
  Clip *c = self->dead ? NULL : g_hash_table_lookup (self->clips, m->key);
  if (c) {
    c->samples = g_steal_pointer (&m->samples);
    c->n = m->n;
    c->rate = m->rate;
    c->failed = !c->samples || !c->n;
    c->ready = TRUE;
    c->make_us = m->make_us;
    play_next (self);
  }
  g_free (m->samples);
  g_free (m->key);
  voice_unref (m->self);
  g_free (m);
  return G_SOURCE_REMOVE;
}

static GstPadProbeReturn
first_buffer (GstPad *pad, GstPadProbeInfo *info, gpointer data)
{
  NaviVoice *self = data;
  g_atomic_pointer_set (&self->heard, GSIZE_TO_POINTER ((gsize) g_get_monotonic_time ()));
  return GST_PAD_PROBE_REMOVE;
}

/* The output: a pipeline for each prompt, to the session's sound server, as Agent's. The next one is made ahead and
 * kept READY (connected to the sound server) from navi_voice_warm () on, so that a prompt only has to open its
 * stream. A per-drive stream paused between prompts would save that too, but a paused live pipeline did not play
 * again on the S10e. */

static void
close_pipeline (GstElement **pipeline)
{
  if (!*pipeline)
    return;
  gst_element_set_state (*pipeline, GST_STATE_NULL);
  g_clear_object (pipeline);
}

static void
close_output (NaviVoice *self)
{
  g_clear_handle_id (&self->bus_watch, g_source_remove);
  close_pipeline (&self->pipeline);
  close_pipeline (&self->spare);
}

static void
played (NaviVoice *self)
{
  Turn *t = g_steal_pointer (&self->playing);
  if (t && self->stats) {
    Clip *c = g_hash_table_lookup (self->clips, t->key);
    gint64 heard = (gint64) GPOINTER_TO_SIZE (g_atomic_pointer_get (&self->heard));
    if (c && c->ready && !c->failed)
      g_print ("navi: voice: %.1f s of speech, made in %.0f ms %s; waited %.0f ms for the one before; its sound at "
               "the output %.0f ms after it could start (%.0f ms after it was due); could start at %" G_GINT64_FORMAT "\n",
               c->n / (double) MAX (c->rate, 1), c->make_us / 1000.0, t->made_ahead ? "ahead" : "when it was due",
               (t->started - t->due) / 1000.0, heard ? (heard - t->started) / 1000.0 : -1.0,
               heard ? (heard - t->due) / 1000.0 : -1.0, t->started);
  }
  if (t) {
    turn_free (t);
    self->last_end = g_get_monotonic_time ();
  }
  g_clear_handle_id (&self->bus_watch, g_source_remove);
  close_pipeline (&self->pipeline);
}

/* a pipeline for the next prompt, READY: connected to the sound server */
static GstElement *
make_pipeline (void)
{
  gst_init (NULL, NULL);
  g_autoptr (GstElementFactory) pulse = gst_element_factory_find ("pulsesink");
  /* a media role, so that the phone's policy can treat it as directions, not as music */
  const char *sink = pulse ? "pulsesink name=sink client-name=Navi stream-properties=\"props,media.role=Navigation\""
                           : "autoaudiosink name=sink";
  g_autofree char *description = g_strdup_printf ("appsrc name=src format=time ! audioconvert ! audioresample ! %s",
                                                  sink);
  g_autoptr (GError) error = NULL;
  GstElement *pipeline = gst_parse_launch (description, &error);
  if (!pipeline) {
    g_message ("voice: %s", error->message);
    return NULL;
  }
  if (gst_element_set_state (pipeline, GST_STATE_READY) == GST_STATE_CHANGE_FAILURE) {
    g_message ("voice: the sound output did not start");
    close_pipeline (&pipeline);
  }
  return pipeline;
}

static void
spare_thread (GTask *task, gpointer source, gpointer data, GCancellable *cancellable)
{
  gint64 t0 = g_get_monotonic_time ();
  GstElement *pipeline = make_pipeline ();
  if (pipeline && ((NaviVoice *) data)->stats)
    g_print ("navi: voice: the next output ready in %.1f ms, on a thread\n", (g_get_monotonic_time () - t0) / 1000.0);
  g_task_return_pointer (task, pipeline, NULL);
}

static void
spare_made (GObject *source, GAsyncResult *result, gpointer data)
{
  NaviVoice *self = data;
  g_autoptr (GstElement) pipeline = g_task_propagate_pointer (G_TASK (result), NULL);
  self->making_spare = FALSE;
  if (!self->dead && self->warm && !self->spare)
    self->spare = g_steal_pointer (&pipeline);
  else
    close_pipeline (&pipeline);
  voice_unref (self);
}

/* a spare pipeline made on a thread, for the next prompt */
static void
open_output (NaviVoice *self)
{
  if (self->spare || self->making_spare)
    return;
  self->making_spare = TRUE;
  g_autoptr (GTask) task = g_task_new (NULL, NULL, spare_made, voice_ref (self));
  g_task_set_task_data (task, self, NULL);
  g_task_run_in_thread (task, spare_thread);
}

static gboolean
on_bus (GstBus *bus, GstMessage *message, gpointer data)
{
  NaviVoice *self = data;
  switch (GST_MESSAGE_TYPE (message)) {
  case GST_MESSAGE_ERROR: {
    g_autoptr (GError) error = NULL;
    gst_message_parse_error (message, &error, NULL);
    g_message ("voice: %s", error->message);
  }
    /* fall through */
  case GST_MESSAGE_EOS:
    self->bus_watch = 0;
    played (self);
    play_next (self);
    /* the next one ready, while a drive is on */
    if (self->warm)
      open_output (self);
    return G_SOURCE_REMOVE;
  default:
    return G_SOURCE_CONTINUE;
  }
}

static gboolean
start_playing (NaviVoice *self, Clip *c)
{
  self->pipeline = g_steal_pointer (&self->spare);
  if (!self->pipeline)
    self->pipeline = make_pipeline ();
  if (!self->pipeline)
    return FALSE;
  g_autoptr (GstElement) src = gst_bin_get_by_name (GST_BIN (self->pipeline), "src");
  g_autoptr (GstElement) out = gst_bin_get_by_name (GST_BIN (self->pipeline), "sink");
  g_autoptr (GstCaps) caps = gst_caps_new_simple ("audio/x-raw", "format", G_TYPE_STRING, "F32LE", "layout",
                                                  G_TYPE_STRING, "interleaved", "channels", G_TYPE_INT, 1, "rate",
                                                  G_TYPE_INT, (int) c->rate, NULL);
  gst_app_src_set_caps (GST_APP_SRC (src), caps);
  GstBuffer *buffer = gst_buffer_new_memdup (c->samples, c->n * sizeof (float));
  GST_BUFFER_PTS (buffer) = 0;
  GST_BUFFER_DURATION (buffer) = gst_util_uint64_scale (c->n, GST_SECOND, c->rate);
  gst_app_src_push_buffer (GST_APP_SRC (src), buffer);
  gst_app_src_end_of_stream (GST_APP_SRC (src));
  g_atomic_pointer_set (&self->heard, NULL);
  if (out && self->stats) {
    /* autoaudiosink is a bin: its pad is a ghost, which a buffer still passes */
    g_autoptr (GstPad) pad = gst_element_get_static_pad (out, "sink");
    if (pad)
      gst_pad_add_probe (pad, GST_PAD_PROBE_TYPE_BUFFER, first_buffer, self, NULL);
  }
  g_autoptr (GstBus) bus = gst_element_get_bus (self->pipeline);
  self->bus_watch = gst_bus_add_watch (bus, on_bus, self);
  if (gst_element_set_state (self->pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
    g_message ("voice: the sound output did not start");
    g_clear_handle_id (&self->bus_watch, g_source_remove);
    close_pipeline (&self->pipeline);
    return FALSE;
  }
  return TRUE;
}

/* the next prompt, once it is made and nothing else is being said */
static void
play_next (NaviVoice *self)
{
  while (!self->playing && !self->dead) {
    Turn *t = g_queue_peek_head (self->turns);
    if (!t)
      return;
    Clip *c = g_hash_table_lookup (self->clips, t->key);
    if (c && !c->ready)
      return; /* still being made */
    g_queue_pop_head (self->turns);
    self->playing = t;
    /* it could have started when it was due, or when the one before it ended: waiting for its making counts */
    t->started = MAX (t->due, self->last_end);
    if (!c || c->failed || !start_playing (self, c))
      played (self);
  }
}
#endif

/* ---- the API ---- */

NaviVoice *
navi_voice_new (void)
{
  NaviVoice *self = g_rc_box_new0 (NaviVoice);
  self->pool = g_thread_pool_new_full (work, self, (GDestroyNotify) job_free, 1, FALSE, NULL);
  self->models = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, choice_free);
#ifdef HAVE_PIPER
  self->agent = dictate_voice_settings ();
  if (self->agent)
    g_signal_connect (self->agent, "changed", G_CALLBACK (agent_changed), self);
  self->clips = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, clip_free);
  self->order = g_queue_new ();
  self->turns = g_queue_new ();
#endif
  return self;
}

void
navi_voice_free (NaviVoice *self)
{
  if (!self)
    return;
  self->dead = TRUE;
  /* what is waiting goes; what the thread does now ends first */
  g_thread_pool_free (self->pool, TRUE, TRUE);
#ifdef HAVE_PIPER
  if (self->agent)
    g_signal_handlers_disconnect_by_data (self->agent, self);
  g_clear_object (&self->agent);
  close_output (self);
  g_clear_pointer (&self->playing, turn_free);
  g_queue_free_full (self->turns, (GDestroyNotify) turn_free);
  g_clear_pointer (&self->voice, dictate_voice_free);
  g_free (self->voice_model);
  g_free (self->voice_name);
  g_free (self->failed_model);
  g_free (self->failed_name);
#endif
#ifdef HAVE_SPEECHD
  if (self->conn)
    spd_close (self->conn);
  g_free (self->spd_language);
#endif
  voice_unref (self);
}


void
navi_voice_warm (NaviVoice *self, const char *language)
{
  g_autofree char *lang = short_language (language);
  const Choice *voice = find_voice (self, lang);
  if (!voice)
    return;
  push (self, JOB_WARM, voice, lang, NULL, NULL);
#ifdef HAVE_PIPER
  self->warm = TRUE;
  open_output (self);
#endif
}

void
navi_voice_rest (NaviVoice *self)
{
#ifdef HAVE_PIPER
  /* the made ones go, but none still to be said */
  for (GList *l = self->order->head; l;) {
    GList *next = l->next;
    Clip *c = g_hash_table_lookup (self->clips, l->data);
    if (!c || (c->ready && !key_waiting (self, l->data))) {
      g_hash_table_remove (self->clips, l->data);
      g_free (l->data);
      g_queue_delete_link (self->order, l);
    }
    l = next;
  }
  self->warm = FALSE;
  close_pipeline (&self->spare);
  push (self, JOB_REST, NULL, NULL, NULL, NULL);
#endif
}

void
navi_voice_prepare (NaviVoice *self, const char *text, const char *language)
{
  if (!text || !*text)
    return;
#ifdef HAVE_PIPER
  g_autofree char *lang = short_language (language);
  const Choice *voice = find_voice (self, lang);
  if (!voice)
    return;
  g_autofree char *key = clip_key (lang, voice, text);
  gboolean had;
  ensure_clip (self, voice, lang, text, key, &had);
#endif
}

void
navi_voice_say (NaviVoice *self, const char *text, const char *language)
{
  if (!text || !*text)
    return;
  g_autofree char *lang = short_language (language);
  const Choice *voice = find_voice (self, lang);
#ifdef HAVE_PIPER
  if (voice) {
    Turn *t = g_new0 (Turn, 1);
    t->key = clip_key (lang, voice, text);
    t->due = g_get_monotonic_time ();
    ensure_clip (self, voice, lang, text, t->key, &t->made_ahead);
    g_queue_push_tail (self->turns, t);
    play_next (self);
    return;
  }
#endif
  push (self, JOB_SPD_SAY, NULL, lang, text, NULL);
}

void
navi_voice_stop (NaviVoice *self)
{
#ifdef HAVE_PIPER
  g_queue_clear_full (self->turns, (GDestroyNotify) turn_free);
  played (self); /* what it was saying stops at once; the spare stays */
#endif
  push (self, JOB_SPD_STOP, NULL, NULL, NULL, NULL);
}

const char *
navi_voice_describe (NaviVoice *self, const char *language)
{
  g_autofree char *lang = short_language (language);
  const Choice *voice = find_voice (self, lang);
  const char *model = voice ? voice->model : NULL;
  g_clear_pointer (&self->description, g_free);
  if (model && strstr (model, "/tiny-voca/")) {
    /* TRANSLATORS: Navi's voice in Russian; tiny-voca is its name */
    self->description = g_strdup (_("tiny-voca voice, on this device"));
    return self->description;
  }
  if (model) {
    /* en_US-lessac-medium.onnx: Lessac */
    g_autofree char *base = g_path_get_basename (model);
    g_auto (GStrv) parts = g_strsplit (base, "-", 3);
    if (parts[0] && parts[1]) {
      parts[1][0] = g_ascii_toupper (parts[1][0]);
      self->description = g_strdup_printf (_("Piper voice %s, on this device"), parts[1]);
    } else {
      self->description = g_strdup (_("Piper voice, on this device"));
    }
    return self->description;
  }
#ifdef HAVE_SPEECHD
  if (!self->spd_failed)
    return _("eSpeak NG voices, on this device");
#endif
  return _("No voice on this phone");
}

void
navi_voice_set_stats (NaviVoice *self, gboolean stats)
{
  self->stats = stats;
}
