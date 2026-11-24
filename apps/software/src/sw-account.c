/* SwAccount: see sw-account.h */
#include "sw-account.h"
#include "sw-device.h"
#include "sw-installer.h"
#include "sw-json.h"

#include <glib/gi18n.h>
#include <glib/gstdio.h>
#include <libsecret/secret.h>

#define EVENTS_MS 2000

static const SecretSchema token_schema = {
  "vibeos.Software.Token", SECRET_SCHEMA_NONE,
  { { "server", SECRET_SCHEMA_ATTRIBUTE_STRING }, { NULL, 0 } },
};

struct _SwAccount {
  GObject     parent_instance;
  SwApi      *api;
  JsonObject *user;
  gboolean    loaded;
  gboolean    synced;         /* the full list went to the store since signing in or starting */
  JsonArray  *events;         /* changes not sent yet */
  guint       events_timeout;
  gboolean    sending;
};

G_DEFINE_FINAL_TYPE (SwAccount, sw_account, G_TYPE_OBJECT)

enum { CHANGED, NOTICE, N_SIGNALS };
static guint signals[N_SIGNALS];

static void sync_device (SwAccount *self);

/* ---- where things are kept ------------------------------------------------------------------------------------- */

static char *
data_path (const char *name)
{
  return g_build_filename (g_get_user_data_dir (), "vibeos-software", name, NULL);
}

static const char *
token_file (void)
{
  const char *path = g_getenv ("VIBEOS_SOFTWARE_TOKEN_FILE");

  return path && *path ? path : NULL;
}

static void
write_private (const char *path, const char *contents)
{
  g_autofree char *dir = g_path_get_dirname (path);
  g_autoptr (GError) error = NULL;

  g_mkdir_with_parents (dir, 0700);
  if (!g_file_set_contents_full (path, contents, -1, G_FILE_SET_CONTENTS_CONSISTENT, 0600, &error))
    g_warning ("could not write %s: %s", path, error->message);
}

static void
save_user (SwAccount *self)
{
  g_autofree char *path = data_path ("account.json");
  g_autoptr (JsonBuilder) b = NULL;
  g_autoptr (JsonNode) root = NULL;
  g_autofree char *text = NULL;

  if (!self->user)
    {
      g_unlink (path);
      return;
    }
  b = json_builder_new ();
  json_builder_begin_object (b);
  json_builder_set_member_name (b, "server");
  json_builder_add_string_value (b, sw_api_get_base_url (self->api));
  json_builder_set_member_name (b, "user");
  json_builder_add_value (b, json_node_init_object (json_node_alloc (), self->user));
  json_builder_end_object (b);
  root = json_builder_get_root (b);
  text = json_to_string (root, TRUE);
  write_private (path, text);
}

static void
load_user (SwAccount *self)
{
  g_autofree char *path = data_path ("account.json");
  g_autoptr (JsonParser) parser = json_parser_new ();
  JsonObject *root;

  if (!json_parser_load_from_file (parser, path, NULL))
    return;
  root = sw_json_node_object (json_parser_get_root (parser));
  /* only the same store's account */
  if (g_strcmp0 (sw_json_string (root, "server"), sw_api_get_base_url (self->api)) || !sw_json_object (root, "user"))
    return;
  g_clear_pointer (&self->user, json_object_unref);
  self->user = json_object_ref (sw_json_object (root, "user"));
}

static void
on_token_stored (GObject *source, GAsyncResult *res, gpointer data)
{
  SwAccount *self = data;
  g_autoptr (GError) error = NULL;

  if (!secret_password_store_finish (res, &error))
    {
      g_warning ("could not keep the store's token in the keyring: %s", error->message);
      g_signal_emit (self, signals[NOTICE], 0,
                     _("Software could not keep your sign-in in the keyring. You will need to sign in again next "
                       "time."));
    }
  g_object_unref (self);
}

static void
store_token (SwAccount *self, const char *token)
{
  g_autofree char *label = NULL;

  if (token_file ())
    {
      write_private (token_file (), token);
      return;
    }
  /* TRANSLATORS: the keyring entry's name, as a password manager shows it; %s is the store's address */
  label = g_strdup_printf (_("VibeOS Software: the store account on %s"), sw_api_get_base_url (self->api));
  secret_password_store (&token_schema, SECRET_COLLECTION_DEFAULT, label, token, NULL, on_token_stored,
                         g_object_ref (self), "server", sw_api_get_base_url (self->api), NULL);
}

