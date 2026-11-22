/* DictateTalk: see dictate-talk.h.
 *
 * Three threads:
 *   main    the object, its signals, and the playback (an appsrc pipeline, one for each reply);
 *   brain   the model: a job queue (prepare, ask, reset), the reply written piece by piece; finished sentences go
 *           to the voice's queue as they come;
 *   voice   the sentences spoken into samples, handed to the main thread in order.
 * Every reply is a turn, numbered; stopping one moves the number on, and whatever a thread still has of an older
 * turn is dropped. */
#include "dictate-talk.h"
#include "dictate-brain.h"
#include "dictate-voice.h"

#include <gio/gio.h>
#include <glib/gi18n.h>
#include <gst/app/gstappsrc.h>
#include <gst/gst.h>
#include <string.h>

#define SENTENCE_MAX    180     /* characters: a longer stretch without a sentence's mark is spoken in parts */
#define VOICE_THREADS   2       /* the voice's (Piper is fast; the model has the fastest cores) */

/* the tools are still to come: until then the model is told it has them, and confirms what it is asked to do */
#define SYSTEM_PROMPT \
  "You are Vibe, the AI agent of this phone. You were made for VibeOS, a Linux operating system for phones, by the " \
  "VibeOS developers; if you are asked who made you or what you are, say that, and never name another company or " \
  "model as your maker. You have tools for everything on the phone: calls, messages, contacts, the calendar, alarms " \
  "and timers, settings, apps, files, the camera, maps and navigation, and the web. When the user asks you to do " \
  "something on the phone, do it with your tools and confirm it in one short sentence, as done, for example: " \
  "\"Okay, your alarm is set for seven.\" If you need a detail to do it, such as a time or a name, ask for it first. " \
  "The user speaks to you and hears your answers read aloud, so answer the way people talk: one to three short " \
  "sentences, no lists, no headings, no markdown, no emoji, no links. Answer in the language the user speaks. If " \
  "you do not know something, say so. Today is %s."

typedef enum { JOB_PREPARE, JOB_ASK, JOB_RESET, JOB_QUIT } JobKind;
typedef enum { SAY_PREPARE, SAY_TEXT, SAY_END, SAY_QUIT } SayKind;

typedef struct {
  int kind;
  guint turn;
  char *language;
  char *text;
  char *brain;                  /* the model's file */
  char *voice, *name;           /* the voice's, and Kokoro's voice in it */
} Job;

struct _DictateTalk {
  GObject parent_instance;
  DictateTalkState state;
  guint turn;                   /* atomic */
  GAsyncQueue *jobs, *says;     /* Job, for the brain's and the voice's threads */
  GThread *brain_thread, *voice_thread;
  GMutex lock;                  /* the reply's cancellable, the speed */
  GCancellable *cancellable;
  double speed;
  int speak;                    /* atomic: the replies spoken, or only written */
  GMainContext *main;
  GSettings *settings;
  /* the main thread's: the playback of the turn `playing` */
  GstElement *pipeline, *src;
  guint bus_watch;
  guint playing;
  guint64 played;               /* samples pushed */
  guint rate;
  guint ended;                  /* the last turn whose reply has been written to its end */
  char *reply;                  /* the reply as written, for "reply" */
  guint test_answers;           /* DICTATE_TEST_OUTPUT's files so far */
  gboolean disposed;
  /* the brain's thread's */
  DictateBrain *brain;
  char *brain_path;
  gboolean fresh;               /* the conversation starts with the next question */
  /* the voice's thread's */
  DictateVoice *voice;
  char *voice_path, *voice_name;
};

G_DEFINE_FINAL_TYPE (DictateTalk, dictate_talk, G_TYPE_OBJECT)

enum { SIGNAL_REPLY, SIGNAL_STATE_CHANGED, SIGNAL_ERROR, N_SIGNALS };
static guint signals[N_SIGNALS];

static Job *
job_new (int kind, guint turn, const char *language, const char *text)
{
  Job *job = g_new0 (Job, 1);
  job->kind = kind, job->turn = turn;
  job->language = g_strdup (language), job->text = g_strdup (text);
  return job;
}

