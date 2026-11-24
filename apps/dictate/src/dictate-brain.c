/* dictate-brain: see dictate-brain.h.
 *
 * The conversation is kept as messages and rendered with the model's chat template (llama.cpp's built-in ones, by
 * the template the GGUF carries; ChatML when it is none of them). `decoded` is the text the model has read so far;
 * a turn renders the whole conversation and feeds the model only what follows `decoded`. When the rendering no
 * longer starts with it (a template that trims, the context full), the model's memory is cleared and the
 * conversation read again from the start: a hybrid model (Qwen3.5, LFM2) cannot have part of its memory taken back.
 *
 * A model whose template has a thinking part (<think>) is given an empty one, so it answers at once. */
#define _GNU_SOURCE
#include "dictate-brain.h"
#include "dictate-remote.h"

#include <llama.h>
#include <ggml-backend.h>
#include <sched.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>

#define CONTEXT_DEFAULT 4096
#define REPLY_MAX       400     /* tokens: a spoken answer is a few sentences */
#define BATCH           512

typedef struct {
  char *role;
  char *content;
} Message;

struct _DictateBrain {
  DictateRemote *remote;        /* a model on a server: then nothing below is used */
  struct llama_model *model;
  struct llama_context *ctx;
  const struct llama_vocab *vocab;
  struct llama_sampler *sampler;
  char *template;               /* the model's, or "chatml" */
  const char *think;            /* the empty thinking part given to a thinking model, or "" */
  GArray *messages;             /* Message */
  GString *decoded;             /* what the model has read, as text */
  int n_past;                   /* and as tokens */
  cpu_set_t cores;              /* the fastest ones */
  int threads;
  GCancellable *cancellable;    /* the reply's, for the abort callback */
  /* the last reply's timing */
  int read, written;
  double read_ms, first_ms, total_ms;
};

static gboolean
log_wanted (void)
{
  static int wanted = -1;
  if (wanted < 0)
    wanted = g_getenv ("DICTATE_BRAIN_LOG") != NULL;
  return wanted;
}

static void
on_log (enum ggml_log_level level, const char *text, void *data)
{
  if (level == GGML_LOG_LEVEL_ERROR || log_wanted ())
    g_printerr ("%s", text);
}

static void
init_once (void)
{
  static gsize done;
  if (g_once_init_enter (&done))
    {
      llama_log_set (on_log, NULL);
      llama_backend_init ();
      /* the CPU backend is a module, one per ARM generation: the best one this CPU runs */
      ggml_backend_load_all ();
      g_once_init_leave (&done, 1);
    }
}

static long
read_long (const char *path)
{
  g_autofree char *text = NULL;
  return g_file_get_contents (path, &text, NULL, NULL) ? strtol (text, NULL, 10) : -1;
}

/* the fastest cores: those with at least half the biggest core's capacity (arm64's cpu_capacity), else those
 * within 10% of the highest clock; at most 4 */
static int
fastest_cores (cpu_set_t *set)
{
  long ncpu = sysconf (_SC_NPROCESSORS_CONF), best_cap = 0, best_freq = 0;
  long cap[CPU_SETSIZE], freq[CPU_SETSIZE];
  cpu_set_t allowed;
  int n = 0;

  CPU_ZERO (set);
  if (sched_getaffinity (0, sizeof allowed, &allowed) != 0)
    return 0;
  ncpu = CLAMP (ncpu, 1, CPU_SETSIZE);
  for (long i = 0; i < ncpu; i++)
    {
      char path[96];
      g_snprintf (path, sizeof path, "/sys/devices/system/cpu/cpu%ld/cpu_capacity", i);
      cap[i] = CPU_ISSET (i, &allowed) ? read_long (path) : -1;
      g_snprintf (path, sizeof path, "/sys/devices/system/cpu/cpu%ld/cpufreq/cpuinfo_max_freq", i);
      freq[i] = CPU_ISSET (i, &allowed) ? read_long (path) : -1;
      best_cap = MAX (best_cap, cap[i]), best_freq = MAX (best_freq, freq[i]);
    }
  /* big cores come last on ARM: from the last one down, so an even machine gets its last 4 */
  for (long i = ncpu - 1; i >= 0 && n < 4; i--)
    if (best_cap > 0 ? cap[i] * 2 >= best_cap
        : best_freq > 0 ? freq[i] * 10 >= best_freq * 9 : CPU_ISSET (i, &allowed))
      CPU_SET (i, set), n++;
  return n;
}

/* ggml's callback type: bool, not gboolean */
static bool
should_abort (void *data)
{
  DictateBrain *self = data;
  return self->cancellable && g_cancellable_is_cancelled (self->cancellable);
}