static void
on_token_cleared (GObject *source, GAsyncResult *res, gpointer data)
{
  g_autoptr (GError) error = NULL;

  if (!secret_password_clear_finish (res, &error) && error)
    g_debug ("could not clear the token: %s", error->message);
}

/* signed out, whatever the reason: nothing of the account stays */
static void
forget (SwAccount *self)
{
  gboolean was = sw_api_get_token (self->api) != NULL;

  if (token_file ())
    g_unlink (token_file ());
  else if (was)
    secret_password_clear (&token_schema, NULL, on_token_cleared, NULL, "server", sw_api_get_base_url (self->api),
                           NULL);
  sw_api_set_token (self->api, NULL);
  sw_api_clear_cache (self->api);
  g_clear_pointer (&self->user, json_object_unref);
  save_user (self);
  self->synced = FALSE;
  g_clear_handle_id (&self->events_timeout, g_source_remove);
  json_array_unref (self->events);
  self->events = json_array_new ();
  g_signal_emit (self, signals[CHANGED], 0);
}

static void
on_unauthorized (SwApi *api, SwAccount *self)
{
  forget (self);
  g_signal_emit (self, signals[NOTICE], 0, _("You were signed out of the store. Sign in again to review apps."));
}

/* ---- signing in ------------------------------------------------------------------------------------------------ */

void
sw_account_set_user (SwAccount *self, JsonObject *user)
{
  if (!user)
    return;
  g_clear_pointer (&self->user, json_object_unref);
  self->user = json_object_ref (user);
  save_user (self);
  g_signal_emit (self, signals[CHANGED], 0);
}

/* an answer with { token, user }: this device is signed in */
static void
signed_in (SwAccount *self, JsonObject *data)
{
  const char *token = sw_json_string (data, "token");

  if (!token)
    return;
  sw_api_clear_cache (self->api);
  sw_api_set_token (self->api, token);
  store_token (self, token);
  g_clear_pointer (&self->user, json_object_unref);
  if (sw_json_object (data, "user"))
    self->user = json_object_ref (sw_json_object (data, "user"));
  save_user (self);
  self->synced = FALSE;
  g_signal_emit (self, signals[CHANGED], 0);
  sync_device (self);
}

typedef enum {
  AFTER_NOTHING,
  AFTER_SIGN_IN,    /* the answer has a token and a user */
  AFTER_USER,       /* the answer is { user } or a User */
  AFTER_SIGN_OUT,   /* forget, whatever the answer */
  AFTER_DELETE,     /* forget on success */
} After;

typedef struct {
  SwAccount     *self;
  After          after;
  GCancellable  *cancellable;
  SwApiCallback  callback;
  gpointer       data;
} Call;

static void
on_call (SwApiResult *result, const SwApiError *error, gpointer data)
{
  Call *call = data;
  SwAccount *self = call->self;
  JsonObject *object = result ? sw_json_node_object (result->data) : NULL;

  if (result)
    switch (call->after)
      {
      case AFTER_SIGN_IN:
        signed_in (self, object);
        break;
      case AFTER_USER:
        sw_account_set_user (self, sw_json_object (object, "user") ? sw_json_object (object, "user") : object);
        break;
      case AFTER_DELETE:
        forget (self);
        break;
      default:
        break;
      }
  if (call->after == AFTER_SIGN_OUT && sw_api_get_token (self->api))
    forget (self);
  /* the page that asked may have gone meanwhile; what the answer meant for the account is done either way */
  if (call->callback && !(call->cancellable && g_cancellable_is_cancelled (call->cancellable)))
    call->callback (result, error, call->data);
  g_clear_object (&call->cancellable);
  g_object_unref (call->self);
  g_free (call);
}

static void
send (SwAccount *self, const char *method, const char *path, JsonNode *body, SwApiFlags flags, After after,
      GCancellable *cancellable, SwApiCallback callback, gpointer data)
{
  Call *call = g_new0 (Call, 1);

  call->self = g_object_ref (self);
  call->after = after;
  call->cancellable = cancellable ? g_object_ref (cancellable) : NULL;
  call->callback = callback;
  call->data = data;
  sw_api_send (self->api, method, path, body, flags, NULL, on_call, call);
}

