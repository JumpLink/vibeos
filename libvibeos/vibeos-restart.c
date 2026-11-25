/* vibeos-restart: see vibeos-restart.h. */
#include "vibeos-restart.h"

#include <errno.h>
#include <fcntl.h>
#include <gio/gio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

#define LOCALE_PATH      "/vibeos/Locale"
#define LOCALE_INTERFACE "vibeos.Locale"
#define SYSTEMD_NAME     "org.freedesktop.systemd1"
#define SYSTEMD_PATH     "/org/freedesktop/systemd1"
#define SYSTEMD_MANAGER  "org.freedesktop.systemd1.Manager"
/* in the environment of a program this file started over */
#define RESTARTED        "VIBEOS_RESTARTED"

/* how long after a change the program is first asked (a second change soon after is one restart, and Settings,
 * starting over itself, goes first), and how often while it is busy; the tests' own through the environment
 * (VIBEOS_RESTART_SETTLE_MS, _RETRY_MS, _UNIT_WAIT_MS) */
#define SETTLE_MS        1500
#define RETRY_MS         3000
/* systemd asked to restart the service and this process is still here after this long: it stays, and asks again */
#define UNIT_WAIT_MS     30000

#ifndef CLOSE_RANGE_CLOEXEC
#define CLOSE_RANGE_CLOEXEC (1U << 2)
#endif

/* the variables the session's language is in (vibeos-locale.c's), the only ones a change sets */
static const char *const variables[] = { "LANG", "LANGUAGE", "LC_ALL", "LC_MESSAGES", "LC_TIME", "LC_NUMERIC",
                                         "LC_MONETARY", "LC_MEASUREMENT", "LC_PAPER", NULL };

static struct {
  VibeosRestartBusyFunc busy;
  VibeosRestartPrepareFunc prepare;
  gpointer data;
  GDBusConnection *bus;
  guint subscription;
  char **target;                /* the language to start over in, every variable NAME=VALUE (empty: unset); NULL for none */
  guint settle_id, retry_id, unit_wait_id;
  gboolean going;               /* on its way: systemd asked, or execve under way */
} R;

static guint
ms_from_env (const char *name, guint fallback)
{
  const char *value = g_getenv (name);
  return value && *value ? (guint) g_ascii_strtoull (value, NULL, 10) : fallback;
}

/* @env's value of @name: NULL when it is unset or empty there */
static const char *
value_in (const char *const *env, const char *name)
{
  gsize n = strlen (name);
  for (guint i = 0; env && env[i]; i++)
    if (!strncmp (env[i], name, n) && env[i][n] == '=')
      return env[i][n + 1] ? env[i] + n + 1 : NULL;
  return NULL;
}

/* every session variable as @env has it (NAME= for one it leaves unset): only those, whoever sent it */
static char **
session_variables (const char *const *env)
{
  GStrvBuilder *b = g_strv_builder_new ();
  for (guint i = 0; variables[i]; i++)
    g_strv_builder_take (b, g_strconcat (variables[i], "=", value_in (env, variables[i]) ?: "", NULL));
  char **all = g_strv_builder_end (b);
  g_strv_builder_unref (b);
  return all;
}

/* whether the program's language is other than @env's */
static gboolean
differs (const char *const *env)
{
  for (guint i = 0; variables[i]; i++) {
    const char *now = g_getenv (variables[i]);
    if (g_strcmp0 (now && *now ? now : NULL, value_in (env, variables[i])))
      return TRUE;
  }
  return FALSE;
}

static void check (void);

static gboolean
settled (gpointer data)
{
  R.settle_id = 0;
  check ();
  return G_SOURCE_REMOVE;
}

static gboolean
retry (gpointer data)
{
  R.retry_id = 0;
  check ();
  return G_SOURCE_REMOVE;
}

/* @env (every session variable) is the session's language now: the program follows it, or stays */
static void
want (char **env)
{
  if (R.going) {
    g_strfreev (env);
    return;
  }
  g_clear_handle_id (&R.settle_id, g_source_remove);
  g_clear_handle_id (&R.retry_id, g_source_remove);
  g_clear_pointer (&R.target, g_strfreev);
  if (!differs ((const char *const *) env)) {
    /* back to the language it is in (a change undone before it was idle): nothing to do */
    g_strfreev (env);
    return;
  }
  R.target = env;
  g_message ("the session's language is now %s: starting over in it once idle", value_in ((const char *const *) env,
                                                                                         "LANG") ?: "(none)");
  R.settle_id = g_timeout_add (ms_from_env ("VIBEOS_RESTART_SETTLE_MS", SETTLE_MS), settled, NULL);
}

