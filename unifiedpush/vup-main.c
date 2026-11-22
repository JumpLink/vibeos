/* vibeos-unifiedpush: VibeOS's UnifiedPush distributor.
 *
 * Apps get push messages without keeping a connection of their own: they register here, over the UnifiedPush D-Bus
 * spec 0.3.0 (org.unifiedpush.Distributor2 on org.unifiedpush.Distributor.vibeos), and this one service keeps a
 * single stream open to the push server, an ntfy server (https://push.vibeos.top unless GSettings vibeos.UnifiedPush
 * says otherwise), for all of them. Each registration is a topic of its own on the server, "up" and 12 random
 * base64url characters; its endpoint, <server>/<topic>?up=1, is the URL the app's server sends its Web Push messages
 * to (RFC 8291 encrypted: neither the push server nor this service can read them). When a message comes in on the
 * stream, it goes to the app's org.unifiedpush.Connector2.Message, and D-Bus starts the app if it does not run.
 *
 *   vup-store.c    the registrations, and how far the stream has got, in $XDG_STATE_HOME/vibeos-unifiedpush
 *   vup-stream.c   the stream: reconnecting, dead streams, the network, sleep and resume, Collect
 *   vup-deliver.c  the calls to the apps, in order, per app; apps that are starting or are gone; staying awake
 *   vup-main.c     the D-Bus interfaces: Distributor2 for apps, vibeos.UnifiedPush1 for Settings and sleep hooks
 *
 * Tokens and topics are secrets: the log shows their first 4 characters at most. */
#include <glib-unix.h>

#include "vup.h"
#include "vup-resources.h"

#define ERROR_NOT_FOUND "vibeos.UnifiedPush1.Error.NotFound"
#define ERROR_FAILED    "vibeos.UnifiedPush1.Error.Failed"

static GDBusConnection *bus;
static GSettings *settings;
static GMainLoop *loop;
static gboolean name_owned;
static guint distributor_id, manager_id;

/* --- what the manager interface tells about */

static GVariant *
manager_property (const char *name)
{
  const char *remote, *local;
  guint16 remote_port, local_port;

  vup_stream_get_addresses (&remote, &remote_port, &local, &local_port);
  if (g_str_equal (name, "State"))
    return g_variant_new_string (vup_stream_state_name (vup_stream_get_state ()));
  if (g_str_equal (name, "Error"))
    return g_variant_new_string (vup_stream_get_error ());
  if (g_str_equal (name, "ServerUrl"))
    return g_variant_new_string (vup_store_get_server () ? vup_store_get_server () : "");
  if (g_str_equal (name, "StreamRemoteAddress"))
    return g_variant_new_string (remote);
  if (g_str_equal (name, "StreamRemotePort"))
    return g_variant_new_uint16 (remote_port);
  if (g_str_equal (name, "StreamLocalAddress"))
    return g_variant_new_string (local);
  if (g_str_equal (name, "StreamLocalPort"))
    return g_variant_new_uint16 (local_port);
  return NULL;
}

void
vup_notify_state (void)
{
  static const char *const names[] = { "State", "Error", "ServerUrl", "StreamRemoteAddress", "StreamRemotePort",
                                       "StreamLocalAddress", "StreamLocalPort" };
  static GVariant *last[G_N_ELEMENTS (names)];
  GVariantBuilder changed;
  gboolean any = FALSE;

  if (!manager_id)
    return;
  g_variant_builder_init (&changed, G_VARIANT_TYPE_VARDICT);
  for (guint i = 0; i < G_N_ELEMENTS (names); i++)
    {
      GVariant *value = g_variant_ref_sink (manager_property (names[i]));

      if (last[i] && g_variant_equal (last[i], value))
        {
          g_variant_unref (value);
          continue;
        }
      g_clear_pointer (&last[i], g_variant_unref);
      last[i] = value;
      g_variant_builder_add (&changed, "{sv}", names[i], value);
      any = TRUE;
    }
  if (!any)
    {
      g_variant_builder_clear (&changed);
      return;
    }
  g_dbus_connection_emit_signal (bus, NULL, VUP_MANAGER_PATH, "org.freedesktop.DBus.Properties", "PropertiesChanged",
                                 g_variant_new ("(sa{sv}as)", VUP_MANAGER_IFACE, &changed, NULL), NULL);
}

void
vup_notify_registrations (void)
{
  if (manager_id)
    g_dbus_connection_emit_signal (bus, NULL, VUP_MANAGER_PATH, VUP_MANAGER_IFACE, "RegistrationsChanged", NULL,
                                   NULL);
}