/* a JSON object from name/value pairs: strings, and "device" for the device object */
static JsonNode *
body_of (const char *first, ...)
{
  g_autoptr (JsonBuilder) b = json_builder_new ();
  va_list args;

  json_builder_begin_object (b);
  va_start (args, first);
  for (const char *name = first; name; name = va_arg (args, const char *))
    {
      json_builder_set_member_name (b, name);
      if (!g_strcmp0 (name, "device"))
        json_builder_add_value (b, sw_device_to_json (TRUE));
      else
        json_builder_add_string_value (b, va_arg (args, const char *));
    }
  va_end (args);
  json_builder_end_object (b);
  return json_builder_get_root (b);
}

void
sw_account_sign_in (SwAccount *self, const char *email, const char *password, GCancellable *cancellable,
                    SwApiCallback callback, gpointer data)
{
  g_autoptr (JsonNode) body = body_of ("email", email, "password", password, "device", NULL);

  send (self, "POST", "/auth/login", body, SW_API_NO_TOKEN, AFTER_SIGN_IN, cancellable, callback, data);
}

void
sw_account_register (SwAccount *self, const char *name, const char *email, const char *password,
                     GCancellable *cancellable, SwApiCallback callback, gpointer data)
{
  g_autoptr (JsonNode) body = body_of ("name", name, "email", email, "password", password, "device", NULL);

  send (self, "POST", "/auth/register", body, SW_API_NO_TOKEN, AFTER_SIGN_IN, cancellable, callback, data);
}

void
sw_account_verify (SwAccount *self, const char *code, GCancellable *cancellable, SwApiCallback callback,
                   gpointer data)
{
  g_autoptr (JsonNode) body = body_of ("code", code, NULL);

  send (self, "POST", "/auth/email/verify", body, SW_API_NONE, AFTER_USER, cancellable, callback, data);
}

void
sw_account_resend (SwAccount *self, GCancellable *cancellable, SwApiCallback callback, gpointer data)
{
  send (self, "POST", "/auth/email/resend", NULL, SW_API_NONE, AFTER_NOTHING, cancellable, callback, data);
}

void
sw_account_forgot (SwAccount *self, const char *email, GCancellable *cancellable, SwApiCallback callback,
                   gpointer data)
{
  g_autoptr (JsonNode) body = body_of ("email", email, NULL);

  send (self, "POST", "/auth/password/forgot", body, SW_API_NO_TOKEN, AFTER_NOTHING, cancellable, callback, data);
}

void
sw_account_check_code (SwAccount *self, const char *email, const char *code, GCancellable *cancellable,
                       SwApiCallback callback, gpointer data)
{
  g_autoptr (JsonNode) body = body_of ("email", email, "code", code, NULL);

  send (self, "POST", "/auth/password/check-code", body, SW_API_NO_TOKEN, AFTER_NOTHING, cancellable, callback,
        data);
}

void
sw_account_reset (SwAccount *self, const char *email, const char *code, const char *password,
                  GCancellable *cancellable, SwApiCallback callback, gpointer data)
{
  g_autoptr (JsonNode) body = body_of ("email", email, "code", code, "password", password, "device", NULL);

  send (self, "POST", "/auth/password/reset", body, SW_API_NO_TOKEN, AFTER_SIGN_IN, cancellable, callback, data);
}

void
sw_account_sign_out (SwAccount *self, GCancellable *cancellable, SwApiCallback callback, gpointer data)
{
  send (self, "POST", "/auth/logout", NULL, SW_API_NONE, AFTER_SIGN_OUT, cancellable, callback, data);
}

void
sw_account_delete (SwAccount *self, const char *password, gboolean delete_reviews, GCancellable *cancellable,
                   SwApiCallback callback, gpointer data)
{
  g_autoptr (JsonBuilder) b = json_builder_new ();
  g_autoptr (JsonNode) body = NULL;

  json_builder_begin_object (b);
  json_builder_set_member_name (b, "password");
  json_builder_add_string_value (b, password);
  json_builder_set_member_name (b, "delete_reviews");
  json_builder_add_boolean_value (b, delete_reviews);
  json_builder_end_object (b);
  body = json_builder_get_root (b);
  send (self, "DELETE", "/me", body, SW_API_NONE, AFTER_DELETE, cancellable, callback, data);
}

void
sw_account_refresh (SwAccount *self)
{
  if (sw_api_get_token (self->api))
    send (self, "GET", "/me", NULL, SW_API_NONE, AFTER_USER, NULL, NULL, NULL);
}

/* ---- what is installed, for the store -------------------------------------------------------------------------- */

static void
on_synced (SwApiResult *result, const SwApiError *error, gpointer data)
{
  SwAccount *self = data;

  if (result)
    self->synced = TRUE;
  else
    g_debug ("the installed list did not reach the store: %s", error->message);
  g_object_unref (self);
}

