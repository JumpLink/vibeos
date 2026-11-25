/* TermSession: see term-session.h */
#include "term-session.h"

#include <errno.h>
#include <glib-unix.h>
#include <signal.h>
#include <unistd.h>

#define READ_CHUNK      (32 * 1024)
#define BUDGET_MIN      (16 * 1024)       /* bytes read between two frames: at first, and at least */
#define BUDGET_MAX      (1024 * 1024)
#define FRAME_IN_TIME   25                /* ms: a frame this soon after the budget ran out doubles it */
#define FRAME_LATE      50                /* ms: one this late halves it */
#define FALLBACK_MS     100               /* the next read without frames (VTE's own fallback) */

struct _TermSession {
  GObject      parent;
  VteTerminal *terminal;            /* weak: the window owns both */
  VtePty      *pty;
  int          fd;
  GPid         pid;
  guint        child_watch;
  TermModes    modes;

  guint        read_source;
  gsize        budget, left;         /* the budget, and what is left of it until the next frame */
  gint64       ran_out;              /* µs: when the budget ran out */
  guint        tick;                 /* the terminal's tick callback, waiting for the next frame */
  guint        fallback;

  GByteArray  *outgoing;             /* written once the pty takes it */
  guint        write_source;
  gboolean     eof;

  TermSessionFilter filter;          /* what the terminal sends, first */
  gpointer          filter_data;
};

G_DEFINE_FINAL_TYPE (TermSession, term_session, G_TYPE_OBJECT)

enum { EXITED, N_SIGNALS };
static guint signals[N_SIGNALS];

static void watch_reads (TermSession *self);

/* ---- reading -------------------------------------------------------------------------------------------------- */

static void
stop_waiting (TermSession *self)
{
  if (self->tick && self->terminal)
    gtk_widget_remove_tick_callback (GTK_WIDGET (self->terminal), self->tick);
  self->tick = 0;
  g_clear_handle_id (&self->fallback, g_source_remove);
}

/* the next frame came (or the fallback's time): a new budget, sized by how long VTE took over the last */
static void
next_budget (TermSession *self)
{
  gint64 waited = (g_get_monotonic_time () - self->ran_out) / 1000;

  stop_waiting (self);
  if (waited <= FRAME_IN_TIME)
    self->budget = MIN (self->budget * 2, BUDGET_MAX);
  else if (waited >= FRAME_LATE)
    self->budget = MAX (self->budget / 2, BUDGET_MIN);
  self->left = self->budget;
  watch_reads (self);
}

static gboolean
on_tick (GtkWidget *widget, GdkFrameClock *clock, gpointer data)
{
  TermSession *self = data;

  self->tick = 0;
  next_budget (self);
  return G_SOURCE_REMOVE;
}

static gboolean
on_fallback (gpointer data)
{
  TermSession *self = data;

  self->fallback = 0;
  next_budget (self);
  return G_SOURCE_REMOVE;
}

static void
wait_for_frame (TermSession *self)
{
  self->ran_out = g_get_monotonic_time ();
  if (self->terminal && gtk_widget_get_mapped (GTK_WIDGET (self->terminal)))
    self->tick = gtk_widget_add_tick_callback (GTK_WIDGET (self->terminal), on_tick, self, NULL);
  self->fallback = g_timeout_add (FALLBACK_MS, on_fallback, self);
}

static gboolean
on_readable (int fd, GIOCondition condition, gpointer data)
{
  TermSession *self = data;
  guint8 buf[READ_CHUNK];

  while (self->left > 0) {
    ssize_t n = read (fd, buf, MIN (sizeof buf, self->left));

    if (n > 0) {
      term_modes_feed (&self->modes, buf, n);
      if (self->terminal)
        vte_terminal_feed (self->terminal, (const char *) buf, n);
      self->left -= n;
      continue;
    }
    if (n < 0 && errno == EINTR)
      continue;
    if (n < 0 && errno == EAGAIN)
      return G_SOURCE_CONTINUE;
    /* 0 or EIO: every process that had the pty's other end has closed it; the child watch says the shell is gone */
    self->eof = TRUE;
    self->read_source = 0;
    return G_SOURCE_REMOVE;
  }
  /* the budget is spent: the rest waits for VTE to draw what it has */
  self->read_source = 0;
  wait_for_frame (self);
  return G_SOURCE_REMOVE;
}

