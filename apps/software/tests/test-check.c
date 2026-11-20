/* Looking for updates without the window (sw-check.h): the update setting's verdict, the request, the answer's
 * entries, what was suggested and its file, the notification's words, and whole runs of `vibeos-software
 * --check-updates` against a local SoupServer that answers like the store and a notification server of our own on a
 * private session bus: the first run suggests, the same versions again do not, a newer version does, "never" asks
 * nothing, and an empty answer takes the notification away. */
#include "sw-check.h"
#include "sw-json.h"

#include <glib/gstdio.h>
#include <libsoup/soup.h>
#include <stdarg.h>
#include <string.h>

static char *base_url;
static int check_hits;           /* requests that reached /updates/check */
static char *check_body;         /* the last one's body */
static char *check_answer;       /* what /updates/check answers */

static void
handler (SoupServer *srv, SoupServerMessage *msg, const char *path, GHashTable *query, gpointer data)
{
  if (!strcmp (path, "/api/v1/updates/check") && !strcmp (soup_server_message_get_method (msg), "POST"))
    {
      g_autoptr (GBytes) bytes = soup_message_body_flatten (soup_server_message_get_request_body (msg));

      check_hits++;
      g_free (check_body);
      check_body = g_strndup (g_bytes_get_data (bytes, NULL), g_bytes_get_size (bytes));
      soup_server_message_set_status (msg, 200, NULL);
      soup_server_message_set_response (msg, "application/json", SOUP_MEMORY_COPY, check_answer,
                                        strlen (check_answer));
      return;
    }
  soup_server_message_set_status (msg, 404, NULL);
}

/* an /updates/check entry as the store writes it */
static char *
entry_json (const char *package, const char *name, const char *installed, const char *version, const char *release)
{
  g_autofree char *rel = release ? g_strdup_printf ("{\"version\":\"%s\",\"notes\":[]}", release) : g_strdup ("null");

  return g_strdup_printf ("{\"app\":{\"id\":\"vibeos.%s\",\"package\":\"%s\",\"name\":\"%s\",\"summary\":\"\","
                          "\"icon\":null},\"installed_version\":\"%s\",\"version\":\"%s\",\"download_size\":1000,"
                          "\"installed_size\":4000,\"release\":%s}", name, package, name, installed, version, rel);
}

static JsonNode *
parse (const char *text)
{
  g_autoptr (JsonParser) parser = json_parser_new ();

  g_assert_true (json_parser_load_from_data (parser, text, -1, NULL));
  return json_node_ref (json_parser_get_root (parser));
}

/* the entries of a {"data":[...]} text */
static GPtrArray *
entries_of (const char *answer)
{
  g_autoptr (JsonNode) root = parse (answer);

  return sw_check_entries (json_object_get_member (json_node_get_object (root), "data"));
}

/* the entries of an answer made of these entry_json texts (NULL after the last) */
static GPtrArray *
entries_list (const char *first, ...)
{
  g_autoptr (GString) answer = g_string_new ("{\"data\":[");
  va_list args;

  va_start (args, first);
  for (const char *entry = first; entry; entry = va_arg (args, const char *))
    g_string_append_printf (answer, "%s%s", entry == first ? "" : ",", entry);
  va_end (args);
  g_string_append (answer, "]}");
  return entries_of (answer->str);
}

static const char *
package_at (GPtrArray *entries, guint i)
{
  return sw_json_string (sw_json_object (g_ptr_array_index (entries, i), "app"), "package");
}

/* ---- without a server ------------------------------------------------------------------------------------------ */