static void
on_changed (GDBusConnection *bus, const char *sender, const char *path, const char *iface, const char *signal,
            GVariant *params, gpointer data)
{
  if (!g_variant_is_of_type (params, G_VARIANT_TYPE ("(as)")))
    return;
  g_autofree const char **env = NULL;
  g_variant_get (params, "(^a&s)", &env);
  /* a language there must be: a change that leaves LANG unset is no session's */
  if (!value_in (env, "LANG"))
    return;
  want (session_variables (env));
}

/* at the start: the language systemd's user manager starts programs in, followed when it is not this one's (it
 * changed while this program was starting) */
static void
on_manager_environment (GObject *source, GAsyncResult *result, gpointer data)
{
  g_autoptr (GVariant) reply = g_dbus_connection_call_finish (G_DBUS_CONNECTION (source), result, NULL);
  if (!reply)
    return;
  g_autoptr (GVariant) value = NULL;
  g_variant_get (reply, "(v)", &value);
  if (!g_variant_is_of_type (value, G_VARIANT_TYPE_STRING_ARRAY))
    return;
  g_autofree const char **env = g_variant_get_strv (value, NULL);
  if (value_in (env, "LANG"))
    want (session_variables (env));
}

/* the well-known names this program has on @bus given up and their release waited for: the program that comes finds
 * them free, and is not taken for a second instance of itself */
static void
release_names (GDBusConnection *bus)
{
  const char *me = g_dbus_connection_get_unique_name (bus);
  g_autoptr (GVariant) names = g_dbus_connection_call_sync (bus, "org.freedesktop.DBus", "/org/freedesktop/DBus",
                                                            "org.freedesktop.DBus", "ListNames", NULL,
                                                            G_VARIANT_TYPE ("(as)"), G_DBUS_CALL_FLAGS_NONE, 2000,
                                                            NULL, NULL);
  if (!names || !me)
    return;
  g_autoptr (GVariantIter) iter = NULL;
  const char *name;
  g_variant_get (names, "(as)", &iter);
  while (g_variant_iter_loop (iter, "&s", &name)) {
    if (*name == ':')
      continue;
    g_autoptr (GVariant) owner = g_dbus_connection_call_sync (bus, "org.freedesktop.DBus", "/org/freedesktop/DBus",
                                                              "org.freedesktop.DBus", "GetNameOwner",
                                                              g_variant_new ("(s)", name), G_VARIANT_TYPE ("(s)"),
                                                              G_DBUS_CALL_FLAGS_NONE, 2000, NULL, NULL);
    const char *who = NULL;
    if (owner)
      g_variant_get (owner, "(&s)", &who);
    if (g_strcmp0 (who, me))
      continue;
    g_autoptr (GVariant) released = g_dbus_connection_call_sync (bus, "org.freedesktop.DBus", "/org/freedesktop/DBus",
                                                                 "org.freedesktop.DBus", "ReleaseName",
                                                                 g_variant_new ("(s)", name), G_VARIANT_TYPE ("(u)"),
                                                                 G_DBUS_CALL_FLAGS_NONE, 2000, NULL, NULL);
  }
}

/* @name's property of systemd's unit @unit, or NULL */
static GVariant *
unit_property (GDBusConnection *bus, const char *unit, const char *iface, const char *name)
{
  g_autoptr (GVariant) reply = g_dbus_connection_call_sync (bus, SYSTEMD_NAME, unit, "org.freedesktop.DBus.Properties",
                                                            "Get", g_variant_new ("(ss)", iface, name),
                                                            G_VARIANT_TYPE ("(v)"), G_DBUS_CALL_FLAGS_NO_AUTO_START,
                                                            3000, NULL, NULL);
  GVariant *value = NULL;
  if (reply)
    g_variant_get (reply, "(v)", &value);
  return value;
}

/* systemd asked to restart the service this program is the main process of, when that service is of Type=dbus;
 * FALSE when it is not one, or systemd could not be asked */