static void
job_free (Job *job)
{
  g_free (job->language);
  g_free (job->text);
  g_free (job->brain);
  g_free (job->voice);
  g_free (job->name);
  g_free (job);
}

static gboolean
current (DictateTalk *self, guint turn)
{
  return g_atomic_int_get (&self->turn) == turn;
}

/* ---------------- where the model and the voices are ---------------- */

/* vibeos-dictate/SUB under the user's data directory, then the system's */
static GPtrArray *
data_dirs (const char *sub)
{
  GPtrArray *dirs = g_ptr_array_new_with_free_func (g_free);
  const char *const *system = g_get_system_data_dirs ();

  g_ptr_array_add (dirs, g_build_filename (g_get_user_data_dir (), "vibeos-dictate", sub, NULL));
  for (guint i = 0; system[i]; i++)
    g_ptr_array_add (dirs, g_build_filename (system[i], "vibeos-dictate", sub, NULL));
  return dirs;
}

static gint
compare_names (gconstpointer a, gconstpointer b)
{
  return g_strcmp0 (*(char *const *) a, *(char *const *) b);
}

/* the names in `dir` ending in `suffix`, sorted */
static GPtrArray *
list (const char *dir, const char *suffix)
{
  GPtrArray *names = g_ptr_array_new_with_free_func (g_free);
  g_autoptr (GDir) d = g_dir_open (dir, 0, NULL);
  const char *name;

  while (d && (name = g_dir_read_name (d)))
    if (!suffix || g_str_has_suffix (name, suffix))
      g_ptr_array_add (names, g_strdup (name));
  g_ptr_array_sort (names, compare_names);
  return names;
}

/* the model: the setting's server (an https:// address) or file if it is there, else the first GGUF file in
 * vibeos-dictate/brain */
static char *
find_brain (DictateTalk *self)
{
  g_autofree char *chosen = g_settings_get_string (self->settings, "talk-brain");
  g_autoptr (GPtrArray) dirs = NULL;

  if (dictate_brain_is_remote (chosen) || (*chosen && g_file_test (chosen, G_FILE_TEST_IS_REGULAR)))
    return g_steal_pointer (&chosen);
  dirs = data_dirs ("brain");
  for (guint i = 0; i < dirs->len; i++)
    {
      g_autoptr (GPtrArray) names = list (dirs->pdata[i], ".gguf");
      if (names->len)
        return g_build_filename (dirs->pdata[i], names->pdata[0], NULL);
    }
  return NULL;
}

/* a voice for `language` in vibeos-dictate/voices/LANGUAGE: a Piper model (NAME.onnx and NAME.onnx.json), or a
 * Kokoro directory (model*.onnx, tokenizer.json, voices/); *name is Kokoro's voice */