static void
test_allowed (void)
{
  g_assert_false (sw_check_allowed ("never", FALSE));
  g_assert_false (sw_check_allowed ("never", TRUE));
  g_assert_true (sw_check_allowed ("always", FALSE));
  g_assert_true (sw_check_allowed ("always", TRUE));
  g_assert_true (sw_check_allowed ("wifi", FALSE));
  g_assert_false (sw_check_allowed ("wifi", TRUE));
  /* no schema installed, or a mode from a later release: as "wifi" */
  g_assert_true (sw_check_allowed (NULL, FALSE));
  g_assert_false (sw_check_allowed (NULL, TRUE));
  g_assert_false (sw_check_allowed ("hourly", TRUE));
}

static void
test_request (void)
{
  g_autoptr (GHashTable) installed = g_hash_table_new (g_str_hash, g_str_equal);
  g_autoptr (JsonNode) body = NULL;
  g_autofree char *text = NULL;

  g_hash_table_insert (installed, "vibeos-weather", "1:0.1.1");
  g_hash_table_insert (installed, "gnome-chess", "1:48.0-1");
  g_hash_table_insert (installed, "vibeos-clock", "1:0.1.1~next2");
  body = sw_check_request (installed, "arm64");
  text = json_to_string (body, FALSE);
  /* in name order, every one with its version */
  g_assert_cmpstr (text, ==, "{\"arch\":\"arm64\",\"packages\":[{\"package\":\"gnome-chess\",\"version\":\"1:48.0-1\"},"
                             "{\"package\":\"vibeos-clock\",\"version\":\"1:0.1.1~next2\"},"
                             "{\"package\":\"vibeos-weather\",\"version\":\"1:0.1.1\"}]}");
}

static void
test_request_limit (void)
{
  g_autoptr (GHashTable) installed = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  g_autoptr (JsonNode) body = NULL;

  for (int i = 0; i < 5100; i++)
    g_hash_table_insert (installed, g_strdup_printf ("pkg%05d", i), "1.0");
  body = sw_check_request (installed, "arm64");
  /* the store takes 5000 at most */
  g_assert_cmpuint (json_array_get_length (sw_json_array (json_node_get_object (body), "packages")), ==, 5000);
}

static void
test_entries (void)
{
  g_autofree char *w = entry_json ("vibeos-weather", "Weather", "1:0.1.1", "1:0.1.2", NULL);
  g_autofree char *c = entry_json ("vibeos-clock", "Clock", "1:0.1.1", "1:0.1.2", "0.1.2");
  g_autofree char *answer = g_strdup_printf ("{\"data\":[%s,{\"app\":{\"name\":\"No package\"},\"version\":\"1\"},"
                                             "{\"app\":{\"package\":\"no-version\"}},%s]}", w, c);
  g_autoptr (GPtrArray) entries = entries_of (answer);
  g_autoptr (GPtrArray) none = entries_of ("{\"data\":[]}");
  g_autoptr (GPtrArray) garbage = entries_of ("{\"data\":{}}");

  /* by name; entries without a package or a version left out */
  g_assert_cmpuint (entries->len, ==, 2);
  g_assert_cmpstr (package_at (entries, 0), ==, "vibeos-clock");
  g_assert_cmpstr (package_at (entries, 1), ==, "vibeos-weather");
  g_assert_cmpuint (none->len, ==, 0);
  g_assert_cmpuint (garbage->len, ==, 0);
}

