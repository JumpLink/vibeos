/* VibeosUnifiedPush: see vibeos-unified-push.h.
 *
 * The app's side of the UnifiedPush D-Bus spec 0.3.0, a connector: org.unifiedpush.Connector2 at
 * /org/unifiedpush/Connector on the app's D-Bus connection, one object for all of the app's registrations (each
 * VibeosUnifiedPush is one, told apart by its token), and org.unifiedpush.Distributor2.Register and Unregister on
 * the distributor. The distributor is the one $UNIFIEDPUSH_DISTRIBUTOR names, else the one the registration was
 * made with, else org.unifiedpush.Distributor.vibeos, else the first there is.
 *
 * A registration's state is a key file, $XDG_DATA_HOME/<app id>/unifiedpush/<instance> (0600, in a 0700
 * directory): the token (16 random bytes, base64url), the distributor, the endpoint last given to the app, and the
 * P-256 key pair and auth secret its messages are encrypted for (RFC 8291). It goes when the registration does: the
 * next one gets a new token, new keys and a new endpoint.
 *
 * Calls with a token this app does not have, and messages that are not 1 to 4096 bytes, are ignored (the spec);
 * they are answered all the same, so that nobody waits for an answer. */
#include <errno.h>
#include <string.h>

#include <gio/gdesktopappinfo.h>
#include <glib/gstdio.h>
#include <gnutls/crypto.h>
#include <gnutls/gnutls.h>

#include "vibeos-unified-push.h"
#include "vibeos-webpush.h"

#define CONNECTOR_PATH     "/org/unifiedpush/Connector"
#define CONNECTOR_IFACE    "org.unifiedpush.Connector2"
#define DISTRIBUTOR_PATH   "/org/unifiedpush/Distributor"
#define DISTRIBUTOR_IFACE  "org.unifiedpush.Distributor2"
#define DISTRIBUTOR_PREFIX "org.unifiedpush.Distributor."
#define OUR_DISTRIBUTOR    "org.unifiedpush.Distributor.vibeos"
#define TOKEN_BYTES        16
#define TEXT_MAX           100      /* bytes: a token, a description */
#define ENDPOINT_MAX       1000
#define MESSAGE_MAX        4096
#define VAPID_LENGTH       87
#define HOLD_SECONDS       10       /* a registration keeps the app up this long for its endpoint */
#define CALL_TIMEOUT_MS    25000

static const char connector_xml[] =
  "<node>"
  "  <interface name='" CONNECTOR_IFACE "'>"
  "    <method name='Message'>"
  "      <arg name='options' type='a{sv}' direction='in'/>"
  "      <arg name='result' type='a{sv}' direction='out'/>"
  "    </method>"
  "    <method name='NewEndpoint'>"
  "      <arg name='options' type='a{sv}' direction='in'/>"
  "      <arg name='result' type='a{sv}' direction='out'/>"
  "    </method>"
  "    <method name='Unregistered'>"
  "      <arg name='options' type='a{sv}' direction='in'/>"
  "      <arg name='result' type='a{sv}' direction='out'/>"
  "    </method>"
  "  </interface>"
  "</node>";

/* the connector object on one D-Bus connection, shared by the app's registrations on it */
typedef struct {
  GDBusConnection *connection;
  guint object_id;
  GList *pushes;              /* VibeosUnifiedPush*, not owned */
} Connector;

static GList *connectors;     /* Connector* */

struct _VibeosUnifiedPush {
  GObject parent;

  GApplication *app;          /* a weak pointer: the app owns us, not the other way round */
  char *instance;
  char *path;                 /* the state's key file */
  Connector *connector;
  gulong registered_handler;
  GCancellable *cancellable;

  /* the state */
  gboolean loaded;
  char *token;
  char *distributor;
  char *endpoint;
  VibeosWebPushKeys keys;

  /* registering */
  char *description;
  char *vapid;
  gboolean waiting_for_bus;   /* register was called before the app was on the bus */
  gboolean got_endpoint;      /* NewEndpoint came since the last Register */
  gboolean held;              /* the app is held for the registration */
  guint hold_id;
};

G_DEFINE_FINAL_TYPE (VibeosUnifiedPush, vibeos_unified_push, G_TYPE_OBJECT)

enum { PROP_0, PROP_APPLICATION, PROP_INSTANCE, PROP_ENDPOINT, N_PROPS };
static GParamSpec *props[N_PROPS];

enum { NEW_ENDPOINT, MESSAGE, UNREGISTERED, REGISTRATION_FAILED, N_SIGNALS };
static guint signals[N_SIGNALS];

