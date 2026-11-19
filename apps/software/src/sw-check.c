/* sw-check: see sw-check.h */
#include "sw-check.h"
#include "sw-device.h"
#include "sw-dpkg.h"
#include "sw-json.h"

#include <errno.h>
#include <glib/gstdio.h>
#include <string.h>

#define MAX_PACKAGES 5000

gboolean
sw_check_allowed (const char *mode, gboolean metered)
{
  if (!g_strcmp0 (mode, "never"))
    return FALSE;
  if (!g_strcmp0 (mode, "always"))
    return TRUE;
  return !metered;
}

static int
by_string (gconstpointer a, gconstpointer b)
{
  return strcmp (*(const char **) a, *(const char **) b);
}

JsonNode *
sw_check_request (GHashTable *installed, const char *arch)
{
  g_autoptr (JsonBuilder) b = json_builder_new ();
  g_autoptr (GPtrArray) packages = g_ptr_array_new ();
  GHashTableIter iter;
  gpointer package;

  /* in name order: the same list makes the same request */
  g_hash_table_iter_init (&iter, installed);
  while (g_hash_table_iter_next (&iter, &package, NULL))
    g_ptr_array_add (packages, package);
  g_ptr_array_sort (packages, by_string);

  json_builder_begin_object (b);
  json_builder_set_member_name (b, "arch");
  json_builder_add_string_value (b, arch);
  json_builder_set_member_name (b, "packages");
  json_builder_begin_array (b);
  /* every installed package: the store keeps those it knows */
  for (guint i = 0; i < packages->len && i < MAX_PACKAGES; i++)
    {
      json_builder_begin_object (b);
      json_builder_set_member_name (b, "package");
      json_builder_add_string_value (b, g_ptr_array_index (packages, i));
      json_builder_set_member_name (b, "version");
      json_builder_add_string_value (b, g_hash_table_lookup (installed, g_ptr_array_index (packages, i)));
      json_builder_end_object (b);
    }
  json_builder_end_array (b);
  json_builder_end_object (b);
  return json_builder_get_root (b);
}

static const char *
entry_package (JsonObject *entry)
{
  return sw_json_string (sw_json_object (entry, "app"), "package");
}

static const char *
entry_name (JsonObject *entry)
{
  const char *name = sw_json_string (sw_json_object (entry, "app"), "name");

  return name && *name ? name : entry_package (entry);
}

static int
by_name (gconstpointer a, gconstpointer b)
{
  return g_utf8_collate (entry_name (*(JsonObject **) a) ?: "", entry_name (*(JsonObject **) b) ?: "");
}

GPtrArray *
sw_check_entries (JsonNode *data)
{
  GPtrArray *entries = g_ptr_array_new_with_free_func ((GDestroyNotify) json_object_unref);
  JsonArray *list = sw_json_node_array (data);

  for (guint i = 0; list && i < json_array_get_length (list); i++)
    {
      JsonObject *entry = sw_json_array_object (list, i);

      if (entry && entry_package (entry) && sw_json_string (entry, "version"))
        g_ptr_array_add (entries, json_object_ref (entry));
    }
  g_ptr_array_sort (entries, by_name);
  return entries;
}

typedef struct {
  SwCheckCallback callback;
  gpointer        data;
} Ask;

static void
on_answer (SwApiResult *result, const SwApiError *error, gpointer data)
{
  Ask *ask = data;

  if (error)
    ask->callback (NULL, error, ask->data);
  else
    {
      g_autoptr (GPtrArray) entries = sw_check_entries (result->data);

      ask->callback (entries, NULL, ask->data);
    }
  g_free (ask);
}

void
sw_check_ask (SwApi *api, GHashTable *installed, const char *arch, GCancellable *cancellable,
              SwCheckCallback callback, gpointer data)
{
  g_autoptr (JsonNode) body = sw_check_request (installed, arch);
  Ask *ask = g_new0 (Ask, 1);

  ask->callback = callback;
  ask->data = data;
  sw_api_send (api, "POST", "/updates/check", body, SW_API_NO_TOKEN, cancellable, on_answer, ask);
}