static void
test_suggested (void)
{
  g_autofree char *dir = g_dir_make_tmp ("sw-check-XXXXXX", NULL);
  g_autofree char *path = g_build_filename (dir, "deeper", "suggested-updates", NULL);
  g_autofree char *c1 = entry_json ("vibeos-clock", "Clock", "1:0.1.1", "1:0.1.2", NULL);
  g_autofree char *w1 = entry_json ("vibeos-weather", "Weather", "1:0.1.1", "1:0.1.2", NULL);
  g_autofree char *c2 = entry_json ("vibeos-clock", "Clock", "1:0.1.1", "1:0.1.3", NULL);
  g_autoptr (GPtrArray) one = entries_list (c1, w1, NULL);
  g_autoptr (GPtrArray) two = entries_list (c2, w1, NULL);
  g_autoptr (GPtrArray) only_weather = entries_list (w1, NULL);
  g_autoptr (GHashTable) suggested = sw_suggested_load (path);
  g_autoptr (GError) error = NULL;
  g_autofree char *contents = NULL;
  GStatBuf st;

  /* no file yet: nothing suggested, everything is new */
  g_assert_cmpuint (g_hash_table_size (suggested), ==, 0);
  {
    g_autoptr (GPtrArray) fresh = sw_suggested_filter (suggested, one);

    g_assert_cmpuint (fresh->len, ==, 2);
  }
  g_assert_true (sw_suggested_replace (suggested, one));
  g_assert_false (sw_suggested_replace (suggested, one));
  g_assert_true (sw_suggested_save (suggested, path, &error));
  g_assert_no_error (error);
  g_assert_cmpint (g_stat (path, &st), ==, 0);
  g_assert_cmpint (st.st_mode & 0777, ==, 0600);
  g_assert_true (g_file_get_contents (path, &contents, NULL, NULL));
  g_assert_cmpstr (contents, ==, "# the updates Software has suggested: package version\n"
                                 "vibeos-clock 1:0.1.2\nvibeos-weather 1:0.1.2\n");
  g_hash_table_unref (g_steal_pointer (&suggested));

  /* read back: the same versions are not new; a newer Clock is */
  suggested = sw_suggested_load (path);
  g_assert_cmpuint (g_hash_table_size (suggested), ==, 2);
  {
    g_autoptr (GPtrArray) same = sw_suggested_filter (suggested, one);
    g_autoptr (GPtrArray) fresh = sw_suggested_filter (suggested, two);

    g_assert_cmpuint (same->len, ==, 0);
    g_assert_cmpuint (fresh->len, ==, 1);
    g_assert_cmpstr (package_at (fresh, 0), ==, "vibeos-clock");
  }

  /* Clock updated: forgotten, so its next version is new again */
  g_assert_true (sw_suggested_replace (suggested, only_weather));
  g_assert_cmpuint (g_hash_table_size (suggested), ==, 1);
  g_assert_false (g_hash_table_contains (suggested, "vibeos-clock"));
  {
    g_autoptr (GPtrArray) fresh = sw_suggested_filter (suggested, one);

    g_assert_cmpuint (fresh->len, ==, 1);
    g_assert_cmpstr (package_at (fresh, 0), ==, "vibeos-clock");
  }

  /* a hand-edited or broken file: what can be read counts, the rest is skipped */
  g_assert_true (g_file_set_contents (path, "# note\n\n  vibeos-clock   1:0.1.2  \nbroken\nx y z\n\tvibeos-files 1:0.1.1\n",
                                      -1, NULL));
  g_hash_table_unref (g_steal_pointer (&suggested));
  suggested = sw_suggested_load (path);
  g_assert_cmpuint (g_hash_table_size (suggested), ==, 2);
  g_assert_cmpstr (g_hash_table_lookup (suggested, "vibeos-clock"), ==, "1:0.1.2");
  g_assert_cmpstr (g_hash_table_lookup (suggested, "vibeos-files"), ==, "1:0.1.1");

  g_unlink (path);
  {
    g_autofree char *deeper = g_path_get_dirname (path);

    g_rmdir (deeper);
  }
  g_rmdir (dir);
}