static gboolean
restart_unit (GDBusConnection *bus)
{
  if (!g_getenv ("INVOCATION_ID"))
    return FALSE;
  g_autoptr (GVariant) path = g_dbus_connection_call_sync (bus, SYSTEMD_NAME, SYSTEMD_PATH, SYSTEMD_MANAGER,
                                                           "GetUnitByPID", g_variant_new ("(u)", (guint32) getpid ()),
                                                           G_VARIANT_TYPE ("(o)"), G_DBUS_CALL_FLAGS_NO_AUTO_START,
                                                           3000, NULL, NULL);
  if (!path)
    return FALSE;
  const char *unit = NULL;
  g_variant_get (path, "(&o)", &unit);
  g_autoptr (GVariant) id = unit_property (bus, unit, "org.freedesktop.systemd1.Unit", "Id");
  g_autoptr (GVariant) type = unit_property (bus, unit, "org.freedesktop.systemd1.Service", "Type");
  g_autoptr (GVariant) main_pid = unit_property (bus, unit, "org.freedesktop.systemd1.Service", "MainPID");
  if (!id || !type || !main_pid || !g_variant_is_of_type (id, G_VARIANT_TYPE_STRING) ||
      !g_variant_is_of_type (type, G_VARIANT_TYPE_STRING) || !g_variant_is_of_type (main_pid, G_VARIANT_TYPE_UINT32) ||
      g_strcmp0 (g_variant_get_string (type, NULL), "dbus") || g_variant_get_uint32 (main_pid) != (guint32) getpid ())
    return FALSE;
  g_autoptr (GError) error = NULL;
  g_autoptr (GVariant) job = g_dbus_connection_call_sync (bus, SYSTEMD_NAME, SYSTEMD_PATH, SYSTEMD_MANAGER,
                                                          "RestartUnit",
                                                          g_variant_new ("(ss)", g_variant_get_string (id, NULL),
                                                                         "replace"),
                                                          G_VARIANT_TYPE ("(o)"), G_DBUS_CALL_FLAGS_NO_AUTO_START,
                                                          5000, NULL, &error);
  if (!job) {
    g_warning ("systemd could not restart %s in the new language: %s", g_variant_get_string (id, NULL),
               error->message);
    return FALSE;
  }
  g_message ("systemd restarts %s in the new language", g_variant_get_string (id, NULL));
  return TRUE;
}

/* the same executable again in this process, with @args after its name (NULL: its own), in the language wanted */
static void
exec_self (char **args)
{
  g_autofree char *exe = g_file_read_link ("/proc/self/exe", NULL);
  g_autofree char *cmdline = NULL;
  gsize length = 0;
  if (!exe || !g_file_get_contents ("/proc/self/cmdline", &cmdline, &length, NULL) || !length) {
    g_warning ("cannot start over in the new language: this program's executable or arguments are unknown");
    return;
  }
  /* replaced by an upgrade since it started: the new one */
  if (g_str_has_suffix (exe, " (deleted)"))
    exe[strlen (exe) - strlen (" (deleted)")] = '\0';

  GPtrArray *argv = g_ptr_array_new ();
  for (gsize at = 0; at < length; at += strlen (cmdline + at) + 1)
    if (!args || at == 0)
      g_ptr_array_add (argv, cmdline + at);
  for (guint i = 0; args && args[i]; i++)
    g_ptr_array_add (argv, args[i]);
  g_ptr_array_add (argv, NULL);

  /* the environment as it is, but the language wanted: no setenv in this process, whose other threads read theirs */
  GPtrArray *envp = g_ptr_array_new_with_free_func (g_free);
  g_auto (GStrv) now = g_get_environ ();
  for (guint i = 0; now[i]; i++) {
    gsize n = strcspn (now[i], "=");
    g_autofree char *name = g_strndup (now[i], n);
    if (!g_strv_contains (variables, name) && strcmp (name, RESTARTED))
      g_ptr_array_add (envp, g_strdup (now[i]));
  }
  for (guint i = 0; R.target[i]; i++)
    if (strchr (R.target[i], '=')[1])
      g_ptr_array_add (envp, g_strdup (R.target[i]));
  g_ptr_array_add (envp, g_strdup (RESTARTED "=1"));
  g_ptr_array_add (envp, NULL);

  /* nothing of this process's own reaches the new one: a socket or a pipe a library left open closes with the exec */
  if (syscall (SYS_close_range, 3, ~0U, CLOSE_RANGE_CLOEXEC) < 0) {
    g_autoptr (GDir) fds = g_dir_open ("/proc/self/fd", 0, NULL);
    const char *fd;
    while (fds && (fd = g_dir_read_name (fds)))
      if (atoi (fd) > 2)
        fcntl (atoi (fd), F_SETFD, FD_CLOEXEC);
  }
  g_message ("starting over in the new language: %s", exe);
  execve (exe, (char **) argv->pdata, (char **) envp->pdata);
  g_warning ("cannot start over in the new language: %s: %s", exe, g_strerror (errno));
  g_ptr_array_unref (argv);
  g_ptr_array_unref (envp);
}

