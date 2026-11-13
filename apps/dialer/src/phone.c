/* What the Phone's service asks of the shell (phone.h). */
#include "phone.h"

#include <gio/gdesktopappinfo.h>

typedef struct {
  char *app_id;
  PhoneFullScreenDone done;
  gpointer data;
} FullScreen;

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
  FullScreen *request = data;
  g_autoptr(GVariant) reply = g_dbus_connection_call_finish (G_DBUS_CONNECTION (source), result, NULL);
  gboolean shown = TRUE;

  if (reply)
    g_variant_get (reply, "(b)", &shown);
  else
    phone_start_app (request->app_id);  /* no shell to ask: opened anyway */
  if (request->done)
    request->done (shown, request->data);
  g_free (request->app_id);
  g_free (request);
}

void
phone_show_full_screen (GDBusConnection *session, const char *app_id, PhoneFullScreenDone done, gpointer data)
{
  FullScreen *request = g_new0 (FullScreen, 1);

  request->app_id = g_strdup (app_id);
  request->done = done;
  request->data = data;
  g_dbus_connection_call (session, "neo.GnomePhone", "/neo/GnomePhone", "neo.GnomePhone",
                          "LaunchFullScreen", g_variant_new ("(s)", app_id), G_VARIANT_TYPE ("(b)"),
                          G_DBUS_CALL_FLAGS_NONE, 5000, NULL, on_full_screen, request);
}

void
phone_open_app (GDBusConnection *session, const char *app_id, const char *action, GVariant *parameter)
{
  /* the app's object path is its id as a path: neo.Dialer is /neo/Dialer */
  g_autofree char *path = g_strdelimit (g_strconcat ("/", app_id, NULL), ".", '/');
  GVariantBuilder parameters;

  g_variant_builder_init (&parameters, G_VARIANT_TYPE ("av"));
  if (parameter)
    g_variant_builder_add (&parameters, "v", parameter);
  g_dbus_connection_call (session, app_id, path, "org.freedesktop.Application", "ActivateAction",
                          g_variant_new ("(sava{sv})", action, &parameters, NULL), NULL, G_DBUS_CALL_FLAGS_NONE, -1,
                          NULL, NULL, NULL);
}