static void
test_text (void)
{
  g_autofree char *c = entry_json ("vibeos-clock", "Clock", "1:0.1.1", "1:0.1.2", NULL);
  g_autofree char *w = entry_json ("vibeos-weather", "Weather", "1:0.1.1", "1:0.1.2", "0.1.2 beta");
  g_autofree char *f = entry_json ("vibeos-files", "Files", "1:0.1.1", "0.1.2+b1", NULL);
  g_autofree char *a = entry_json ("vibeos-calculator", "Calculator", "1:0.1.1", "1:0.1.2", NULL);
  g_autofree char *d = entry_json ("vibeos-dialer", "Phone", "1:0.1.1", "1:0.1.2", NULL);
  g_autoptr (GPtrArray) one = entries_list (c, NULL);
  g_autoptr (GPtrArray) two = entries_list (c, w, NULL);
  g_autoptr (GPtrArray) three = entries_list (c, w, f, NULL);
  g_autoptr (GPtrArray) five = entries_list (c, w, f, a, d, NULL);
  g_autoptr (GPtrArray) only_weather = entries_list (w, NULL);
  g_autoptr (GPtrArray) phone_files = entries_list (d, f, NULL);
  char *title, *body;

  /* the version people read: the store's release, else the package's without the epoch */
  {
    g_autofree char *v1 = sw_check_display_version (g_ptr_array_index (one, 0));
    g_autofree char *v2 = sw_check_display_version (g_ptr_array_index (only_weather, 0));

    g_assert_cmpstr (v1, ==, "0.1.2");
    g_assert_cmpstr (v2, ==, "0.1.2 beta");
  }

  sw_check_notification_text (one, one, &title, &body);
  g_assert_cmpstr (title, ==, "Clock can be updated");
  g_assert_cmpstr (body, ==, "Version 0.1.2 is ready to install.");
  g_free (title);
  g_free (body);

  /* by name, unless some are new: those first */
  sw_check_notification_text (two, two, &title, &body);
  g_assert_cmpstr (title, ==, "2 apps can be updated");
  g_assert_cmpstr (body, ==, "Clock and Weather");
  g_free (title);
  g_free (body);
  sw_check_notification_text (two, only_weather, &title, &body);
  g_assert_cmpstr (body, ==, "Weather and Clock");
  g_free (title);
  g_free (body);

  sw_check_notification_text (three, three, &title, &body);
  g_assert_cmpstr (title, ==, "3 apps can be updated");
  g_assert_cmpstr (body, ==, "Clock, Files and Weather");
  g_free (title);
  g_free (body);

  sw_check_notification_text (five, phone_files, &title, &body);
  g_assert_cmpstr (title, ==, "5 apps can be updated");
  g_assert_cmpstr (body, ==, "Files, Phone, Calculator and 2 more");
  g_free (title);
  g_free (body);

  sw_check_notification_text (NULL, NULL, &title, &body);
  g_assert_null (title);
  g_assert_null (body);
}

/* ---- against the store ----------------------------------------------------------------------------------------- */

typedef struct {
  gboolean   done;
  GPtrArray *entries;
  char      *error;
} Asked;

static void
on_asked (GPtrArray *entries, const SwApiError *error, gpointer data)
{
  Asked *asked = data;

  asked->done = TRUE;
  if (entries)
    asked->entries = g_ptr_array_ref (entries);
  if (error)
    asked->error = g_strdup (error->code);
}

static void
wait_until (gboolean *flag)
{
  gint64 until = g_get_monotonic_time () + 10 * G_USEC_PER_SEC;

  while (!*flag && g_get_monotonic_time () < until)
    g_main_context_iteration (NULL, TRUE);
  g_assert_true (*flag);
}