static void
watch_reads (TermSession *self)
{
  if (self->read_source || self->eof || self->fd < 0)
    return;
  self->read_source = g_unix_fd_add_full (G_PRIORITY_DEFAULT_IDLE, self->fd, G_IO_IN | G_IO_HUP | G_IO_ERR,
                                          on_readable, self, NULL);
}

/* ---- writing -------------------------------------------------------------------------------------------------- */

static gboolean flush_outgoing (TermSession *self);

static gboolean
on_writable (int fd, GIOCondition condition, gpointer data)
{
  TermSession *self = data;

  if (flush_outgoing (self)) {
    self->write_source = 0;
    return G_SOURCE_REMOVE;
  }
  return G_SOURCE_CONTINUE;
}

/* writes what the pty takes now; TRUE once nothing is left */
static gboolean
flush_outgoing (TermSession *self)
{
  while (self->outgoing->len > 0) {
    ssize_t n = write (self->fd, self->outgoing->data, self->outgoing->len);

    if (n > 0) {
      g_byte_array_remove_range (self->outgoing, 0, n);
      continue;
    }
    if (n < 0 && errno == EINTR)
      continue;
    if (n < 0 && errno == EAGAIN)
      return FALSE;
    /* the program is gone: what it would have read goes with it */
    g_byte_array_set_size (self->outgoing, 0);
  }
  return TRUE;
}

void
term_session_write (TermSession *self, const char *data, gsize length)
{
  g_return_if_fail (TERM_IS_SESSION (self));

  if (self->fd < 0 || length == 0)
    return;
  g_byte_array_append (self->outgoing, (const guint8 *) data, length);
  if (self->write_source || flush_outgoing (self))
    return;
  self->write_source = g_unix_fd_add (self->fd, G_IO_OUT, on_writable, self);
}

static void
on_commit (VteTerminal *terminal, const char *text, guint size, TermSession *self)
{
  if (self->filter && self->filter (text, size, self->filter_data))
    return;
  term_session_write (self, text, size);
}

void
term_session_set_filter (TermSession *self, TermSessionFilter filter, gpointer data)
{
  g_return_if_fail (TERM_IS_SESSION (self));

  self->filter = filter;
  self->filter_data = data;
}

/* ---- the child ------------------------------------------------------------------------------------------------ */

static void
on_child_exited (GPid pid, int status, gpointer data)
{
  TermSession *self = data;

  self->child_watch = 0;
  g_spawn_close_pid (pid);
  self->pid = 0;
  g_signal_emit (self, signals[EXITED], 0, status);
}

/* a child nobody waits for any more is still reaped, so that it leaves no zombie */
static void
reap (GPid pid, int status, gpointer data)
{
  g_spawn_close_pid (pid);
}

static void
on_spawned (GObject *source, GAsyncResult *result, gpointer data)
{
  g_autoptr (TermSession) self = data;
  g_autoptr (GError) error = NULL;
  GPid pid = 0;

  if (!vte_pty_spawn_finish (VTE_PTY (source), result, &pid, &error)) {
    if (self->terminal && !g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
      /* said in the terminal, where the shell would have been */
      g_autofree char *text = g_strdup_printf ("%s\r\n", error->message);
      vte_terminal_feed (self->terminal, text, -1);
    }
    return;
  }
  if (!self->terminal) {
    /* the window closed while the shell was starting */
    kill (pid, SIGHUP);
    g_child_watch_add (pid, reap, NULL);
    return;
  }
  self->pid = pid;
  self->child_watch = g_child_watch_add (pid, on_child_exited, self);
}

static char *
user_shell (void)
{
  char *shell = vte_get_user_shell ();

  if (shell && *shell && g_file_test (shell, G_FILE_TEST_IS_EXECUTABLE))
    return shell;
  g_free (shell);
  shell = g_strdup (g_getenv ("SHELL"));
  if (shell && *shell && g_file_test (shell, G_FILE_TEST_IS_EXECUTABLE))
    return shell;
  g_free (shell);
  return g_strdup ("/bin/sh");
}

