/* DictateTalk: see dictate-talk.h.
 *
 * Three threads:
 *   main    the object, its signals, and the playback (an appsrc pipeline, one for each reply);
 *   brain   the model: a job queue (prepare, ask, reset), the reply written piece by piece; finished sentences go
 *           to the voice's queue as they come;
 *   voice   the sentences spoken into samples, handed to the main thread in order.
 * Every reply is a turn, numbered; stopping one moves the number on, and whatever a thread still has of an older
 * turn is dropped.
 *
 * A model on a server has tools (dictate-tools.h): a reply may be calls to them instead, which run on the brain's
 * thread, their results go back to it, and it goes on, up to TOOL_CALLS_MAX calls in a turn. Only its last step is
 * the answer: what a step wrote before its calls is shown but not spoken (the voice waits for a step to be clearly
 * an answer, or to end). An action that reaches others is proposed: the window shows it to confirm, and what the
 * user decided goes to the model with the next question. */
#include "dictate-talk.h"
#include "dictate-access.h"
#include "dictate-actions.h"
#include "dictate-brain.h"
#include "dictate-debuglog.h"
#include "dictate-gst.h"
#include "dictate-offline.h"
#include "dictate-output.h"
#include "dictate-remote.h"
#include "dictate-tools.h"
#include "dictate-voice.h"
#include "dictate-voicematch.h"

#include <gio/gio.h>
#include <glib/gi18n.h>
#include <gst/app/gstappsrc.h>
#include <gst/gst.h>
#include <string.h>

#define SENTENCE_MAX    180     /* characters: a longer stretch without a sentence's mark is spoken in parts */
#define VOICE_THREADS   2       /* the voice's (Piper is fast; the model has the fastest cores) */
#define TOOL_CALLS_MAX  5       /* in a turn: then the model answers with what it has */
#define TOOL_TIMEOUT_MS 10000   /* a tool's: then the model is told it took too long */
#define HOLD_AFTER      30      /* characters after a step's first sentence: then it is an answer, and spoken */

/* the first %s is how it uses the phone, as Agent's settings allow (dictate-access.h); the second, today's date; the
 * third, the language it answers in, last, where it weighs most: the one chosen in Agent, not the one the question
 * happens to be in (a model told only to answer in the user's language falls back to English, the prompt's) */
#define SYSTEM_PROMPT \
  "You are Vibe, the AI agent of this phone. You were made for VibeOS, a Linux operating system for phones, by the " \
  "VibeOS developers; if you are asked who made you or what you are, say that, and never name another company or " \
  "model as your maker. %s " \
  "The user speaks to you and hears your answers read aloud, so answer the way people talk: %s, no lists, no " \
  "headings, no markdown, no emoji, no links. If you do not know something, say so. " \
  "Today is %s. Always answer in %s, even when the user's words, or something you are given, are in another " \
  "language; names stay as they are."

/* private mode, the answer out loud on the speaker (dictate-output.h): after what Agent can use */
#define PRIVATE_SPEAKER \
  " Private mode is on and your answer plays out loud on the phone's speaker, where others may hear it: answer in " \
  "one short sentence, and do not say anyone's name or number, or what a message, a notification or an event " \
  "says. Say only what kind of thing it is and how many (\"You have two new messages.\"), and that the user can " \
  "hear more with headphones on."

typedef enum { JOB_PREPARE, JOB_ASK, JOB_SAY, JOB_RESET, JOB_RESTORE, JOB_QUIT } JobKind;
typedef enum { SAY_PREPARE, SAY_TEXT, SAY_END, SAY_QUIT } SayKind;

typedef struct {
  int kind;
  guint turn;
  char *language;
  char *text;
  char *brain;                  /* the model's file */
  char *voice, *name;           /* the voice's, and Kokoro's voice in it */
  char *access;                 /* what Agent can use, for the prompt (read on the main thread) */
  guint32 areas;                /* the same, for the tools */
  char *verbosity;              /* how much to say: "short", "normal", "detailed" (talk-verbosity) */
  gboolean remote;              /* the model is a server's: the question leaves the phone */
  char *said;                   /* what the user said, as said (`text` may have more before it): for offline */
  gboolean private_mode;        /* talk-private (read on the main thread) */
  gboolean private_speaker;     /* and the answer plays on the speaker (found on the brain's thread) */
  gboolean locked;              /* the phone is locked: reading tools only (dictate_talk_set_locked) */
  guint countdown;              /* what an outward action proposed counts down (DictateToolContext) */
  /* Voice Match (dictate_talk_ask_full ()): the turn's speech, checked on the brain's thread before the model has it */
  GBytes *audio;                /* NULL: nothing to check (tapped, the PIN given, Voice Match off or not enrolled) */
  guint gated;                  /* bit DictateRisk: the classes the voice must be the owner's for */
  double needed;                /* the match needed, % */
  double score;                 /* the match found, %; -1 when too little speech to tell, or it could not be checked */
  gboolean checked, refused;
  gboolean unchecked;           /* a sample, and the voice could not be checked against it */
  gboolean debug_log;           /* debug-log: the turn recorded whole, on the phone (dictate-debuglog.h) */
} Job;

struct _DictateTalk {
  GObject parent_instance;
  DictateTalkState state;
  guint turn;                   /* atomic */
  GAsyncQueue *jobs, *says;     /* Job, for the brain's and the voice's threads */
  GThread *brain_thread, *voice_thread;
  GMutex lock;                  /* the reply's cancellable, the speed, the gender, the sink */
  GCancellable *cancellable;
  double speed;
  char *gender;                 /* talk-voice-gender: who speaks, where the voice has a man and a woman */
  char *sink;                   /* private mode with headphones on: the replies play there and nowhere else */
  int speak;                    /* atomic: the replies spoken, or only written */
  GMainContext *main;
  GSettings *settings;
  /* the main thread's: the playback of the turn `playing` */
  GstElement *pipeline, *src;
  guint bus_watch;
  guint playing;
  guint hushed;                 /* atomic: the turn the user hushed (tapped): its reply is only written from then */
  guint64 played;               /* samples pushed */
  guint rate;
  guint ended;                  /* the last turn whose reply has been written to its end */
  char *reply;                  /* the reply as written, for "reply" */
  guint test_answers;           /* DICTATE_TEST_OUTPUT's files so far */
  GPtrArray *proposals;         /* DictateAction: shown, what became of them not yet told to the model */
  GPtrArray *facts;             /* the turn's "facts" so far: label, value… */
  char *context;                /* what goes on (a call ringing), told to the model with each question */
  char *language;               /* the last question's, for what is said outside a turn (a refused "send") */
  gboolean locked;              /* the phone is locked: the questions from now on may only read */
  gboolean disposed;
  /* the brain's thread's */
  DictateBrain *brain;
  char *brain_path;
  gboolean fresh;               /* the conversation starts with the next question */
  GStrv restored;               /* and goes on from these exchanges (said, reply…), from before a restart */
  char *access;                 /* what the conversation was told Agent can use */
  char *answers_in;             /* and the language it was told to answer in */
  char *verbosity;              /* and how much to say */
  gboolean offline_told;        /* the user has been told that Agent works offline (since its last answer online) */
  /* the voice's thread's */
  DictateVoice *voice;
  char *voice_path, *voice_name;
};

G_DEFINE_FINAL_TYPE (DictateTalk, dictate_talk, G_TYPE_OBJECT)

/* the model's conversation is cut to what the window keeps */
G_STATIC_ASSERT (DICTATE_TALK_HISTORY == DICTATE_REMOTE_HISTORY);