static void
test_ask (void)
{
  g_autoptr (SwApi) api = sw_api_new (base_url, NULL);
  g_autoptr (GHashTable) installed = g_hash_table_new (g_str_hash, g_str_equal);
  g_autofree char *c = entry_json ("vibeos-clock", "Clock", "1:0.1.1", "1:0.1.2", NULL);
  g_autoptr (JsonNode) sent = NULL;
  Asked asked = { 0 };

  g_free (check_answer);
  check_answer = g_strdup_printf ("{\"data\":[%s]}", c);
  g_hash_table_insert (installed, "vibeos-clock", "1:0.1.1");
  sw_check_ask (api, installed, "arm64", NULL, on_asked, &asked);
  wait_until (&asked.done);
  g_assert_null (asked.error);
  g_assert_cmpuint (asked.entries->len, ==, 1);
  g_assert_cmpstr (package_at (asked.entries, 0), ==, "vibeos-clock");
  sent = parse (check_body);
  g_assert_cmpstr (sw_json_string (json_node_get_object (sent), "arch"), ==, "arm64");
  g_ptr_array_unref (asked.entries);

  /* a store that is not there: the error, no entries */
  {
    g_autoptr (SwApi) gone = sw_api_new ("http://127.0.0.1:9", NULL);
    Asked failed = { 0 };

    sw_check_ask (gone, installed, "arm64", NULL, on_asked, &failed);
    wait_until (&failed.done);
    g_assert_null (failed.entries);
    g_assert_cmpstr (failed.error, ==, "network");
    g_free (failed.error);
  }
}

/* ---- whole runs, with a notification server ------------------------------------------------------------------- */

typedef struct {
  int   added, removed;
  char *app_id, *id, *title, *body, *action;
} Notifications;

static Notifications shown;

static const char notifications_xml[] =
  "<node><interface name='org.gtk.Notifications'>"
  "<method name='AddNotification'><arg type='s' direction='in'/><arg type='s' direction='in'/>"
  "<arg type='a{sv}' direction='in'/></method>"
  "<method name='RemoveNotification'><arg type='s' direction='in'/><arg type='s' direction='in'/></method>"
  "</interface></node>";

static void
on_notifications_call (GDBusConnection *bus, const char *sender, const char *path, const char *iface,
                       const char *method, GVariant *params, GDBusMethodInvocation *invocation, gpointer data)
{
  if (!strcmp (method, "AddNotification"))
    {
      g_autoptr (GVariant) dict = NULL;

      shown.added++;
      g_clear_pointer (&shown.app_id, g_free);
      g_clear_pointer (&shown.id, g_free);
      g_clear_pointer (&shown.title, g_free);
      g_clear_pointer (&shown.body, g_free);
      g_clear_pointer (&shown.action, g_free);
      g_variant_get (params, "(ss@a{sv})", &shown.app_id, &shown.id, &dict);
      g_variant_lookup (dict, "title", "s", &shown.title);
      g_variant_lookup (dict, "body", "s", &shown.body);
      g_variant_lookup (dict, "default-action", "s", &shown.action);
    }
  else
    shown.removed++;
  g_dbus_method_invocation_return_value (invocation, NULL);
}

static const GDBusInterfaceVTable notifications_vtable = { on_notifications_call, NULL, NULL };

static void
spin (int ms)
{
  gint64 until = g_get_monotonic_time () + ms * 1000;

  while (g_get_monotonic_time () < until)
    g_main_context_iteration (NULL, FALSE);
}

/* one run, then the notification calls it made (they arrive on the loop after it returns) */
static void
run_once (int expect_added, int expect_removed)
{
  int added = shown.added, removed = shown.removed;
  gint64 until;

  g_assert_cmpint (sw_check_run (), ==, 0);
  until = g_get_monotonic_time () + 5 * G_USEC_PER_SEC;
  while ((shown.added - added < expect_added || shown.removed - removed < expect_removed) &&
         g_get_monotonic_time () < until)
    g_main_context_iteration (NULL, TRUE);
  /* and nothing more than that */
  spin (200);
  g_assert_cmpint (shown.added - added, ==, expect_added);
  g_assert_cmpint (shown.removed - removed, ==, expect_removed);
}

static char *
suggested_file (void)
{
  g_autofree char *path = sw_suggested_path ();
  char *contents = NULL;

  if (!g_file_get_contents (path, &contents, NULL, NULL))
    return g_strdup ("");
  return contents;
}