/* the whole installed list: PUT /me/device/apps */
static void
sync_apps (SwAccount *self)
{
  SwInstaller *installer = sw_installer_get_default ();
  GHashTableIter iter;
  gpointer package, version;
  g_autoptr (JsonBuilder) b = NULL;
  g_autoptr (JsonNode) body = NULL;

  if (!sw_api_get_token (self->api) || !sw_installer_is_loaded (installer) || self->synced)
    return;
  b = json_builder_new ();
  json_builder_begin_object (b);
  json_builder_set_member_name (b, "packages");
  json_builder_begin_array (b);
  g_hash_table_iter_init (&iter, sw_installer_get_installed_all (installer));
  for (guint n = 0; g_hash_table_iter_next (&iter, &package, &version) && n < 5000; n++)
    {
      json_builder_begin_object (b);
      json_builder_set_member_name (b, "package");
      json_builder_add_string_value (b, package);
      json_builder_set_member_name (b, "version");
      json_builder_add_string_value (b, version);
      json_builder_end_object (b);
    }
  json_builder_end_array (b);
  json_builder_end_object (b);
  body = json_builder_get_root (b);
  /* a change while this is out would be in the list: the queued ones are older */
  json_array_unref (self->events);
  self->events = json_array_new ();
  sw_api_send (self->api, "PUT", "/me/device/apps", body, SW_API_NONE, NULL, on_synced, g_object_ref (self));
}

static void
on_device_put (SwApiResult *result, const SwApiError *error, gpointer data)
{
  if (error)
    g_debug ("this device's details did not reach the store: %s", error->message);
}

/* this device's model and release (a VibeOS upgrade changes them), then the installed list */
static void
sync_device (SwAccount *self)
{
  g_autoptr (JsonNode) body = NULL;

  if (!sw_api_get_token (self->api))
    return;
  body = sw_device_to_json (FALSE);
  sw_api_send (self->api, "PUT", "/me/device", body, SW_API_NONE, NULL, on_device_put, NULL);
  sync_apps (self);
}

static void
on_events_sent (SwApiResult *result, const SwApiError *error, gpointer data)
{
  SwAccount *self = data;

  self->sending = FALSE;
  if (error)
    {
      /* offline: the next start's full list says it all */
      g_debug ("installed-app events did not reach the store: %s", error->message);
      self->synced = FALSE;
    }
  g_object_unref (self);
}

static gboolean
on_events_timeout (gpointer data)
{
  SwAccount *self = data;
  g_autoptr (JsonBuilder) b = NULL;
  g_autoptr (JsonNode) body = NULL;
  guint n = json_array_get_length (self->events);

  self->events_timeout = 0;
  if (!n || !sw_api_get_token (self->api))
    return G_SOURCE_REMOVE;
  if (!self->synced)
    {
      sync_apps (self);
      return G_SOURCE_REMOVE;
    }
  b = json_builder_new ();
  json_builder_begin_object (b);
  json_builder_set_member_name (b, "events");
  json_builder_begin_array (b);
  /* at most 100 a request; more than that and the full list is cheaper */
  if (n > 100)
    {
      self->synced = FALSE;
      sync_apps (self);
      return G_SOURCE_REMOVE;
    }
  for (guint i = 0; i < n; i++)
    json_builder_add_value (b, json_node_copy (json_array_get_element (self->events, i)));
  json_builder_end_array (b);
  json_builder_end_object (b);
  body = json_builder_get_root (b);
  json_array_unref (self->events);
  self->events = json_array_new ();
  self->sending = TRUE;
  sw_api_send (self->api, "POST", "/me/device/apps/events", body, SW_API_NONE, NULL, on_events_sent,
               g_object_ref (self));
  return G_SOURCE_REMOVE;
}

static void
on_package_changed (SwInstaller *installer, const char *package, const char *old, const char *now, SwAccount *self)
{
  JsonObject *event;
  g_autoptr (GDateTime) time = NULL;
  g_autofree char *at = NULL;

  if (!sw_api_get_token (self->api))
    return;
  time = g_date_time_new_now_utc ();
  at = g_date_time_format (time, "%Y-%m-%dT%H:%M:%SZ");
  event = json_object_new ();
  json_object_set_string_member (event, "type", !now ? "uninstalled" : old ? "updated" : "installed");
  json_object_set_string_member (event, "package", package);
  if (now)
    json_object_set_string_member (event, "version", now);
  json_object_set_string_member (event, "at", at);
  json_array_add_object_element (self->events, event);
  /* dpkg changes several packages in a row: one request for them */
  g_clear_handle_id (&self->events_timeout, g_source_remove);
  self->events_timeout = g_timeout_add (EVENTS_MS, on_events_timeout, self);
}