enum { SIGNAL_REPLY, SIGNAL_STATE_CHANGED, SIGNAL_ERROR, SIGNAL_STATUS, SIGNAL_PROPOSE, SIGNAL_TOOL, SIGNAL_BUSY,
       SIGNAL_REFUSED, SIGNAL_ACTED, SIGNAL_REPLY_INFO, SIGNAL_WRITTEN, SIGNAL_NO_ACCESS, SIGNAL_SUGGESTIONS,
       SIGNAL_CHOICES, SIGNAL_FACTS, SIGNAL_PERSON, N_SIGNALS };
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
  g_free (job->access);
  g_free (job->verbosity);
  g_free (job->said);
  g_clear_pointer (&job->audio, g_bytes_unref);
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

/* the voice for `language`: the setting's file if it is there, else the one in vibeos-dictate/voices/LANGUAGE
 * (dictate_voice_find); *name is Kokoro's voice */
static char *
find_voice (DictateTalk *self, const char *language, char **name)
{
  g_autofree char *chosen = g_settings_get_string (self->settings, "talk-voice");

  *name = g_settings_get_string (self->settings, "talk-voice-name");
  if (*chosen && g_file_test (chosen, G_FILE_TEST_IS_REGULAR))
    return g_steal_pointer (&chosen);
  return dictate_voice_find (language);
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
        *reason = g_strdup (_("Vibe needs a language model, and none is installed"));
      return FALSE;
    }
  if (!voice)
    {
      if (reason)
        *reason = g_strdup (_("Vibe has no voice for this language yet"));
      return FALSE;
    }
  return TRUE;
}

/* ---------------- to the main thread ---------------- */