static void
test_run (void)
{
  g_autoptr (GTestDBus) dbus = NULL;
  g_autoptr (GDBusConnection) bus = NULL;
  g_autoptr (GDBusNodeInfo) info = NULL;
  g_autoptr (GSettings) settings = NULL;
  g_autoptr (GError) error = NULL;
  g_autofree char *status_path = NULL;
  g_autofree char *c1 = entry_json ("vibeos-clock", "Clock", "1:0.1.1", "1:0.1.2", NULL);
  g_autofree char *w1 = entry_json ("vibeos-weather", "Weather", "1:0.1.1", "1:0.1.2", "0.1.2");
  g_autofree char *c2 = entry_json ("vibeos-clock", "Clock", "1:0.1.1", "1:0.1.3", NULL);
  g_autofree char *contents = NULL;
  g_autofree char *daemon = g_find_program_in_path ("dbus-daemon");
  g_autoptr (GSettingsSchema) schema = g_settings_schema_source_lookup (g_settings_schema_source_get_default (),
                                                                         "vibeos.Software", TRUE);
  guint registration, owner;
  int hits;

  if (!daemon)
    {
      g_test_skip ("no dbus-daemon");
      return;
    }
  if (!schema)
    {
      g_test_skip ("the vibeos.Software schema is not compiled (GSETTINGS_SCHEMA_DIR)");
      return;
    }
  dbus = g_test_dbus_new (G_TEST_DBUS_NONE);
  g_test_dbus_up (dbus);
  bus = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, &error);
  g_assert_no_error (error);

  /* dpkg's list: Clock and Weather installed */
  status_path = g_build_filename (g_get_user_data_dir (), "dpkg-status", NULL);
  g_mkdir_with_parents (g_get_user_data_dir (), 0700);
  g_assert_true (g_file_set_contents (status_path,
                                      "Package: vibeos-clock\nStatus: install ok installed\nVersion: 1:0.1.1\n\n"
                                      "Package: vibeos-weather\nStatus: install ok installed\nVersion: 1:0.1.1\n",
                                      -1, NULL));
  g_setenv ("VIBEOS_SOFTWARE_DPKG_STATUS", status_path, TRUE);
  settings = g_settings_new ("vibeos.Software");
  g_settings_set_string (settings, "update-mode", "wifi");

  /* no notification server yet: the store is not even asked */
  hits = check_hits;
  g_assert_cmpint (sw_check_run (), ==, 0);
  g_assert_cmpint (check_hits, ==, hits);

  info = g_dbus_node_info_new_for_xml (notifications_xml, &error);
  g_assert_no_error (error);
  registration = g_dbus_connection_register_object (bus, "/org/gtk/Notifications", info->interfaces[0],
                                                    &notifications_vtable, NULL, NULL, &error);
  g_assert_no_error (error);
  {
    g_autoptr (GVariant) reply = g_dbus_connection_call_sync (bus, "org.freedesktop.DBus", "/org/freedesktop/DBus",
                                                              "org.freedesktop.DBus", "RequestName",
                                                              g_variant_new ("(su)", "org.gtk.Notifications", 0),
                                                              G_VARIANT_TYPE ("(u)"), G_DBUS_CALL_FLAGS_NONE, -1,
                                                              NULL, &error);

    g_assert_no_error (error);
    g_variant_get (reply, "(u)", &owner);
    g_assert_cmpuint (owner, ==, 1);
  }

  /* Clock and Weather are new: one notification, by name, and both remembered */
  g_free (check_answer);
  check_answer = g_strdup_printf ("{\"data\":[%s,%s]}", w1, c1);
  hits = check_hits;
  run_once (1, 0);
  g_assert_cmpint (check_hits, ==, hits + 1);
  g_assert_nonnull (strstr (check_body, "{\"package\":\"vibeos-clock\",\"version\":\"1:0.1.1\"}"));
  g_assert_cmpstr (shown.app_id, ==, "vibeos.Software");
  g_assert_cmpstr (shown.id, ==, "updates");
  g_assert_cmpstr (shown.title, ==, "2 apps can be updated");
  g_assert_cmpstr (shown.body, ==, "Clock and Weather");
  g_assert_cmpstr (shown.action, ==, "app.show-updates");
  contents = suggested_file ();
  g_assert_nonnull (strstr (contents, "vibeos-clock 1:0.1.2\n"));
  g_assert_nonnull (strstr (contents, "vibeos-weather 1:0.1.2\n"));
  g_clear_pointer (&contents, g_free);

  /* the same answer again: asked, nothing told */
  hits = check_hits;
  run_once (0, 0);
  g_assert_cmpint (check_hits, ==, hits + 1);

  /* a newer Clock: told again, Clock first */
  g_free (check_answer);
  check_answer = g_strdup_printf ("{\"data\":[%s,%s]}", c2, w1);
  run_once (1, 0);
  g_assert_cmpstr (shown.title, ==, "2 apps can be updated");
  g_assert_cmpstr (shown.body, ==, "Clock and Weather");
  contents = suggested_file ();
  g_assert_nonnull (strstr (contents, "vibeos-clock 1:0.1.3\n"));
  g_clear_pointer (&contents, g_free);

  /* "never": nothing asked, nothing told */
  g_settings_set_string (settings, "update-mode", "never");
  hits = check_hits;
  run_once (0, 0);
  g_assert_cmpint (check_hits, ==, hits);
  g_settings_set_string (settings, "update-mode", "always");

  /* everything updated meanwhile: the notification goes, and so does the list */
  g_free (check_answer);
  check_answer = g_strdup ("{\"data\":[]}");
  run_once (0, 1);
  contents = suggested_file ();
  g_assert_cmpstr (contents, ==, "# the updates Software has suggested: package version\n");
  g_clear_pointer (&contents, g_free);

  /* Weather's 0.1.2 again later (forgotten when it went): told again */
  g_free (check_answer);
  check_answer = g_strdup_printf ("{\"data\":[%s]}", w1);
  run_once (1, 0);
  g_assert_cmpstr (shown.title, ==, "Weather can be updated");
  g_assert_cmpstr (shown.body, ==, "Version 0.1.2 is ready to install.");

  g_settings_reset (settings, "update-mode");
  g_dbus_connection_unregister_object (bus, registration);
  g_unsetenv ("VIBEOS_SOFTWARE_DPKG_STATUS");
  g_unlink (status_path);
  g_clear_object (&bus);
  g_test_dbus_down (dbus);
}

