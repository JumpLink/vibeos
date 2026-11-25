/* test-restart-child: a program that follows the session's language by starting over (vibeos-restart.h), for
 * test-restart.py. At each start it writes a line of JSON to $RESTART_LOG: its pid, its language's variables and
 * KEEP (one of no language's), its arguments, its name as the kernel has it, and whether the descriptor named by
 * --leak (a pipe the run before left open without close-on-exec) reached it. Busy while the file $RESTART_BUSY
 * exists; SIGUSR1 says it became idle (vibeos_restart_check). It owns vibeos.Test.Restart, and says when it has it.
 * Each call of its prepare is a line too: {"event": "prepare", "going": …}. */
#include "vibeos-restart.h"

#include <fcntl.h>
#include <gio/gio.h>
#include <glib-unix.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int leak[2] = { -1, -1 };

/* a JSON string (the values here are ASCII) */
static void
add_string (GString *out, const char *value)
{
  if (!value) {
    g_string_append (out, "null");
    return;
  }
  g_autofree char *escaped = g_strescape (value, NULL);
  g_string_append_printf (out, "\"%s\"", escaped);
}

static void
say (GString *line)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (GFile) file = g_file_new_for_path (g_getenv ("RESTART_LOG"));
  g_autoptr (GFileOutputStream) out = g_file_append_to (file, G_FILE_CREATE_NONE, NULL, &error);
  g_string_append_c (line, '\n');
  if (!out || !g_output_stream_write_all (G_OUTPUT_STREAM (out), line->str, line->len, NULL, NULL, &error))
    g_error ("cannot write the log: %s", error->message);
}

static gboolean
busy (gpointer data)
{
  return g_file_test (g_getenv ("RESTART_BUSY"), G_FILE_TEST_EXISTS);
}

/* the pipe left open goes along with its number; the test's own arguments after it */
static char **
prepare (gboolean going, gpointer data)
{
  g_autoptr (GString) line = g_string_new (NULL);
  g_string_append_printf (line, "{\"event\": \"prepare\", \"pid\": %d, \"going\": %s}", getpid (),
                          going ? "true" : "false");
  say (line);
  if (!going)
    return NULL;
  GStrvBuilder *b = g_strv_builder_new ();
  g_strv_builder_add (b, "--leak");
  g_strv_builder_take (b, g_strdup_printf ("%d", leak[0]));
  const char *more = g_getenv ("RESTART_ARGS");
  if (more && *more) {
    g_auto (GStrv) words = g_strsplit (more, " ", -1);
    g_strv_builder_addv (b, (const char **) words);
  }
  char **args = g_strv_builder_end (b);
  g_strv_builder_unref (b);
  return args;
}

static gboolean
on_idle_now (gpointer data)
{
  vibeos_restart_check ();
  return G_SOURCE_CONTINUE;
}

static void
acquired (GDBusConnection *bus, const char *name, gpointer data)
{
  g_autoptr (GString) line = g_string_new (NULL);
  g_string_append_printf (line, "{\"event\": \"name\", \"pid\": %d}", getpid ());
  say (line);
}

int
main (int argc, char **argv)
{
  int leaked = -1;
  for (int i = 1; i + 1 < argc; i++)
    if (!strcmp (argv[i], "--leak"))
      leaked = atoi (argv[i + 1]);

  g_autoptr (GString) line = g_string_new ("{\"event\": \"start\"");
  g_string_append_printf (line, ", \"pid\": %d", getpid ());
  const char *const names[] = { "LANG", "LANGUAGE", "LC_ALL", "LC_MESSAGES", "LC_TIME", "KEEP", "PATH",
                                "VIBEOS_RESTARTED", NULL };
  for (guint i = 0; names[i]; i++) {
    g_string_append_printf (line, ", \"%s\": ", names[i]);
    add_string (line, g_getenv (names[i]));
  }
  g_autofree char *comm = NULL;
  g_file_get_contents ("/proc/self/comm", &comm, NULL, NULL);
  g_string_append (line, ", \"comm\": ");
  add_string (line, comm ? g_strstrip (comm) : NULL);
  g_string_append (line, ", \"argv\": [");
  for (int i = 0; i < argc; i++) {
    if (i)
      g_string_append (line, ", ");
    add_string (line, argv[i]);
  }
  g_string_append_printf (line, "], \"leaked\": %s}",
                          leaked < 0 ? "null" : fcntl (leaked, F_GETFD) >= 0 ? "true" : "false");
  say (line);

  /* a descriptor of its own, without close-on-exec, as a library might leave one */
  if (pipe (leak) < 0)
    g_error ("no pipe");

  g_bus_own_name (G_BUS_TYPE_SESSION, "vibeos.Test.Restart", G_BUS_NAME_OWNER_FLAGS_DO_NOT_QUEUE, NULL, acquired,
                  NULL, NULL, NULL);
  vibeos_restart_follow_language (busy, prepare, NULL);
  g_unix_signal_add (SIGUSR1, on_idle_now, NULL);
  g_autoptr (GMainLoop) loop = g_main_loop_new (NULL, FALSE);
  g_main_loop_run (loop);
  return 0;
}
