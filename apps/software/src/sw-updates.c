/* SwUpdates: see sw-updates.h */
#include "sw-updates.h"
#include "sw-device.h"
#include "sw-installer.h"
#include "sw-json.h"

#define RECHECK_MS 2000

struct _SwUpdates {
  GObject       parent_instance;
  GHashTable   *entries;      /* package -> JsonObject */
  gboolean      checking, again, checked;
  SwApiError    error;
  gboolean      has_error;
  GCancellable *cancellable;
  GSettings    *settings;
  guint         recheck;
};

G_DEFINE_FINAL_TYPE (SwUpdates, sw_updates, G_TYPE_OBJECT)

static guint changed_signal;

static void check_now (SwUpdates *self);

/* the setting allows a check nobody asked for */
static gboolean
may_check (SwUpdates *self)
{
  g_autofree char *mode = self->settings ? g_settings_get_string (self->settings, "update-mode") : g_strdup ("wifi");

  if (!g_strcmp0 (mode, "never"))
    return FALSE;
  if (!g_strcmp0 (mode, "wifi"))
    return !g_network_monitor_get_network_metered (g_network_monitor_get_default ());
  return TRUE;
}

static void
on_checked (SwApiResult *result, const SwApiError *error, gpointer data)
{
  SwUpdates *self = data;
  JsonArray *list = result ? sw_json_node_array (result->data) : NULL;

  self->checking = FALSE;
  if (self->has_error)
    sw_api_error_clear (&self->error);
  self->has_error = FALSE;
  if (error)
    {
      self->error = *error;
      self->error.code = g_strdup (error->code);
      self->error.message = g_strdup (error->message);
      self->error.reason = g_strdup (error->reason);
      self->error.fields = error->fields ? json_object_ref (error->fields) : NULL;
      self->has_error = TRUE;
    }
  else
    {
      g_hash_table_remove_all (self->entries);
      for (guint i = 0; list && i < json_array_get_length (list); i++)
        {
          JsonObject *entry = sw_json_array_object (list, i);
          const char *package = sw_json_string (sw_json_object (entry, "app"), "package");

          if (package)
            g_hash_table_replace (self->entries, g_strdup (package), json_object_ref (entry));
        }
      self->checked = TRUE;
    }
  g_signal_emit (self, changed_signal, 0);
  if (self->again)
    {
      self->again = FALSE;
      check_now (self);
    }
}

static void
check_now (SwUpdates *self)
{
  SwInstaller *installer = sw_installer_get_default ();
  GHashTable *installed = sw_installer_get_installed_all (installer);
  g_autoptr (JsonBuilder) b = NULL;
  g_autoptr (JsonNode) body = NULL;
  GHashTableIter iter;
  gpointer package, version;

  if (self->checking)
    {
      self->again = TRUE;
      return;
    }
  if (!sw_installer_is_loaded (installer) || !g_hash_table_size (installed))
    return;
  b = json_builder_new ();
  json_builder_begin_object (b);
  json_builder_set_member_name (b, "arch");
  json_builder_add_string_value (b, sw_device_arch ());
  json_builder_set_member_name (b, "packages");
  json_builder_begin_array (b);
  /* every installed package: the store keeps those it knows (at most 5000) */
  g_hash_table_iter_init (&iter, installed);
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
  self->checking = TRUE;
  g_signal_emit (self, changed_signal, 0);
  sw_api_send (sw_api_get_default (), "POST", "/updates/check", body, SW_API_NO_TOKEN, self->cancellable,
               on_checked, self);
}

void
sw_updates_check (SwUpdates *self, gboolean asked)
{
  if (asked || may_check (self))
    check_now (self);
}

static gboolean
on_recheck (gpointer data)
{
  SwUpdates *self = data;

  self->recheck = 0;
  sw_updates_check (self, FALSE);
  return G_SOURCE_REMOVE;
}

static void
on_installed_changed (SwInstaller *installer, const char *package, SwUpdates *self)
{
  /* the installed list (not a job's progress) changed: after a while, in case more follows */
  if (package)
    return;
  if (self->checked || self->has_error)
    {
      g_clear_handle_id (&self->recheck, g_source_remove);
      self->recheck = g_timeout_add (RECHECK_MS, on_recheck, self);
    }
  else if (!self->checking)
    /* the list's first reading: the check at start */
    sw_updates_check (self, FALSE);
}

static void
on_package_changed (SwInstaller *installer, const char *package, const char *old, const char *now, SwUpdates *self)
{
  JsonObject *entry = g_hash_table_lookup (self->entries, package);

  /* updated (or removed): no longer an update until the store says it is */
  if (entry && g_strcmp0 (now, sw_json_string (entry, "installed_version")))
    {
      g_hash_table_remove (self->entries, package);
      g_signal_emit (self, changed_signal, 0);
    }
}

JsonObject *
sw_updates_get (SwUpdates *self, const char *package)
{
  return package ? g_hash_table_lookup (self->entries, package) : NULL;
}

static int
by_name (gconstpointer a, gconstpointer b)
{
  JsonObject *x = *(JsonObject **) a, *y = *(JsonObject **) b;

  return g_utf8_collate (sw_json_string (sw_json_object (x, "app"), "name") ?: "",
                         sw_json_string (sw_json_object (y, "app"), "name") ?: "");
}

GPtrArray *
sw_updates_list (SwUpdates *self)
{
  GPtrArray *list = g_ptr_array_new ();
  GHashTableIter iter;
  gpointer entry;

  g_hash_table_iter_init (&iter, self->entries);
  while (g_hash_table_iter_next (&iter, NULL, &entry))
    g_ptr_array_add (list, entry);
  g_ptr_array_sort (list, by_name);
  return list;
}

gboolean
sw_updates_is_checking (SwUpdates *self)
{
  return self->checking;
}

const SwApiError *
sw_updates_get_error (SwUpdates *self)
{
  return self->has_error ? &self->error : NULL;
}

gboolean
sw_updates_has_checked (SwUpdates *self)
{
  return self->checked;
}

static void
sw_updates_class_init (SwUpdatesClass *klass)
{
  changed_signal = g_signal_new ("changed", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                                 G_TYPE_NONE, 0);
}

static void
sw_updates_init (SwUpdates *self)
{
  GSettingsSchema *schema = g_settings_schema_source_lookup (g_settings_schema_source_get_default (),
                                                             "vibeos.Software", TRUE);

  self->entries = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, (GDestroyNotify) json_object_unref);
  self->cancellable = g_cancellable_new ();
  if (schema)
    {
      self->settings = g_settings_new ("vibeos.Software");
      g_settings_schema_unref (schema);
    }
  g_signal_connect (sw_installer_get_default (), "changed", G_CALLBACK (on_installed_changed), self);
  g_signal_connect (sw_installer_get_default (), "package-changed", G_CALLBACK (on_package_changed), self);
}

SwUpdates *
sw_updates_get_default (void)
{
  static SwUpdates *updates;

  if (!updates)
    updates = g_object_new (SW_TYPE_UPDATES, NULL);
  return updates;
}