int
main (int argc, char **argv)
{
  g_autoptr (SoupServer) server = NULL;
  g_autoptr (GError) error = NULL;
  GSList *uris;

  g_test_init (&argc, &argv, G_TEST_OPTION_ISOLATE_DIRS, NULL);
  server = soup_server_new (NULL, NULL);
  soup_server_add_handler (server, NULL, handler, NULL, NULL);
  if (!soup_server_listen_local (server, 0, SOUP_SERVER_LISTEN_IPV4_ONLY, &error))
    g_error ("could not listen: %s", error->message);
  uris = soup_server_get_uris (server);
  base_url = g_uri_to_string (uris->data);
  g_slist_free_full (uris, (GDestroyNotify) g_uri_unref);
  /* the store of sw_api_get_default (), which a run uses */
  g_setenv ("VIBEOS_STORE_URL", base_url, TRUE);
  check_answer = g_strdup ("{\"data\":[]}");

  g_test_add_func ("/check/allowed", test_allowed);
  g_test_add_func ("/check/request", test_request);
  g_test_add_func ("/check/request-limit", test_request_limit);
  g_test_add_func ("/check/entries", test_entries);
  g_test_add_func ("/check/suggested", test_suggested);
  g_test_add_func ("/check/text", test_text);
  g_test_add_func ("/check/ask", test_ask);
  g_test_add_func ("/check/run", test_run);
  return g_test_run ();
}