/* ---- what was suggested ---------------------------------------------------------------------------------------- */

char *
sw_suggested_path (void)
{
  return g_build_filename (g_get_user_data_dir (), "vibeos-software", "suggested-updates", NULL);
}

GHashTable *
sw_suggested_load (const char *path)
{
  GHashTable *suggested = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
  g_autofree char *contents = NULL;
  g_auto (GStrv) lines = NULL;

  if (!g_file_get_contents (path, &contents, NULL, NULL))
    return suggested;
  lines = g_strsplit (contents, "\n", -1);
  for (guint i = 0; lines[i]; i++)
    {
      g_auto (GStrv) fields = NULL;

      g_strstrip (lines[i]);
      if (!*lines[i] || *lines[i] == '#')
        continue;
      fields = g_strsplit_set (lines[i], " \t", 2);
      if (fields[0] && fields[1] && *fields[0])
        {
          g_strstrip (fields[1]);
          if (*fields[1] && !strpbrk (fields[1], " \t"))
            g_hash_table_replace (suggested, g_strdup (fields[0]), g_strdup (fields[1]));
        }
    }
  return suggested;
}

gboolean
sw_suggested_save (GHashTable *suggested, const char *path, GError **error)
{
  g_autoptr (GString) text = g_string_new ("# the updates Software has suggested: package version\n");
  g_autoptr (GPtrArray) packages = g_ptr_array_new ();
  g_autofree char *dir = g_path_get_dirname (path);
  GHashTableIter iter;
  gpointer package;

  g_hash_table_iter_init (&iter, suggested);
  while (g_hash_table_iter_next (&iter, &package, NULL))
    g_ptr_array_add (packages, package);
  g_ptr_array_sort (packages, by_string);
  for (guint i = 0; i < packages->len; i++)
    g_string_append_printf (text, "%s %s\n", (char *) g_ptr_array_index (packages, i),
                            (char *) g_hash_table_lookup (suggested, g_ptr_array_index (packages, i)));
  if (g_mkdir_with_parents (dir, 0700) < 0)
    {
      int saved = errno;

      g_set_error (error, G_FILE_ERROR, g_file_error_from_errno (saved), "could not make %s: %s", dir,
                   g_strerror (saved));
      return FALSE;
    }
  return g_file_set_contents_full (path, text->str, text->len, G_FILE_SET_CONTENTS_CONSISTENT, 0600, error);
}

GPtrArray *
sw_suggested_filter (GHashTable *suggested, GPtrArray *entries)
{
  GPtrArray *fresh = g_ptr_array_new_with_free_func ((GDestroyNotify) json_object_unref);

  for (guint i = 0; entries && i < entries->len; i++)
    {
      JsonObject *entry = g_ptr_array_index (entries, i);

      if (g_strcmp0 (g_hash_table_lookup (suggested, entry_package (entry)), sw_json_string (entry, "version")))
        g_ptr_array_add (fresh, json_object_ref (entry));
    }
  return fresh;
}

gboolean
sw_suggested_replace (GHashTable *suggested, GPtrArray *entries)
{
  g_autoptr (GHashTable) now = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
  gboolean changed;
  GHashTableIter iter;
  gpointer package, version;

  for (guint i = 0; entries && i < entries->len; i++)
    {
      JsonObject *entry = g_ptr_array_index (entries, i);

      g_hash_table_replace (now, g_strdup (entry_package (entry)), g_strdup (sw_json_string (entry, "version")));
    }
  changed = g_hash_table_size (now) != g_hash_table_size (suggested);
  g_hash_table_iter_init (&iter, now);
  while (!changed && g_hash_table_iter_next (&iter, &package, &version))
    changed = g_strcmp0 (g_hash_table_lookup (suggested, package), version) != 0;
  if (!changed)
    return FALSE;
  g_hash_table_remove_all (suggested);
  g_hash_table_iter_init (&iter, now);
  while (g_hash_table_iter_next (&iter, &package, &version))
    g_hash_table_replace (suggested, g_strdup (package), g_strdup (version));
  return TRUE;
}