gboolean
term_session_start (TermSession *self, const char *directory, GError **error)
{
  g_autofree char *shell = user_shell ();
  const char *argv[] = { shell, NULL };

  g_return_val_if_fail (TERM_IS_SESSION (self), FALSE);
  g_return_val_if_fail (self->pty == NULL, FALSE);

  self->pty = vte_pty_new_sync (VTE_PTY_DEFAULT, NULL, error);
  if (!self->pty)
    return FALSE;
  self->fd = vte_pty_get_fd (self->pty);
  if (!g_unix_set_fd_nonblocking (self->fd, TRUE, error)) {
    self->fd = -1;
    g_clear_object (&self->pty);
    return FALSE;
  }
  if (self->terminal)
    term_session_set_size (self, vte_terminal_get_row_count (self->terminal),
                           vte_terminal_get_column_count (self->terminal));
  /* VtePty's spawn sets TERM (xterm-256color), COLORTERM and VTE_VERSION, and makes the pty the child's terminal */
  vte_pty_spawn_with_fds_async (self->pty, directory ? directory : g_get_home_dir (), argv, NULL, NULL, 0, NULL, 0,
                                G_SPAWN_DEFAULT, NULL, NULL, NULL, -1, NULL, on_spawned, g_object_ref (self));
  watch_reads (self);
  return TRUE;
}

void
term_session_set_size (TermSession *self, long rows, long columns)
{
  g_return_if_fail (TERM_IS_SESSION (self));

  if (self->pty && rows > 0 && columns > 0)
    vte_pty_set_size (self->pty, rows, columns, NULL);
}

const TermModes *
term_session_get_modes (TermSession *self)
{
  g_return_val_if_fail (TERM_IS_SESSION (self), NULL);
  return &self->modes;
}

/* ---- the object ----------------------------------------------------------------------------------------------- */

static void
term_session_dispose (GObject *object)
{
  TermSession *self = TERM_SESSION (object);

  stop_waiting (self);
  g_clear_handle_id (&self->read_source, g_source_remove);
  g_clear_handle_id (&self->write_source, g_source_remove);
  if (self->child_watch) {
    g_clear_handle_id (&self->child_watch, g_source_remove);
    g_child_watch_add (self->pid, reap, NULL);
  }
  if (self->terminal) {
    g_signal_handlers_disconnect_by_data (self->terminal, self);
    g_object_remove_weak_pointer (G_OBJECT (self->terminal), (gpointer *) &self->terminal);
    self->terminal = NULL;
  }
  /* the shell goes as a shell does when its terminal closes: SIGHUP, from the kernel, once the pty's master is closed */
  self->fd = -1;
  g_clear_object (&self->pty);
  G_OBJECT_CLASS (term_session_parent_class)->dispose (object);
}

static void
term_session_finalize (GObject *object)
{
  TermSession *self = TERM_SESSION (object);

  g_byte_array_unref (self->outgoing);
  G_OBJECT_CLASS (term_session_parent_class)->finalize (object);
}

static void
term_session_class_init (TermSessionClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = term_session_dispose;
  object_class->finalize = term_session_finalize;
  signals[EXITED] = g_signal_new ("exited", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                                  G_TYPE_NONE, 1, G_TYPE_INT);
}

static void
term_session_init (TermSession *self)
{
  self->fd = -1;
  self->budget = self->left = BUDGET_MIN;
  self->outgoing = g_byte_array_new ();
  term_modes_init (&self->modes);
}

TermSession *
term_session_new (VteTerminal *terminal)
{
  TermSession *self = g_object_new (TERM_TYPE_SESSION, NULL);

  self->terminal = terminal;
  g_object_add_weak_pointer (G_OBJECT (terminal), (gpointer *) &self->terminal);
  g_signal_connect (terminal, "commit", G_CALLBACK (on_commit), self);
  return self;
}
