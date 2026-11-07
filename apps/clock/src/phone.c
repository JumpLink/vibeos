/* What the Clock's service asks of the phone (phone.h). */
#include "phone.h"

#include <gio/gdesktopappinfo.h>
#include <gio/gunixfdlist.h>
#include <unistd.h>

#define APP_ID "de.yesman.Clock"

int
phone_inhibit_sleep (GDBusConnection *system, const char *why)
{
  g_autoptr(GUnixFDList) fds = NULL;
  g_autoptr(GVariant) reply = NULL;
  g_autoptr(GError) error = NULL;
  int index;

  if (!system)
    return -1;
  reply = g_dbus_connection_call_with_unix_fd_list_sync (system, "org.freedesktop.login1", "/org/freedesktop/login1",
                                                         "org.freedesktop.login1.Manager", "Inhibit",
                                                         g_variant_new ("(ssss)", "sleep", "Clock", why, "block"),
                                                         G_VARIANT_TYPE ("(h)"), G_DBUS_CALL_FLAGS_NONE, 3000, NULL,
                                                         &fds, NULL, &error);
  if (!reply)
    {
      g_warning ("cannot keep the phone awake: %s", error->message);
      return -1;
    }
  g_variant_get (reply, "(h)", &index);
  return g_unix_fd_list_get (fds, index, NULL);
}

void
phone_allow_sleep (int *inhibitor)
{
  if (*inhibitor >= 0)
    close (*inhibitor);
  *inhibitor = -1;
}

void
phone_start_app (const char *app_id)
{
  g_autofree char *desktop_id = g_strconcat (app_id, ".desktop", NULL);
  g_autoptr(GDesktopAppInfo) info = g_desktop_app_info_new (desktop_id);
  g_autoptr(GError) error = NULL;

  if (!info || !g_app_info_launch (G_APP_INFO (info), NULL, NULL, &error))
    g_warning ("cannot open %s: %s", app_id, error ? error->message : "not installed");
}

static void
on_full_screen (GObject *source, GAsyncResult *result, gpointer data)
{
  g_autoptr(GVariant) reply = g_dbus_connection_call_finish (G_DBUS_CONNECTION (source), result, NULL);
  g_autofree char *app_id = data;

  /* no shell to ask: the screen is opened anyway (the shell says no only when the phone is in use, unlocked) */
  if (!reply)
    phone_start_app (app_id);
}

void
phone_show_full_screen (GDBusConnection *session, const char *app_id)
{
  g_dbus_connection_call (session, "de.yesman.GnomePhone", "/de/yesman/GnomePhone", "de.yesman.GnomePhone",
                          "LaunchFullScreen", g_variant_new ("(s)", app_id), G_VARIANT_TYPE ("(b)"),
                          G_DBUS_CALL_FLAGS_NONE, 5000, NULL, on_full_screen, g_strdup (app_id));
}

void
phone_wake_at (GDBusConnection *system, const char *name, gint64 time, gboolean alarm_clock)
{
  GVariantBuilder options;

  if (!system)
    return;
  g_variant_builder_init (&options, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_add (&options, "{sv}", "alarm-clock", g_variant_new_boolean (alarm_clock));
  g_variant_builder_add (&options, "{sv}", "app", g_variant_new_string (APP_ID));
  g_dbus_connection_call (system, "de.yesman.Wakeup", "/de/yesman/Wakeup", "de.yesman.Wakeup", "Set",
                          g_variant_new ("(sxa{sv})", name, time, &options), NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL,
                          NULL, NULL);
}

void
phone_open_app (GDBusConnection *session, const char *app_id, const char *action, GVariant *parameter)
{
  /* the app's object path is its id as a path: de.yesman.Clock is /de/yesman/Clock */
  g_autofree char *path = g_strdelimit (g_strconcat ("/", app_id, NULL), ".", '/');
  GVariantBuilder parameters;

  g_variant_builder_init (&parameters, G_VARIANT_TYPE ("av"));
  if (parameter)
    g_variant_builder_add (&parameters, "v", parameter);
  g_dbus_connection_call (session, app_id, path, "org.freedesktop.Application", "ActivateAction",
                          g_variant_new ("(sava{sv})", action, &parameters, NULL), NULL, G_DBUS_CALL_FLAGS_NONE, -1,
                          NULL, NULL, NULL);
}