/* a secret's first 4 characters, for the log */
static char *
redact (const char *secret)
{
  return secret && strlen (secret) >= 12 ? g_strdup_printf ("%.4s...", secret) : g_strdup ("...");
}

static const char *
app_id (VibeosUnifiedPush *self)
{
  return self->app ? g_application_get_application_id (self->app) : NULL;
}

/* --- the state */

static void
forget_state (VibeosUnifiedPush *self)
{
  g_clear_pointer (&self->token, g_free);
  g_clear_pointer (&self->distributor, g_free);
  g_clear_pointer (&self->endpoint, g_free);
  memset (&self->keys, 0, sizeof self->keys);
}

static gboolean
load_key (GKeyFile *file, const char *name, guint8 *out, gsize size)
{
  g_autofree char *text = g_key_file_get_string (file, "keys", name, NULL);
  gsize length = 0;
  g_autofree guint8 *raw = _vibeos_base64url_decode (text, &length);

  if (!raw || length != size)
    return FALSE;
  memcpy (out, raw, size);
  memset (raw, 0, length);
  return TRUE;
}

static void
load_state (VibeosUnifiedPush *self)
{
  g_autoptr(GKeyFile) file = g_key_file_new ();
  g_autoptr(GError) error = NULL;

  if (self->loaded)
    return;
  self->loaded = TRUE;
  if (!g_key_file_load_from_file (file, self->path, G_KEY_FILE_NONE, &error))
    {
      if (!g_error_matches (error, G_FILE_ERROR, G_FILE_ERROR_NOENT))
        g_warning ("%s: %s", self->path, error->message);
      return;
    }
  self->token = g_key_file_get_string (file, "registration", "token", NULL);
  self->distributor = g_key_file_get_string (file, "registration", "distributor", NULL);
  self->endpoint = g_key_file_get_string (file, "registration", "endpoint", NULL);
  if (!self->token || !*self->token || strlen (self->token) > TEXT_MAX
      || !load_key (file, "private", self->keys.private_key, sizeof self->keys.private_key)
      || !load_key (file, "public", self->keys.public_key, sizeof self->keys.public_key)
      || !load_key (file, "auth", self->keys.auth, sizeof self->keys.auth)
      || !_vibeos_webpush_check (&self->keys, NULL))
    {
      g_warning ("%s: not a registration's state; registering anew", self->path);
      forget_state (self);
    }
}

static gboolean
save_state (VibeosUnifiedPush *self)
{
  g_autoptr(GKeyFile) file = g_key_file_new ();
  g_autoptr(GError) error = NULL;
  g_autofree char *dir = g_path_get_dirname (self->path), *data = NULL;
  g_autofree char *private_key = _vibeos_base64url_encode (self->keys.private_key, sizeof self->keys.private_key);
  g_autofree char *public_key = _vibeos_base64url_encode (self->keys.public_key, sizeof self->keys.public_key);
  g_autofree char *auth = _vibeos_base64url_encode (self->keys.auth, sizeof self->keys.auth);
  gsize length;

  g_key_file_set_string (file, "registration", "token", self->token);
  if (self->distributor)
    g_key_file_set_string (file, "registration", "distributor", self->distributor);
  if (self->endpoint)
    g_key_file_set_string (file, "registration", "endpoint", self->endpoint);
  g_key_file_set_string (file, "keys", "private", private_key);
  g_key_file_set_string (file, "keys", "public", public_key);
  g_key_file_set_string (file, "keys", "auth", auth);
  data = g_key_file_to_data (file, &length, NULL);
  memset (private_key, 0, strlen (private_key));
  if (g_mkdir_with_parents (dir, 0700) < 0)
    {
      g_warning ("%s: %s", dir, g_strerror (errno));
      return FALSE;
    }
  if (!g_file_set_contents_full (self->path, data, length, G_FILE_SET_CONTENTS_CONSISTENT, 0600, &error))
    {
      g_warning ("%s", error->message);
      memset (data, 0, length);
      return FALSE;
    }
  memset (data, 0, length);
  return TRUE;
}

static void
delete_state (VibeosUnifiedPush *self)
{
  if (g_unlink (self->path) < 0 && errno != ENOENT)
    g_warning ("%s: %s", self->path, g_strerror (errno));
  forget_state (self);
}

