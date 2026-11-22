/* vibeos-push-test: an app that registers for push messages with VibeosUnifiedPush and prints what it gets, for
 * trying push by hand (tools/unifiedpush/e2e.sh). Not installed.
 *
 *   vibeos-push-test [--app-id ID] [--instance NAME]... [--description TEXT] [--vapid KEY] [--log FILE]
 *   vibeos-push-test [--app-id ID] [--instance NAME]... --unregister
 *
 * Each line it prints (and appends to --log) is one event:
 *   ENDPOINT <instance> <endpoint> <p256dh> <auth>
 *   MESSAGE <instance> <bytes> <the message, if it is printable text; else its first bytes in hex>
 *   UNREGISTERED <instance>
 *   FAILED <instance> <reason>
 * Started by D-Bus (--gapplication-service) it exits by itself once its messages are delivered. */
#include <stdio.h>
#include <string.h>

#include <gio/gio.h>

#include "vibeos-unified-push.h"

static char *app_id = NULL, *description = NULL, *vapid = NULL, *log_path = NULL;
static char **instances = NULL;
static gboolean unregister = FALSE;
static GPtrArray *pushes;
static guint unregistering;

static void
say (const char *format, ...) G_GNUC_PRINTF (1, 2);

static void
say (const char *format, ...)
{
  va_list args;
  g_autofree char *line = NULL;

  va_start (args, format);
  line = g_strdup_vprintf (format, args);
  va_end (args);
  printf ("%s\n", line);
  fflush (stdout);
  if (log_path)
    {
      FILE *log = fopen (log_path, "a");
      if (log)
        {
          fprintf (log, "%s\n", line);
          fclose (log);
        }
    }
}

static const char *
instance_of (VibeosUnifiedPush *push)
{
  return g_object_get_data (G_OBJECT (push), "name");
}

static void
on_new_endpoint (VibeosUnifiedPush *push, const char *endpoint, const char *p256dh, const char *auth, gpointer data)
{
  say ("ENDPOINT %s %s %s %s", instance_of (push), endpoint, p256dh, auth);
}

static void
on_message (VibeosUnifiedPush *push, GBytes *message, gpointer data)
{
  gsize size = 0;
  const guint8 *bytes = g_bytes_get_data (message, &size);
  gboolean text = g_utf8_validate ((const char *) bytes, size, NULL);
  g_autoptr(GString) shown = g_string_new (NULL);

  for (gsize i = 0; text && i < size; i++)
    if (bytes[i] < 0x20 && bytes[i] != '\t')
      text = FALSE;
  if (text)
    g_string_append_len (shown, (const char *) bytes, size);
  else
    for (gsize i = 0; i < MIN (size, 32); i++)
      g_string_append_printf (shown, "%02x", bytes[i]);
  /* long ones in short: their length and their ends */
  if (shown->len > 120)
    {
      g_autofree char *tail = g_strdup (shown->str + shown->len - 20);
      g_string_truncate (shown, 80);
      g_string_append_printf (shown, "...%s", tail);
    }
  say ("MESSAGE %s %" G_GSIZE_FORMAT " %s", instance_of (push), size, shown->str);
}

static void
on_unregistered (VibeosUnifiedPush *push, GApplication *app)
{
  say ("UNREGISTERED %s", instance_of (push));
  if (unregister && --unregistering == 0)
    g_application_release (app);
}

static void
on_failed (VibeosUnifiedPush *push, const char *reason, GApplication *app)
{
  say ("FAILED %s %s", instance_of (push), reason);
}

static void
startup (GApplication *app)
{
  const char *default_instances[] = { "default", NULL };
  const char **names = instances ? (const char **) instances : default_instances;

  pushes = g_ptr_array_new_with_free_func (g_object_unref);
  for (guint i = 0; names[i]; i++)
    {
      VibeosUnifiedPush *push = vibeos_unified_push_new (app, g_str_equal (names[i], "default") ? NULL : names[i]);

      if (!push)
        {
          g_printerr ("\"%s\" is not an instance name\n", names[i]);
          continue;
        }
      g_object_set_data_full (G_OBJECT (push), "name", g_strdup (names[i]), g_free);
      g_signal_connect (push, "new-endpoint", G_CALLBACK (on_new_endpoint), app);
      g_signal_connect (push, "message", G_CALLBACK (on_message), app);
      g_signal_connect (push, "unregistered", G_CALLBACK (on_unregistered), app);
      g_signal_connect (push, "registration-failed", G_CALLBACK (on_failed), app);
      g_ptr_array_add (pushes, push);
      if (unregister && !vibeos_unified_push_get_endpoint (push))
        say ("UNREGISTERED %s (it was not registered)", names[i]);
      else if (unregister)
        {
          unregistering++;
          vibeos_unified_push_unregister (push);
        }
      else
        vibeos_unified_push_register (push, description, vapid);
    }
  if (unregister && unregistering)
    g_application_hold (app);
}

static void
activate (GApplication *app)
{
  /* run until stopped, as an app the user opened would */
  if (!unregister && !(g_application_get_flags (app) & G_APPLICATION_IS_SERVICE))
    g_application_hold (app);
}

static int
local_options (GApplication *app, GVariantDict *options)
{
  if (app_id)
    {
      if (!g_application_id_is_valid (app_id))
        {
          g_printerr ("\"%s\" is not an application id\n", app_id);
          return 2;
        }
      g_application_set_application_id (app, app_id);
    }
  return -1;
}

int
main (int argc, char **argv)
{
  const GOptionEntry entries[] = {
    { "app-id", 0, 0, G_OPTION_ARG_STRING, &app_id, "The application id (test.vibeos.PushTest)", "ID" },
    { "instance", 0, 0, G_OPTION_ARG_STRING_ARRAY, &instances, "A registration of the app's (default)", "NAME" },
    { "description", 0, 0, G_OPTION_ARG_STRING, &description, "What Settings shows (the app's name)", "TEXT" },
    { "vapid", 0, 0, G_OPTION_ARG_STRING, &vapid, "The app server's VAPID public key", "KEY" },
    { "unregister", 0, 0, G_OPTION_ARG_NONE, &unregister, "Unregister, then quit", NULL },
    { "log", 0, 0, G_OPTION_ARG_FILENAME, &log_path, "Append each event to this file as well", "FILE" },
    { NULL }
  };
  g_autoptr(GApplication) app = g_application_new ("test.vibeos.PushTest", G_APPLICATION_DEFAULT_FLAGS);
  int status;

  g_application_add_main_option_entries (app, entries);
  g_signal_connect (app, "handle-local-options", G_CALLBACK (local_options), NULL);
  g_signal_connect (app, "startup", G_CALLBACK (startup), NULL);
  g_signal_connect (app, "activate", G_CALLBACK (activate), NULL);
  g_application_set_inactivity_timeout (app, 2000);
  status = g_application_run (app, argc, argv);
  g_clear_pointer (&pushes, g_ptr_array_unref);
  return status;
}