/* ---- the notification's words ---------------------------------------------------------------------------------- */

char *
sw_check_display_version (JsonObject *entry)
{
  const char *release = sw_json_string (sw_json_object (entry, "release"), "version");
  const char *version = sw_json_string (entry, "version");
  const char *colon;

  if (release && *release)
    return g_strdup (release);
  if (!version)
    return g_strdup ("");
  /* "1:0.1.2": the epoch only orders versions, nobody reads it */
  colon = strchr (version, ':');
  return g_strdup (colon ? colon + 1 : version);
}

void
sw_check_notification_text (GPtrArray *entries, GPtrArray *fresh, char **title, char **body)
{
  g_autoptr (GPtrArray) order = g_ptr_array_new ();
  g_autoptr (GString) names = NULL;
  guint shown;

  *title = *body = NULL;
  if (!entries || !entries->len)
    return;
  /* the updates not suggested before first: they are why the notification is here */
  for (guint i = 0; fresh && i < fresh->len; i++)
    g_ptr_array_add (order, g_ptr_array_index (fresh, i));
  for (guint i = 0; i < entries->len; i++)
    {
      JsonObject *entry = g_ptr_array_index (entries, i);
      gboolean listed = FALSE;

      for (guint j = 0; j < order->len && !listed; j++)
        listed = !g_strcmp0 (entry_package (g_ptr_array_index (order, j)), entry_package (entry));
      if (!listed)
        g_ptr_array_add (order, entry);
    }

  if (order->len == 1)
    {
      g_autofree char *version = sw_check_display_version (g_ptr_array_index (order, 0));

      *title = g_strdup_printf ("%s can be updated", entry_name (g_ptr_array_index (order, 0)));
      *body = *version ? g_strdup_printf ("Version %s is ready to install.", version)
                       : g_strdup ("A new version is ready to install.");
      return;
    }
  *title = g_strdup_printf ("%u apps can be updated", order->len);
  /* three names at most, then how many more */
  shown = order->len <= 3 ? order->len : 3;
  names = g_string_new (NULL);
  for (guint i = 0; i < shown; i++)
    {
      if (i)
        g_string_append (names, i == shown - 1 && shown == order->len ? " and " : ", ");
      g_string_append (names, entry_name (g_ptr_array_index (order, i)));
    }
  if (shown < order->len)
    g_string_append_printf (names, " and %u more", order->len - shown);
  *body = g_string_free (g_steal_pointer (&names), FALSE);
}

/* ---- vibeos-software --check-updates --------------------------------------------------------------------------- */

typedef struct {
  GApplication *app;
  GMainLoop    *loop;
  char         *path;
} Run;

static gboolean
has_owner (GDBusConnection *bus, const char *name)
{
  g_autoptr (GVariant) reply = g_dbus_connection_call_sync (bus, "org.freedesktop.DBus", "/org/freedesktop/DBus",
                                                            "org.freedesktop.DBus", "NameHasOwner",
                                                            g_variant_new ("(s)", name), G_VARIANT_TYPE ("(b)"),
                                                            G_DBUS_CALL_FLAGS_NONE, 5000, NULL, NULL);
  gboolean owned = FALSE;

  if (reply)
    g_variant_get (reply, "(b)", &owned);
  return owned;
}

/* Debian's name for this system's architecture, as the app sends it (sw-device.h) */
static char *
dpkg_arch (void)
{
  const char *argv[] = { "dpkg", "--print-architecture", NULL };
  g_autofree char *out = NULL;
  int status = 0;

  if (g_spawn_sync (NULL, (char **) argv, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_STDERR_TO_DEV_NULL, NULL, NULL, &out,
                    NULL, &status, NULL) && g_spawn_check_wait_status (status, NULL) && out && *g_strstrip (out))
    return g_steal_pointer (&out);
  return g_strdup (sw_device_built_arch ());
}