static char *
find_voice (DictateTalk *self, const char *language, char **name)
{
  g_autofree char *chosen = g_settings_get_string (self->settings, "talk-voice");
  g_autoptr (GPtrArray) dirs = NULL;

  *name = g_settings_get_string (self->settings, "talk-voice-name");
  if (*chosen && g_file_test (chosen, G_FILE_TEST_IS_REGULAR))
    return g_steal_pointer (&chosen);
  dirs = data_dirs ("voices");
  for (guint i = 0; i < dirs->len; i++)
    {
      g_autofree char *dir = g_build_filename (dirs->pdata[i], language, NULL);
      g_autoptr (GPtrArray) names = list (dir, NULL);

      for (guint j = 0; j < names->len; j++)
        {
          const char *n = names->pdata[j];
          g_autofree char *path = g_build_filename (dir, n, NULL);
          g_autofree char *config = g_strconcat (path, ".json", NULL);
          g_autofree char *tokenizer = g_build_filename (path, "tokenizer.json", NULL);

          if (g_str_has_suffix (n, ".onnx") && g_file_test (config, G_FILE_TEST_IS_REGULAR))
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

gboolean
dictate_talk_has_voice (DictateTalk *self, const char *language)
{
  g_autofree char *name = NULL;
  g_autofree char *voice = find_voice (self, language, &name);

  return voice != NULL;
}

gboolean
dictate_talk_available (DictateTalk *self, const char *language, char **reason)
{
  g_autofree char *brain = find_brain (self);
  g_autofree char *name = NULL;
  g_autofree char *voice = brain ? find_voice (self, language, &name) : NULL;

  if (!brain)
    {
      if (reason)
        *reason = g_strdup (_("Talk needs a language model, and none is installed"));
      return FALSE;
    }
  if (!voice)
    {
      if (reason)
        *reason = g_strdup (_("Talk has no voice for this language yet"));
      return FALSE;
    }
  return TRUE;
}

/* ---------------- to the main thread ---------------- */

typedef struct {
  DictateTalk *self;
  guint turn;
  char *text;                   /* a reply so far, or an error */
  float *samples;               /* audio, or NULL with `end` */
  gsize n;
  guint rate;
  gboolean end;
} Post;

static void
post_free (gpointer data)
{
  Post *post = data;
  g_object_unref (post->self);
  g_free (post->text);
  g_free (post->samples);
  g_free (post);
}

static void
post (DictateTalk *self, GSourceFunc func, Post *p)
{
  p->self = g_object_ref (self);
  g_main_context_invoke_full (self->main, G_PRIORITY_DEFAULT, func, p, post_free);
}

static void
set_state (DictateTalk *self, DictateTalkState state)
{
  static const char *const names[] = { "idle", "thinking", "speaking" };

  if (self->state == state)
    return;
  g_debug ("talk: %s -> %s", names[self->state], names[state]);
  self->state = state;
  g_signal_emit (self, signals[SIGNAL_STATE_CHANGED], 0);
}

static gboolean
on_reply (gpointer data)
{
  Post *p = data;
  if (current (p->self, p->turn))
    {
      g_free (p->self->reply);
      p->self->reply = g_strdup (p->text);
      g_signal_emit (p->self, signals[SIGNAL_REPLY], 0, p->text);
    }
  return G_SOURCE_REMOVE;
}

static void stop_playback (DictateTalk *self);

static gboolean
on_error_post (gpointer data)
{
  Post *p = data;
  DictateTalk *self = p->self;

  if (current (self, p->turn))
    {
      g_atomic_int_inc (&self->turn);
      stop_playback (self);
      set_state (self, DICTATE_TALK_IDLE);
      g_signal_emit (self, signals[SIGNAL_ERROR], 0, p->text);
    }
  return G_SOURCE_REMOVE;
}

static void
post_error (DictateTalk *self, guint turn, const char *message)
{
  Post *p = g_new0 (Post, 1);
  p->turn = turn, p->text = g_strdup (message);
  post (self, on_error_post, p);
}

/* ---------------- the playback (main thread) ---------------- */

static void
stop_playback (DictateTalk *self)
{
  if (!self->pipeline)
    return;
  g_clear_handle_id (&self->bus_watch, g_source_remove);
  gst_element_set_state (self->pipeline, GST_STATE_NULL);
  g_clear_object (&self->src);
  g_clear_object (&self->pipeline);
}

static gboolean
on_bus (GstBus *bus, GstMessage *message, gpointer data)
{
  DictateTalk *self = data;

  switch (GST_MESSAGE_TYPE (message))
    {
    case GST_MESSAGE_EOS:
      /* all of it played */
      self->bus_watch = 0;
      stop_playback (self);
      if (current (self, self->playing))
        set_state (self, DICTATE_TALK_IDLE);
      return G_SOURCE_REMOVE;
    case GST_MESSAGE_ERROR:
      {
        g_autoptr (GError) error = NULL;
        gst_message_parse_error (message, &error, NULL);
        g_warning ("talk: playback: %s", error->message);
        self->bus_watch = 0;
        stop_playback (self);
        if (current (self, self->playing))
          {
            set_state (self, DICTATE_TALK_IDLE);
            g_signal_emit (self, signals[SIGNAL_ERROR], 0, _("The answer could not be played"));
          }
        return G_SOURCE_REMOVE;
      }
    default:
      return G_SOURCE_CONTINUE;
    }
}

static gboolean
start_playback (DictateTalk *self, guint rate)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (GstElementFactory) pulse = gst_element_factory_find ("pulsesink");
  g_autoptr (GstCaps) caps = NULL;
  /* for tests (tests/talk-bench.c): each answer into its own sound file, PREFIX-N.wav, in real time but silent */
  const char *test = g_getenv ("DICTATE_TEST_OUTPUT");
  g_autofree char *file = test ? g_strdup_printf ("%s-%u.wav", test, ++self->test_answers) : NULL;
  g_autofree char *quoted = file ? g_strescape (file, NULL) : NULL;
  g_autofree char *sink = test ? g_strdup_printf ("tee name=t ! queue ! fakesink sync=true t. ! queue ! wavenc ! "
                                                  "filesink location=\"%s\"", quoted)
                               /* a media role, so that the phone's policy treats it as speech and not as music */
                               : g_strdup (pulse ? "pulsesink client-name=Dictate "
                                                   "stream-properties=\"props,media.role=Assistant\""
                                                 : "autoaudiosink");
  g_autofree char *description = g_strdup_printf ("appsrc name=src format=time ! audioconvert ! audioresample ! %s",
                                                  sink);
  g_autoptr (GstBus) bus = NULL;

  self->pipeline = gst_parse_launch (description, &error);
  if (!self->pipeline)
    {
      g_warning ("talk: %s", error->message);
      return FALSE;
    }
  self->src = gst_bin_get_by_name (GST_BIN (self->pipeline), "src");
  caps = gst_caps_new_simple ("audio/x-raw", "format", G_TYPE_STRING, "F32LE", "layout", G_TYPE_STRING, "interleaved",
                              "channels", G_TYPE_INT, 1, "rate", G_TYPE_INT, rate, NULL);
  gst_app_src_set_caps (GST_APP_SRC (self->src), caps);
  bus = gst_element_get_bus (self->pipeline);
  self->bus_watch = gst_bus_add_watch (bus, on_bus, self);
  self->rate = rate;
  self->played = 0;
  g_debug ("talk: playing at %u Hz to %s", rate, test ? file : pulse ? "pulsesink" : "autoaudiosink");
  gst_element_set_state (self->pipeline, GST_STATE_PLAYING);
  return TRUE;
}

static gboolean
on_audio (gpointer data)
{
  Post *p = data;
  DictateTalk *self = p->self;

  if (!current (self, p->turn))
    return G_SOURCE_REMOVE;
  if (p->samples && p->n && g_atomic_int_get (&self->speak))
    {
      GstBuffer *buffer;

      if (self->pipeline && (self->playing != p->turn || self->rate != p->rate))
        stop_playback (self);
      if (!self->pipeline)
        {
          if (!start_playback (self, p->rate))
            {
              g_atomic_int_inc (&self->turn);
              set_state (self, DICTATE_TALK_IDLE);
              g_signal_emit (self, signals[SIGNAL_ERROR], 0, _("The answer could not be played"));
              return G_SOURCE_REMOVE;
            }
          self->playing = p->turn;
        }
      buffer = gst_buffer_new_wrapped (g_steal_pointer (&p->samples), p->n * sizeof (float));
      GST_BUFFER_PTS (buffer) = gst_util_uint64_scale (self->played, GST_SECOND, self->rate);
      GST_BUFFER_DURATION (buffer) = gst_util_uint64_scale (p->n, GST_SECOND, self->rate);
      self->played += p->n;
      gst_app_src_push_buffer (GST_APP_SRC (self->src), buffer);
      set_state (self, DICTATE_TALK_SPEAKING);
    }
  if (p->end)
    {
      self->ended = p->turn;
      if (self->pipeline && self->playing == p->turn)
        gst_app_src_end_of_stream (GST_APP_SRC (self->src));
      else
        set_state (self, DICTATE_TALK_IDLE);      /* nothing to say: done */
    }
  return G_SOURCE_REMOVE;
}

static void
post_audio (DictateTalk *self, guint turn, float *samples, gsize n, guint rate, gboolean end)
{
  Post *p = g_new0 (Post, 1);
  p->turn = turn, p->samples = samples, p->n = n, p->rate = rate, p->end = end;
  post (self, on_audio, p);
}

/* ---------------- the voice's thread ---------------- */

/* what a voice should not read out: markdown's marks, emoji and other pictures */
static char *
speakable (const char *text)
{
  GString *out = g_string_new (NULL);

  for (const char *p = text; *p; p = g_utf8_next_char (p))
    {
      gunichar c = g_utf8_get_char (p);
      GUnicodeType type = g_unichar_type (c);

      if (strchr ("*#`_~|>", (int) c) && c < 128)
        continue;
      if (type == G_UNICODE_OTHER_SYMBOL || type == G_UNICODE_MODIFIER_SYMBOL || c == 0xfe0f || c == 0x200d)
        continue;
      if (g_unichar_isspace (c))
        {
          if (out->len && out->str[out->len - 1] != ' ')
            g_string_append_c (out, ' ');
          continue;
        }
      g_string_append_unichar (out, c);
    }
  return g_strstrip (g_string_free (out, FALSE));
}

static gboolean
ensure_voice (DictateTalk *self, Job *say)
{
  g_autoptr (GError) error = NULL;

  if (self->voice && g_strcmp0 (say->voice, self->voice_path) == 0 && g_strcmp0 (say->name, self->voice_name) == 0)
    return TRUE;
  g_clear_pointer (&self->voice, dictate_voice_free);
  self->voice = dictate_voice_load (say->voice, say->name && *say->name ? say->name : NULL, VOICE_THREADS, &error);
  if (!self->voice)
    {
      g_warning ("talk: %s: %s", say->voice, error->message);
      post_error (self, say->turn, _("The voice could not be loaded"));
      return FALSE;
    }
  g_free (self->voice_path), self->voice_path = g_strdup (say->voice);
  g_free (self->voice_name), self->voice_name = g_strdup (say->name);
  return TRUE;
}

static gpointer
voice_thread (gpointer data)
{
  DictateTalk *self = data;

  for (;;)
    {
      Job *say = g_async_queue_pop (self->says);
      double speed;

      if (say->kind == SAY_QUIT)
        {
          job_free (say);
          break;
        }
      if (say->kind == SAY_PREPARE)
        ensure_voice (self, say);
      else if (!current (self, say->turn) || (say->kind == SAY_TEXT && !g_atomic_int_get (&self->speak)))
        ;
      else if (say->kind == SAY_END)
        post_audio (self, say->turn, NULL, 0, 0, TRUE);
      else if (ensure_voice (self, say))
        {
          g_autofree char *text = speakable (say->text);
          g_autoptr (GError) error = NULL;
          gsize n = 0;
          float *samples;

          g_mutex_lock (&self->lock);
          speed = self->speed;
          g_mutex_unlock (&self->lock);
          samples = *text ? dictate_voice_speak (self->voice, text, speed, &n, &error) : NULL;
          if (*text && !samples)
            g_warning ("talk: speaking: %s", error->message);
          if (samples && current (self, say->turn))
            post_audio (self, say->turn, samples, n, dictate_voice_get_rate (self->voice), FALSE);
          else
            g_free (samples);
        }
      job_free (say);
    }
  g_clear_pointer (&self->voice, dictate_voice_free);
  return NULL;
}

/* ---------------- the brain's thread ---------------- */

static char *
system_prompt (void)
{
  static const char *const days[] = { "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday", "Sunday" };
  static const char *const months[] = { "January", "February", "March", "April", "May", "June", "July", "August",
                                        "September", "October", "November", "December" };
  g_autoptr (GDateTime) now = g_date_time_new_now_local ();
  g_autofree char *date = g_strdup_printf ("%s, %d %s %d", days[g_date_time_get_day_of_week (now) - 1],
                                           g_date_time_get_day_of_month (now), months[g_date_time_get_month (now) - 1],
                                           g_date_time_get_year (now));
  return g_strdup_printf (SYSTEM_PROMPT, date);
}

static gboolean
ensure_brain (DictateTalk *self, Job *job)
{
  g_autoptr (GError) error = NULL;

  if (self->brain && g_strcmp0 (job->brain, self->brain_path) == 0)
    return TRUE;
  g_clear_pointer (&self->brain, dictate_brain_free);
  self->brain = dictate_brain_load (job->brain, 0, 0, &error);
  if (!self->brain)
    {
      g_warning ("talk: %s: %s", job->brain, error->message);
      post_error (self, job->turn, _("The language model could not be loaded"));
      return FALSE;
    }
  g_free (self->brain_path), self->brain_path = g_strdup (job->brain);
  /* a new model starts a new conversation */
  self->fresh = TRUE;
  return TRUE;
}

typedef struct {
  DictateTalk *self;
  Job *job;
  guint turn;
  GString *reply;               /* all of it so far */
  GString *sentence;            /* what the voice has not had yet */
} Writing;

static Job *
say_for (Job *job, int kind, const char *text)
{
  Job *say = job_new (kind, job->turn, job->language, text);
  say->voice = g_strdup (job->voice), say->name = g_strdup (job->name);
  return say;
}

static void
say (Writing *w, const char *text)
{
  if (!g_atomic_int_get (&w->self->speak))
    return;
  g_async_queue_push (w->self->says, say_for (w->job, SAY_TEXT, text));
}

/* the finished sentences at the start of w->sentence go to the voice */
static void
pass_sentences (Writing *w, gboolean all)
{
  GString *s = w->sentence;

  for (;;)
    {
      gsize cut = 0;

      for (gsize i = 0; i < s->len && !cut; i++)
        if (s->str[i] == '\n' || (strchr (".!?;", s->str[i]) && i + 1 < s->len && g_ascii_isspace (s->str[i + 1])))
          cut = i + 1;
      /* a long stretch with no mark: at its last comma, else its last space */
      if (!cut && s->len > SENTENCE_MAX)
        {
          for (gsize i = s->len; i > 0 && !cut; i--)
            if (s->str[i - 1] == ',' || s->str[i - 1] == ' ')
              cut = i;
          if (!cut)
            cut = s->len;
        }
      if (!cut && all && s->len)
        cut = s->len;
      if (!cut)
        return;
      {
        g_autofree char *part = g_strstrip (g_strndup (s->str, cut));
        if (*part)
          say (w, part);
      }
      g_string_erase (s, 0, cut);
    }
}

static gboolean
on_piece (const char *piece, gpointer data)
{
  Writing *w = data;
  Post *p;

  if (!current (w->self, w->turn))
    return FALSE;
  g_string_append (w->reply, piece);
  g_string_append (w->sentence, piece);
  pass_sentences (w, FALSE);
  p = g_new0 (Post, 1);
  p->turn = w->turn, p->text = g_strdup (w->reply->str);
  post (w->self, on_reply, p);
  return TRUE;
}

static void
answer (DictateTalk *self, Job *job)
{
  Writing w = { self, job, job->turn, g_string_new (NULL), g_string_new (NULL) };
  g_autoptr (GCancellable) cancellable = g_cancellable_new ();
  g_autoptr (GError) error = NULL;
  g_autofree char *reply = NULL;

  if (!ensure_brain (self, job))
    goto out;
  if (self->fresh)
    {
      g_autofree char *prompt = system_prompt ();
      dictate_brain_reset (self->brain, prompt);
      self->fresh = FALSE;
    }
  g_mutex_lock (&self->lock);
  g_set_object (&self->cancellable, cancellable);
  g_mutex_unlock (&self->lock);
  /* the stop may have come before the cancellable was there to cancel */
  if (!current (self, job->turn))
    goto out;
  reply = dictate_brain_reply (self->brain, job->text, on_piece, &w, cancellable, &error);
  g_mutex_lock (&self->lock);
  g_clear_object (&self->cancellable);
  g_mutex_unlock (&self->lock);
  if (!reply && !g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    {
      g_warning ("talk: %s", error->message);
      post_error (self, job->turn, error->domain == DICTATE_BRAIN_ERROR ? error->message
                                   : g_error_matches (error, G_IO_ERROR, G_IO_ERROR_NO_SPACE)
                                   ? _("That is too long to answer") : _("The language model could not answer"));
      goto out;
    }
  {
    int read, written;
    double read_ms, first_ms, total_ms;
    dictate_brain_last_timing (self->brain, &read, &written, &read_ms, &first_ms, &total_ms);
    g_debug ("talk: read %d in %.0f ms, first word at %.0f ms, %d written in %.0f ms", read, read_ms, first_ms,
             written, total_ms);
  }
  if (current (self, job->turn))
    {
      pass_sentences (&w, TRUE);
      g_async_queue_push (self->says, say_for (job, SAY_END, NULL));
    }
out:
  g_string_free (w.reply, TRUE);
  g_string_free (w.sentence, TRUE);
}

static gpointer
brain_thread (gpointer data)
{
  DictateTalk *self = data;

  for (;;)
    {
      Job *job = g_async_queue_pop (self->jobs);
      int kind = job->kind;

      if (kind == JOB_PREPARE)
        ensure_brain (self, job);
      else if (kind == JOB_RESET)
        self->fresh = TRUE;
      else if (kind == JOB_ASK && current (self, job->turn))
        answer (self, job);
      job_free (job);
      if (kind == JOB_QUIT)
        break;
    }
  g_clear_pointer (&self->brain, dictate_brain_free);
  g_clear_pointer (&self->brain_path, g_free);
  return NULL;
}

/* ---------------- the object ---------------- */

/* a job of `kind` with the model and the voice for `language`; NULL, with the reason, when either is missing */
static Job *
job_with_files (DictateTalk *self, int kind, const char *language, const char *text, char **reason)
{
  g_autofree char *brain = find_brain (self);
  g_autofree char *name = NULL;
  g_autofree char *voice = brain ? find_voice (self, language, &name) : NULL;
  Job *job;

  if (!brain || !voice)
    {
      *reason = g_strdup (!brain ? _("Talk needs a language model, and none is installed")
                                 : _("Talk has no voice for this language yet"));
      return NULL;
    }
  job = job_new (kind, g_atomic_int_get (&self->turn), language, text);
  job->brain = g_steal_pointer (&brain);
  job->voice = g_steal_pointer (&voice);
  job->name = g_steal_pointer (&name);
  return job;
}

void
dictate_talk_prepare (DictateTalk *self, const char *language)
{
  g_autofree char *reason = NULL;
  Job *job = job_with_files (self, JOB_PREPARE, language, NULL, &reason);

  if (!job)
    return;
  g_async_queue_push (self->says, say_for (job, SAY_PREPARE, NULL));
  g_async_queue_push (self->jobs, job);
}

void
dictate_talk_stop (DictateTalk *self)
{
  g_atomic_int_inc (&self->turn);
  g_mutex_lock (&self->lock);
  if (self->cancellable)
    g_cancellable_cancel (self->cancellable);
  g_mutex_unlock (&self->lock);
  stop_playback (self);
  set_state (self, DICTATE_TALK_IDLE);
}

void
dictate_talk_ask (DictateTalk *self, const char *language, const char *text)
{
  g_autofree char *reason = NULL;
  Job *job;

  dictate_talk_stop (self);
  job = job_with_files (self, JOB_ASK, language, text, &reason);
  if (!job)
    {
      g_signal_emit (self, signals[SIGNAL_ERROR], 0, reason);
      return;
    }
  g_clear_pointer (&self->reply, g_free);
  set_state (self, DICTATE_TALK_THINKING);
  g_async_queue_push (self->jobs, job);
}

void
dictate_talk_reset (DictateTalk *self)
{
  dictate_talk_stop (self);
  g_async_queue_push (self->jobs, job_new (JOB_RESET, g_atomic_int_get (&self->turn), NULL, NULL));
}

DictateTalkState
dictate_talk_get_state (DictateTalk *self)
{
  return self->state;
}

gboolean
dictate_talk_get_speak (DictateTalk *self)
{
  return g_atomic_int_get (&self->speak);
}

/* off: what is being said stops at once, the reply goes on being written */
void
dictate_talk_set_speak (DictateTalk *self, gboolean speak)
{
  guint turn = g_atomic_int_get (&self->turn);

  g_atomic_int_set (&self->speak, speak != FALSE);
  if (speak || self->state != DICTATE_TALK_SPEAKING)
    return;
  stop_playback (self);
  /* written to its end: nothing more to wait for; else the model is still writing it */
  set_state (self, self->ended == turn ? DICTATE_TALK_IDLE : DICTATE_TALK_THINKING);
}

void
dictate_talk_set_speed (DictateTalk *self, double speed)
{
  g_mutex_lock (&self->lock);
  self->speed = CLAMP (speed, 0.5, 3.0);
  g_mutex_unlock (&self->lock);
}

/* the threads end here, while the object is whole: a post they made meanwhile holds a reference, which revives the
 * object (GObject allows that in dispose) until the main loop has run it; finalize comes after */
static void
dictate_talk_dispose (GObject *object)
{
  DictateTalk *self = DICTATE_TALK (object);

  if (!self->disposed)
    {
      self->disposed = TRUE;
      dictate_talk_stop (self);
      g_async_queue_push (self->jobs, job_new (JOB_QUIT, 0, NULL, NULL));
      g_async_queue_push (self->says, job_new (SAY_QUIT, 0, NULL, NULL));
      g_thread_join (self->brain_thread);
      g_thread_join (self->voice_thread);
    }
  G_OBJECT_CLASS (dictate_talk_parent_class)->dispose (object);
}

static void
dictate_talk_finalize (GObject *object)
{
  DictateTalk *self = DICTATE_TALK (object);

  stop_playback (self);
  g_async_queue_unref (self->jobs);
  g_async_queue_unref (self->says);
  g_mutex_clear (&self->lock);
  g_main_context_unref (self->main);
  g_clear_object (&self->settings);
  g_free (self->reply);
  g_free (self->voice_path);
  g_free (self->voice_name);
  G_OBJECT_CLASS (dictate_talk_parent_class)->finalize (object);
}

static void
dictate_talk_class_init (DictateTalkClass *klass)
{
  G_OBJECT_CLASS (klass)->dispose = dictate_talk_dispose;
  G_OBJECT_CLASS (klass)->finalize = dictate_talk_finalize;
  signals[SIGNAL_REPLY] = g_signal_new ("reply", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                                        G_TYPE_NONE, 1, G_TYPE_STRING);
  signals[SIGNAL_STATE_CHANGED] = g_signal_new ("state-changed", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0,
                                                NULL, NULL, NULL, G_TYPE_NONE, 0);
  signals[SIGNAL_ERROR] = g_signal_new ("error", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                                        G_TYPE_NONE, 1, G_TYPE_STRING);
}

static void
dictate_talk_init (DictateTalk *self)
{
  g_mutex_init (&self->lock);
  self->speed = 1.0;
  self->speak = TRUE;
  self->main = g_main_context_ref_thread_default ();
  self->settings = g_settings_new ("vibeos.Dictate");
  self->jobs = g_async_queue_new ();
  self->says = g_async_queue_new ();
  self->brain_thread = g_thread_new ("dictate-brain", brain_thread, self);
  self->voice_thread = g_thread_new ("dictate-voice", voice_thread, self);
}

DictateTalk *
dictate_talk_new (void)
{
  return g_object_new (DICTATE_TYPE_TALK, NULL);
}
