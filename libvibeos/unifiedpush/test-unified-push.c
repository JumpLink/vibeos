/* VibeosUnifiedPush against a distributor of the test's own, on a private bus (GTestDBus), with the test's own
 * home (G_TEST_OPTION_ISOLATE_DIRS): registering, the endpoint and the keys, messages (decrypted, routed by token,
 * the ones to ignore), registering again, a new endpoint, two instances, unregistering both ways, failures. */
#include <string.h>
#include <sys/stat.h>

#include <gio/gio.h>
#include <glib/gstdio.h>

#include "vibeos-unified-push.h"
#include "vibeos-webpush.h"

#define APP_ID      "test.vibeos.PushApp"
#define DISTRIBUTOR "org.unifiedpush.Distributor.vibeos"

static const char distributor_xml[] =
  "<node><interface name='org.unifiedpush.Distributor2'>"
  "<method name='Register'><arg type='a{sv}' direction='in'/><arg type='a{sv}' direction='out'/></method>"
  "<method name='Unregister'><arg type='a{sv}' direction='in'/><arg type='a{sv}' direction='out'/></method>"
  "</interface></node>";

/* the test's distributor */
typedef struct {
  GDBusConnection *connection;
  guint object_id, name_id;
  char *fail_reason;        /* Register fails with this, when set */
  char *endpoint_host;      /* what the endpoints start with */
  guint registers, unregisters;
  char *last_token, *last_description, *last_vapid;
  GHashTable *endpoints;    /* token -> endpoint */
} Distributor;

static Distributor distributor;

/* what the app's objects emitted */
typedef struct {
  guint new_endpoints, messages, unregistered, failed;
  char *endpoint, *p256dh, *auth, *reason;
  GBytes *message;
} Seen;

static void
seen_clear (Seen *seen)
{
  g_free (seen->endpoint);
  g_free (seen->p256dh);
  g_free (seen->auth);
  g_free (seen->reason);
  g_clear_pointer (&seen->message, g_bytes_unref);
  memset (seen, 0, sizeof *seen);
}

static void
call_connector (const char *method, GVariant *options)
{
  g_dbus_connection_call (distributor.connection, APP_ID, "/org/unifiedpush/Connector",
                          "org.unifiedpush.Connector2", method, g_variant_new ("(@a{sv})", options), NULL,
                          G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);
}

static void
on_distributor_call (GDBusConnection *connection, const char *sender, const char *path, const char *interface,
                     const char *method, GVariant *parameters, GDBusMethodInvocation *invocation, gpointer data)
{
  g_autoptr(GVariant) options = NULL;
  const char *token = NULL, *service = NULL;
  GVariantBuilder result;

  g_variant_get (parameters, "(@a{sv})", &options);
  g_variant_lookup (options, "token", "&s", &token);
  g_variant_lookup (options, "service", "&s", &service);
  g_variant_builder_init (&result, G_VARIANT_TYPE_VARDICT);
  if (g_str_equal (method, "Register"))
    {
      const char *description = NULL, *vapid = NULL;

      distributor.registers++;
      g_variant_lookup (options, "description", "&s", &description);
      g_variant_lookup (options, "vapid", "&s", &vapid);
      g_free (distributor.last_token);
      distributor.last_token = g_strdup (token);
      g_free (distributor.last_description);
      distributor.last_description = g_strdup (description);
      g_free (distributor.last_vapid);
      distributor.last_vapid = g_strdup (vapid);
      g_assert_cmpstr (service, ==, APP_ID);
      if (distributor.fail_reason)
        {
          g_variant_builder_add (&result, "{sv}", "success", g_variant_new_string ("REGISTRATION_FAILED"));
          g_variant_builder_add (&result, "{sv}", "reason", g_variant_new_string (distributor.fail_reason));
          g_dbus_method_invocation_return_value (invocation, g_variant_new ("(a{sv})", &result));
          return;
        }
      g_variant_builder_add (&result, "{sv}", "success", g_variant_new_string ("REGISTRATION_SUCCEEDED"));
      g_dbus_method_invocation_return_value (invocation, g_variant_new ("(a{sv})", &result));
      /* the endpoint after the reply, as the spec has it; the same one for the same token */
      if (!g_hash_table_contains (distributor.endpoints, token))
        g_hash_table_insert (distributor.endpoints, g_strdup (token),
                             g_strdup_printf ("%s/up%.12s?up=1", distributor.endpoint_host, token));
      call_connector ("NewEndpoint",
                      g_variant_new_parsed ("{'token': <%s>, 'endpoint': <%s>}", token,
                                            g_hash_table_lookup (distributor.endpoints, token)));
    }
  else
    {
      distributor.unregisters++;
      g_dbus_method_invocation_return_value (invocation, g_variant_new ("(a{sv})", &result));
      call_connector ("Unregistered", g_variant_new_parsed ("{'token': <%s>}", token));
    }
}

