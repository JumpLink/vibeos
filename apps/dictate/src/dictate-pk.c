/* dictate-pk: see dictate-pk.h. App Installer's transaction (inst-pk.c), without its demo. */
#include "dictate-pk.h"

#include <string.h>

#define PK_NAME         "org.freedesktop.PackageKit"
#define PK_PATH         "/org/freedesktop/PackageKit"
#define PK_IFACE        "org.freedesktop.PackageKit"
#define PK_TX_IFACE     "org.freedesktop.PackageKit.Transaction"

struct _DictatePkTx {
  DictatePkFunc    changed, finished;
  gpointer         data;
  GCancellable    *cancellable;
  GDBusConnection *bus;
  char            *path;
  guint            sub_signals, sub_props;
  char            *method;
  GVariant        *params;
  gboolean         method_called, cancel_pending;

  gboolean         done;
  guint            exit, error_code, status, percentage;
  char            *error_details;
  guint64          download_size;
  GPtrArray       *packages;
};

static DictatePkPackage *
package_new (guint info, const char *package_id)
{
  g_auto (GStrv) parts = g_strsplit (package_id ? package_id : "", ";", 4);
  DictatePkPackage *p;

  if (g_strv_length (parts) != 4 || !*parts[0])
    return NULL;
  p = g_new0 (DictatePkPackage, 1);
  p->info = info;
  p->id = g_strdup (package_id);
  p->name = g_strdup (parts[0]);
  p->version = g_strdup (parts[1]);
  p->arch = g_strdup (parts[2]);
  p->data = g_strdup (parts[3]);
  return p;
}

void
dictate_pk_package_free (DictatePkPackage *p)
{
  if (!p)
    return;
  g_free (p->id);
  g_free (p->name);
  g_free (p->version);
  g_free (p->arch);
  g_free (p->data);
  g_free (p);
}

DictatePkTx *
dictate_pk_tx_new (DictatePkFunc changed, DictatePkFunc finished, gpointer data)
{
  DictatePkTx *tx = g_new0 (DictatePkTx, 1);

  tx->changed = changed;
  tx->finished = finished;
  tx->data = data;
  tx->cancellable = g_cancellable_new ();
  tx->percentage = 101;
  tx->packages = g_ptr_array_new_with_free_func ((GDestroyNotify) dictate_pk_package_free);
  return tx;
}

static void
unsubscribe (DictatePkTx *tx)
{
  if (tx->bus && tx->sub_signals)
    g_dbus_connection_signal_unsubscribe (tx->bus, tx->sub_signals);
  if (tx->bus && tx->sub_props)
    g_dbus_connection_signal_unsubscribe (tx->bus, tx->sub_props);
  tx->sub_signals = tx->sub_props = 0;
}

void
dictate_pk_tx_free (DictatePkTx *tx)
{
  if (!tx)
    return;
  g_cancellable_cancel (tx->cancellable);
  unsubscribe (tx);
  g_clear_object (&tx->cancellable);
  g_clear_object (&tx->bus);
  g_clear_pointer (&tx->params, g_variant_unref);
  g_free (tx->path);
  g_free (tx->method);
  g_free (tx->error_details);
  g_ptr_array_unref (tx->packages);
  g_free (tx);
}

static void
emit_changed (DictatePkTx *tx)
{
  if (tx->changed && !tx->done)
    tx->changed (tx, tx->data);
}

static void
set_error (DictatePkTx *tx, guint code, const char *details)
{
  /* the first error is the cause; PackageKit may add more as it gives up */
  if (tx->error_code)
    return;
  tx->error_code = code;
  g_free (tx->error_details);
  tx->error_details = g_strdup (details);
}

static void
finish (DictatePkTx *tx, guint exit)
{
  if (tx->done)
    return;
  tx->exit = exit;
  if (exit == DICTATE_PK_EXIT_FAILED && !tx->error_code)
    set_error (tx, DICTATE_PK_ERROR_REFUSED, NULL);
  tx->status = DICTATE_PK_STATUS_FINISHED;
  unsubscribe (tx);
  tx->done = TRUE;
  if (tx->finished)
    tx->finished (tx, tx->data);
}

/* a D-Bus failure: PackageKit is not there, or refused the call */
static void
fail_dbus (DictatePkTx *tx, GError *error)
{
  g_autofree char *remote = g_dbus_error_get_remote_error (error);

  g_dbus_error_strip_remote_error (error);
  if (remote && g_str_has_prefix (remote, "org.freedesktop.PackageKit"))
    set_error (tx, DICTATE_PK_ERROR_REFUSED, error->message);
  else
    set_error (tx, DICTATE_PK_ERROR_NO_PACKAGEKIT, error->message);
  finish (tx, DICTATE_PK_EXIT_FAILED);
}

/* ---- the signals and properties ---- */

static void
add_package (DictatePkTx *tx, guint info, const char *id)
{
  DictatePkPackage *p = package_new (info, id);

  if (p)
    g_ptr_array_add (tx->packages, p);
}

