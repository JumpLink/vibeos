/* The permissions and their store (vs-perms.h) */
#include "vs-perms.h"

#include <string.h>

typedef enum {
  KIND_YESNO,       /* "yes", "no", "ask" */
  KIND_LOCATION,    /* an accuracy ("EXACT"…, "NONE") and when it was last used */
} Kind;

static const struct {
  const char *name, *table, *id;
  Kind        kind;
  VsGrant     fallback;
  gboolean    can_ask, live;
} perms[VS_N_PERMS] = {
  [VS_PERM_NETWORK]    = { "network",    VS_STORE_TABLE, "network",    KIND_YESNO,    VS_GRANT_ALLOW, FALSE, FALSE },
  [VS_PERM_LOCATION]   = { "location",   "location",     "location",   KIND_LOCATION, VS_GRANT_ASK,   TRUE,  TRUE },
  [VS_PERM_CAMERA]     = { "camera",     "devices",      "camera",     KIND_YESNO,    VS_GRANT_ASK,   TRUE,  TRUE },
  [VS_PERM_MICROPHONE] = { "microphone", VS_STORE_TABLE, "microphone", KIND_YESNO,    VS_GRANT_ASK,   TRUE,  TRUE },
  [VS_PERM_CONTACTS]   = { "contacts",   VS_STORE_TABLE, "contacts",   KIND_YESNO,    VS_GRANT_DENY,  FALSE, FALSE },
  [VS_PERM_CALENDAR]   = { "calendar",   VS_STORE_TABLE, "calendar",   KIND_YESNO,    VS_GRANT_DENY,  FALSE, FALSE },
  [VS_PERM_PHONE]      = { "phone",      VS_STORE_TABLE, "phone",      KIND_YESNO,    VS_GRANT_DENY,  FALSE, FALSE },
  [VS_PERM_SMS]        = { "sms",        VS_STORE_TABLE, "sms",        KIND_YESNO,    VS_GRANT_DENY,  FALSE, FALSE },
  [VS_PERM_FILES]      = { "files",      VS_STORE_TABLE, "files",      KIND_YESNO,    VS_GRANT_DENY,  FALSE, FALSE },
  [VS_PERM_BACKGROUND] = { "background", "background",   "background", KIND_YESNO,    VS_GRANT_ALLOW, TRUE,  TRUE },
};

static const char *const grant_names[] = { [VS_GRANT_DENY] = "deny", [VS_GRANT_ALLOW] = "allow",
                                           [VS_GRANT_ASK] = "ask" };

const char *
vs_perm_name (VsPerm perm)
{
  return perm < VS_N_PERMS ? perms[perm].name : NULL;
}

int
vs_perm_from_name (const char *name)
{
  for (int i = 0; name && i < VS_N_PERMS; i++)
    if (!strcmp (perms[i].name, name))
      return i;
  return -1;
}

VsGrant
vs_perm_default (VsPerm perm)
{
  return perms[perm].fallback;
}

gboolean
vs_perm_can_ask (VsPerm perm)
{
  return perms[perm].can_ask;
}

gboolean
vs_perm_is_live (VsPerm perm)
{
  return perms[perm].live;
}

const char *
vs_grant_name (VsGrant grant)
{
  return grant <= VS_GRANT_ASK ? grant_names[grant] : NULL;
}

int
vs_grant_from_name (const char *name)
{
  for (int i = 0; name && i < (int) G_N_ELEMENTS (grant_names); i++)
    if (!strcmp (grant_names[i], name))
      return i;
  return -1;
}

/* what the store's strings mean: -1 for none (or nothing it understands) */
static int
decode (VsPerm perm, char **value)
{
  if (!value || !value[0])
    return -1;
  if (perms[perm].kind == KIND_LOCATION)
    return !strcmp (value[0], "NONE") ? VS_GRANT_DENY : VS_GRANT_ALLOW;
  if (!strcmp (value[0], "yes"))
    return VS_GRANT_ALLOW;
  if (!strcmp (value[0], "no"))
    return VS_GRANT_DENY;
  if (!strcmp (value[0], "ask"))
    return perms[perm].can_ask ? VS_GRANT_ASK : VS_GRANT_DENY;
  return -1;
}

VsGrant
vs_perm_get (VsStore *store, const char *app_id, VsPerm perm)
{
  g_autoptr (GError) error = NULL;
  g_auto (GStrv) value = store ? store->get (store, perms[perm].table, perms[perm].id, app_id, &error) : NULL;
  int grant = decode (perm, value);

  if (error)
    g_debug ("%s's %s: %s", app_id, perms[perm].name, error->message);
  return grant < 0 ? perms[perm].fallback : (VsGrant) grant;
}

