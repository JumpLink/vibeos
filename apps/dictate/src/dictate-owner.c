/* dictate-owner: see dictate-owner.h. */
#include "dictate-owner.h"

#define SHELL_NAME      "vibeos.Shell"
#define SHELL_PATH      "/vibeos/Shell"
#define SHELL_INTERFACE "vibeos.Shell"
#define CONFIRM_TIMEOUT_MS (5 * 60 * 1000)      /* the bouncer may be up a while: a reply no later than this */

static gboolean shell_confirms;                 /* the shell has ConfirmOwner */

static void
on_introspected (GObject *source, GAsyncResult *result, gpointer data)
{
  g_autoptr (GVariant) reply = g_dbus_connection_call_finish (G_DBUS_CONNECTION (source), result, NULL);
  const char *xml = NULL;
  g_autoptr (GDBusNodeInfo) node = NULL;
  GDBusInterfaceInfo *iface;

  if (!reply)
    return;
  g_variant_get (reply, "(&s)", &xml);
  node = g_dbus_node_info_new_for_xml (xml, NULL);
  iface = node ? g_dbus_node_info_lookup_interface (node, SHELL_INTERFACE) : NULL;
  shell_confirms = iface && g_dbus_interface_info_lookup_method (iface, "ConfirmOwner");
}

void
dictate_owner_probe (void)
{
  g_autoptr (GDBusConnection) bus = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, NULL);

  if (bus)
    g_dbus_connection_call (bus, SHELL_NAME, SHELL_PATH, "org.freedesktop.DBus.Introspectable", "Introspect", NULL,
                            G_VARIANT_TYPE ("(s)"), G_DBUS_CALL_FLAGS_NO_AUTO_START, -1, NULL, on_introspected, NULL);
}

gboolean
dictate_owner_has_secret (void)
{
  GSettingsSchemaSource *source = g_settings_schema_source_get_default ();
  g_autoptr (GSettingsSchema) schema = source ? g_settings_schema_source_lookup (source, "vibeos.Shell", TRUE) : NULL;
  g_autoptr (GSettings) shell = NULL;
  g_autofree char *type = NULL;

  if (!shell_confirms || !schema || !g_settings_schema_has_key (schema, "lock-type"))
    return FALSE;
  shell = g_settings_new_full (schema, NULL, NULL);
  type = g_settings_get_string (shell, "lock-type");
  /* a swipe has no secret: nothing would confirm the owner */
  return g_strcmp0 (type, "swipe") != 0;
}

static void
on_confirmed (GObject *source, GAsyncResult *result, gpointer data)
{
  g_autoptr (GTask) task = data;
  g_autoptr (GError) error = NULL;
  g_autoptr (GVariant) reply = g_dbus_connection_call_finish (G_DBUS_CONNECTION (source), result, &error);
  gboolean confirmed = FALSE;

  if (!reply)
    {
      g_task_return_error (task, g_steal_pointer (&error));
      return;
    }
  g_variant_get (reply, "(b)", &confirmed);
  g_task_return_boolean (task, confirmed);
}

void
dictate_owner_confirm_async (const char *app_id, GCancellable *cancellable, GAsyncReadyCallback callback,
                             gpointer data)
{
  GTask *task = g_task_new (NULL, cancellable, callback, data);
  g_autoptr (GError) error = NULL;
  g_autoptr (GDBusConnection) bus = g_bus_get_sync (G_BUS_TYPE_SESSION, cancellable, &error);

  if (!bus)
    {
      g_task_return_error (task, g_steal_pointer (&error));
      g_object_unref (task);
      return;
    }
  g_dbus_connection_call (bus, SHELL_NAME, SHELL_PATH, SHELL_INTERFACE, "ConfirmOwner",
                          g_variant_new ("(s)", app_id), G_VARIANT_TYPE ("(b)"), G_DBUS_CALL_FLAGS_NO_AUTO_START,
                          CONFIRM_TIMEOUT_MS, cancellable, on_confirmed, task);
}

gboolean
dictate_owner_confirm_finish (GAsyncResult *result, GError **error)
{
  return g_task_propagate_boolean (G_TASK (result), error);
}