static void
on_signal (GDBusConnection *bus, const char *sender, const char *path, const char *iface, const char *member,
           GVariant *params, gpointer data)
{
  DictatePkTx *tx = data;

  if (!g_strcmp0 (member, "Package") && g_variant_is_of_type (params, G_VARIANT_TYPE ("(uss)")))
    {
      guint info;
      const char *id;

      g_variant_get (params, "(u&s&s)", &info, &id, NULL);
      add_package (tx, info, id);
      emit_changed (tx);
    }
  else if (!g_strcmp0 (member, "Packages") && g_variant_is_of_type (params, G_VARIANT_TYPE ("(a(uss))")))
    {
      g_autoptr (GVariantIter) iter = NULL;
      guint info;
      const char *id;

      g_variant_get (params, "(a(uss))", &iter);
      while (g_variant_iter_next (iter, "(u&s&s)", &info, &id, NULL))
        add_package (tx, info, id);
      emit_changed (tx);
    }
  else if (!g_strcmp0 (member, "Details") && g_variant_is_of_type (params, G_VARIANT_TYPE ("(a{sv})")))
    {
      g_autoptr (GVariant) dict = g_variant_get_child_value (params, 0);
      guint64 size;

      if (g_variant_lookup (dict, "download-size", "t", &size))
        tx->download_size = size;
    }
  else if (!g_strcmp0 (member, "ErrorCode") && g_variant_is_of_type (params, G_VARIANT_TYPE ("(us)")))
    {
      guint code;
      const char *details;

      g_variant_get (params, "(u&s)", &code, &details);
      set_error (tx, code, details);
    }
  else if (!g_strcmp0 (member, "ItemProgress") && g_variant_is_of_type (params, G_VARIANT_TYPE ("(suu)")))
    emit_changed (tx);
  else if (!g_strcmp0 (member, "Finished") && g_variant_is_of_type (params, G_VARIANT_TYPE ("(uu)")))
    {
      guint exit;

      g_variant_get (params, "(uu)", &exit, NULL);
      finish (tx, exit);
    }
  else if (!g_strcmp0 (member, "Destroy"))
    finish (tx, DICTATE_PK_EXIT_FAILED);
}

static void
on_properties (GDBusConnection *bus, const char *sender, const char *path, const char *iface, const char *member,
               GVariant *params, gpointer data)
{
  DictatePkTx *tx = data;
  g_autoptr (GVariantIter) iter = NULL;
  const char *name;
  GVariant *value;

  if (!g_variant_is_of_type (params, G_VARIANT_TYPE ("(sa{sv}as)")))
    return;
  g_variant_get (params, "(&sa{sv}as)", NULL, &iter, NULL);
  while (g_variant_iter_next (iter, "{&sv}", &name, &value))
    {
      if (!g_strcmp0 (name, "Status") && g_variant_is_of_type (value, G_VARIANT_TYPE_UINT32))
        tx->status = g_variant_get_uint32 (value);
      else if (!g_strcmp0 (name, "Percentage") && g_variant_is_of_type (value, G_VARIANT_TYPE_UINT32))
        tx->percentage = g_variant_get_uint32 (value);
      g_variant_unref (value);
    }
  emit_changed (tx);
}

/* ---- the calls: a transaction, its hints, the method ---- */

static void
on_cancel_reply (GObject *source, GAsyncResult *result, gpointer data)
{
  g_autoptr (GVariant) reply = g_dbus_connection_call_finish (G_DBUS_CONNECTION (source), result, NULL);
}

static void
call_cancel (DictatePkTx *tx)
{
  /* not tied to the transaction's cancellable: freeing the transaction does not take the request back */
  g_dbus_connection_call (tx->bus, PK_NAME, tx->path, PK_TX_IFACE, "Cancel", NULL, NULL, G_DBUS_CALL_FLAGS_NONE, -1,
                          NULL, on_cancel_reply, NULL);
}

static void
on_method (GObject *source, GAsyncResult *result, gpointer data)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (GVariant) reply = g_dbus_connection_call_finish (G_DBUS_CONNECTION (source), result, &error);
  DictatePkTx *tx = data;

  if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    return;
  if (!reply)
    {
      fail_dbus (tx, error);
      return;
    }
  if (tx->cancel_pending)
    call_cancel (tx);
}

static void
on_hints (GObject *source, GAsyncResult *result, gpointer data)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (GVariant) reply = g_dbus_connection_call_finish (G_DBUS_CONNECTION (source), result, &error);
  DictatePkTx *tx = data;

  if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    return;
  /* hints PackageKit does not take are not a reason to stop */
  tx->method_called = TRUE;
  g_dbus_connection_call (tx->bus, PK_NAME, tx->path, PK_TX_IFACE, tx->method, g_steal_pointer (&tx->params), NULL,
                          G_DBUS_CALL_FLAGS_NONE, -1, tx->cancellable, on_method, tx);
}