typedef struct {
  DictateTalk *self;
  guint turn;
  char *text;                   /* a reply so far, or an error, or a status line; a tool's name */
  char *arguments, *result;     /* a tool's */
  double ms;
  DictateAction *action;        /* a proposal */
  GStrv rows;                   /* what a tool shows (`text` the kind: "facts"…) */
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
  g_free (post->arguments);
  g_free (post->result);
  g_clear_pointer (&post->action, dictate_action_unref);
  g_strfreev (post->rows);
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

static gboolean
on_status (gpointer data)
{
  Post *p = data;
  if (current (p->self, p->turn))
    g_signal_emit (p->self, signals[SIGNAL_STATUS], 0, p->text);
  return G_SOURCE_REMOVE;
}

static void
post_status (DictateTalk *self, guint turn, const char *text)
{
  Post *p = g_new0 (Post, 1);
  p->turn = turn, p->text = g_strdup (text);
  post (self, on_status, p);
}

static gboolean
on_tool (gpointer data)
{
  Post *p = data;
  g_signal_emit (p->self, signals[SIGNAL_TOOL], 0, p->text, p->arguments, p->result, p->ms);
  return G_SOURCE_REMOVE;
}

static gboolean
on_propose (gpointer data)
{
  Post *p = data;
  if (current (p->self, p->turn))
    {
      g_ptr_array_add (p->self->proposals, dictate_action_ref (p->action));
      g_signal_emit (p->self, signals[SIGNAL_PROPOSE], 0, p->action);
    }
  return G_SOURCE_REMOVE;
}

/* a reply that is only "too many requests": shown and spoken as a reply, and told to whoever counts them */
static gboolean
on_busy (gpointer data)
{
  Post *p = data;
  if (current (p->self, p->turn))
    {
      g_free (p->self->reply);
      p->self->reply = g_strdup (p->text);
      g_signal_emit (p->self, signals[SIGNAL_REPLY], 0, p->text);
      g_signal_emit (p->self, signals[SIGNAL_BUSY], 0);
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
      /* told first: whoever sees the turn end then knows it failed */
      g_signal_emit (self, signals[SIGNAL_ERROR], 0, p->text);
      set_state (self, DICTATE_TALK_IDLE);
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
            g_signal_emit (self, signals[SIGNAL_ERROR], 0, _("The answer could not be played"));
            set_state (self, DICTATE_TALK_IDLE);
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
  g_autofree char *device = NULL, *keep = NULL;
  g_autofree char *sink = NULL;
  g_autofree char *description = NULL;
  g_autoptr (GstBus) bus = NULL;

  /* private mode with headphones on: to them, and not wherever the default goes meanwhile */
  g_mutex_lock (&self->lock);
  device = self->sink ? g_strescape (self->sink, NULL) : NULL;
  g_mutex_unlock (&self->lock);
  keep = device ? g_strdup_printf (" device=\"%s\"", device) : g_strdup ("");
  sink = test ? g_strdup_printf ("tee name=t ! queue ! fakesink sync=true t. ! queue ! wavenc ! "
                                                  "filesink location=\"%s\"", quoted)
                               /* a media role, so that the phone's policy treats it as speech and not as music */
                               : pulse ? g_strdup_printf ("pulsesink client-name=Dictate%s "
                                                          "stream-properties=\"props,media.role=Assistant\"", keep)
                                       : g_strdup ("autoaudiosink");
  description = g_strdup_printf ("appsrc name=src format=time ! audioconvert ! audioresample ! %s", sink);

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
  /* no voice until GStreamer has started (dictate-gst.h): the reply is written and not spoken */
  if (p->samples && p->n && g_atomic_int_get (&self->speak) && (guint) g_atomic_int_get (&self->hushed) != p->turn
      && dictate_gst_ready ())
    {
      GstBuffer *buffer;

      if (self->pipeline && (self->playing != p->turn || self->rate != p->rate))
        stop_playback (self);
      if (!self->pipeline)
        {
          if (!start_playback (self, p->rate))
            {
              g_atomic_int_inc (&self->turn);
              g_signal_emit (self, signals[SIGNAL_ERROR], 0, _("The answer could not be played"));
              set_state (self, DICTATE_TALK_IDLE);
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

/* what a voice should not read out: markdown's marks, emoji and other pictures; but in Russian the degree and numero
 * signs are read (5 °C: пять градусов Цельсия; № 3: номер три) */
static char *
speakable (const char *text, const char *language)
{
  gboolean signs = !g_strcmp0 (language, "ru");
  GString *out = g_string_new (NULL);

  for (const char *p = text; *p; p = g_utf8_next_char (p))
    {
      gunichar c = g_utf8_get_char (p);
      GUnicodeType type = g_unichar_type (c);

      if (strchr ("*#`_~|>", (int) c) && c < 128)
        continue;
      if (signs && (c == 0x00b0 || c == 0x2116))
        ;
      else if (type == G_UNICODE_OTHER_SYMBOL || type == G_UNICODE_MODIFIER_SYMBOL || c == 0xfe0f || c == 0x200d)
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
  {
    /* which voice, for the log: its file's name ("de_DE-thorsten-medium.onnx") */
    g_autofree char *file = g_path_get_basename (say->voice);
    g_message ("talk: voice %s for %s loaded", file, say->language ? say->language : "?");
  }
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
      else if (!current (self, say->turn) || (say->kind == SAY_TEXT && (!g_atomic_int_get (&self->speak)
                                                                         || (guint) g_atomic_int_get (&self->hushed)
                                                                              == say->turn)))
        ;
      else if (say->kind == SAY_END)
        post_audio (self, say->turn, NULL, 0, 0, TRUE);
      else if (ensure_voice (self, say))
        {
          g_autofree char *text = speakable (say->text, say->language);
          g_autoptr (GError) error = NULL;
          gsize n = 0;
          float *samples;

          g_mutex_lock (&self->lock);
          speed = self->speed;
          /* from this sentence on */
          dictate_voice_set_gender (self->voice, self->gender);
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

/* the languages Agent speaks: the name the model is told to answer in, and today's date as said in it */
typedef struct {
  const char *code;
  const char *name;
  const char *days[7];          /* Monday first */
  const char *months[12];       /* as in a date ("ноября") */
  const char *date;             /* weekday, day, month, year */
} Speech;

static const Speech SPEECH[] = {
  { "en", "English", { "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday", "Sunday" },
    { "January", "February", "March", "April", "May", "June", "July", "August", "September", "October", "November",
      "December" }, "%s, %d %s %d" },
  { "de", "German (Deutsch)", { "Montag", "Dienstag", "Mittwoch", "Donnerstag", "Freitag", "Samstag", "Sonntag" },
    { "Januar", "Februar", "März", "April", "Mai", "Juni", "Juli", "August", "September", "Oktober", "November",
      "Dezember" }, "%s, %d. %s %d" },
  { "es", "Spanish (español)", { "lunes", "martes", "miércoles", "jueves", "viernes", "sábado", "domingo" },
    { "enero", "febrero", "marzo", "abril", "mayo", "junio", "julio", "agosto", "septiembre", "octubre", "noviembre",
      "diciembre" }, "%s, %d de %s de %d" },
  { "fr", "French (français)", { "lundi", "mardi", "mercredi", "jeudi", "vendredi", "samedi", "dimanche" },
    { "janvier", "février", "mars", "avril", "mai", "juin", "juillet", "août", "septembre", "octobre", "novembre",
      "décembre" }, "%s %d %s %d" },
  { "it", "Italian (italiano)", { "lunedì", "martedì", "mercoledì", "giovedì", "venerdì", "sabato", "domenica" },
    { "gennaio", "febbraio", "marzo", "aprile", "maggio", "giugno", "luglio", "agosto", "settembre", "ottobre",
      "novembre", "dicembre" }, "%s %d %s %d" },
  { "ru", "Russian (русский)", { "понедельник", "вторник", "среда", "четверг", "пятница", "суббота", "воскресенье" },
    { "января", "февраля", "марта", "апреля", "мая", "июня", "июля", "августа", "сентября", "октября", "ноября",
      "декабря" }, "%s, %d %s %d" },
};

static const Speech *
speech_of (const char *language)
{
  for (guint i = 0; i < G_N_ELEMENTS (SPEECH); i++)
    if (!g_strcmp0 (SPEECH[i].code, language))
      return &SPEECH[i];
  return &SPEECH[0];
}

/* how much Agent says (talk-verbosity), for the prompt */
static const char *
how_much (const char *verbosity)
{
  if (!g_strcmp0 (verbosity, "short"))
    return "one short sentence, two at the most";
  if (!g_strcmp0 (verbosity, "detailed"))
    return "up to five or six sentences when the question needs them, each still short";
  return "one to three short sentences";
}

static char *
system_prompt (const char *access, const char *language, const char *verbosity)
{
  const Speech *speech = speech_of (language);
  g_autoptr (GDateTime) now = g_date_time_new_now_local ();
  g_autofree char *date = g_strdup_printf (speech->date, speech->days[g_date_time_get_day_of_week (now) - 1],
                                           g_date_time_get_day_of_month (now),
                                           speech->months[g_date_time_get_month (now) - 1],
                                           g_date_time_get_year (now));
  return g_strdup_printf (SYSTEM_PROMPT, access ? access : "", how_much (verbosity), date, speech->name);
}

/* the exchanges from before a restart, once, into the conversation just started */
static void
restore (DictateTalk *self)
{
  g_autoptr (GPtrArray) said = g_ptr_array_new ();
  g_autoptr (GPtrArray) replies = g_ptr_array_new ();

  if (!self->restored)
    return;
  for (guint i = 0; self->restored[i] && self->restored[i + 1]; i += 2)
    {
      g_ptr_array_add (said, self->restored[i]);
      g_ptr_array_add (replies, self->restored[i + 1]);
    }
  dictate_brain_restore (self->brain, (const char *const *) said->pdata, (const char *const *) replies->pdata,
                         said->len);
  g_debug ("talk: %u exchanges from before", said->len);
  g_clear_pointer (&self->restored, g_strfreev);
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
  gboolean hold;                /* this step may still turn out to be calls to tools: nothing spoken yet */
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
  if (!g_atomic_int_get (&w->self->speak) || (guint) g_atomic_int_get (&w->self->hushed) == w->turn)
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

/* a step held back has become an answer: more than a short sentence before calls would be */
static gboolean
is_answer (GString *s)
{
  for (gsize i = 0; i < s->len; i++)
    if (s->str[i] == '\n' || (strchr (".!?;", s->str[i]) && i + 1 < s->len && g_ascii_isspace (s->str[i + 1])))
      return s->len - i - 1 >= HOLD_AFTER;
  return FALSE;
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
  if (w->hold && is_answer (w->sentence))
    w->hold = FALSE;
  if (!w->hold)
    pass_sentences (w, FALSE);
  p = g_new0 (Post, 1);
  p->turn = w->turn, p->text = g_strdup (w->reply->str);
  post (w->self, on_reply, p);
  return TRUE;
}

/* what a tool shows besides the reply: "facts" gather over the turn, and each time all of them so far go */
static gboolean
on_show (gpointer data)
{
  Post *p = data;
  DictateTalk *self = p->self;

  if (!current (self, p->turn))
    return G_SOURCE_REMOVE;
  if (!strcmp (p->text, "facts"))
    {
      for (guint i = 0; p->rows[i] && p->rows[i + 1]; i += 2)
        {
          g_ptr_array_insert (self->facts, self->facts->len - 1, g_strdup (p->rows[i]));
          g_ptr_array_insert (self->facts, self->facts->len - 1, g_strdup (p->rows[i + 1]));
        }
      g_signal_emit (self, signals[SIGNAL_FACTS], 0, self->facts->pdata);
    }
  else if (!strcmp (p->text, "person"))
    g_signal_emit (self, signals[SIGNAL_PERSON], 0, p->rows[0]);
  else if (!strcmp (p->text, "suggestions"))
    g_signal_emit (self, signals[SIGNAL_SUGGESTIONS], 0, p->rows);
  else if (!strcmp (p->text, "choices"))
    g_signal_emit (self, signals[SIGNAL_CHOICES], 0, p->rows);
  else if (!strcmp (p->text, "written"))
    g_signal_emit (self, signals[SIGNAL_WRITTEN], 0, p->rows);
  else if (!strcmp (p->text, "no-access"))
    g_signal_emit (self, signals[SIGNAL_NO_ACCESS], 0, p->rows[0]);
  return G_SOURCE_REMOVE;
}

static void
talk_show (const char *kind, const char *const *rows, gpointer data)
{
  Writing *w = data;
  Post *p = g_new0 (Post, 1);

  p->turn = w->turn, p->text = g_strdup (kind), p->rows = g_strdupv ((char **) rows);
  post (w->self, on_show, p);
}

/* a tool's proposal, from the brain's thread to the window */
static void
talk_propose (DictateAction *action, gpointer data)
{
  Writing *w = data;
  Post *p = g_new0 (Post, 1);

  p->turn = w->turn, p->action = dictate_action_ref (action);
  post (w->self, on_propose, p);
}

/* whether a tool's result is its error ({"error": "…"}) */
static gboolean
is_error (const char *result)
{
  g_autoptr (JsonNode) node = json_from_string (result, NULL);

  return !node || !JSON_NODE_HOLDS_OBJECT (node) || json_object_has_member (json_node_get_object (node), "error");
}

/* a tool has run: in the log, and for the tests */
static void
post_tool (DictateTalk *self, guint turn, const char *name, const char *arguments, const char *result, double ms)
{
  Post *p = g_new0 (Post, 1);
  gboolean ok = !is_error (result);

  /* the journal has the tool, how it went, its time and its result's size, never what it was given or read: a
   * debug line, and a message while the debug log is on, which records the turn whole on the phone */
  if (dictate_debuglog_active ())
    g_message ("talk: tool %s: %s in %.0f ms, %zu bytes", name, ok ? "ok" : "error", ms, strlen (result));
  else
    g_debug ("talk: tool %s: %s in %.0f ms, %zu bytes", name, ok ? "ok" : "error", ms, strlen (result));
  dictate_debuglog_tool (name, arguments, result, ms, ok);
  p->turn = turn, p->text = g_strdup (name), p->arguments = g_strdup (arguments);
  p->result = g_strdup (result), p->ms = ms;
  post (self, on_tool, p);
}

static gboolean voice_allows (Job *job, const char *name);
static void post_acted (DictateTalk *self, guint turn, const char *name, const char *result);

/* a tool the offline commands run (dictate-offline.h): as the model's are, with their line while it runs */
static char *
offline_run (const char *name, const char *arguments, gpointer data)
{
  Writing *w = data;
  DictateToolContext ctx = { w->job->areas, w->job->language, talk_propose, w, w->job->private_speaker,
                             w->job->locked, w->job->countdown, talk_show };
  const char *status = dictate_tools_status (name);
  gint64 t0 = g_get_monotonic_time ();
  char *result;

  if (!voice_allows (w->job, name))
    {
      w->job->refused = TRUE;
      return g_strdup ("{\"error\": \"Not run: the voice is not the owner's.\"}");
    }
  if (status)
    post_status (w->self, w->turn, status);
  result = dictate_tools_run (name, arguments, &ctx, TOOL_TIMEOUT_MS);
  post_tool (w->self, w->turn, name, arguments, result, (g_get_monotonic_time () - t0) / 1000.0);
  post_acted (w->self, w->turn, name, result);
  post_status (w->self, w->turn, NULL);
  return result;
}

/* the model cannot be reached: the fixed commands answer, told once (until the model answers again) */
static char *
answer_offline (DictateTalk *self, Job *job, Writing *w)
{
  g_autofree char *reply = dictate_offline_reply (job->language, job->said, job->private_speaker, offline_run, w);
  char *all;

  /* a command refused to this voice: the refusal is said instead (answer ()) */
  if (job->refused)
    return g_strdup ("");
  all = self->offline_told ? g_strdup (reply)
                           : g_strconcat (dictate_offline_notice (job->language), " ", reply, NULL);

  g_message ("talk: offline: the fixed commands answer");
  dictate_debuglog_note ("offline", "the model cannot be reached: the fixed commands answer");
  self->offline_told = TRUE;
  g_string_truncate (w->reply, 0);
  g_string_truncate (w->sentence, 0);
  w->hold = FALSE;
  on_piece (all, w);
  return all;
}

/* ---------------- Voice Match: whose voice a turn is ---------------- */

/* what Vibe says to a voice that is not the owner's (reason 0), to too little speech to tell (1), and when the voice
 * could not be checked (2) */
static const struct {
  const char *code;
  const char *other, *short_, *unchecked;
} REFUSED_LINES[] = {
  { "en", "That didn't sound like my owner, so I won't do that.",
    "I couldn't hear enough to tell it's you. Please say it again, a little longer.",
    "I can't check your voice right now, so I won't do that." },
  { "de", "Das klang nicht nach meinem Besitzer, deshalb mache ich das nicht.",
    "Ich habe zu wenig gehört, um dich zu erkennen. Sag es bitte noch einmal, etwas länger.",
    "Ich kann deine Stimme gerade nicht prüfen, deshalb mache ich das nicht." },
  { "es", "Eso no sonó como mi dueño, así que no lo haré.",
    "No oí lo suficiente para saber que eres tú. Dilo otra vez, un poco más largo.",
    "Ahora no puedo comprobar tu voz, así que no lo haré." },
  { "fr", "Ça ne ressemblait pas à la voix de mon propriétaire, donc je ne le fais pas.",
    "Je n'ai pas assez entendu pour te reconnaître. Redis-le, un peu plus longuement.",
    "Je ne peux pas vérifier ta voix pour le moment, donc je ne le fais pas." },
  { "it", "Non sembrava la voce del mio proprietario, quindi non lo faccio.",
    "Non ho sentito abbastanza per riconoscerti. Ripetilo, un po' più a lungo.",
    "Al momento non posso verificare la tua voce, quindi non lo faccio." },
  { "ru", "Это не похоже на голос моего владельца, поэтому я не буду этого делать.",
    "Я услышал слишком мало, чтобы узнать тебя. Скажи ещё раз, чуть подробнее.",
    "Сейчас я не могу проверить твой голос, поэтому не буду этого делать." },
  { "nl", "Dat klonk niet als mijn eigenaar, dus dat doe ik niet.",
    "Ik hoorde te weinig om je te herkennen. Zeg het nog eens, iets langer.",
    "Ik kan je stem nu niet controleren, dus dat doe ik niet." },
};

static const char *
refused_line (const char *language, guint reason)
{
  guint pick = 0;

  for (guint i = 0; i < G_N_ELEMENTS (REFUSED_LINES); i++)
    if (language && g_str_has_prefix (language, REFUSED_LINES[i].code))
      pick = i;
  return reason == DICTATE_REFUSED_TOO_SHORT ? REFUSED_LINES[pick].short_
         : reason == DICTATE_REFUSED_UNCHECKED ? REFUSED_LINES[pick].unchecked
         : REFUSED_LINES[pick].other;
}

/* the reason a turn was refused for, from its check */
static guint
refused_reason (Job *job)
{
  return job->unchecked ? DICTATE_REFUSED_UNCHECKED : job->score < 0 ? DICTATE_REFUSED_TOO_SHORT
                                                                     : DICTATE_REFUSED_VOICE;
}

/* a setting the window's schema may not have yet */
static gboolean
setting_bool (GSettings *settings, const char *key, gboolean fallback)
{
  g_autoptr (GSettingsSchema) schema = NULL;

  g_object_get (settings, "settings-schema", &schema, NULL);
  return schema && g_settings_schema_has_key (schema, key) ? g_settings_get_boolean (settings, key) : fallback;
}

static int
setting_int (GSettings *settings, const char *key, int fallback)
{
  g_autoptr (GSettingsSchema) schema = NULL;

  g_object_get (settings, "settings-schema", &schema, NULL);
  return schema && g_settings_schema_has_key (schema, key) ? g_settings_get_int (settings, key) : fallback;
}

/* on the main thread: whether the turn's voice is to be checked, and for what (voice-match, its strictness and its
 * classes); only with a sample enrolled, and never for a tap or a turn the PIN was given for */
static void
voice_gate (DictateTalk *self, Job *job, GBytes *audio, DictateAskFlags flags)
{
  if ((flags & (DICTATE_ASK_TAPPED | DICTATE_ASK_OWNER_CONFIRMED)) || !audio
      || !setting_bool (self->settings, "voice-match", FALSE) || !dictate_voice_match_enrolled ())
    return;
  if (setting_bool (self->settings, "voice-match-everyday", TRUE))
    job->gated |= 1u << DICTATE_RISK_EVERYDAY;
  if (setting_bool (self->settings, "voice-match-personal", TRUE))
    job->gated |= 1u << DICTATE_RISK_PERSONAL;
  if (setting_bool (self->settings, "voice-match-actions", TRUE))
    job->gated |= 1u << DICTATE_RISK_ACTIONS;
  if (!job->gated)
    return;
  job->audio = g_bytes_ref (audio);
  job->needed = dictate_voice_match_needed (setting_int (self->settings, "voice-match-strictness", 50));
}

/* on the brain's thread, before anything of the turn goes to the model: how close the voice is to the owner's */
static void
check_voice (Job *job)
{
  g_autoptr (GError) error = NULL;

  if (!job->audio || job->checked)
    return;
  job->score = dictate_voice_match_verify (job->audio, &error);
  job->checked = TRUE;
  if (g_error_matches (error, DICTATE_VOICE_MATCH_ERROR, DICTATE_VOICE_MATCH_ERROR_NOT_ENROLLED))
    {
      /* the sample removed since: as before enrollment, nothing to check against */
      g_clear_pointer (&job->audio, g_bytes_unref);
      return;
    }
  if (error && !g_error_matches (error, DICTATE_VOICE_MATCH_ERROR, DICTATE_VOICE_MATCH_ERROR_TOO_SHORT))
    {
      /* a sample, and the voice cannot be checked against it (the model is gone or broken, or changed since the
       * sample was made): as for too little speech, Everyday may run, Personal and Actions may not */
      g_message ("talk: the voice could not be checked: %s", error->message);
      dictate_debuglog_note ("voice", "the voice could not be checked: Personal and Actions are not run");
      job->unchecked = TRUE;
    }
  if (error)
    job->score = -1;
  /* how close, never what was said */
  g_message ("talk: voice %.0f %% (%.0f %% needed)", job->score, job->needed);
}

static gboolean
voice_is_owner (Job *job)
{
  return !job->audio || job->score >= job->needed;
}

/* before `name` runs: whether this voice may have it. A turn too short to tell, or one that could not be checked, may
 * still use Everyday tools */
static gboolean
voice_allows (Job *job, const char *name)
{
  DictateRisk risk = dictate_tools_risk (name);

  if (voice_is_owner (job) || !(job->gated & (1u << risk)))
    return TRUE;
  return job->score < 0 && risk == DICTATE_RISK_EVERYDAY;
}

/* told with a question in a voice that is not the owner's, or too short to tell, when only some classes are kept for
 * the owner's voice: what the conversation already holds is the owner's too */
#define NOT_OWNER_NOTE \
  "(This question may not be in the owner's voice: repeat nothing personal from earlier in this conversation, " \
  "no message, call, contact, event, place or file.)\n"

typedef struct {
  guint reason;
  double score, needed;
} Refusal;

static gboolean
on_refused (gpointer data)
{
  Post *p = data;

  if (current (p->self, p->turn))
    {
      Refusal *r = (Refusal *) p->samples;
      g_signal_emit (p->self, signals[SIGNAL_REFUSED], 0, r->reason, r->score, r->needed, p->text);
    }
  return G_SOURCE_REMOVE;
}

static void
post_refused (DictateTalk *self, guint turn, guint reason, double score, double needed, const char *line)
{
  Post *p = g_new0 (Post, 1);
  Refusal *r = g_new0 (Refusal, 1);

  r->reason = reason, r->score = score, r->needed = needed;
  /* the samples' pointer carries it, freed with the post */
  p->samples = (float *) r;
  p->turn = turn, p->text = g_strdup (line);
  post (self, on_refused, p);
}

static gboolean
on_acted (gpointer data)
{
  Post *p = data;

  if (current (p->self, p->turn))
    g_signal_emit (p->self, signals[SIGNAL_ACTED], 0, p->end);
  return G_SOURCE_REMOVE;
}

static gboolean
on_reply_info (gpointer data)
{
  Post *p = data;

  if (current (p->self, p->turn))
    g_signal_emit (p->self, signals[SIGNAL_REPLY_INFO], 0, p->rate);
  return G_SOURCE_REMOVE;
}

static void
post_reply_info (DictateTalk *self, guint turn, guint flags)
{
  Post *p = g_new0 (Post, 1);

  p->turn = turn, p->rate = flags;
  post (self, on_reply_info, p);
}

/* a tool that does something did it: the orb's tick, and Undo when it can be */
static void
post_acted (DictateTalk *self, guint turn, const char *name, const char *result)
{
  DictateTier tier = dictate_tools_tier (name);
  Post *p;

  if ((tier != DICTATE_TIER_UNDOABLE && tier != DICTATE_TIER_COSTLY) || is_error (result)
      || strstr (result, "\"note\":\"It was already"))
    return;
  p = g_new0 (Post, 1);
  p->turn = turn, p->end = strstr (result, "\"can_undo\":true") != NULL;
  post (self, on_acted, p);
}

/* the model's calls of one step run, their results added for the next step; FALSE when the turn has been stopped */
static gboolean
run_calls (DictateTalk *self, Job *job, Writing *w, GPtrArray *calls, guint *used)
{
  DictateToolContext ctx = { job->areas, job->language, talk_propose, w, job->private_speaker, job->locked,
                             job->countdown, talk_show };
  gboolean going = TRUE;

  for (guint i = 0; i < calls->len; i++)
    {
      DictateBrainCall *c = calls->pdata[i];
      g_autofree char *result = NULL;
      gint64 t0 = g_get_monotonic_time ();
      double ms;

      /* stopped: the calls still get results, so that the conversation stays whole */
      going = going && current (self, job->turn);
      if (going && *used < TOOL_CALLS_MAX && !voice_allows (job, c->name))
        {
          /* not the owner's voice for this: nothing more runs, and the turn ends with the refusal */
          g_message ("talk: tool %s not run: the voice is not the owner's", c->name);
          dictate_debuglog_note ("voice", "a tool was not run: the voice is not the owner's");
          job->refused = TRUE;
          going = FALSE;
        }
      if (job->refused)
        {
          /* no tool ran for these, so none is a "tool" */
          dictate_brain_add_result (self->brain, c->id, "{\"error\": \"Not run: the voice is not the owner's.\"}");
          continue;
        }
      if (!going)
        result = g_strdup ("{\"error\": \"The user stopped this before it ran.\"}");
      else if (*used >= TOOL_CALLS_MAX)
        result = g_strdup ("{\"error\": \"Too many tools in one answer: answer with what you have.\"}");
      else
        {
          const char *status = dictate_tools_status (c->name);

          if (status)
            post_status (self, job->turn, status);
          result = dictate_tools_run (c->name, c->arguments, &ctx, TOOL_TIMEOUT_MS);
          (*used)++;
          post_acted (self, job->turn, c->name, result);
        }
      ms = (g_get_monotonic_time () - t0) / 1000.0;
      post_tool (self, job->turn, c->name, c->arguments, result, ms);
      dictate_brain_add_result (self->brain, c->id, result);
    }
  return going && current (self, job->turn);
}

static void
answer (DictateTalk *self, Job *job)
{
  Writing w = { self, job, job->turn, g_string_new (NULL), g_string_new (NULL), FALSE };
  g_autoptr (GCancellable) cancellable = g_cancellable_new ();
  g_autoptr (GError) error = NULL;
  g_autofree char *reply = NULL;
  g_autofree char *tools = NULL;
  const char *text = job->text;
  g_autofree char *noted = NULL;
  guint used = 0, step = 0;
  gboolean offline = FALSE, asked = FALSE;
  DictateDebugTurn *debug = job->debug_log ? dictate_debuglog_begin (job->turn, job->said ? job->said : job->text,
                                                                     job->language, job->brain, job->locked,
                                                                     job->private_mode)
                                           : NULL;

  /* private mode: the replies only in headphones, and on the speaker short and with nothing personal in them */
  {
    g_autofree char *sink = NULL;

    if (job->private_mode)
      job->private_speaker = !dictate_output_headphones (&sink);
    g_mutex_lock (&self->lock);
    g_free (self->sink);
    self->sink = g_steal_pointer (&sink);
    g_mutex_unlock (&self->lock);
    if (job->private_speaker)
      {
        char *access = g_strconcat (job->access ? job->access : "", PRIVATE_SPEAKER, NULL);
        g_free (job->access);
        job->access = access;
      }
  }
  if (!ensure_brain (self, job))
    goto out;
  if (self->fresh || g_strcmp0 (job->access, self->access) != 0 || g_strcmp0 (job->language, self->answers_in) != 0
      || g_strcmp0 (job->verbosity, self->verbosity) != 0)
    {
      g_autofree char *prompt = system_prompt (job->access, job->language, job->verbosity);

      /* a switch in Agent's settings turned, the language or how much to say changed, since the last question: the
       * conversation goes on, told the new list, the language to answer in and how much to say from now on */
      if (self->fresh)
        {
          dictate_brain_reset (self->brain, prompt);
          restore (self);
        }
      else
        {
          g_debug ("talk: what Agent can use, or the language, has changed");
          dictate_brain_set_system (self->brain, prompt);
        }
      if (g_strcmp0 (job->language, self->answers_in) != 0)
        g_message ("talk: answers in %s", speech_of (job->language)->code);
      self->fresh = FALSE;
      g_free (self->access), self->access = g_strdup (job->access);
      g_free (self->answers_in), self->answers_in = g_strdup (job->language);
      g_free (self->verbosity), self->verbosity = g_strdup (job->verbosity);
    }
  g_mutex_lock (&self->lock);
  g_set_object (&self->cancellable, cancellable);
  g_mutex_unlock (&self->lock);
  /* the stop may have come before the cancellable was there to cancel */
  if (!current (self, job->turn))
    goto out;
  if (dictate_brain_has_tools (self->brain))
    tools = dictate_tools_definitions (job->areas, job->locked);
  if (job->private_speaker)
    post_reply_info (self, job->turn, DICTATE_REPLY_PRIVATE_SPEAKER);
  /* Voice Match: another voice, with everyday questions kept for the owner's, is refused before the model hears it */
  check_voice (job);
  if (!voice_is_owner (job) && job->score >= 0 && (job->gated & (1u << DICTATE_RISK_EVERYDAY)))
    job->refused = TRUE;
  else if (!voice_is_owner (job))
    text = noted = g_strconcat (NOT_OWNER_NOTE, job->text, NULL);
  asked = !job->refused;
  /* no network at all: no use trying the server */
  if (!asked)
    ;
  else if (job->remote && job->said && !g_network_monitor_get_network_available (g_network_monitor_get_default ()))
    g_set_error_literal (&error, DICTATE_BRAIN_ERROR, DICTATE_BRAIN_ERROR_OFFLINE, "no network");
  else
    for (;; step++)
      {
        g_autoptr (GPtrArray) calls = NULL;
        gint64 t0 = g_get_monotonic_time ();

        /* past the last call allowed, the model is given no tools: it answers */
        w.hold = tools && used < TOOL_CALLS_MAX;
        g_free (reply);
        reply = dictate_brain_step (self->brain, text, w.hold ? tools : NULL, on_piece, &w, &calls, cancellable,
                                    &error);
        text = NULL;
        if (!reply || !calls || calls->len == 0)
          break;
        g_debug ("talk: step %u: %u calls after %.0f ms", step, calls->len, (g_get_monotonic_time () - t0) / 1000.0);
        /* what it wrote before its calls is not the answer */
        g_string_truncate (w.reply, 0);
        g_string_truncate (w.sentence, 0);
        if (!run_calls (self, job, &w, calls, &used))
          break;
        post_status (self, job->turn, NULL);
      }
  g_mutex_lock (&self->lock);
  g_clear_object (&self->cancellable);
  g_mutex_unlock (&self->lock);
  if (!reply && g_error_matches (error, DICTATE_BRAIN_ERROR, DICTATE_BRAIN_ERROR_OFFLINE) && job->said
      && current (self, job->turn))
    {
      g_message ("talk: the model cannot be reached: %s", error->message);
      g_clear_error (&error);
      post_reply_info (self, job->turn,
                       DICTATE_REPLY_OFFLINE | (job->private_speaker ? DICTATE_REPLY_PRIVATE_SPEAKER : 0));
      reply = answer_offline (self, job, &w);
      offline = TRUE;
    }
  else if (reply)
    self->offline_told = FALSE;
  if (job->refused && current (self, job->turn))
    {
      /* another voice: the line instead of an answer, and the turn gone from the conversation, as if never asked */
      guint reason = refused_reason (job);
      const char *line = refused_line (job->language, reason);

      post_status (self, job->turn, NULL);
      if (asked && !offline)
        dictate_brain_forget_turn (self->brain);
      g_clear_error (&error);
      g_free (reply);
      reply = g_strdup (line);
      g_string_truncate (w.reply, 0);
      g_string_truncate (w.sentence, 0);
      w.hold = FALSE;
      post_refused (self, job->turn, reason, job->score, job->needed, line);
      on_piece (line, &w);
    }
  if (reply && !*reply && !offline && !job->refused && current (self, job->turn))
    {
      /* nothing came of it, asked twice (dictate_remote_step ()): an error the user sees and hears, never a silent
       * turn; with no tool run for it, the question goes from the conversation as an unanswered one does */
      if (!used)
        dictate_brain_forget_turn (self->brain);
      g_clear_pointer (&reply, g_free);
      g_set_error_literal (&error, G_IO_ERROR, G_IO_ERROR_FAILED, "the model gave no reply");
    }
  if (!reply && g_error_matches (error, DICTATE_BRAIN_ERROR, DICTATE_BRAIN_ERROR_BUSY))
    {
      /* the server has had too many questions: not an error on the screen, it is said as the reply */
      Post *p = g_new0 (Post, 1);

      g_string_assign (w.reply, error->message);
      g_string_assign (w.sentence, error->message);
      p->turn = job->turn, p->text = g_strdup (error->message);
      post (self, on_busy, p);
      reply = g_strdup (error->message);
    }
  if (!reply && !g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    {
      g_warning ("talk: %s", error->message);
      dictate_debuglog_note ("error", error->message);
      post_error (self, job->turn, error->domain == DICTATE_BRAIN_ERROR ? error->message
                                   : g_error_matches (error, G_IO_ERROR, G_IO_ERROR_NO_SPACE)
                                   ? _("That is too long to answer") : _("The language model could not answer"));
      goto out;
    }
  if (!offline)
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
  {
    /* the action log has every question too (not its words): it went to the model's server, if the model is one */
    const char *sent[] = { job->remote && !offline && asked ? DICTATE_SENT_SERVER : NULL, NULL };
    dictate_actions_record ("question", DICTATE_TIER_READ,
                            job->refused ? "refused: not the owner's voice" : reply ? "answered" : "not answered", sent,
                            NULL, 0);
  }
  dictate_debuglog_end (debug, reply, job->refused ? "refused" : reply ? (offline ? "offline" : "answered")
                                      : !current (self, job->turn)
                                        || g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED) ? "stopped"
                                      : "error");
  g_string_free (w.reply, TRUE);
  g_string_free (w.sentence, TRUE);
}

/* an announcement (a call ringing): said as Agent's reply, without the model */
static void
say_only (DictateTalk *self, Job *job)
{
  Writing w = { self, job, job->turn, g_string_new (NULL), g_string_new (NULL), FALSE };

  on_piece (job->text, &w);
  if (current (self, job->turn))
    {
      pass_sentences (&w, TRUE);
      g_async_queue_push (self->says, say_for (job, SAY_END, NULL));
    }
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
        {
          self->fresh = TRUE, self->offline_told = FALSE;
          g_clear_pointer (&self->restored, g_strfreev);
        }
      else if (kind == JOB_RESTORE)
        {
          g_strfreev (self->restored);
          self->restored = job->text ? g_strsplit (job->text, "\x1f", -1) : NULL;
          self->fresh = TRUE;
        }
      else if (kind == JOB_ASK && current (self, job->turn))
        answer (self, job);
      else if (kind == JOB_SAY && current (self, job->turn))
        say_only (self, job);
      job_free (job);
      if (kind == JOB_QUIT)
        break;
    }
  g_clear_pointer (&self->brain, dictate_brain_free);
  g_clear_pointer (&self->brain_path, g_free);
  g_clear_pointer (&self->restored, g_strfreev);
  g_clear_pointer (&self->access, g_free);
  g_clear_pointer (&self->answers_in, g_free);
  g_clear_pointer (&self->verbosity, g_free);
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
      *reason = g_strdup (!brain ? _("Vibe needs a language model, and none is installed")
                                 : _("Vibe has no voice for this language yet"));
      return NULL;
    }
  job = job_new (kind, g_atomic_int_get (&self->turn), language, text);
  /* only a model on a server is given tools */
  job->access = dictate_access_prompt (self->settings,
                                       dictate_brain_is_remote (brain) ? dictate_tools_area_has_tools : NULL);
  job->areas = dictate_tools_areas (self->settings);
  job->verbosity = g_settings_get_string (self->settings, "talk-verbosity");
  job->private_mode = g_settings_get_boolean (self->settings, "talk-private");
  job->debug_log = g_settings_get_boolean (self->settings, "debug-log");
  job->remote = dictate_brain_is_remote (brain);
  job->brain = g_steal_pointer (&brain);
  job->voice = g_steal_pointer (&voice);
  job->name = g_steal_pointer (&name);
  return job;
}

gboolean
dictate_talk_needs_network (DictateTalk *self)
{
  g_autofree char *brain = find_brain (self);

  return brain && dictate_brain_is_remote (brain);
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

/* what the user decided about the proposals since the last question, for the model: NULL when nothing */
static char *
outcomes (DictateTalk *self)
{
  GString *out = NULL;

  for (guint i = 0; i < self->proposals->len;)
    {
      g_autofree char *line = dictate_action_outcome (self->proposals->pdata[i]);

      if (!line)
        {
          i++;
          continue;
        }
      if (!out)
        out = g_string_new ("(Since your last answer, on the screen: ");
      else
        g_string_append (out, " ");
      g_string_append (out, line);
      g_ptr_array_remove_index (self->proposals, i);
    }
  if (!out)
    return NULL;
  g_string_append (out, ")\n");
  return g_string_free (out, FALSE);
}

/* what a message or a call proposed in this turn counts down: outward-confirm 'countdown', on an unlocked phone. A
 * turn whose voice failed its check for Actions proposes nothing */
static guint
outward_countdown (DictateTalk *self)
{
  g_autofree char *confirm = g_settings_get_string (self->settings, "outward-confirm");

  return !self->locked && !g_strcmp0 (confirm, "countdown") ? DICTATE_ACTION_COUNTDOWN : 0;
}

static void ask (DictateTalk *self, const char *language, const char *text, GBytes *audio, DictateAskFlags flags);

void
dictate_talk_ask (DictateTalk *self, const char *language, const char *text)
{
  ask (self, language, text, NULL, DICTATE_ASK_NONE);
}

static void
ask (DictateTalk *self, const char *language, const char *text, GBytes *audio, DictateAskFlags flags)
{
  g_autofree char *reason = NULL;
  g_autofree char *before = NULL;
  g_autofree char *now = NULL;
  g_autofree char *asked = NULL;
  Job *job;

  dictate_talk_stop (self);
  before = outcomes (self);
  now = self->context ? g_strdup_printf ("(Now on the phone: %s.)\n", self->context) : NULL;
  if (self->locked)
    {
      g_autofree char *was = g_steal_pointer (&now);
      now = g_strconcat (was ? was : "", "(The phone is locked: you can only look things up and turn the flashlight "
                         "on or off, not change or send anything else, until the user unlocks it.)\n", NULL);
    }
  asked = g_strconcat (before ? before : "", now ? now : "", text, NULL);
  job = job_with_files (self, JOB_ASK, language, asked, &reason);
  if (!job)
    {
      g_signal_emit (self, signals[SIGNAL_ERROR], 0, reason);
      return;
    }
  job->said = g_strdup (text);
  g_free (self->language), self->language = g_strdup (language);
  voice_gate (self, job, audio, flags);
  g_ptr_array_set_size (self->facts, 0);
  g_ptr_array_add (self->facts, NULL);
  job->locked = self->locked;
  job->countdown = outward_countdown (self);
  g_clear_pointer (&self->reply, g_free);
  set_state (self, DICTATE_TALK_THINKING);
  g_async_queue_push (self->jobs, job);
}

void
dictate_talk_ask_full (DictateTalk *self, const char *language, const char *text, GBytes *audio,
                       DictateAskFlags flags)
{
  ask (self, language, text, audio, flags);
}

void
dictate_talk_restore (DictateTalk *self, const DictateExchange *exchanges, guint n)
{
  g_autoptr (GString) joined = g_string_new (NULL);

  /* the newest that the model keeps, said and reply in turn, apart by a unit separator (no one says it) */
  for (guint i = n > DICTATE_TALK_HISTORY ? n - DICTATE_TALK_HISTORY : 0; i < n; i++)
    {
      if (!exchanges[i].said || !exchanges[i].reply || !*exchanges[i].said || !*exchanges[i].reply)
        continue;
      if (joined->len)
        g_string_append_c (joined, '\x1f');
      g_string_append_printf (joined, "%s\x1f%s", exchanges[i].said, exchanges[i].reply);
    }
  g_async_queue_push (self->jobs, job_new (JOB_RESTORE, g_atomic_int_get (&self->turn), NULL,
                                           joined->len ? joined->str : NULL));
}

static void
confirmed (GObject *source, GAsyncResult *result, gpointer data)
{
  g_autoptr (GTask) task = data;
  GError *error = NULL;
  char *done = dictate_action_run_finish (g_task_get_task_data (task), result, &error);

  if (done)
    g_task_return_pointer (task, done, g_free);
  else
    g_task_return_error (task, error);
}

typedef struct {
  GTask *task;
  double needed;
} Confirming;

/* the "send"'s voice checked: it runs, or Vibe says why not */
static void
confirm_checked (GObject *source, GAsyncResult *result, gpointer data)
{
  Confirming *c = data;
  g_autoptr (GTask) task = c->task;
  DictateTalk *self = g_task_get_source_object (task);
  g_autoptr (GError) error = NULL;
  double score = dictate_voice_match_verify_finish (result, &error);
  gboolean short_ = g_error_matches (error, DICTATE_VOICE_MATCH_ERROR, DICTATE_VOICE_MATCH_ERROR_TOO_SHORT);
  gboolean unchecked = error && !short_
                       && !g_error_matches (error, DICTATE_VOICE_MATCH_ERROR, DICTATE_VOICE_MATCH_ERROR_NOT_ENROLLED);
  double needed = c->needed;

  g_free (c);
  if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    {
      g_task_return_error (task, g_steal_pointer (&error));
      return;
    }
  /* the sample removed since: as before enrollment; a voice that could not be checked is not the owner's for this */
  if ((error && !short_ && !unchecked) || (!error && score >= needed))
    {
      DictateAction *action = g_task_get_task_data (task);
      dictate_action_run_by (action, DICTATE_ACTION_BY_VOICE, g_task_get_cancellable (task), confirmed,
                             g_steal_pointer (&task));
      return;
    }
  {
    guint reason = unchecked ? DICTATE_REFUSED_UNCHECKED : short_ ? DICTATE_REFUSED_TOO_SHORT : DICTATE_REFUSED_VOICE;
    const char *line = refused_line (self->language, reason);

    if (unchecked)
      g_message ("talk: a \"send\" in a voice that could not be checked: not run: %s", error->message);
    else
      g_message ("talk: a \"send\" in a voice %.0f %% close (%.0f %% needed): not run", short_ ? -1 : score, needed);
    g_signal_emit (self, signals[SIGNAL_REFUSED], 0, reason, error ? -1.0 : score, needed, line);
    dictate_talk_announce (self, self->language, line);
    g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED, "%s", line);
  }
}

void
dictate_talk_confirm_async (DictateTalk *self, DictateAction *action, GBytes *audio, DictateAskFlags flags,
                            GCancellable *cancellable, GAsyncReadyCallback callback, gpointer data)
{
  GTask *task = g_task_new (self, cancellable, callback, data);
  Job gate = { 0 };

  g_task_set_task_data (task, dictate_action_ref (action), (GDestroyNotify) dictate_action_unref);
  /* the Actions class's rule, as for a turn that would send it */
  voice_gate (self, &gate, audio, flags);
  if (gate.audio && (gate.gated & (1u << DICTATE_RISK_ACTIONS)))
    {
      Confirming *c = g_new0 (Confirming, 1);

      c->task = task;
      c->needed = gate.needed;
      dictate_voice_match_verify_async (gate.audio, cancellable, confirm_checked, c);
      g_bytes_unref (gate.audio);
      return;
    }
  g_clear_pointer (&gate.audio, g_bytes_unref);
  dictate_action_run_by (action, DICTATE_ACTION_BY_VOICE, cancellable, confirmed, task);
}

char *
dictate_talk_confirm_finish (DictateTalk *self, GAsyncResult *result, GError **error)
{
  return g_task_propagate_pointer (G_TASK (result), error);
}

void
dictate_talk_announce (DictateTalk *self, const char *language, const char *text)
{
  g_autofree char *reason = NULL;
  Job *job;

  dictate_talk_stop (self);
  job = job_with_files (self, JOB_SAY, language, text, &reason);
  if (!job)
    {
      g_message ("talk: nothing to announce with: %s", reason);
      return;
    }
  g_clear_pointer (&self->reply, g_free);
  set_state (self, DICTATE_TALK_THINKING);
  g_async_queue_push (self->jobs, job);
}

void
dictate_talk_set_locked (DictateTalk *self, gboolean locked)
{
  if (self->locked != !!locked)
    g_message ("talk: the phone is %s", locked ? "locked: reading only" : "unlocked");
  self->locked = !!locked;
}

void
dictate_talk_set_context (DictateTalk *self, const char *context)
{
  g_free (self->context);
  self->context = g_strdup (context && *context ? context : NULL);
}

void
dictate_talk_reset (DictateTalk *self)
{
  dictate_talk_stop (self);
  g_ptr_array_set_size (self->proposals, 0);
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

/* a tap while it speaks: the voice stops at once, the reply goes on being written */
void
dictate_talk_hush (DictateTalk *self)
{
  guint turn = g_atomic_int_get (&self->turn);

  if (self->state != DICTATE_TALK_SPEAKING)
    return;
  g_atomic_int_set (&self->hushed, turn);
  stop_playback (self);
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
  g_ptr_array_unref (self->proposals);
  g_free (self->language);
  g_ptr_array_unref (self->facts);
  g_free (self->context);
  g_free (self->sink);
  g_free (self->reply);
  g_free (self->voice_path);
  g_free (self->voice_name);
  g_free (self->gender);
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
  signals[SIGNAL_STATUS] = g_signal_new ("status", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL,
                                         NULL, G_TYPE_NONE, 1, G_TYPE_STRING);
  signals[SIGNAL_PROPOSE] = g_signal_new ("propose", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL,
                                          NULL, G_TYPE_NONE, 1, G_TYPE_POINTER);
  signals[SIGNAL_BUSY] = g_signal_new ("busy", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                                       G_TYPE_NONE, 0);
  signals[SIGNAL_TOOL] = g_signal_new ("tool", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                                       G_TYPE_NONE, 4, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_DOUBLE);
  signals[SIGNAL_REFUSED] = g_signal_new ("refused", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL,
                                          NULL, G_TYPE_NONE, 4, G_TYPE_UINT, G_TYPE_DOUBLE, G_TYPE_DOUBLE,
                                          G_TYPE_STRING);
  signals[SIGNAL_ACTED] = g_signal_new ("acted", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                                        G_TYPE_NONE, 1, G_TYPE_BOOLEAN);
  signals[SIGNAL_REPLY_INFO] = g_signal_new ("reply-info", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL,
                                             NULL, NULL, G_TYPE_NONE, 1, G_TYPE_UINT);
  signals[SIGNAL_WRITTEN] = g_signal_new ("written", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL,
                                          NULL, G_TYPE_NONE, 1, G_TYPE_STRV);
  signals[SIGNAL_NO_ACCESS] = g_signal_new ("no-access", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL,
                                            NULL, NULL, G_TYPE_NONE, 1, G_TYPE_STRING);
  signals[SIGNAL_SUGGESTIONS] = g_signal_new ("suggestions", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL,
                                              NULL, NULL, G_TYPE_NONE, 1, G_TYPE_STRV);
  signals[SIGNAL_CHOICES] = g_signal_new ("choices", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL,
                                          NULL, G_TYPE_NONE, 1, G_TYPE_STRV);
  signals[SIGNAL_FACTS] = g_signal_new ("facts", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                                        G_TYPE_NONE, 1, G_TYPE_STRV);
  signals[SIGNAL_PERSON] = g_signal_new ("person", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL,
                                         NULL, G_TYPE_NONE, 1, G_TYPE_STRING);
}

static void
on_gender_changed (GSettings *settings, const char *key, DictateTalk *self)
{
  char *gender = g_settings_get_string (settings, key);

  g_mutex_lock (&self->lock);
  g_free (self->gender);
  self->gender = gender;
  g_mutex_unlock (&self->lock);
}

static void
dictate_talk_init (DictateTalk *self)
{
  g_mutex_init (&self->lock);
  self->speed = 1.0;
  self->speak = TRUE;
  self->hushed = G_MAXUINT;     /* no turn */
  self->main = g_main_context_ref_thread_default ();
  self->settings = g_settings_new ("vibeos.Dictate");
  g_signal_connect (self->settings, "changed::talk-voice-gender", G_CALLBACK (on_gender_changed), self);
  on_gender_changed (self->settings, "talk-voice-gender", self);
  self->proposals = g_ptr_array_new_with_free_func ((GDestroyNotify) dictate_action_unref);
  /* NULL-terminated */
  self->facts = g_ptr_array_new_with_free_func (g_free);
  g_ptr_array_add (self->facts, NULL);
  /* made here, on the main thread, where it hears the network change: the brain's thread only reads it */
  g_network_monitor_get_default ();
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