/* a token and keys for a registration that has none */
static gboolean
make_state (VibeosUnifiedPush *self)
{
  guint8 raw[TOKEN_BYTES];
  g_autoptr(GError) error = NULL;
  int code;

  load_state (self);
  if (self->token)
    return TRUE;
  if ((code = gnutls_rnd (GNUTLS_RND_KEY, raw, sizeof raw)) < 0)
    {
      g_warning ("a new push token: %s", gnutls_strerror (code));
      return FALSE;
    }
  if (!_vibeos_webpush_generate (&self->keys, &error))
    {
      g_warning ("new push keys: %s", error->message);
      return FALSE;
    }
  self->token = _vibeos_base64url_encode (raw, sizeof raw);
  memset (raw, 0, sizeof raw);
  g_clear_pointer (&self->endpoint, g_free);
  return save_state (self);
}

/* --- holding the app */

static void
release_hold (VibeosUnifiedPush *self)
{
  g_clear_handle_id (&self->hold_id, g_source_remove);
  if (self->held && self->app)
    g_application_release (self->app);
  self->held = FALSE;
}

static gboolean
on_hold_over (gpointer data)
{
  VibeosUnifiedPush *self = data;

  self->hold_id = 0;
  release_hold (self);
  return G_SOURCE_REMOVE;
}

/* while registering: an app started by D-Bus stays up until the endpoint is in, or HOLD_SECONDS */
static void
hold_for_registration (VibeosUnifiedPush *self)
{
  if (!self->held)
    g_application_hold (self->app);
  self->held = TRUE;
  g_clear_handle_id (&self->hold_id, g_source_remove);
  self->hold_id = g_timeout_add_seconds (HOLD_SECONDS, on_hold_over, self);
}

/* --- the connector */

static VibeosUnifiedPush *
connector_find (Connector *connector, const char *token)
{
  for (GList *l = connector->pushes; l; l = l->next)
    {
      VibeosUnifiedPush *self = l->data;
      if (token && self->token && g_str_equal (self->token, token))
        return self;
    }
  return NULL;
}

static const char *
option_string (GVariant *options, const char *key)
{
  const char *value = NULL;

  g_variant_lookup (options, key, "&s", &value);
  return value;
}

static void
fail (VibeosUnifiedPush *self, const char *reason)
{
  release_hold (self);
  g_signal_emit (self, signals[REGISTRATION_FAILED], 0, reason);
}

static void
handle_new_endpoint (VibeosUnifiedPush *self, const char *endpoint)
{
  g_autofree char *p256dh = NULL, *auth = NULL;

  if (!endpoint || !*endpoint || strlen (endpoint) > ENDPOINT_MAX || !g_utf8_validate (endpoint, -1, NULL))
    return;
  self->got_endpoint = TRUE;
  if (g_strcmp0 (self->endpoint, endpoint) != 0)
    {
      g_free (self->endpoint);
      self->endpoint = g_strdup (endpoint);
      save_state (self);
      g_object_notify_by_pspec (G_OBJECT (self), props[PROP_ENDPOINT]);
      p256dh = _vibeos_base64url_encode (self->keys.public_key, sizeof self->keys.public_key);
      auth = _vibeos_base64url_encode (self->keys.auth, sizeof self->keys.auth);
      g_debug ("%s/%s: a new endpoint", app_id (self), self->instance);
      g_signal_emit (self, signals[NEW_ENDPOINT], 0, self->endpoint, p256dh, auth);
    }
  release_hold (self);
}

static void
handle_message (VibeosUnifiedPush *self, GVariant *options, const char *id)
{
  g_autoptr(GVariant) value = g_variant_lookup_value (options, "message", G_VARIANT_TYPE_BYTESTRING);
  g_autoptr(GError) error = NULL;
  g_autoptr(GBytes) plain = NULL;
  g_autoptr(GApplication) app = self->app ? g_object_ref (self->app) : NULL;
  gsize size = value ? g_variant_get_size (value) : 0;

  if (size == 0 || size > MESSAGE_MAX)
    {
      g_debug ("%s/%s: a push message of %" G_GSIZE_FORMAT " bytes, not 1 to %u: ignored", app_id (self),
               self->instance, size, MESSAGE_MAX);
      return;
    }
  if (!(plain = _vibeos_webpush_decrypt (&self->keys, g_variant_get_data (value), size, &error)))
    {
      g_warning ("%s/%s: a push message (%s) that cannot be decrypted: %s", app_id (self), self->instance,
                 id ? id : "no id", error->message);
      return;
    }
  /* the app stays up while its handlers run, and goes once they are done if nothing else holds it */
  if (app)
    g_application_hold (app);
  g_signal_emit (self, signals[MESSAGE], 0, plain);
  if (app)
    g_application_release (app);
}