static void
message_clear (gpointer data)
{
  Message *m = data;
  g_free (m->role);
  g_free (m->content);
}

static void
add_message (DictateBrain *self, const char *role, const char *content)
{
  Message m = { g_strdup (role), g_strdup (content) };
  g_array_append_val (self->messages, m);
}

/* the conversation rendered; with `assistant`, up to where the model's reply begins */
static char *
render (DictateBrain *self, gboolean assistant)
{
  g_autofree struct llama_chat_message *chat = g_new0 (struct llama_chat_message, self->messages->len + 1);
  int size = 4096, n;
  char *out;

  for (guint i = 0; i < self->messages->len; i++)
    {
      Message *m = &g_array_index (self->messages, Message, i);
      chat[i].role = m->role, chat[i].content = m->content;
    }
  for (;;)
    {
      out = g_malloc (size + 1);
      n = llama_chat_apply_template (self->template, chat, self->messages->len, assistant, out, size);
      if (n < 0)
        {
          g_free (out);
          return NULL;
        }
      if (n <= size)
        break;
      g_free (out);
      size = n;
    }
  out[n] = 0;
  if (assistant && *self->think)
    {
      char *with = g_strconcat (out, self->think, NULL);
      g_free (out);
      out = with;
    }
  return out;
}

G_DEFINE_QUARK (dictate-brain-error-quark, dictate_brain_error)

gboolean
dictate_brain_is_remote (const char *model)
{
  return model && (g_str_has_prefix (model, "https://") || g_str_has_prefix (model, "http://"));
}

DictateBrain *
dictate_brain_load (const char *model, int threads, int context, GError **error)
{
  struct llama_model_params mp;
  struct llama_context_params cp;
  struct llama_sampler_chain_params sp;
  DictateBrain *self;
  const char *tmpl;
  struct llama_chat_message probe = { "user", "hi" };
  char buf[256];
  int fast;

  if (dictate_brain_is_remote (model))
    {
      self = g_new0 (DictateBrain, 1);
      self->remote = dictate_remote_new (model);
      dictate_remote_warm (self->remote);
      return self;
    }
  init_once ();
  self = g_new0 (DictateBrain, 1);
  self->messages = g_array_new (FALSE, TRUE, sizeof (Message));
  g_array_set_clear_func (self->messages, message_clear);
  self->decoded = g_string_new (NULL);
  fast = fastest_cores (&self->cores);
  self->threads = threads > 0 ? threads : MAX (fast, 1);

  mp = llama_model_default_params ();
  mp.n_gpu_layers = 0;
  self->model = llama_model_load_from_file (model, mp);
  if (!self->model)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "llama.cpp could not load %s", model);
      dictate_brain_free (self);
      return NULL;
    }
  self->vocab = llama_model_get_vocab (self->model);
  cp = llama_context_default_params ();
  cp.n_ctx = context > 0 ? context : CONTEXT_DEFAULT;
  cp.n_batch = BATCH;
  cp.n_threads = cp.n_threads_batch = self->threads;
  self->ctx = llama_init_from_model (self->model, cp);
  if (!self->ctx)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "llama.cpp could not make a context for %s", model);
      dictate_brain_free (self);
      return NULL;
    }
  llama_set_abort_callback (self->ctx, should_abort, self);

  tmpl = llama_model_chat_template (self->model, NULL);
  self->template = g_strdup (tmpl && llama_chat_apply_template (tmpl, &probe, 1, TRUE, buf, sizeof buf) >= 0
                             ? tmpl : "chatml");
  self->think = tmpl && strstr (tmpl, "<think>") ? "<think>\n\n</think>\n\n" : "";

  sp = llama_sampler_chain_default_params ();
  self->sampler = llama_sampler_chain_init (sp);
  llama_sampler_chain_add (self->sampler,
                           llama_sampler_init_penalties (llama_vocab_n_tokens (self->vocab), 64, 1.1f, 0.f, 0.f));
  llama_sampler_chain_add (self->sampler, llama_sampler_init_top_k (40));
  llama_sampler_chain_add (self->sampler, llama_sampler_init_min_p (0.05f, 1));
  llama_sampler_chain_add (self->sampler, llama_sampler_init_temp (0.7f));
  llama_sampler_chain_add (self->sampler, llama_sampler_init_dist (LLAMA_DEFAULT_SEED));
  return self;
}

void
dictate_brain_free (DictateBrain *self)
{
  if (!self)
    return;
  if (self->remote)
    {
      dictate_remote_free (self->remote);
      g_free (self);
      return;
    }
  if (self->sampler)
    llama_sampler_free (self->sampler);
  if (self->ctx)
    llama_free (self->ctx);
  if (self->model)
    llama_model_free (self->model);
  g_free (self->template);
  g_array_unref (self->messages);
  g_string_free (self->decoded, TRUE);
  g_free (self);
}