gboolean
vs_perm_set (VsStore *store, const char *app_id, VsPerm perm, VsGrant grant, GError **error)
{
  if (grant == VS_GRANT_ASK && !perms[perm].can_ask) {
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, "Nothing asks for the %s permission",
                 perms[perm].name);
    return FALSE;
  }
  if (perms[perm].kind == KIND_LOCATION) {
    /* "ask": no entry, and the portal asks the next time; the time is when the app last had it, which only the
     * portal knows: 0 */
    const char *exact[] = { "EXACT", "0", NULL }, *none[] = { "NONE", "0", NULL };
    return store->set (store, perms[perm].table, perms[perm].id, app_id,
                       grant == VS_GRANT_ASK ? NULL : grant == VS_GRANT_ALLOW ? exact : none, error);
  }
  const char *value[] = { grant == VS_GRANT_ALLOW ? "yes" : grant == VS_GRANT_DENY ? "no" : "ask", NULL };
  return store->set (store, perms[perm].table, perms[perm].id, app_id, value, error);
}

/* where xdg-desktop-portal and the document portal keep what an app may do */
static const char *const app_tables[] = {
  VS_STORE_TABLE, VS_ASK_TABLE, "location", "devices", "background", "notifications", "screenshot", "screencast", "remote-desktop",
  "inhibit", "wallpaper", "realtime", "gamemode", "camera", "documents", NULL
};

gboolean
vs_perm_forget (VsStore *store, const char *app_id, GError **error)
{
  gboolean ok = TRUE;
  for (guint t = 0; app_tables[t]; t++) {
    g_autoptr (GError) e = NULL;
    g_auto (GStrv) ids = store->ids (store, app_tables[t], &e);
    if (e) {
      if (ok)
        g_propagate_prefixed_error (error, g_steal_pointer (&e), "%s: ", app_tables[t]);
      ok = FALSE;
      continue;
    }
    for (guint i = 0; ids && ids[i]; i++) {
      g_autoptr (GError) ge = NULL;
      g_auto (GStrv) value = store->get (store, app_tables[t], ids[i], app_id, &ge);
      if (!value && !ge)
        continue;
      if (!store->set (store, app_tables[t], ids[i], app_id, NULL, ok ? error : NULL))
        ok = FALSE;
    }
  }
  return ok;
}

void
vs_grants_load (VsStore *store, const char *app_id, VsGrants *grants)
{
  for (int i = 0; i < VS_N_PERMS; i++)
    grants->grant[i] = vs_perm_get (store, app_id, i);
}

gboolean
vs_grants_allowed (const VsGrants *grants, VsPerm perm)
{
  return grants->grant[perm] == VS_GRANT_ALLOW;
}

void
vs_store_free (VsStore *store)
{
  if (store)
    store->free (store);
}

/* ---- the permission store over D-Bus ---- */

#define PS_NAME  "org.freedesktop.impl.portal.PermissionStore"
#define PS_PATH  "/org/freedesktop/impl/portal/PermissionStore"
#define PS_IFACE "org.freedesktop.impl.portal.PermissionStore"

typedef struct {
  VsStore          base;
  GDBusConnection *bus;
} DbusStore;

/* "No entry for…", "No such table…": the portal's NotFound */
static gboolean
not_found (GError *error)
{
  g_autofree char *remote = error ? g_dbus_error_get_remote_error (error) : NULL;
  return remote && g_str_has_suffix (remote, ".NotFound");
}

static char **
dbus_get (VsStore *store, const char *table, const char *id, const char *app, GError **error)
{
  DbusStore *self = (DbusStore *) store;
  g_autoptr (GError) local = NULL;
  g_autoptr (GVariant) reply = g_dbus_connection_call_sync (self->bus, PS_NAME, PS_PATH, PS_IFACE, "GetPermission",
                                                            g_variant_new ("(sss)", table, id, app),
                                                            G_VARIANT_TYPE ("(as)"), G_DBUS_CALL_FLAGS_NONE, 5000,
                                                            NULL, &local);
  if (!reply) {
    if (!not_found (local))
      g_propagate_error (error, g_steal_pointer (&local));
    return NULL;
  }
  char **value = NULL;
  g_variant_get (reply, "(^as)", &value);
  if (value && !value[0])
    g_clear_pointer (&value, g_strfreev);
  return value;
}