static void
handle_unregistered (VibeosUnifiedPush *self)
{
  g_debug ("%s/%s: unregistered", app_id (self), self->instance);
  release_hold (self);
  delete_state (self);
  g_object_notify_by_pspec (G_OBJECT (self), props[PROP_ENDPOINT]);
  g_signal_emit (self, signals[UNREGISTERED], 0);
}

static void
on_connector_call (GDBusConnection *connection, const char *sender, const char *path, const char *interface,
                   const char *method, GVariant *parameters, GDBusMethodInvocation *invocation, gpointer data)
{
  Connector *connector = data;
  g_autoptr(GVariant) options = NULL;
  g_autoptr(VibeosUnifiedPush) self = NULL;
  GVariantBuilder result;
  const char *token, *id;

  g_variant_get (parameters, "(@a{sv})", &options);
  token = option_string (options, "token");
  id = option_string (options, "id");
  if ((self = connector_find (connector, token)))
    g_object_ref (self);

  /* answered first: the distributor does not wait for what the app does with it */
  g_variant_builder_init (&result, G_VARIANT_TYPE_VARDICT);
  if (g_str_equal (method, "Message") && id)
    g_variant_builder_add (&result, "{sv}", "id", g_variant_new_string (id));
  g_dbus_method_invocation_return_value (invocation, g_variant_new ("(a{sv})", &result));

  if (!self)
    {
      g_debug ("%s with a token this app does not have: ignored", method);
      return;
    }
  if (g_str_equal (method, "Message"))
    handle_message (self, options, id);
  else if (g_str_equal (method, "NewEndpoint"))
    handle_new_endpoint (self, option_string (options, "endpoint"));
  else if (g_str_equal (method, "Unregistered"))
    handle_unregistered (self);
}

static void
connector_detach (VibeosUnifiedPush *self)
{
  Connector *connector = self->connector;

  if (!connector)
    return;
  self->connector = NULL;
  connector->pushes = g_list_remove (connector->pushes, self);
  if (connector->pushes)
    return;
  g_dbus_connection_unregister_object (connector->connection, connector->object_id);
  g_object_unref (connector->connection);
  connectors = g_list_remove (connectors, connector);
  g_free (connector);
}

static void
connector_attach (VibeosUnifiedPush *self, GDBusConnection *connection)
{
  static GDBusNodeInfo *node;
  static const GDBusInterfaceVTable vtable = { on_connector_call, NULL, NULL };
  g_autoptr(GError) error = NULL;
  Connector *connector = NULL;

  if (self->connector && self->connector->connection == connection)
    return;
  connector_detach (self);
  for (GList *l = connectors; l && !connector; l = l->next)
    if (((Connector *) l->data)->connection == connection)
      connector = l->data;
  if (!connector)
    {
      if (!node)
        node = g_dbus_node_info_new_for_xml (connector_xml, NULL);
      connector = g_new0 (Connector, 1);
      connector->object_id = g_dbus_connection_register_object (connection, CONNECTOR_PATH, node->interfaces[0],
                                                                &vtable, connector, NULL, &error);
      if (!connector->object_id)
        {
          g_warning ("push messages cannot reach this app: %s", error->message);
          g_free (connector);
          return;
        }
      connector->connection = g_object_ref (connection);
      connectors = g_list_prepend (connectors, connector);
    }
  for (GList *l = connector->pushes; l; l = l->next)
    if (g_str_equal (((VibeosUnifiedPush *) l->data)->instance, self->instance))
      g_warning ("%s: two VibeosUnifiedPush for the instance \"%s\"", app_id (self), self->instance);
  connector->pushes = g_list_prepend (connector->pushes, self);
  self->connector = connector;
}

/* the connector on the connection the app has, or will have: GApplication uses the session bus's shared one, so it
 * is there before the app's name is, and a message sent the moment the name appears finds it */
static void
attach (VibeosUnifiedPush *self)
{
  g_autoptr(GDBusConnection) session = NULL;
  GDBusConnection *connection = NULL;

  if (g_application_get_is_registered (self->app))
    connection = g_application_get_dbus_connection (self->app);
  else
    connection = session = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, NULL);
  if (connection)
    connector_attach (self, connection);
}

/* --- registering */

typedef struct {
  VibeosUnifiedPush *self;
  GDBusConnection *connection;
  GVariant *activatable;
} Choosing;