/* the program did not go after all: it goes on as it was, and puts back what it took down */
static void
stayed (void)
{
  R.going = FALSE;
  if (R.prepare)
    g_strfreev (R.prepare (FALSE, R.data));
}

/* systemd did not restart the service: the program goes on as it was, and asks again */
static gboolean
unit_not_restarted (gpointer data)
{
  R.unit_wait_id = 0;
  g_warning ("systemd did not restart this service in the new language: asking again");
  stayed ();
  check ();
  return G_SOURCE_REMOVE;
}

static void
start_over (void)
{
  R.going = TRUE;
  g_auto (GStrv) args = R.prepare ? R.prepare (TRUE, R.data) : NULL;
  g_settings_sync ();
  if (R.bus) {
    g_dbus_connection_flush_sync (R.bus, NULL, NULL);
    if (restart_unit (R.bus)) {
      R.unit_wait_id = g_timeout_add (ms_from_env ("VIBEOS_RESTART_UNIT_WAIT_MS", UNIT_WAIT_MS), unit_not_restarted,
                                      NULL);
      return;
    }
    release_names (R.bus);
    g_dbus_connection_flush_sync (R.bus, NULL, NULL);
  }
  exec_self (args);
  /* it could not: it goes on in its language, and tries again at the next change */
  stayed ();
  g_clear_pointer (&R.target, g_strfreev);
}

static void
check (void)
{
  if (!R.target || R.going || R.settle_id)
    return;
  g_clear_handle_id (&R.retry_id, g_source_remove);
  if (R.busy && R.busy (R.data)) {
    R.retry_id = g_timeout_add (ms_from_env ("VIBEOS_RESTART_RETRY_MS", RETRY_MS), retry, NULL);
    return;
  }
  start_over ();
}

void
vibeos_restart_check (void)
{
  /* from the main loop: never from inside the caller's own callback (a window being removed, a call's state) */
  if (R.target && !R.going && !R.settle_id) {
    g_clear_handle_id (&R.retry_id, g_source_remove);
    R.retry_id = g_idle_add (retry, NULL);
  }
}

gboolean
vibeos_restart_pending (void)
{
  return R.target != NULL;
}

void
vibeos_restart_follow_language (VibeosRestartBusyFunc busy, VibeosRestartPrepareFunc prepare, gpointer user_data)
{
  g_return_if_fail (!R.subscription);
  g_autoptr (GError) error = NULL;
  R.busy = busy;
  R.prepare = prepare;
  R.data = user_data;
  R.bus = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, &error);
  if (!R.bus) {
    g_warning ("no session bus to hear of a new language on: %s", error->message);
    return;
  }
  R.subscription = g_dbus_connection_signal_subscribe (R.bus, NULL, LOCALE_INTERFACE, "Changed", LOCALE_PATH, NULL,
                                                       G_DBUS_SIGNAL_FLAGS_NONE, on_changed, NULL, NULL);
  /* started by systemd, or started over here: the language may have changed again while it was starting */
  if (g_getenv ("INVOCATION_ID") || g_getenv (RESTARTED))
    g_dbus_connection_call (R.bus, SYSTEMD_NAME, SYSTEMD_PATH, "org.freedesktop.DBus.Properties", "Get",
                            g_variant_new ("(ss)", SYSTEMD_MANAGER, "Environment"), G_VARIANT_TYPE ("(v)"),
                            G_DBUS_CALL_FLAGS_NO_AUTO_START, 5000, NULL, on_manager_environment, NULL);
}
