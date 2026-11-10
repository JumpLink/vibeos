/* Another app's action: see app-action.h. */
#include "app-action.h"

static GDesktopAppInfo *
app_info (const char *app_id)
{
  g_autofree char *desktop_id = g_strconcat (app_id, ".desktop", NULL);

  return g_desktop_app_info_new (desktop_id);
}

gboolean
app_action_installed (const char *app_id)
{
  g_autoptr(GDesktopAppInfo) info = app_info (app_id);

  return info != NULL;
}

/* the platform data of an activation: the token that lets the app's window come to the front (xdg-activation),
 * as GtkApplication reads it ("activation-token") */
static GVariant *
platform_data (GtkWidget *from, GDesktopAppInfo *info)
{
  g_autoptr(GdkAppLaunchContext) context =
    gdk_display_get_app_launch_context (from ? gtk_widget_get_display (from) : gdk_display_get_default ());
  g_autofree char *token = g_app_launch_context_get_startup_notify_id (G_APP_LAUNCH_CONTEXT (context),
                                                                      G_APP_INFO (info), NULL);
  GVariantBuilder builder;

  g_variant_builder_init (&builder, G_VARIANT_TYPE_VARDICT);
  if (token)
    {
      g_variant_builder_add (&builder, "{sv}", "activation-token", g_variant_new_string (token));
      g_variant_builder_add (&builder, "{sv}", "desktop-startup-id", g_variant_new_string (token));
    }
  return g_variant_builder_end (&builder);
}

static void
on_activated (GObject *source, GAsyncResult *result, gpointer data)
{
  g_autofree char *what = data;
  g_autoptr(GError) error = NULL;
  g_autoptr(GVariant) reply = g_dbus_connection_call_finish (G_DBUS_CONNECTION (source), result, &error);

  if (!reply)
    g_warning ("%s: %s", what, error->message);
}

gboolean
app_action_activate (GtkWidget *from, const char *app_id, const char *action, GVariant *parameter)
{
  g_autoptr(GDesktopAppInfo) info = app_info (app_id);
  GApplication *app = g_application_get_default ();
  GDBusConnection *bus = app ? g_application_get_dbus_connection (app) : NULL;
  g_autoptr(GDBusConnection) own = NULL;
  g_autofree char *path = NULL;
  GVariantBuilder parameters;

  if (parameter)
    g_variant_ref_sink (parameter);
  if (info && !bus)
    bus = own = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, NULL);
  if (!info || !bus)
    {
      g_clear_pointer (&parameter, g_variant_unref);
      return FALSE;
    }
  /* GApplication's object path: the id with its dots as slashes */
  path = g_strconcat ("/", app_id, NULL);
  g_strdelimit (path, ".", '/');
  g_variant_builder_init (&parameters, G_VARIANT_TYPE ("av"));
  if (parameter)
    g_variant_builder_add (&parameters, "v", parameter);
  g_dbus_connection_call (bus, app_id, path, "org.freedesktop.Application", "ActivateAction",
                          g_variant_new ("(sav@a{sv})", action, &parameters, platform_data (from, info)), NULL,
                          G_DBUS_CALL_FLAGS_NONE, -1, NULL, on_activated, g_strdup_printf ("%s %s", app_id, action));
  g_clear_pointer (&parameter, g_variant_unref);
  return TRUE;
}