static void
on_checked (GPtrArray *entries, const SwApiError *error, gpointer data)
{
  Run *run = data;
  g_autoptr (GHashTable) suggested = NULL;
  g_autoptr (GPtrArray) fresh = NULL;
  g_autoptr (GError) save_error = NULL;

  if (error)
    {
      g_autofree char *text = sw_api_error_describe (error);

      g_message ("the store did not answer: %s", text);
      g_main_loop_quit (run->loop);
      return;
    }
  suggested = sw_suggested_load (run->path);
  fresh = sw_suggested_filter (suggested, entries);
  if (fresh->len)
    {
      g_autoptr (GNotification) notification = NULL;
      g_autofree char *title = NULL, *body = NULL;

      sw_check_notification_text (entries, fresh, &title, &body);
      notification = g_notification_new (title);
      g_notification_set_body (notification, body);
      g_notification_set_default_action (notification, "app." SW_CHECK_ACTION);
      g_application_send_notification (run->app, SW_CHECK_NOTIFICATION_ID, notification);
      g_message ("%u update%s, %u not suggested before: notified", entries->len, entries->len == 1 ? "" : "s",
                 fresh->len);
    }
  else if (!entries->len)
    {
      /* nothing left to update (apt or Software did it): a notification still up is wrong now */
      g_application_withdraw_notification (run->app, SW_CHECK_NOTIFICATION_ID);
      g_message ("no updates");
    }
  else
    g_message ("%u update%s, all suggested before", entries->len, entries->len == 1 ? "" : "s");
  if (sw_suggested_replace (suggested, entries) && !sw_suggested_save (suggested, run->path, &save_error))
    g_warning ("could not keep the updates suggested: %s", save_error->message);
  g_main_loop_quit (run->loop);
}

int
sw_check_run (void)
{
  g_autoptr (GApplication) app = g_application_new ("vibeos.Software", G_APPLICATION_NON_UNIQUE);
  g_autoptr (GSettingsSchema) schema = NULL;
  g_autoptr (GSettings) settings = NULL;
  g_autoptr (GError) error = NULL;
  g_autoptr (GHashTable) installed = NULL;
  g_autofree char *mode = NULL, *contents = NULL, *arch = NULL;
  g_autoptr (GMainLoop) loop = NULL;
  GDBusConnection *bus;
  gboolean metered;
  gsize length = 0;
  Run run = { 0 };

  /* registered for the notification only: a non-unique instance never takes the app's name, so Software opened
   * meanwhile (or from the notification) is the real one */
  if (!g_application_register (app, NULL, &error))
    {
      g_warning ("could not register on the session bus: %s", error->message);
      return 1;
    }
  bus = g_application_get_dbus_connection (app);
  if (!bus || (!has_owner (bus, "org.gtk.Notifications") && !has_owner (bus, "org.freedesktop.Notifications")))
    {
      g_message ("no notification server here: no check");
      return 0;
    }

  schema = g_settings_schema_source_lookup (g_settings_schema_source_get_default (), "vibeos.Software", TRUE);
  if (schema)
    {
      settings = g_settings_new ("vibeos.Software");
      mode = g_settings_get_string (settings, "update-mode");
    }
  metered = g_network_monitor_get_network_metered (g_network_monitor_get_default ());
  if (!sw_check_allowed (mode, metered))
    {
      g_message ("update-mode is %s%s: no check", mode ? mode : "wifi", metered ? " and the network is metered" : "");
      return 0;
    }

  if (!g_file_get_contents (sw_dpkg_status_path (), &contents, &length, &error))
    {
      g_warning ("could not read %s: %s", sw_dpkg_status_path (), error->message);
      return 1;
    }
  installed = sw_dpkg_parse_status (contents, length);
  if (!g_hash_table_size (installed))
    return 0;
  arch = dpkg_arch ();
  sw_api_set_identity (sw_api_get_default (), sw_device_version (), arch);

  loop = g_main_loop_new (NULL, FALSE);
  run.app = app;
  run.loop = loop;
  run.path = sw_suggested_path ();
  sw_check_ask (sw_api_get_default (), installed, arch, NULL, on_checked, &run);
  g_main_loop_run (loop);
  /* the notification is a call without a reply: out before the process goes */
  g_dbus_connection_flush_sync (bus, NULL, NULL);
  g_free (run.path);
  return 0;
}