static void
on_register_reply (GObject *source, GAsyncResult *result, gpointer data)
{
  g_autofree char **call = data;     /* the object, the token it registered */
  g_autoptr(VibeosUnifiedPush) self = (VibeosUnifiedPush *) call[0];
  g_autofree char *token = call[1];
  g_autoptr(GError) error = NULL;
  g_autoptr(GVariant) reply = g_dbus_connection_call_finish (G_DBUS_CONNECTION (source), result, &error);
  g_autoptr(GVariant) options = NULL;
  const char *success = NULL, *reason = NULL;

  if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    return;
  /* Unregistered came while the Register was on its way (the registration was removed in Settings as the app
   * started), so the distributor has the token again: it is taken back, and the app registers anew on its next
   * start */
  if (reply && g_strcmp0 (self->token, token) != 0)
    {
      g_dbus_connection_call (G_DBUS_CONNECTION (source), self->distributor ? self->distributor : OUR_DISTRIBUTOR,
                              DISTRIBUTOR_PATH, DISTRIBUTOR_IFACE, "Unregister",
                              g_variant_new_parsed ("({'token': <%s>},)", token), NULL, G_DBUS_CALL_FLAGS_NONE, -1,
                              NULL, NULL, NULL);
      release_hold (self);
      return;
    }
  if (!reply)
    {
      g_warning ("%s: the push distributor did not take the registration: %s", app_id (self), error->message);
      fail (self, "INTERNAL_ERROR");
      return;
    }
  g_variant_get (reply, "(@a{sv})", &options);
  g_variant_lookup (options, "success", "&s", &success);
  if (g_strcmp0 (success, "REGISTRATION_SUCCEEDED") != 0)
    {
      g_variant_lookup (options, "reason", "&s", &reason);
      g_message ("%s: push registration failed: %s", app_id (self), reason ? reason : "no reason given");
      fail (self, reason ? reason : "INTERNAL_ERROR");
      return;
    }
  /* NewEndpoint follows (it may have come already): the hold lasts until it is in */
  if (self->got_endpoint)
    release_hold (self);
}