static void
distributor_start (void)
{
  static const GDBusInterfaceVTable vtable = { on_distributor_call, NULL, NULL };
  g_autoptr(GDBusNodeInfo) node = g_dbus_node_info_new_for_xml (distributor_xml, NULL);
  g_autofree char *address = g_dbus_address_get_for_bus_sync (G_BUS_TYPE_SESSION, NULL, NULL);
  g_autoptr(GError) error = NULL;

  distributor.connection = g_dbus_connection_new_for_address_sync (address,
    G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT | G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION, NULL, NULL,
    &error);
  g_assert_no_error (error);
  distributor.object_id = g_dbus_connection_register_object (distributor.connection, "/org/unifiedpush/Distributor",
                                                             node->interfaces[0], &vtable, NULL, NULL, &error);
  g_assert_no_error (error);
  distributor.endpoints = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
  distributor.endpoint_host = g_strdup ("https://push.example.net");
  distributor.name_id = g_bus_own_name_on_connection (distributor.connection, DISTRIBUTOR,
                                                      G_BUS_NAME_OWNER_FLAGS_NONE, NULL, NULL, NULL, NULL);
}

static void
iterate_until (gboolean *flag)
{
  gint64 end = g_get_monotonic_time () + 10 * G_USEC_PER_SEC;

  while (!*flag && g_get_monotonic_time () < end)
    g_main_context_iteration (NULL, TRUE);
  g_assert_true (*flag);
}

/* runs the main loop for a moment: what was to come has come */
static void
settle (void)
{
  gint64 end = g_get_monotonic_time () + 300 * 1000;

  while (g_get_monotonic_time () < end)
    {
      while (g_main_context_iteration (NULL, FALSE))
        ;
      g_usleep (2000);
    }
}

static gboolean seen_flag;

static void
on_new_endpoint (VibeosUnifiedPush *push, const char *endpoint, const char *p256dh, const char *auth, Seen *seen)
{
  seen->new_endpoints++;
  g_free (seen->endpoint);
  seen->endpoint = g_strdup (endpoint);
  g_free (seen->p256dh);
  seen->p256dh = g_strdup (p256dh);
  g_free (seen->auth);
  seen->auth = g_strdup (auth);
  seen_flag = TRUE;
}

static void
on_message (VibeosUnifiedPush *push, GBytes *message, Seen *seen)
{
  seen->messages++;
  g_clear_pointer (&seen->message, g_bytes_unref);
  seen->message = g_bytes_ref (message);
  seen_flag = TRUE;
}

static void
on_unregistered (VibeosUnifiedPush *push, Seen *seen)
{
  seen->unregistered++;
  seen_flag = TRUE;
}

static void
on_failed (VibeosUnifiedPush *push, const char *reason, Seen *seen)
{
  seen->failed++;
  g_free (seen->reason);
  seen->reason = g_strdup (reason);
  seen_flag = TRUE;
}

static VibeosUnifiedPush *
make_push (GApplication *app, const char *instance, Seen *seen)
{
  VibeosUnifiedPush *push = vibeos_unified_push_new (app, instance);

  g_signal_connect (push, "new-endpoint", G_CALLBACK (on_new_endpoint), seen);
  g_signal_connect (push, "message", G_CALLBACK (on_message), seen);
  g_signal_connect (push, "unregistered", G_CALLBACK (on_unregistered), seen);
  g_signal_connect (push, "registration-failed", G_CALLBACK (on_failed), seen);
  return push;
}