static void
on_installed_read (SwInstaller *installer, const char *package, SwAccount *self)
{
  /* the list is known: the full sync that waited for it */
  if (!package && !self->synced && self->loaded)
    sync_apps (self);
}

static void
on_ping (SwApiResult *result, const SwApiError *error, gpointer data)
{
  if (error)
    g_debug ("the install ping did not reach the store: %s", error->message);
}

void
sw_account_note_installed (SwAccount *self, const char *package)
{
  g_autoptr (JsonBuilder) b = NULL;
  g_autoptr (JsonNode) body = NULL;

  /* signed in, the device's own report counts it */
  if (sw_api_get_token (self->api) || !package)
    return;
  b = json_builder_new ();
  json_builder_begin_object (b);
  json_builder_set_member_name (b, "device_id");
  json_builder_add_string_value (b, sw_device_id ());
  json_builder_set_member_name (b, "package");
  json_builder_add_string_value (b, package);
  json_builder_end_object (b);
  body = json_builder_get_root (b);
  sw_api_send (self->api, "POST", "/installs/anonymous", body, SW_API_NO_TOKEN, NULL, on_ping, NULL);
}

/* ---- loading --------------------------------------------------------------------------------------------------- */

static void
loaded_token (SwAccount *self, const char *token)
{
  self->loaded = TRUE;
  if (token && *token)
    {
      sw_api_set_token (self->api, token);
      load_user (self);
    }
  g_signal_emit (self, signals[CHANGED], 0);
  if (token && *token)
    {
      sw_account_refresh (self);
      sync_device (self);
    }
}

static void
on_token_found (GObject *source, GAsyncResult *res, gpointer data)
{
  SwAccount *self = data;
  g_autoptr (GError) error = NULL;
  char *token = secret_password_lookup_finish (res, &error);

  if (error)
    g_message ("no keyring to read the store's token from: %s", error->message);
  loaded_token (self, token);
  secret_password_free (token);
  g_object_unref (self);
}

void
sw_account_load (SwAccount *self)
{
  if (token_file ())
    {
      g_autofree char *token = NULL;

      if (g_file_get_contents (token_file (), &token, NULL, NULL))
        g_strstrip (token);
      loaded_token (self, token);
      return;
    }
  secret_password_lookup (&token_schema, NULL, on_token_found, g_object_ref (self), "server",
                          sw_api_get_base_url (self->api), NULL);
}

gboolean
sw_account_is_loaded (SwAccount *self)
{
  return self->loaded;
}

gboolean
sw_account_is_signed_in (SwAccount *self)
{
  return sw_api_get_token (self->api) != NULL;
}

JsonObject *
sw_account_get_user (SwAccount *self)
{
  return sw_api_get_token (self->api) ? self->user : NULL;
}

const char *
sw_account_get_name (SwAccount *self)
{
  return sw_json_string (sw_account_get_user (self), "name");
}

const char *
sw_account_get_email (SwAccount *self)
{
  return sw_json_string (sw_account_get_user (self), "email");
}

gboolean
sw_account_is_verified (SwAccount *self)
{
  return sw_json_bool (sw_account_get_user (self), "email_verified", FALSE);
}

static void
sw_account_class_init (SwAccountClass *klass)
{
  signals[CHANGED] = g_signal_new ("changed", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                                   G_TYPE_NONE, 0);
  signals[NOTICE] = g_signal_new ("notice", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                                  G_TYPE_NONE, 1, G_TYPE_STRING);
}

static void
sw_account_init (SwAccount *self)
{
  self->api = sw_api_get_default ();
  self->events = json_array_new ();
  g_signal_connect (self->api, "unauthorized", G_CALLBACK (on_unauthorized), self);
  g_signal_connect (sw_installer_get_default (), "package-changed", G_CALLBACK (on_package_changed), self);
  g_signal_connect (sw_installer_get_default (), "changed", G_CALLBACK (on_installed_read), self);
}

SwAccount *
sw_account_get_default (void)
{
  static SwAccount *account;

  if (!account)
    account = g_object_new (SW_TYPE_ACCOUNT, NULL);
  return account;
}