static void
send_register (VibeosUnifiedPush *self, GDBusConnection *connection, const char *distributor)
{
  GVariantBuilder options;
  g_autofree char *token = redact (self->token);
  char **call;

  if (g_strcmp0 (self->distributor, distributor) != 0)
    {
      g_free (self->distributor);
      self->distributor = g_strdup (distributor);
      save_state (self);
    }
  g_variant_builder_init (&options, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_add (&options, "{sv}", "service", g_variant_new_string (app_id (self)));
  g_variant_builder_add (&options, "{sv}", "token", g_variant_new_string (self->token));
  g_variant_builder_add (&options, "{sv}", "description", g_variant_new_string (self->description));
  if (self->vapid)
    g_variant_builder_add (&options, "{sv}", "vapid", g_variant_new_string (self->vapid));
  g_debug ("%s/%s: registering with %s (token %s)", app_id (self), self->instance, distributor, token);
  self->got_endpoint = FALSE;
  call = g_new (char *, 2);
  call[0] = (char *) g_object_ref (self);
  call[1] = g_strdup (self->token);
  g_dbus_connection_call (connection, distributor, DISTRIBUTOR_PATH, DISTRIBUTOR_IFACE, "Register",
                          g_variant_new ("(a{sv})", &options), G_VARIANT_TYPE ("(a{sv})"), G_DBUS_CALL_FLAGS_NONE,
                          CALL_TIMEOUT_MS, self->cancellable, on_register_reply, call);
}

static gboolean
names_contain (const char **running, const char **activatable, const char *name)
{
  return name && (g_strv_contains (running, name) || g_strv_contains (activatable, name));
}

static int
compare_names (gconstpointer a, gconstpointer b)
{
  return strcmp (*(const char **) a, *(const char **) b);
}

static void
on_running_names (GObject *source, GAsyncResult *result, gpointer data)
{
  Choosing *choosing = data;
  g_autoptr(VibeosUnifiedPush) self = choosing->self;
  g_autoptr(GDBusConnection) connection = choosing->connection;
  g_autoptr(GVariant) activatable = choosing->activatable;
  g_autoptr(GVariant) running = g_dbus_connection_call_finish (G_DBUS_CONNECTION (source), result, NULL);
  g_autofree const char **running_names = NULL, **activatable_names = NULL;
  g_autoptr(GPtrArray) found = g_ptr_array_new ();
  const char *wanted = g_getenv ("UNIFIEDPUSH_DISTRIBUTOR"), *chosen = NULL;

  g_free (choosing);
  if (g_cancellable_is_cancelled (self->cancellable) || !self->app)
    return;
  /* Unregistered came meanwhile (the user removed the registration): the app has been told, and registers anew on
   * its next start */
  if (!self->token)
    {
      release_hold (self);
      return;
    }
  if (!running)
    {
      fail (self, "INTERNAL_ERROR");
      return;
    }
  g_variant_get (running, "(^a&s)", &running_names);
  g_variant_get (activatable, "(^a&s)", &activatable_names);
  if (wanted && g_str_has_prefix (wanted, DISTRIBUTOR_PREFIX) && names_contain (running_names, activatable_names, wanted))
    chosen = wanted;
  else if (names_contain (running_names, activatable_names, self->distributor))
    chosen = self->distributor;
  else if (names_contain (running_names, activatable_names, OUR_DISTRIBUTOR))
    chosen = OUR_DISTRIBUTOR;
  else
    {
      for (guint i = 0; running_names[i]; i++)
        if (g_str_has_prefix (running_names[i], DISTRIBUTOR_PREFIX))
          g_ptr_array_add (found, (gpointer) running_names[i]);
      for (guint i = 0; activatable_names[i]; i++)
        if (g_str_has_prefix (activatable_names[i], DISTRIBUTOR_PREFIX))
          g_ptr_array_add (found, (gpointer) activatable_names[i]);
      g_ptr_array_sort (found, compare_names);
      if (found->len > 0)
        chosen = found->pdata[0];
    }
  if (!chosen)
    {
      g_message ("%s: no UnifiedPush distributor is installed", app_id (self));
      fail (self, "ACTION_REQUIRED");
      return;
    }
  send_register (self, connection, chosen);
}

static void
on_activatable_names (GObject *source, GAsyncResult *result, gpointer data)
{
  Choosing *choosing = data;
  g_autoptr(GError) error = NULL;

  choosing->activatable = g_dbus_connection_call_finish (G_DBUS_CONNECTION (source), result, &error);
  if (!choosing->activatable)
    {
      if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        fail (choosing->self, "INTERNAL_ERROR");
      g_object_unref (choosing->self);
      g_object_unref (choosing->connection);
      g_free (choosing);
      return;
    }
  g_dbus_connection_call (choosing->connection, "org.freedesktop.DBus", "/org/freedesktop/DBus",
                          "org.freedesktop.DBus", "ListNames", NULL, G_VARIANT_TYPE ("(as)"), G_DBUS_CALL_FLAGS_NONE,
                          -1, choosing->self->cancellable, on_running_names, choosing);
}

static void
start_registration (VibeosUnifiedPush *self)
{
  GDBusConnection *connection = g_application_get_dbus_connection (self->app);
  Choosing *choosing;

  if (g_application_get_is_remote (self->app))
    {
      g_debug ("%s: push registers in the app's first instance, not here", app_id (self));
      return;
    }
  if (!connection)
    {
      g_warning ("%s is not on the session bus: no push messages", app_id (self));
      g_signal_emit (self, signals[REGISTRATION_FAILED], 0, "INTERNAL_ERROR");
      return;
    }
  connector_attach (self, connection);
  if (!make_state (self))
    {
      g_signal_emit (self, signals[REGISTRATION_FAILED], 0, "INTERNAL_ERROR");
      return;
    }
  hold_for_registration (self);
  choosing = g_new0 (Choosing, 1);
  choosing->self = g_object_ref (self);
  choosing->connection = g_object_ref (connection);
  g_dbus_connection_call (connection, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus",
                          "ListActivatableNames", NULL, G_VARIANT_TYPE ("(as)"), G_DBUS_CALL_FLAGS_NONE, -1,
                          self->cancellable, on_activatable_names, choosing);
}

static void
on_app_registered (GApplication *app, GParamSpec *pspec, VibeosUnifiedPush *self)
{
  if (!g_application_get_is_registered (app))
    return;
  attach (self);
  if (self->waiting_for_bus)
    {
      self->waiting_for_bus = FALSE;
      start_registration (self);
    }
}

static gboolean
emit_failed_internal (gpointer data)
{
  g_autoptr(VibeosUnifiedPush) self = data;

  g_signal_emit (self, signals[REGISTRATION_FAILED], 0, "INTERNAL_ERROR");
  return G_SOURCE_REMOVE;
}

static gboolean
is_vapid_key (const char *key)
{
  gsize length = 0;
  g_autofree guint8 *raw = NULL;

  if (strlen (key) != VAPID_LENGTH || !(raw = _vibeos_base64url_decode (key, &length)))
    return FALSE;
  return length == 65 && raw[0] == 0x04;
}

/* the app's name, as its desktop file has it: what Settings shows when the app gives no description */
static char *
app_name (VibeosUnifiedPush *self)
{
  g_autofree char *desktop_id = g_strconcat (app_id (self), ".desktop", NULL);
  g_autoptr(GDesktopAppInfo) info = g_desktop_app_info_new (desktop_id);

  if (info)
    return g_strdup (g_app_info_get_name (G_APP_INFO (info)));
  if (g_get_application_name () && g_strcmp0 (g_get_application_name (), g_get_prgname ()) != 0)
    return g_strdup (g_get_application_name ());
  return g_strdup (app_id (self));
}

/* at most @max bytes, cut between characters */
static char *
utf8_cut (const char *text, gsize max)
{
  g_autofree char *valid = g_utf8_make_valid (text, -1);
  const char *end = valid;

  text = valid;
  while (*end)
    {
      const char *next = g_utf8_next_char (end);
      if ((gsize) (next - text) > max)
        break;
      end = next;
    }
  return g_strndup (text, end - text);
}

void
vibeos_unified_push_register (VibeosUnifiedPush *self, const char *description, const char *vapid_public_key)
{
  g_autofree char *name = NULL;

  g_return_if_fail (VIBEOS_IS_UNIFIED_PUSH (self));
  if (!self->app)
    return;

  g_free (self->description);
  if (!description || !*description)
    description = name = app_name (self);
  self->description = utf8_cut (description, TEXT_MAX);
  g_clear_pointer (&self->vapid, g_free);
  if (vapid_public_key && !is_vapid_key (vapid_public_key))
    {
      g_warning ("%s: \"%s\" is not a VAPID public key (an uncompressed P-256 point, base64url)", app_id (self),
                 vapid_public_key);
      g_idle_add (emit_failed_internal, g_object_ref (self));
      return;
    }
  self->vapid = g_strdup (vapid_public_key);
  if (!g_application_get_is_registered (self->app))
    {
      self->waiting_for_bus = TRUE;     /* on_app_registered */
      return;
    }
  start_registration (self);
}

static void
on_unregister_reply (GObject *source, GAsyncResult *result, gpointer data)
{
  g_autoptr(VibeosUnifiedPush) self = data;
  g_autoptr(GError) error = NULL;
  g_autoptr(GVariant) reply = g_dbus_connection_call_finish (G_DBUS_CONNECTION (source), result, &error);
  g_autofree char *token = g_object_steal_data (G_OBJECT (self), "unregistering");

  if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    return;
  if (error)
    g_message ("%s: the push distributor: %s", app_id (self), error->message);
  /* Unregistered may have come first, and done this already */
  if (self->token && g_strcmp0 (self->token, token) == 0)
    handle_unregistered (self);
}

void
vibeos_unified_push_unregister (VibeosUnifiedPush *self)
{
  GDBusConnection *connection;
  GVariantBuilder options;

  g_return_if_fail (VIBEOS_IS_UNIFIED_PUSH (self));
  if (!self->app)
    return;
  load_state (self);
  if (!self->token)
    return;
  self->waiting_for_bus = FALSE;
  connection = g_application_get_is_registered (self->app) ? g_application_get_dbus_connection (self->app) : NULL;
  if (!connection || !self->distributor)
    {
      handle_unregistered (self);
      return;
    }
  g_variant_builder_init (&options, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_add (&options, "{sv}", "token", g_variant_new_string (self->token));
  g_object_set_data_full (G_OBJECT (self), "unregistering", g_strdup (self->token), g_free);
  g_dbus_connection_call (connection, self->distributor, DISTRIBUTOR_PATH, DISTRIBUTOR_IFACE, "Unregister",
                          g_variant_new ("(a{sv})", &options), NULL, G_DBUS_CALL_FLAGS_NONE, CALL_TIMEOUT_MS,
                          self->cancellable, on_unregister_reply, g_object_ref (self));
}

const char *
vibeos_unified_push_get_endpoint (VibeosUnifiedPush *self)
{
  g_return_val_if_fail (VIBEOS_IS_UNIFIED_PUSH (self), NULL);
  load_state (self);
  return self->endpoint;
}

/* --- the object */

static gboolean
is_instance_name (const char *name)
{
  if (!name || !*name || *name == '.' || strlen (name) > 64)
    return FALSE;
  for (const char *c = name; *c; c++)
    if (!g_ascii_isalnum (*c) && *c != '-' && *c != '_' && *c != '.')
      return FALSE;
  return TRUE;
}

VibeosUnifiedPush *
vibeos_unified_push_new (GApplication *app, const char *instance)
{
  g_return_val_if_fail (G_IS_APPLICATION (app), NULL);
  g_return_val_if_fail (g_application_get_application_id (app) != NULL, NULL);
  g_return_val_if_fail (instance == NULL || is_instance_name (instance), NULL);
  return g_object_new (VIBEOS_TYPE_UNIFIED_PUSH, "application", app, "instance", instance, NULL);
}

static void
vibeos_unified_push_constructed (GObject *object)
{
  VibeosUnifiedPush *self = VIBEOS_UNIFIED_PUSH (object);

  G_OBJECT_CLASS (vibeos_unified_push_parent_class)->constructed (object);
  if (!self->instance)
    self->instance = g_strdup ("default");
  if (!self->app)
    return;
  self->path = g_build_filename (g_get_user_data_dir (), app_id (self), "unifiedpush", self->instance, NULL);
  self->registered_handler = g_signal_connect (self->app, "notify::is-registered", G_CALLBACK (on_app_registered),
                                               self);
  attach (self);
}

static void
vibeos_unified_push_dispose (GObject *object)
{
  VibeosUnifiedPush *self = VIBEOS_UNIFIED_PUSH (object);

  g_cancellable_cancel (self->cancellable);
  release_hold (self);
  connector_detach (self);
  if (self->app)
    {
      g_clear_signal_handler (&self->registered_handler, self->app);
      g_object_remove_weak_pointer (G_OBJECT (self->app), (gpointer *) &self->app);
      self->app = NULL;
    }
  G_OBJECT_CLASS (vibeos_unified_push_parent_class)->dispose (object);
}

static void
vibeos_unified_push_finalize (GObject *object)
{
  VibeosUnifiedPush *self = VIBEOS_UNIFIED_PUSH (object);

  forget_state (self);
  g_clear_object (&self->cancellable);
  g_free (self->instance);
  g_free (self->path);
  g_free (self->description);
  g_free (self->vapid);
  G_OBJECT_CLASS (vibeos_unified_push_parent_class)->finalize (object);
}

static void
vibeos_unified_push_set_property (GObject *object, guint id, const GValue *value, GParamSpec *pspec)
{
  VibeosUnifiedPush *self = VIBEOS_UNIFIED_PUSH (object);

  switch (id)
    {
    case PROP_APPLICATION:
      self->app = g_value_get_object (value);
      if (self->app)
        g_object_add_weak_pointer (G_OBJECT (self->app), (gpointer *) &self->app);
      break;
    case PROP_INSTANCE:
      self->instance = g_value_dup_string (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, id, pspec);
    }
}

static void
vibeos_unified_push_get_property (GObject *object, guint id, GValue *value, GParamSpec *pspec)
{
  VibeosUnifiedPush *self = VIBEOS_UNIFIED_PUSH (object);

  switch (id)
    {
    case PROP_APPLICATION:
      g_value_set_object (value, self->app);
      break;
    case PROP_INSTANCE:
      g_value_set_string (value, self->instance);
      break;
    case PROP_ENDPOINT:
      g_value_set_string (value, vibeos_unified_push_get_endpoint (self));
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, id, pspec);
    }
}

static void
vibeos_unified_push_class_init (VibeosUnifiedPushClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->constructed = vibeos_unified_push_constructed;
  object_class->dispose = vibeos_unified_push_dispose;
  object_class->finalize = vibeos_unified_push_finalize;
  object_class->set_property = vibeos_unified_push_set_property;
  object_class->get_property = vibeos_unified_push_get_property;

  props[PROP_APPLICATION] = g_param_spec_object ("application", NULL, NULL, G_TYPE_APPLICATION,
                                                 G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS);
  props[PROP_INSTANCE] = g_param_spec_string ("instance", NULL, NULL, NULL,
                                              G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS);
  props[PROP_ENDPOINT] = g_param_spec_string ("endpoint", NULL, NULL, NULL,
                                              G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
  g_object_class_install_properties (object_class, N_PROPS, props);

  signals[NEW_ENDPOINT] = g_signal_new ("new-endpoint", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL,
                                        NULL, G_TYPE_NONE, 3, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING);
  signals[MESSAGE] = g_signal_new ("message", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                                   G_TYPE_NONE, 1, G_TYPE_BYTES);
  signals[UNREGISTERED] = g_signal_new ("unregistered", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL,
                                        NULL, G_TYPE_NONE, 0);
  signals[REGISTRATION_FAILED] = g_signal_new ("registration-failed", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
                                               0, NULL, NULL, NULL, G_TYPE_NONE, 1, G_TYPE_STRING);
}

static void
vibeos_unified_push_init (VibeosUnifiedPush *self)
{
  self->cancellable = g_cancellable_new ();
}