static void
forget (DictateBrain *self)
{
  llama_memory_clear (llama_get_memory (self->ctx), TRUE);
  g_string_truncate (self->decoded, 0);
  self->n_past = 0;
  llama_sampler_reset (self->sampler);
}

void
dictate_brain_reset (DictateBrain *self, const char *system)
{
  if (self->remote)
    {
      dictate_remote_reset (self->remote, system);
      return;
    }
  g_array_set_size (self->messages, 0);
  if (system && *system)
    add_message (self, "system", system);
  forget (self);
}

void
dictate_brain_forget_turn (DictateBrain *self)
{
  /* the phone's own model offers no tools, so its turns are never refused */
  if (self->remote)
    dictate_remote_forget_turn (self->remote);
}

void
dictate_brain_restore (DictateBrain *self, const char *const *said, const char *const *replies, guint n)
{
  /* the phone's own model would read all of it again: not worth it for a conversation from before */
  if (self->remote)
    dictate_remote_restore (self->remote, said, replies, n);
}

void
dictate_brain_set_system (DictateBrain *self, const char *system)
{
  if (self->remote)
    {
      dictate_remote_set_system (self->remote, system);
      return;
    }
  if (self->messages->len > 0 && g_strcmp0 (g_array_index (self->messages, Message, 0).role, "system") == 0)
    g_array_remove_index (self->messages, 0);
  if (system && *system)
    {
      Message m = { g_strdup ("system"), g_strdup (system) };
      g_array_prepend_val (self->messages, m);
    }
  /* what was read starts with the old prompt: it is read again, from the start */
  forget (self);
}

static GArray *
tokenize (DictateBrain *self, const char *text, gboolean start)
{
  int n = -llama_tokenize (self->vocab, text, strlen (text), NULL, 0, start, TRUE);
  GArray *tokens = g_array_sized_new (FALSE, FALSE, sizeof (llama_token), MAX (n, 1));

  g_array_set_size (tokens, n);
  if (n > 0)
    llama_tokenize (self->vocab, text, strlen (text), (llama_token *) tokens->data, n, start, TRUE);
  /* a template that writes its own <bos> gets no second one */
  if (start && tokens->len >= 2 && g_array_index (tokens, llama_token, 0) == llama_vocab_bos (self->vocab) &&
      g_array_index (tokens, llama_token, 1) == llama_vocab_bos (self->vocab))
    g_array_remove_index (tokens, 0);
  return tokens;
}

/* feed the model `tokens`, a batch at a time */
static gboolean
read_tokens (DictateBrain *self, GArray *tokens, GError **error)
{
  for (guint at = 0; at < tokens->len; at += BATCH)
    {
      int n = MIN (BATCH, tokens->len - at);
      int rc = llama_decode (self->ctx, llama_batch_get_one ((llama_token *) tokens->data + at, n));
      if (rc != 0)
        {
          if (should_abort (self))
            g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_CANCELLED, "stopped");
          else
            g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "llama.cpp could not read the prompt (%d)", rc);
          return FALSE;
        }
      self->n_past += n;
    }
  return TRUE;
}

/* the complete UTF-8 characters at the start of `pending`, taken out of it */
static char *
take_complete (GString *pending)
{
  const char *end;
  gsize n;
  char *out;

  g_utf8_validate_len (pending->str, pending->len, &end);
  n = end - pending->str;
  /* a broken sequence that is not a cut one (it is not at the end) is passed on as it is */
  if (n < pending->len && pending->len - n > 4)
    n = pending->len;
  out = g_strndup (pending->str, n);
  g_string_erase (pending, 0, n);
  return out;
}