static gboolean
dbus_set (VsStore *store, const char *table, const char *id, const char *app, const char *const *value,
          GError **error)
{
  DbusStore *self = (DbusStore *) store;
  g_autoptr (GError) local = NULL;
  g_autoptr (GVariant) reply = NULL;

  if (value)
    reply = g_dbus_connection_call_sync (self->bus, PS_NAME, PS_PATH, PS_IFACE, "SetPermission",
                                         g_variant_new ("(sbss^as)", table, TRUE, id, app, value), NULL,
                                         G_DBUS_CALL_FLAGS_NONE, 5000, NULL, &local);
  else
    reply = g_dbus_connection_call_sync (self->bus, PS_NAME, PS_PATH, PS_IFACE, "DeletePermission",
                                         g_variant_new ("(sss)", table, id, app), NULL, G_DBUS_CALL_FLAGS_NONE, 5000,
                                         NULL, &local);
  /* taking out what is not there is done */
  if (!reply && !(not_found (local) && !value)) {
    g_propagate_error (error, g_steal_pointer (&local));
    return FALSE;
  }
  return TRUE;
}

static char **
dbus_ids (VsStore *store, const char *table, GError **error)
{
  DbusStore *self = (DbusStore *) store;
  g_autoptr (GError) local = NULL;
  g_autoptr (GVariant) reply = g_dbus_connection_call_sync (self->bus, PS_NAME, PS_PATH, PS_IFACE, "List",
                                                            g_variant_new ("(s)", table), G_VARIANT_TYPE ("(as)"),
                                                            G_DBUS_CALL_FLAGS_NONE, 5000, NULL, &local);
  if (!reply) {
    if (!not_found (local))
      g_propagate_error (error, g_steal_pointer (&local));
    return NULL;
  }
  char **ids = NULL;
  g_variant_get (reply, "(^as)", &ids);
  return ids;
}

static void
dbus_free (VsStore *store)
{
  DbusStore *self = (DbusStore *) store;
  g_clear_object (&self->bus);
  g_free (self);
}

VsStore *
vs_store_new_dbus (GDBusConnection *bus)
{
  DbusStore *self = g_new0 (DbusStore, 1);
  self->base.get = dbus_get;
  self->base.set = dbus_set;
  self->base.ids = dbus_ids;
  self->base.free = dbus_free;
  self->bus = g_object_ref (bus);
  return &self->base;
}

/* ---- in memory ---- */

typedef struct {
  VsStore     base;
  GHashTable *values;   /* "table\nid\napp" -> GStrv */
} MemoryStore;

static char *
memory_key (const char *table, const char *id, const char *app)
{
  return g_strjoin ("\n", table, id, app, NULL);
}

static char **
memory_get (VsStore *store, const char *table, const char *id, const char *app, GError **error)
{
  MemoryStore *self = (MemoryStore *) store;
  g_autofree char *key = memory_key (table, id, app);
  return g_strdupv (g_hash_table_lookup (self->values, key));
}

static gboolean
memory_set (VsStore *store, const char *table, const char *id, const char *app, const char *const *value,
            GError **error)
{
  MemoryStore *self = (MemoryStore *) store;
  char *key = memory_key (table, id, app);
  if (value)
    g_hash_table_replace (self->values, key, g_strdupv ((char **) value));
  else {
    g_hash_table_remove (self->values, key);
    g_free (key);
  }
  return TRUE;
}

static char **
memory_ids (VsStore *store, const char *table, GError **error)
{
  MemoryStore *self = (MemoryStore *) store;
  g_autofree char *prefix = g_strconcat (table, "\n", NULL);
  g_autoptr (GPtrArray) ids = g_ptr_array_new_with_free_func (g_free);
  GHashTableIter it;
  gpointer key;
  g_hash_table_iter_init (&it, self->values);
  while (g_hash_table_iter_next (&it, &key, NULL)) {
    if (!g_str_has_prefix (key, prefix))
      continue;
    const char *id = (const char *) key + strlen (prefix);
    g_autofree char *only = g_strndup (id, strcspn (id, "\n"));
    gboolean seen = FALSE;
    for (guint i = 0; i < ids->len && !seen; i++)
      seen = !strcmp (ids->pdata[i], only);
    if (!seen)
      g_ptr_array_add (ids, g_steal_pointer (&only));
  }
  if (!ids->len)
    return NULL;
  g_ptr_array_add (ids, NULL);
  return (char **) g_ptr_array_free (g_steal_pointer (&ids), FALSE);
}

static void
memory_free (VsStore *store)
{
  MemoryStore *self = (MemoryStore *) store;
  g_hash_table_unref (self->values);
  g_free (self);
}

VsStore *
vs_store_new_memory (void)
{
  MemoryStore *self = g_new0 (MemoryStore, 1);
  self->base.get = memory_get;
  self->base.set = memory_set;
  self->base.ids = memory_ids;
  self->base.free = memory_free;
  self->values = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, (GDestroyNotify) g_strfreev);
  return &self->base;
}