/* a registration goes (Unregister, Settings, or its app is gone); @tell: whether, and why, the app hears of it
 * (Unregistered) */
void
vup_registration_removed (VupRegistration *reg, VupTell tell)
{
  g_autofree char *service = g_strdup (reg->service), *token = g_strdup (reg->token);

  vup_deliver_forget (reg);
  vup_store_remove (reg);
  vup_stream_topics_changed ();
  vup_notify_registrations ();
  if (tell != VUP_TELL_NOBODY)
    vup_deliver_unregistered (service, token, tell);
}

/* --- org.unifiedpush.Distributor2 */

static void
reply_registration (GDBusMethodInvocation *invocation, const char *reason)
{
  GVariantBuilder result;

  g_variant_builder_init (&result, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_add (&result, "{sv}", "success",
                         g_variant_new_string (reason ? "REGISTRATION_FAILED" : "REGISTRATION_SUCCEEDED"));
  if (reason)
    g_variant_builder_add (&result, "{sv}", "reason", g_variant_new_string (reason));
  g_dbus_method_invocation_return_value (invocation, g_variant_new ("(a{sv})", &result));
}

/* @key in @options: NULL when it is not there, @error set when it is there but not a string */
static const char *
option_string (GVariant *options, const char *key, const char **bad)
{
  g_autoptr(GVariant) value = g_variant_lookup_value (options, key, NULL);

  if (!value)
    return NULL;
  if (!g_variant_is_of_type (value, G_VARIANT_TYPE_STRING))
    {
      *bad = key;
      return NULL;
    }
  return g_variant_get_string (value, NULL);   /* the value lives on in @options */
}

typedef struct {
  GDBusMethodInvocation *invocation;
  char *service, *token, *description, *vapid;
} Registering;

static void
registering_free (Registering *registering)
{
  g_object_unref (registering->invocation);
  g_free (registering->service);
  g_free (registering->token);
  g_free (registering->description);
  g_free (registering->vapid);
  g_free (registering);
}

static void
on_service_owner (GObject *source, GAsyncResult *result, gpointer data)
{
  Registering *registering = data;
  g_autoptr(GVariant) owner = g_dbus_connection_call_finish (G_DBUS_CONNECTION (source), result, NULL);
  const char *sender = g_dbus_method_invocation_get_sender (registering->invocation);
  VupRegistration *reg = vup_store_by_token (registering->token);
  const char *owner_name = NULL;

  if (owner)
    g_variant_get (owner, "(&s)", &owner_name);
  if (g_strcmp0 (owner_name, sender) != 0)
    {
      g_message ("Register from %s for %s, a name it does not own: refused", sender, registering->service);
      reply_registration (registering->invocation, "UNAUTHORIZED");
    }
  else if (reg && !g_str_equal (reg->service, registering->service))
    {
      g_message ("Register from %s with the token of another app's registration: refused", registering->service);
      reply_registration (registering->invocation, "UNAUTHORIZED");
    }
  else if (!reg && vup_store_count (NULL) >= VUP_MAX_REGISTRATIONS)
    {
      g_warning ("%s: there are %u registrations already: refused", registering->service, VUP_MAX_REGISTRATIONS);
      reply_registration (registering->invocation, "ACTION_REQUIRED");
    }
  else if (!reg && vup_store_count (registering->service) >= VUP_MAX_PER_SERVICE)
    {
      g_warning ("%s has %u registrations already: refused", registering->service, VUP_MAX_PER_SERVICE);
      reply_registration (registering->invocation, "ACTION_REQUIRED");
    }
  else
    {
      if (!reg)
        {
          VupTell waiting = vup_deliver_unregistered_waiting (registering->service, registering->token);

          if (waiting == VUP_TELL_REMOVED)
            {
              g_message ("%s registered a token removed in Settings before it heard of that: it hears of it now",
                         registering->service);
              reply_registration (registering->invocation, NULL);
              registering_free (registering);
              return;
            }
          if (waiting == VUP_TELL_CONFIRM)
            g_message ("%s registered again before its Unregister was confirmed: it is not confirmed now",
                       registering->service);
          reg = vup_store_add (registering->token, registering->service, registering->description,
                               registering->vapid);
          g_message ("%s registered (token %s, topic %s)", reg->service, vup_redact (reg->token),
                     vup_redact (reg->topic));
          vup_stream_topics_changed ();
          vup_notify_registrations ();
        }
      else
        {
          g_message ("%s registered again (token %s)", reg->service, vup_redact (reg->token));
          if (g_strcmp0 (reg->description, registering->description) != 0
              || g_strcmp0 (reg->vapid, registering->vapid) != 0)
            {
              g_free (reg->description);
              reg->description = g_strdup (registering->description);
              g_free (reg->vapid);
              reg->vapid = g_strdup (registering->vapid);
              vup_store_save ();
              vup_notify_registrations ();
            }
        }
      vup_deliver_check_activatable (reg);
      reply_registration (registering->invocation, NULL);
      /* the endpoint follows the reply; for a topic the server has not subscribed yet, once it has */
      if (reg->active)
        vup_deliver_new_endpoint (reg, TRUE);
      else
        reg->endpoints_owed = MIN (reg->endpoints_owed + 1, 16);
    }
  registering_free (registering);
}

static void
handle_register (GDBusMethodInvocation *invocation, GVariant *parameters)
{
  g_autoptr(GVariant) options = NULL;
  const char *service, *token, *description, *vapid, *bad = NULL;
  Registering *registering;

  g_variant_get (parameters, "(@a{sv})", &options);
  service = option_string (options, "service", &bad);
  token = option_string (options, "token", &bad);
  description = option_string (options, "description", &bad);
  vapid = option_string (options, "vapid", &bad);
  if (bad)
    bad = "an option is not a string";
  else if (!service || !g_dbus_is_name (service) || g_dbus_is_unique_name (service))
    bad = "\"service\" must be the app's D-Bus name";
  else if (!token || !*token || !vup_utf8_within (token, VUP_TOKEN_MAX))
    bad = "\"token\" must be UTF-8, 1 to 100 bytes";
  else if (description && !vup_utf8_within (description, VUP_DESCRIPTION_MAX))
    bad = "\"description\" must be UTF-8, up to 100 bytes";
  else if (vapid && !vup_vapid_is_valid (vapid))
    bad = "\"vapid\" must be an uncompressed P-256 public key in base64url (87 characters)";
  if (bad)
    {
      g_message ("Register from %s: %s", g_dbus_method_invocation_get_sender (invocation), bad);
      g_dbus_method_invocation_return_dbus_error (invocation, "org.freedesktop.DBus.Error.InvalidArgs", bad);
      return;
    }

  registering = g_new0 (Registering, 1);
  registering->invocation = g_object_ref (invocation);
  registering->service = g_strdup (service);
  registering->token = g_strdup (token);
  registering->description = description && *description ? g_strdup (description) : NULL;
  registering->vapid = g_strdup (vapid);
  /* the caller must be the app: messages go to the name, and Settings shows it */
  g_dbus_connection_call (bus, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus",
                          "GetNameOwner", g_variant_new ("(s)", service), G_VARIANT_TYPE ("(s)"),
                          G_DBUS_CALL_FLAGS_NONE, -1, NULL, on_service_owner, registering);
}

static void
handle_unregister (GDBusMethodInvocation *invocation, GVariant *parameters)
{
  g_autoptr(GVariant) options = NULL;
  const char *token, *bad = NULL;
  VupRegistration *reg;

  g_variant_get (parameters, "(@a{sv})", &options);
  token = option_string (options, "token", &bad);
  if (bad || !token)
    {
      g_dbus_method_invocation_return_dbus_error (invocation, "org.freedesktop.DBus.Error.InvalidArgs",
                                                  "\"token\" must be a string");
      return;
    }
  reg = vup_store_by_token (token);
  g_dbus_method_invocation_return_value (invocation, g_variant_new_parsed ("(@a{sv} {},)"));
  if (!reg)
    return;
  g_message ("%s unregistered (token %s)", reg->service, vup_redact (reg->token));
  vup_registration_removed (reg, VUP_TELL_CONFIRM);
}

static void
on_distributor_call (GDBusConnection *connection, const char *sender, const char *path, const char *interface,
                     const char *method, GVariant *parameters, GDBusMethodInvocation *invocation, gpointer data)
{
  if (g_str_equal (method, "Register"))
    handle_register (invocation, parameters);
  else if (g_str_equal (method, "Unregister"))
    handle_unregister (invocation, parameters);
  else
    g_dbus_method_invocation_return_dbus_error (invocation, "org.freedesktop.DBus.Error.UnknownMethod", method);
}

/* --- vibeos.UnifiedPush1 */

static GVariant *
list_registrations (void)
{
  GVariantBuilder list;
  GList *registrations = vup_store_list ();

  g_variant_builder_init (&list, G_VARIANT_TYPE ("aa{sv}"));
  for (GList *l = registrations; l; l = l->next)
    {
      VupRegistration *reg = l->data;
      GVariantBuilder entry;

      g_variant_builder_init (&entry, G_VARIANT_TYPE_VARDICT);
      g_variant_builder_add (&entry, "{sv}", "id", g_variant_new_string (reg->id));
      g_variant_builder_add (&entry, "{sv}", "service", g_variant_new_string (reg->service));
      g_variant_builder_add (&entry, "{sv}", "description",
                             g_variant_new_string (reg->description ? reg->description : ""));
      g_variant_builder_add (&entry, "{sv}", "vapid", g_variant_new_boolean (reg->vapid != NULL));
      g_variant_builder_add (&entry, "{sv}", "created", g_variant_new_int64 (reg->created));
      g_variant_builder_add (&entry, "{sv}", "last-message", g_variant_new_int64 (reg->last_message));
      g_variant_builder_add (&entry, "{sv}", "messages", g_variant_new_uint64 (reg->messages));
      g_variant_builder_add (&entry, "{sv}", "pending", g_variant_new_uint32 (vup_deliver_pending (reg->service)));
      g_variant_builder_add (&entry, "{sv}", "active", g_variant_new_boolean (reg->active));
      g_variant_builder_add (&entry, "{sv}", "activatable", g_variant_new_boolean (reg->activatable));
      g_variant_builder_add_value (&list, g_variant_builder_end (&entry));
    }
  g_list_free (registrations);
  return g_variant_new ("(aa{sv})", &list);
}

typedef struct {
  GDBusMethodInvocation *invocation;
  guint delivered_before;
} Collecting;

static void
on_collected (GError *error, gpointer data)
{
  Collecting *collecting = data;

  if (error)
    {
      g_message ("Collect: %s", error->message);
      g_dbus_method_invocation_return_dbus_error (collecting->invocation, ERROR_FAILED, error->message);
    }
  else
    {
      guint delivered = vup_deliver_count () - collecting->delivered_before;
      g_message ("Collect: %u delivered", delivered);
      g_dbus_method_invocation_return_value (collecting->invocation, g_variant_new ("(u)", delivered));
    }
  g_object_unref (collecting->invocation);
  g_free (collecting);
}

static void
on_manager_call (GDBusConnection *connection, const char *sender, const char *path, const char *interface,
                 const char *method, GVariant *parameters, GDBusMethodInvocation *invocation, gpointer data)
{
  if (g_str_equal (method, "ListRegistrations"))
    g_dbus_method_invocation_return_value (invocation, list_registrations ());
  else if (g_str_equal (method, "RemoveRegistration"))
    {
      const char *id;
      VupRegistration *reg;

      g_variant_get (parameters, "(&s)", &id);
      if (!(reg = vup_store_by_id (id)))
        {
          g_dbus_method_invocation_return_dbus_error (invocation, ERROR_NOT_FOUND, "no registration has that id");
          return;
        }
      g_message ("%s's registration %s removed over D-Bus (by %s)", reg->service, reg->id, sender);
      g_dbus_method_invocation_return_value (invocation, NULL);
      vup_registration_removed (reg, VUP_TELL_REMOVED);
    }
  else if (g_str_equal (method, "Collect"))
    {
      Collecting *collecting = g_new0 (Collecting, 1);

      collecting->invocation = g_object_ref (invocation);
      collecting->delivered_before = vup_deliver_count ();
      vup_stream_collect (on_collected, collecting);
    }
  else if (g_str_equal (method, "Reconnect"))
    {
      g_dbus_method_invocation_return_value (invocation, NULL);
      if (vup_store_count (NULL) > 0)
        vup_stream_reconnect ("asked over D-Bus");
    }
  else
    g_dbus_method_invocation_return_dbus_error (invocation, "org.freedesktop.DBus.Error.UnknownMethod", method);
}

static GVariant *
on_manager_get (GDBusConnection *connection, const char *sender, const char *path, const char *interface,
                const char *property, GError **error, gpointer data)
{
  GVariant *value = manager_property (property);

  if (!value)
    g_set_error (error, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_PROPERTY, "no property %s", property);
  return value;
}

static gboolean
on_manager_set (GDBusConnection *connection, const char *sender, const char *path, const char *interface,
                const char *property, GVariant *value, GError **error, gpointer data)
{
  g_autofree char *server = NULL;

  if (!g_str_equal (property, "ServerUrl"))
    {
      g_set_error (error, G_DBUS_ERROR, G_DBUS_ERROR_PROPERTY_READ_ONLY, "%s is read-only", property);
      return FALSE;
    }
  if (!(server = vup_normalize_server (g_variant_get_string (value, NULL))))
    {
      g_set_error (error, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS, "not an http or https URL");
      return FALSE;
    }
  g_message ("the push server set to %s (by %s)", server, sender);
  g_settings_set_string (settings, "server-url", server);   /* the setting's change handler takes it from here */
  return TRUE;
}

/* --- setting up */

static GDBusInterfaceInfo *
interface_from (const char *resource, const char *name)
{
  g_autoptr(GBytes) xml = g_resources_lookup_data (resource, G_RESOURCE_LOOKUP_FLAGS_NONE, NULL);
  g_autoptr(GError) error = NULL;
  g_autoptr(GDBusNodeInfo) node = g_dbus_node_info_new_for_xml (g_bytes_get_data (xml, NULL), &error);

  if (!node)
    g_error ("%s: %s", resource, error->message);
  return g_dbus_interface_info_ref (g_dbus_node_info_lookup_interface (node, name));
}

static void
on_name_acquired (GDBusConnection *connection, const char *name, gpointer data)
{
  g_message ("%s is ours", name);
  name_owned = TRUE;
  vup_stream_start ();
}

static void
on_name_lost (GDBusConnection *connection, const char *name, gpointer data)
{
  if (!connection)
    g_warning ("lost the session bus");
  else if (!name_owned)
    g_warning ("%s belongs to another process: another distributor runs", name);
  else
    g_warning ("lost %s", name);
  g_main_loop_quit (loop);
}

static gboolean
on_quit_signal (gpointer data)
{
  g_main_loop_quit (loop);
  return G_SOURCE_REMOVE;
}

int
main (int argc, char **argv)
{
  static const GDBusInterfaceVTable distributor_vtable = { on_distributor_call, NULL, NULL };
  static const GDBusInterfaceVTable manager_vtable = { on_manager_call, on_manager_get, on_manager_set };
  GDBusInterfaceInfo *distributor_info, *manager_info;
  g_autoptr(GError) error = NULL;
  guint owner_id;

  if (argc > 1)
    {
      g_printerr ("usage: %s\n  VibeOS's UnifiedPush distributor, a D-Bus service (vibeos-unifiedpush(8))\n", argv[0]);
      return 2;
    }

  g_resources_register (vup_get_resource ());
  distributor_info = interface_from ("/vibeos/UnifiedPush/org.unifiedpush.Distributor2.xml", VUP_DISTRIBUTOR_IFACE);
  manager_info = interface_from ("/vibeos/UnifiedPush/vibeos.UnifiedPush1.xml", VUP_MANAGER_IFACE);

  settings = g_settings_new (VUP_SCHEMA);
  vup_store_load ();
  if (!(bus = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, &error)))
    {
      g_printerr ("no session bus: %s\n", error->message);
      return 1;
    }
  /* the objects are there before the name: a call made the moment the name appears finds them */
  distributor_id = g_dbus_connection_register_object (bus, VUP_DISTRIBUTOR_PATH, distributor_info,
                                                      &distributor_vtable, NULL, NULL, &error);
  if (distributor_id)
    manager_id = g_dbus_connection_register_object (bus, VUP_MANAGER_PATH, manager_info, &manager_vtable, NULL,
                                                    NULL, &error);
  if (!manager_id)
    {
      g_printerr ("%s\n", error->message);
      return 1;
    }
  vup_deliver_init (bus);
  vup_stream_init (settings);
  vup_notify_state ();

  loop = g_main_loop_new (NULL, FALSE);
  g_unix_signal_add (SIGTERM, on_quit_signal, NULL);
  g_unix_signal_add (SIGINT, on_quit_signal, NULL);
  owner_id = g_bus_own_name_on_connection (bus, VUP_BUS_NAME, G_BUS_NAME_OWNER_FLAGS_DO_NOT_QUEUE, on_name_acquired,
                                           on_name_lost, NULL, NULL);
  g_main_loop_run (loop);

  g_bus_unown_name (owner_id);
  vup_stream_shutdown ();
  vup_deliver_shutdown ();
  vup_store_flush ();
  g_dbus_connection_flush_sync (bus, NULL, NULL);
  return name_owned ? 0 : 1;
}