static void
on_reply (GObject *source, GAsyncResult *result, gpointer data)
{
  GVariant **reply = data;
  g_autoptr(GError) error = NULL;

  *reply = g_dbus_connection_call_finish (G_DBUS_CONNECTION (source), result, &error);
  g_assert_no_error (error);   /* answered, whatever the message was */
}

/* a Message call as the distributor makes it; the app is in this process, so the main loop runs while it waits */
static GVariant *
call_message (GVariant *options)
{
  GVariant *reply = NULL;
  gint64 end = g_get_monotonic_time () + 10 * G_USEC_PER_SEC;
  GVariant *options_out = NULL;

  g_dbus_connection_call (distributor.connection, APP_ID, "/org/unifiedpush/Connector", "org.unifiedpush.Connector2",
                          "Message", g_variant_new ("(@a{sv})", options), G_VARIANT_TYPE ("(a{sv})"),
                          G_DBUS_CALL_FLAGS_NONE, -1, NULL, on_reply, &reply);
  while (!reply && g_get_monotonic_time () < end)
    g_main_context_iteration (NULL, TRUE);
  g_assert_nonnull (reply);
  g_variant_get (reply, "(@a{sv})", &options_out);
  g_variant_unref (reply);
  return options_out;
}

/* a message for the keys of @seen, sent as the distributor does; the reply carries the id back */
static void
send_message (const char *token, const Seen *seen, const char *text, const char *id)
{
  gsize length = 0;
  g_autofree guint8 *p256dh = _vibeos_base64url_decode (seen->p256dh, &length);
  g_autofree guint8 *auth = _vibeos_base64url_decode (seen->auth, &length);
  g_autoptr(GError) error = NULL;
  g_autoptr(GBytes) body = _vibeos_webpush_encrypt (p256dh, auth, (const guint8 *) text, strlen (text), NULL, NULL,
                                                    0, &error);
  g_autoptr(GVariant) reply = NULL;
  const char *echoed = NULL;

  g_assert_no_error (error);
  reply = call_message (g_variant_new_parsed ("{'token': <%s>, 'message': <%@ay>, 'id': <%s>}", token,
                                              g_variant_new_from_bytes (G_VARIANT_TYPE_BYTESTRING, body, TRUE), id));
  g_variant_lookup (reply, "id", "&s", &echoed);
  g_assert_cmpstr (echoed, ==, id);
}

static void
send_raw (const char *token, const guint8 *data, gsize length)
{
  g_autoptr(GVariant) reply = call_message (g_variant_new_parsed ("{'token': <%s>, 'message': <%@ay>}", token,
    g_variant_new_fixed_array (G_VARIANT_TYPE_BYTE, data, length, 1)));
}

static char *
state_path (const char *instance)
{
  return g_build_filename (g_get_user_data_dir (), APP_ID, "unifiedpush", instance, NULL);
}