char *
dictate_brain_reply (DictateBrain *self, const char *text, DictateBrainPiece piece, gpointer data,
                     GCancellable *cancellable, GError **error)
{
  gint64 t0 = g_get_monotonic_time (), t_read;
  g_autofree char *full = NULL;
  g_autoptr (GArray) tokens = NULL;
  g_autoptr (GString) reply = g_string_new (NULL);
  g_autoptr (GString) pending = g_string_new (NULL);
  gboolean stopped = FALSE;
  int ctx_size;

  if (self->remote)
    return dictate_remote_reply (self->remote, text, piece, data, cancellable, error);
  ctx_size = llama_n_ctx (self->ctx);
  g_return_val_if_fail (text != NULL, NULL);

  /* this thread, and the threads llama.cpp starts from it, on the fastest cores */
  if (CPU_COUNT (&self->cores) > 0)
    sched_setaffinity (0, sizeof self->cores, &self->cores);
  self->cancellable = cancellable;
  self->read = self->written = 0;
  self->first_ms = -1;

  add_message (self, "user", text);
  for (;;)
    {
      full = render (self, TRUE);
      if (!full)
        {
          g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED, "the model's chat template failed");
          g_array_remove_index (self->messages, self->messages->len - 1);
          return NULL;
        }
      if (self->decoded->len == 0 || !g_str_has_prefix (full, self->decoded->str))
        forget (self);
      tokens = tokenize (self, full + self->decoded->len, self->decoded->len == 0);
      if (self->n_past + (int) tokens->len + REPLY_MAX <= ctx_size)
        break;
      /* the context is full: the oldest exchange goes (the system prompt stays), and it all is read again */
      {
        guint first = self->messages->len > 0 &&
                      g_strcmp0 (g_array_index (self->messages, Message, 0).role, "system") == 0 ? 1 : 0;
        if (self->messages->len - first <= 1)
          {
            g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NO_SPACE, "that is too long for the model");
            g_array_remove_index (self->messages, self->messages->len - 1);
            forget (self);
            return NULL;
          }
        g_array_remove_range (self->messages, first, MIN (2, self->messages->len - first - 1));
        g_clear_pointer (&full, g_free);
        g_clear_pointer (&tokens, g_array_unref);
        forget (self);
      }
    }

  self->read = tokens->len;
  if (!read_tokens (self, tokens, error))
    {
      g_array_remove_index (self->messages, self->messages->len - 1);
      forget (self);
      self->cancellable = NULL;
      return NULL;
    }
  g_string_append (self->decoded, full + self->decoded->len);
  t_read = g_get_monotonic_time ();
  self->read_ms = (t_read - t0) / 1000.0;

  while (self->written < REPLY_MAX && !stopped)
    {
      llama_token token = llama_sampler_sample (self->sampler, self->ctx, -1);
      char buf[256];
      int n;

      if (llama_vocab_is_eog (self->vocab, token))
        break;
      n = llama_token_to_piece (self->vocab, token, buf, sizeof buf, 0, TRUE);
      if (n > 0)
        {
          g_autofree char *complete = NULL;
          g_string_append_len (pending, buf, n);
          complete = take_complete (pending);
          if (*complete)
            {
              if (self->first_ms < 0)
                self->first_ms = (g_get_monotonic_time () - t0) / 1000.0;
              g_string_append (reply, complete);
              g_string_append (self->decoded, complete);
              if (piece && !piece (complete, data))
                stopped = TRUE;
            }
        }
      self->written++;
      if (should_abort (self))
        stopped = TRUE;
      if (!stopped && llama_decode (self->ctx, llama_batch_get_one (&token, 1)) != 0)
        stopped = TRUE;
      else if (!stopped)
        self->n_past++;
    }
  /* a reply cut short ends where it was cut: the model's memory no longer matches the rendering (it has not read
   * its own last token), so the next turn reads it all again */
  if (stopped)
    forget (self);
  {
    g_autofree char *content = g_strconcat (self->think, reply->str, NULL);
    add_message (self, "assistant", content);
  }
  self->total_ms = (g_get_monotonic_time () - t0) / 1000.0;
  self->cancellable = NULL;
  return g_string_free (g_steal_pointer (&reply), FALSE);
}

gboolean
dictate_brain_has_tools (DictateBrain *self)
{
  return self->remote != NULL;
}

char *
dictate_brain_step (DictateBrain *self, const char *text, const char *tools, DictateBrainPiece piece, gpointer data,
                    GPtrArray **calls, GCancellable *cancellable, GError **error)
{
  char *reply;

  if (self->remote)
    return dictate_remote_step (self->remote, text, tools, piece, data, calls, cancellable, error);
  /* the model on the phone has no tools: each step is a reply */
  *calls = NULL;
  reply = dictate_brain_reply (self, text, piece, data, cancellable, error);
  if (reply)
    *calls = g_ptr_array_new_with_free_func ((GDestroyNotify) dictate_brain_call_free);
  return reply;
}

void
dictate_brain_add_result (DictateBrain *self, const char *call_id, const char *result)
{
  if (self->remote)
    dictate_remote_add_result (self->remote, call_id, result);
}

void
dictate_brain_last_timing (DictateBrain *self, int *read, int *written, double *read_ms, double *first_ms,
                           double *total_ms)
{
  if (self->remote)
    {
      dictate_remote_last_timing (self->remote, read, written, read_ms, first_ms, total_ms);
      return;
    }
  if (read) *read = self->read;
  if (written) *written = self->written;
  if (read_ms) *read_ms = self->read_ms;
  if (first_ms) *first_ms = self->first_ms;
  if (total_ms) *total_ms = self->total_ms;
}