static void
on_created (GObject *source, GAsyncResult *result, gpointer data)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (GVariant) reply = g_dbus_connection_call_finish (G_DBUS_CONNECTION (source), result, &error);
  DictatePkTx *tx = data;
  GVariantBuilder hints;
  const char *locale = g_getenv ("LC_ALL");

  if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    return;
  if (!reply)
    {
      fail_dbus (tx, error);
      return;
    }
  g_variant_get (reply, "(o)", &tx->path);
  /* signals first, so that none is missed */
  tx->sub_signals = g_dbus_connection_signal_subscribe (tx->bus, PK_NAME, PK_TX_IFACE, NULL, tx->path, NULL,
                                                        G_DBUS_SIGNAL_FLAGS_NONE, on_signal, tx, NULL);
  tx->sub_props = g_dbus_connection_signal_subscribe (tx->bus, PK_NAME, "org.freedesktop.DBus.Properties",
                                                      "PropertiesChanged", tx->path, PK_TX_IFACE,
                                                      G_DBUS_SIGNAL_FLAGS_NONE, on_properties, tx, NULL);
  if (!locale || !*locale)
    locale = g_getenv ("LC_MESSAGES");
  if (!locale || !*locale)
    locale = g_getenv ("LANG");
  g_variant_builder_init (&hints, G_VARIANT_TYPE ("as"));
  /* interactive: polkit may ask for the password (through the shell's agent); without it the call is refused */
  g_variant_builder_add (&hints, "s", "interactive=true");
  g_variant_builder_add (&hints, "s", "background=false");
  if (locale && *locale)
    {
      g_autofree char *hint = g_strdup_printf ("locale=%s", locale);

      g_variant_builder_add (&hints, "s", hint);
    }
  g_dbus_connection_call (tx->bus, PK_NAME, tx->path, PK_TX_IFACE, "SetHints", g_variant_new ("(as)", &hints), NULL,
                          G_DBUS_CALL_FLAGS_NONE, -1, tx->cancellable, on_hints, tx);
}

static void
on_bus (GObject *source, GAsyncResult *result, gpointer data)
{
  g_autoptr (GError) error = NULL;
  GDBusConnection *bus = g_bus_get_finish (result, &error);
  DictatePkTx *tx = data;

  if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    return;
  if (!bus)
    {
      fail_dbus (tx, error);
      return;
    }
  tx->bus = bus;
  g_dbus_connection_call (bus, PK_NAME, PK_PATH, PK_IFACE, "CreateTransaction", NULL, G_VARIANT_TYPE ("(o)"),
                          G_DBUS_CALL_FLAGS_NONE, -1, tx->cancellable, on_created, tx);
}

static void
start (DictatePkTx *tx, const char *method, GVariant *params)
{
  g_return_if_fail (!tx->method);
  tx->method = g_strdup (method);
  tx->params = g_variant_ref_sink (params);
  g_bus_get (G_BUS_TYPE_SYSTEM, tx->cancellable, on_bus, tx);
}

static GVariant *
one (const char *value)
{
  const char *values[] = { value, NULL };

  return g_variant_new_strv (values, -1);
}

void
dictate_pk_tx_resolve (DictatePkTx *tx, guint64 filters, const char *name)
{
  start (tx, "Resolve", g_variant_new ("(t@as)", filters, one (name)));
}

void
dictate_pk_tx_get_details (DictatePkTx *tx, const char *package_id)
{
  start (tx, "GetDetails", g_variant_new ("(@as)", one (package_id)));
}

void
dictate_pk_tx_refresh_cache (DictatePkTx *tx)
{
  start (tx, "RefreshCache", g_variant_new ("(b)", FALSE));
}

void
dictate_pk_tx_install_packages (DictatePkTx *tx, guint64 flags, const char *package_id)
{
  start (tx, "InstallPackages", g_variant_new ("(t@as)", flags, one (package_id)));
}

void
dictate_pk_tx_cancel (DictatePkTx *tx)
{
  if (tx->done)
    return;
  /* before the method is called there is nothing to cancel in PackageKit yet: it is cancelled once it is */
  if (!tx->method_called || !tx->path)
    {
      tx->cancel_pending = TRUE;
      return;
    }
  call_cancel (tx);
}

gboolean    dictate_pk_tx_is_finished (DictatePkTx *tx) { return tx->done; }
guint       dictate_pk_tx_get_exit (DictatePkTx *tx) { return tx->exit; }
guint       dictate_pk_tx_get_error_code (DictatePkTx *tx) { return tx->error_code; }
const char *dictate_pk_tx_get_error_details (DictatePkTx *tx) { return tx->error_details; }
guint       dictate_pk_tx_get_status (DictatePkTx *tx) { return tx->status; }
guint       dictate_pk_tx_get_percentage (DictatePkTx *tx) { return tx->percentage; }
GPtrArray  *dictate_pk_tx_get_packages (DictatePkTx *tx) { return tx->packages; }
guint64     dictate_pk_tx_get_download_size (DictatePkTx *tx) { return tx->download_size; }