static void
test_connector (void)
{
  g_autoptr(GApplication) app = g_application_new (APP_ID, G_APPLICATION_DEFAULT_FLAGS);
  g_autoptr(VibeosUnifiedPush) push = NULL, second = NULL;
  g_autoptr(GError) error = NULL;
  g_autofree char *path = state_path ("default"), *dir = g_path_get_dirname (path);
  g_autofree char *token = NULL, *first_endpoint = NULL;
  Seen seen = { 0 }, seen2 = { 0 };
  GStatBuf st;
  gsize length;
  g_autofree guint8 *p256dh = NULL, *auth = NULL;
  guint8 garbage[300];

  /* made before the app is on the bus: it registers once the app is */
  push = make_push (app, NULL, &seen);
  g_assert_null (vibeos_unified_push_get_endpoint (push));
  vibeos_unified_push_register (push, "Test notes", NULL);
  g_assert_cmpuint (distributor.registers, ==, 0);
  g_application_register (app, NULL, &error);
  g_assert_no_error (error);
  seen_flag = FALSE;
  iterate_until (&seen_flag);
  g_assert_cmpuint (distributor.registers, ==, 1);
  g_assert_cmpuint (seen.new_endpoints, ==, 1);
  g_assert_cmpstr (distributor.last_description, ==, "Test notes");
  g_assert_null (distributor.last_vapid);
  g_assert_cmpstr (vibeos_unified_push_get_endpoint (push), ==, seen.endpoint);
  g_assert_true (g_str_has_prefix (seen.endpoint, "https://push.example.net/up"));
  /* the keys, as a browser gives them: a P-256 point, a 16-byte secret */
  p256dh = _vibeos_base64url_decode (seen.p256dh, &length);
  g_assert_cmpuint (length, ==, 65);
  g_assert_cmpuint (p256dh[0], ==, 0x04);
  auth = _vibeos_base64url_decode (seen.auth, &length);
  g_assert_cmpuint (length, ==, 16);
  /* the token: 16 random bytes */
  g_assert_cmpuint (strlen (distributor.last_token), ==, 22);
  token = g_strdup (distributor.last_token);
  /* the state, readable by the user alone */
  g_assert_cmpint (g_stat (path, &st), ==, 0);
  g_assert_cmpint (st.st_mode & 0777, ==, 0600);
  g_assert_cmpint (g_stat (dir, &st), ==, 0);
  g_assert_cmpint (st.st_mode & 0777, ==, 0700);

  /* a message: decrypted, the id echoed */
  seen_flag = FALSE;
  send_message (token, &seen, "Hello from the server", "msg-1");
  iterate_until (&seen_flag);
  g_assert_cmpuint (seen.messages, ==, 1);
  g_assert_cmpmem (g_bytes_get_data (seen.message, NULL), g_bytes_get_size (seen.message), "Hello from the server",
                   21);

  /* ignored: another token, nothing, more than 4096 bytes, and one that does not decrypt (a warning) */
  send_message ("not-our-token-at-all", &seen, "for someone else", "msg-2");
  send_raw (token, garbage, 0);
  {
    g_autofree guint8 *big = g_malloc0 (4097);
    send_raw (token, big, 4097);
  }
  for (guint i = 0; i < sizeof garbage; i++)
    garbage[i] = i;
  g_test_expect_message ("VibeosUnifiedPush", G_LOG_LEVEL_WARNING, "*cannot be decrypted*");
  send_raw (token, garbage, sizeof garbage);
  settle ();
  g_test_assert_expected_messages ();
  g_assert_cmpuint (seen.messages, ==, 1);

  /* registering again (the next start): the same token, the same endpoint, so no signal */
  vibeos_unified_push_register (push, NULL, "BP4z9KsN6nGRTbVYI_c7VJSPQTBtkgcy27mlmlMoZIIgDll6e3vCYLocInmYWAmS6TlzAC8wEqKK6PBru3jl7A8");
  settle ();
  g_assert_cmpuint (distributor.registers, ==, 2);
  g_assert_cmpstr (distributor.last_token, ==, token);
  g_assert_cmpstr (distributor.last_description, ==, APP_ID);   /* no desktop file: the app id */
  g_assert_cmpstr (distributor.last_vapid, ==, "BP4z9KsN6nGRTbVYI_c7VJSPQTBtkgcy27mlmlMoZIIgDll6e3vCYLocInmYWAmS6TlzAC8wEqKK6PBru3jl7A8");
  g_assert_cmpuint (seen.new_endpoints, ==, 1);

  /* a new endpoint (another push server): the signal, with the same keys */
  first_endpoint = g_strdup (seen.endpoint);
  g_free (distributor.endpoint_host);
  distributor.endpoint_host = g_strdup ("https://other.example.org");
  g_hash_table_remove_all (distributor.endpoints);
  seen_flag = FALSE;
  vibeos_unified_push_register (push, "Test notes", NULL);
  iterate_until (&seen_flag);
  g_assert_cmpuint (seen.new_endpoints, ==, 2);
  g_assert_cmpstr (seen.endpoint, !=, first_endpoint);
  g_assert_true (g_str_has_prefix (seen.endpoint, "https://other.example.org/up"));

  /* a second instance: its own token and keys; messages go to the one whose token they carry */
  second = make_push (app, "account-2", &seen2);
  seen_flag = FALSE;
  vibeos_unified_push_register (second, "Second account", NULL);
  iterate_until (&seen_flag);
  g_assert_cmpuint (seen2.new_endpoints, ==, 1);
  g_assert_cmpstr (distributor.last_token, !=, token);
  g_assert_cmpstr (seen2.p256dh, !=, seen.p256dh);
  seen_flag = FALSE;
  send_message (distributor.last_token, &seen2, "to the second", "msg-3");
  iterate_until (&seen_flag);
  g_assert_cmpuint (seen2.messages, ==, 1);
  g_assert_cmpuint (seen.messages, ==, 1);
  /* the first one's keys cannot read the second's messages */
  g_test_expect_message ("VibeosUnifiedPush", G_LOG_LEVEL_WARNING, "*cannot be decrypted*");
  send_message (distributor.last_token, &seen, "wrong keys", "msg-4");
  settle ();
  g_test_assert_expected_messages ();
  g_assert_cmpuint (seen2.messages, ==, 1);

  /* unregistering: the distributor confirms, the state goes */
  seen_flag = FALSE;
  vibeos_unified_push_unregister (push);
  iterate_until (&seen_flag);
  settle ();
  g_assert_cmpuint (distributor.unregisters, ==, 1);
  g_assert_cmpuint (seen.unregistered, ==, 1);
  g_assert_null (vibeos_unified_push_get_endpoint (push));
  g_assert_cmpint (g_stat (path, &st), ==, -1);
  /* messages for the old token are ignored now */
  send_message (token, &seen, "too late", "msg-5");
  settle ();
  g_assert_cmpuint (seen.messages, ==, 1);

  /* removed in Settings: the distributor says Unregistered by itself */
  seen_flag = FALSE;
  call_connector ("Unregistered", g_variant_new_parsed ("{'token': <%s>}", distributor.last_token));
  iterate_until (&seen_flag);
  g_assert_cmpuint (seen2.unregistered, ==, 1);
  g_assert_null (vibeos_unified_push_get_endpoint (second));

  /* registering after that: a new token and new keys */
  seen_flag = FALSE;
  vibeos_unified_push_register (push, NULL, NULL);
  iterate_until (&seen_flag);
  g_assert_cmpstr (distributor.last_token, !=, token);
  g_assert_cmpuint (seen.new_endpoints, ==, 3);

  /* failures: as the distributor gives them, a key that is not one, and no distributor at all */
  distributor.fail_reason = g_strdup ("VAPID_REQUIRED");
  seen_flag = FALSE;
  vibeos_unified_push_register (push, NULL, NULL);
  iterate_until (&seen_flag);
  g_assert_cmpstr (seen.reason, ==, "VAPID_REQUIRED");
  g_clear_pointer (&distributor.fail_reason, g_free);

  seen_flag = FALSE;
  g_test_expect_message ("VibeosUnifiedPush", G_LOG_LEVEL_WARNING, "*is not a VAPID public key*");
  vibeos_unified_push_register (push, NULL, "not-a-key");
  iterate_until (&seen_flag);
  g_test_assert_expected_messages ();
  g_assert_cmpstr (seen.reason, ==, "INTERNAL_ERROR");

  g_bus_unown_name (distributor.name_id);
  settle ();
  seen_flag = FALSE;
  vibeos_unified_push_register (push, NULL, NULL);
  iterate_until (&seen_flag);
  g_assert_cmpstr (seen.reason, ==, "ACTION_REQUIRED");

  seen_clear (&seen);
  seen_clear (&seen2);
}

int
main (int argc, char **argv)
{
  g_autoptr(GTestDBus) bus = NULL;
  int result;

  g_test_init (&argc, &argv, G_TEST_OPTION_ISOLATE_DIRS, NULL);
  bus = g_test_dbus_new (G_TEST_DBUS_NONE);
  g_test_dbus_up (bus);
  distributor_start ();
  g_test_add_func ("/unified-push/connector", test_connector);
  result = g_test_run ();
  g_dbus_connection_close_sync (distributor.connection, NULL, NULL);
  g_test_dbus_down (bus);
  return result;
}
