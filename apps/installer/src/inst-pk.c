/* One PackageKit transaction over GDBus (inst-pk.h), or the demo's script for it */
#include "inst-pk.h"
#include "inst-deb.h"
#include "inst-demo.h"
#include "inst-dpkg.h"
#include "inst-version.h"

#include <glib/gi18n.h>
#include <string.h>

#define PK_NAME         "org.freedesktop.PackageKit"
#define PK_PATH         "/org/freedesktop/PackageKit"
#define PK_IFACE        "org.freedesktop.PackageKit"
#define PK_TX_IFACE     "org.freedesktop.PackageKit.Transaction"

struct _InstPkTx {
  InstPkFunc       changed, finished;
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
  gboolean         allow_cancel;
  InstPkPackage   *item;
  guint64          download_size;
  GPtrArray       *packages, *details;

  /* the demo's script */
  guint64          demo_flags;
  int              demo_step;
  guint            demo_source;
  char            *demo_target;
};

InstPkPackage *
inst_pk_package_new (guint info, const char *package_id, const char *summary)
{
  g_auto (GStrv) parts = g_strsplit (package_id ? package_id : "", ";", 4);
  InstPkPackage *p;

  if (g_strv_length (parts) != 4 || !*parts[0])
    return NULL;
  p = g_new0 (InstPkPackage, 1);
  p->info = info;
  p->id = g_strdup (package_id);
  p->name = g_strdup (parts[0]);
  p->version = g_strdup (parts[1]);
  p->arch = g_strdup (parts[2]);
  p->data = g_strdup (parts[3]);
  p->summary = g_strdup (summary);
  return p;
}

void
inst_pk_package_free (InstPkPackage *p)
{
  if (!p)
    return;
  g_free (p->id);
  g_free (p->name);
  g_free (p->version);
  g_free (p->arch);
  g_free (p->data);
  g_free (p->summary);
  g_free (p);
}

InstPkTx *
inst_pk_tx_new (InstPkFunc changed, InstPkFunc finished, gpointer data)
{
  InstPkTx *tx = g_new0 (InstPkTx, 1);

  tx->changed = changed;
  tx->finished = finished;
  tx->data = data;
  tx->cancellable = g_cancellable_new ();
  tx->percentage = 101;
  tx->packages = g_ptr_array_new_with_free_func ((GDestroyNotify) inst_pk_package_free);
  tx->details = g_ptr_array_new_with_free_func ((GDestroyNotify) g_variant_unref);
  return tx;
}

static void
unsubscribe (InstPkTx *tx)
{
  if (tx->bus && tx->sub_signals)
    g_dbus_connection_signal_unsubscribe (tx->bus, tx->sub_signals);
  if (tx->bus && tx->sub_props)
    g_dbus_connection_signal_unsubscribe (tx->bus, tx->sub_props);
  tx->sub_signals = tx->sub_props = 0;
}

void
inst_pk_tx_free (InstPkTx *tx)
{
  if (!tx)
    return;
  g_cancellable_cancel (tx->cancellable);
  unsubscribe (tx);
  g_clear_handle_id (&tx->demo_source, g_source_remove);
  g_clear_object (&tx->cancellable);
  g_clear_object (&tx->bus);
  g_clear_pointer (&tx->params, g_variant_unref);
  g_clear_pointer (&tx->item, inst_pk_package_free);
  g_ptr_array_unref (tx->packages);
  g_ptr_array_unref (tx->details);
  g_free (tx->path);
  g_free (tx->method);
  g_free (tx->error_details);
  g_free (tx->demo_target);
  g_free (tx);
}

static void
emit_changed (InstPkTx *tx)
{
  if (tx->changed && !tx->done)
    tx->changed (tx, tx->data);
}

static void
set_error (InstPkTx *tx, guint code, const char *details)
{
  /* the first error is the cause; PackageKit may add more as it gives up */
  if (tx->error_code)
    return;
  tx->error_code = code;
  g_free (tx->error_details);
  tx->error_details = g_strdup (details);
}

static void
finish (InstPkTx *tx, guint exit)
{
  if (tx->done)
    return;
  tx->exit = exit;
  if (exit == INST_PK_EXIT_FAILED && !tx->error_code)
    set_error (tx, INST_PK_ERROR_INTERNAL_ERROR, NULL);
  tx->status = INST_PK_STATUS_FINISHED;
  unsubscribe (tx);
  g_clear_handle_id (&tx->demo_source, g_source_remove);
  tx->done = TRUE;
  if (tx->finished)
    tx->finished (tx, tx->data);
}

/* a D-Bus failure: PackageKit is not there, or refused the call */
static void
fail_dbus (InstPkTx *tx, GError *error)
{
  g_autofree char *remote = g_dbus_error_get_remote_error (error);

  g_dbus_error_strip_remote_error (error);
  if (remote && g_str_has_prefix (remote, "org.freedesktop.PackageKit"))
    set_error (tx, INST_PK_ERROR_REFUSED, error->message);
  else
    set_error (tx, INST_PK_ERROR_NO_PACKAGEKIT, error->message);
  finish (tx, INST_PK_EXIT_FAILED);
}

/* ---- the signals and properties ------------------------------------------------------------------------------- */

static void
add_package (InstPkTx *tx, guint info, const char *id, const char *summary)
{
  InstPkPackage *p = inst_pk_package_new (info, id, summary);

  if (p)
    g_ptr_array_add (tx->packages, p);
}

static void
on_signal (GDBusConnection *bus, const char *sender, const char *path, const char *iface, const char *member,
           GVariant *params, gpointer data)
{
  InstPkTx *tx = data;

  if (!g_strcmp0 (member, "Package") && g_variant_is_of_type (params, G_VARIANT_TYPE ("(uss)")))
    {
      guint info;
      const char *id, *summary;

      g_variant_get (params, "(u&s&s)", &info, &id, &summary);
      add_package (tx, info, id, summary);
      emit_changed (tx);
    }
  else if (!g_strcmp0 (member, "Packages") && g_variant_is_of_type (params, G_VARIANT_TYPE ("(a(uss))")))
    {
      g_autoptr (GVariantIter) iter = NULL;
      guint info;
      const char *id, *summary;

      g_variant_get (params, "(a(uss))", &iter);
      while (g_variant_iter_next (iter, "(u&s&s)", &info, &id, &summary))
        add_package (tx, info, id, summary);
      emit_changed (tx);
    }
  else if (!g_strcmp0 (member, "Details") && g_variant_is_of_type (params, G_VARIANT_TYPE ("(a{sv})")))
    g_ptr_array_add (tx->details, g_variant_get_child_value (params, 0));
  else if (!g_strcmp0 (member, "ErrorCode") && g_variant_is_of_type (params, G_VARIANT_TYPE ("(us)")))
    {
      guint code;
      const char *details;

      g_variant_get (params, "(u&s)", &code, &details);
      set_error (tx, code, details);
    }
  else if (!g_strcmp0 (member, "ItemProgress") && g_variant_is_of_type (params, G_VARIANT_TYPE ("(suu)")))
    {
      const char *id;
      guint status, percentage;

      g_variant_get (params, "(&suu)", &id, &status, &percentage);
      g_clear_pointer (&tx->item, inst_pk_package_free);
      tx->item = inst_pk_package_new (status, id, NULL);
      emit_changed (tx);
    }
  else if (!g_strcmp0 (member, "Finished") && g_variant_is_of_type (params, G_VARIANT_TYPE ("(uu)")))
    {
      guint exit, runtime;

      g_variant_get (params, "(uu)", &exit, &runtime);
      finish (tx, exit);
    }
  else if (!g_strcmp0 (member, "Destroy"))
    finish (tx, INST_PK_EXIT_FAILED);
}

static void
on_properties (GDBusConnection *bus, const char *sender, const char *path, const char *iface, const char *member,
               GVariant *params, gpointer data)
{
  InstPkTx *tx = data;
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
      else if (!g_strcmp0 (name, "AllowCancel") && g_variant_is_of_type (value, G_VARIANT_TYPE_BOOLEAN))
        tx->allow_cancel = g_variant_get_boolean (value);
      else if (!g_strcmp0 (name, "DownloadSizeRemaining") && g_variant_is_of_type (value, G_VARIANT_TYPE_UINT64))
        tx->download_size = g_variant_get_uint64 (value);
      g_variant_unref (value);
    }
  emit_changed (tx);
}

/* ---- the calls: a transaction, its hints, the method ---------------------------------------------------------- */

static void
on_cancel_reply (GObject *source, GAsyncResult *result, gpointer data)
{
  g_autoptr (GVariant) reply = g_dbus_connection_call_finish (G_DBUS_CONNECTION (source), result, NULL);
}

static void
call_cancel (InstPkTx *tx)
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
  InstPkTx *tx = data;

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
  InstPkTx *tx = data;

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
  InstPkTx *tx = data;
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
  InstPkTx *tx = data;

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

static void demo_start (InstPkTx *tx);

static void
start (InstPkTx *tx, const char *method, GVariant *params)
{
  g_return_if_fail (!tx->method);
  tx->method = g_strdup (method);
  tx->params = g_variant_ref_sink (params);
  if (inst_demo_active ())
    demo_start (tx);
  else
    g_bus_get (G_BUS_TYPE_SYSTEM, tx->cancellable, on_bus, tx);
}

static GVariant *
strv (const char *const *values)
{
  return g_variant_new_strv (values, -1);
}

void
inst_pk_tx_resolve (InstPkTx *tx, guint64 filters, const char *const *names)
{
  start (tx, "Resolve", g_variant_new ("(t@as)", filters, strv (names)));
}

void
inst_pk_tx_get_details (InstPkTx *tx, const char *const *ids)
{
  start (tx, "GetDetails", g_variant_new ("(@as)", strv (ids)));
}

void
inst_pk_tx_refresh_cache (InstPkTx *tx, gboolean force)
{
  start (tx, "RefreshCache", g_variant_new ("(b)", force));
}

void
inst_pk_tx_install_files (InstPkTx *tx, guint64 flags, const char *path)
{
  const char *paths[] = { path, NULL };

  tx->demo_flags = flags;
  start (tx, "InstallFiles", g_variant_new ("(t@as)", flags, strv (paths)));
}

void
inst_pk_tx_install_packages (InstPkTx *tx, guint64 flags, const char *const *ids)
{
  tx->demo_flags = flags;
  start (tx, "InstallPackages", g_variant_new ("(t@as)", flags, strv (ids)));
}

void
inst_pk_tx_remove_packages (InstPkTx *tx, guint64 flags, const char *const *ids, gboolean allow_deps,
                            gboolean autoremove)
{
  tx->demo_flags = flags;
  start (tx, "RemovePackages", g_variant_new ("(t@asbb)", flags, strv (ids), allow_deps, autoremove));
}

static void demo_cancel (InstPkTx *tx);

void
inst_pk_tx_cancel (InstPkTx *tx)
{
  if (tx->done)
    return;
  if (inst_demo_active ())
    demo_cancel (tx);
  else if (tx->path && tx->method_called)
    call_cancel (tx);
  else
    tx->cancel_pending = TRUE;
}

gboolean inst_pk_tx_is_finished (InstPkTx *tx) { return tx->done; }
guint inst_pk_tx_get_exit (InstPkTx *tx) { return tx->exit; }
guint inst_pk_tx_get_error_code (InstPkTx *tx) { return tx->error_code; }
const char *inst_pk_tx_get_error_details (InstPkTx *tx) { return tx->error_details; }
guint inst_pk_tx_get_status (InstPkTx *tx) { return tx->status; }
guint inst_pk_tx_get_percentage (InstPkTx *tx) { return tx->percentage; }
gboolean inst_pk_tx_get_allow_cancel (InstPkTx *tx) { return tx->allow_cancel; }
const InstPkPackage *inst_pk_tx_get_item (InstPkTx *tx) { return tx->item; }
guint64 inst_pk_tx_get_download_size (InstPkTx *tx) { return tx->download_size; }
GPtrArray *inst_pk_tx_get_packages (InstPkTx *tx) { return tx->packages; }
GPtrArray *inst_pk_tx_get_details_list (InstPkTx *tx) { return tx->details; }

/* ---- the demo: each method's script --------------------------------------------------------------------------- */

static guint
demo_delay (void)
{
  return (guint) inst_demo_number ("delay", 600);
}

static char *
demo_id (const char *name, const char *version, const char *data)
{
  return g_strdup_printf ("%s;%s;%s;%s", name, version, inst_dpkg_native_arch (), data);
}

static const char *
demo_candidate (const char *name)
{
  const char *candidate = inst_demo_get_for ("candidate", name);

  return candidate ? candidate : "1.3-1";
}

/* the packages a check finds besides the target: deps=N to install, removes=A+B to remove */
static void
demo_extras (InstPkTx *tx)
{
  guint64 deps = inst_demo_number ("deps", 0);
  const char *removes = inst_demo_get ("removes");

  for (guint64 i = 0; i < deps; i++)
    {
      g_autofree char *name = g_strdup_printf ("libdemo%u", (guint) i + 1);
      g_autofree char *id = demo_id (name, "2.0-1", "debian-unstable-main");

      add_package (tx, INST_PK_INFO_INSTALLING, id, _("A library the app needs"));
    }
  if (removes)
    {
      g_auto (GStrv) names = g_strsplit (removes, "+", -1);

      for (guint i = 0; names[i]; i++)
        {
          g_autofree char *id = demo_id (names[i], "1.0-1", "installed:debian-unstable-main");

          add_package (tx, INST_PK_INFO_REMOVING, id, NULL);
        }
    }
  tx->download_size = inst_demo_number ("download", deps ? deps * 1400000 : 0);
}

/* the target of an install or a removal, and what happens to it */
static void
demo_target (InstPkTx *tx, const char *name, const char *version)
{
  InstDpkgInfo info;
  g_autofree char *id = NULL;
  guint pk_info = INST_PK_INFO_INSTALLING;

  inst_dpkg_query (name, NULL, &info, NULL);
  if (!strcmp (tx->method, "RemovePackages"))
    pk_info = INST_PK_INFO_REMOVING;
  else if (info.version)
    {
      int c = inst_version_compare (version, info.version);

      pk_info = c > 0 ? INST_PK_INFO_UPDATING : c < 0 ? INST_PK_INFO_DOWNGRADING : INST_PK_INFO_REINSTALLING;
      /* PackageKit's apt backend does nothing at all with the same version again */
      if (c == 0 && inst_demo_has ("noreinstall"))
        {
          inst_dpkg_info_clear (&info);
          return;
        }
    }
  inst_dpkg_info_clear (&info);
  id = demo_id (name, version, "local");
  add_package (tx, pk_info, id, NULL);
}

static void
demo_set (InstPkTx *tx, guint status, guint percentage, gboolean allow_cancel)
{
  tx->status = status;
  tx->percentage = percentage;
  tx->allow_cancel = allow_cancel;
  emit_changed (tx);
}

static void
demo_fail (InstPkTx *tx, guint code, const char *details)
{
  set_error (tx, code, details);
  finish (tx, code == INST_PK_ERROR_TRANSACTION_CANCELLED ? INST_PK_EXIT_CANCELLED : INST_PK_EXIT_FAILED);
}

static gboolean
demo_check (InstPkTx *tx)
{
  const char *missing = inst_demo_get ("missing");

  if (inst_demo_has ("hold") && !g_strcmp0 (inst_demo_get ("hold"), "checking"))
    return G_SOURCE_CONTINUE;
  if (missing)
    {
      g_autofree char *details = g_strdup_printf ("The following packages have unmet dependencies:\n"
                                                  "  %s: Depends: %s but it is not installable", tx->demo_target,
                                                  !strcmp (missing, "yes") ? "libdemo-runtime (>= 2.0)" : missing);

      demo_fail (tx, INST_PK_ERROR_DEP_RESOLUTION_FAILED, details);
      return G_SOURCE_REMOVE;
    }
  demo_extras (tx);
  finish (tx, INST_PK_EXIT_SUCCESS);
  return G_SOURCE_REMOVE;
}

/* a real install or removal: the password, the download, the install, each failing where fail= says */
static gboolean
demo_run (InstPkTx *tx)
{
  const char *fail = inst_demo_get ("fail"), *hold = inst_demo_get ("hold");
  gboolean removing = !strcmp (tx->method, "RemovePackages");
  int step = tx->demo_step++;

  switch (step)
    {
    case 0:
      if (!g_strcmp0 (fail, "nopk"))
        {
          demo_fail (tx, INST_PK_ERROR_NO_PACKAGEKIT, "The name org.freedesktop.PackageKit was not provided by any "
                                                      ".service files");
          return G_SOURCE_REMOVE;
        }
      demo_set (tx, INST_PK_STATUS_WAITING_FOR_AUTH, 101, FALSE);
      return G_SOURCE_CONTINUE;
    case 1:
      if (!g_strcmp0 (hold, "auth"))
        {
          tx->demo_step--;
          return G_SOURCE_CONTINUE;
        }
      if (!g_strcmp0 (fail, "auth"))
        {
          demo_fail (tx, INST_PK_ERROR_NOT_AUTHORIZED, "Failed to obtain authentication.");
          return G_SOURCE_REMOVE;
        }
      if (!g_strcmp0 (fail, "lock"))
        {
          demo_set (tx, INST_PK_STATUS_WAITING_FOR_LOCK, 101, TRUE);
          return G_SOURCE_CONTINUE;
        }
      demo_set (tx, INST_PK_STATUS_DOWNLOAD, removing ? 30 : 10, TRUE);
      return G_SOURCE_CONTINUE;
    case 2:
      if (!g_strcmp0 (fail, "lock"))
        {
          demo_fail (tx, INST_PK_ERROR_CANNOT_GET_LOCK, "E:Could not get lock /var/lib/dpkg/lock-frontend. It is "
                                                        "held by process 2034 (apt-get)");
          return G_SOURCE_REMOVE;
        }
      if (!removing)
        {
          g_autofree char *id = demo_id ("libdemo1", "2.0-1", "debian-unstable-main");

          g_clear_pointer (&tx->item, inst_pk_package_free);
          tx->item = inst_pk_package_new (INST_PK_INFO_DOWNLOADING, id, NULL);
        }
      demo_set (tx, removing ? INST_PK_STATUS_REMOVE : INST_PK_STATUS_DOWNLOAD, 45, TRUE);
      return G_SOURCE_CONTINUE;
    case 3:
      if (!g_strcmp0 (hold, "progress"))
        {
          tx->demo_step--;
          return G_SOURCE_CONTINUE;
        }
      if (!g_strcmp0 (fail, "network"))
        {
          demo_fail (tx, INST_PK_ERROR_PACKAGE_DOWNLOAD_FAILED, "Failed to fetch http://deb.debian.org/debian/pool/"
                                                                "main/libd/libdemo/libdemo1_2.0-1_arm64.deb  "
                                                                "Temporary failure resolving 'deb.debian.org'");
          return G_SOURCE_REMOVE;
        }
      if (!g_strcmp0 (fail, "cancelled"))
        {
          demo_fail (tx, INST_PK_ERROR_TRANSACTION_CANCELLED, "The task was stopped successfully");
          return G_SOURCE_REMOVE;
        }
      g_clear_pointer (&tx->item, inst_pk_package_free);
      demo_set (tx, removing ? INST_PK_STATUS_REMOVE : INST_PK_STATUS_INSTALL, 80, FALSE);
      return G_SOURCE_CONTINUE;
    case 4:
      if (!g_strcmp0 (hold, "installing"))
        {
          tx->demo_step--;
          return G_SOURCE_CONTINUE;
        }
      if (!g_strcmp0 (fail, "broken"))
        demo_fail (tx, INST_PK_ERROR_PACKAGE_CORRUPT, "dpkg-deb: error: archive has premature member 'control.tar.gz'"
                                                      " before 'debian-binary'");
      else if (!g_strcmp0 (fail, "setup"))
        demo_fail (tx, INST_PK_ERROR_PACKAGE_FAILED_TO_CONFIGURE, "dpkg: error processing package demo "
                                                                  "(--configure): installed demo package "
                                                                  "post-installation script subprocess returned "
                                                                  "error exit status 1");
      else if (!g_strcmp0 (fail, "space"))
        demo_fail (tx, INST_PK_ERROR_NO_SPACE_ON_DEVICE, "You don't have enough free space in /var/cache/apt/archives/");
      else if (!g_strcmp0 (fail, "conflict"))
        demo_fail (tx, INST_PK_ERROR_FILE_CONFLICTS, "trying to overwrite '/usr/bin/demo', which is also in package "
                                                     "otherdemo 1.0-1");
      else
        {
          demo_set (tx, INST_PK_STATUS_CLEANUP, 100, FALSE);
          finish (tx, INST_PK_EXIT_SUCCESS);
        }
      return G_SOURCE_REMOVE;
    }
  return G_SOURCE_REMOVE;
}

static gboolean
demo_resolve (InstPkTx *tx)
{
  g_autofree const char **names = NULL;
  guint64 filters;

  g_variant_get (tx->params, "(t^a&s)", &filters, &names);
  for (guint i = 0; names && names[i]; i++)
    {
      InstDpkgInfo info;

      if (inst_demo_get_for ("notfound", names[i]))
        continue;
      inst_dpkg_query (names[i], NULL, &info, NULL);
      if (info.version && !(filters & INST_PK_FILTER_NOT_INSTALLED))
        {
          g_autofree char *id = demo_id (names[i], info.version, "installed:debian-unstable-main");

          add_package (tx, INST_PK_INFO_INSTALLED, id, _("A sample app"));
        }
      if (!(filters & INST_PK_FILTER_INSTALLED) && g_strcmp0 (info.version, demo_candidate (names[i])))
        {
          g_autofree char *id = demo_id (names[i], demo_candidate (names[i]), "debian-unstable-main");

          add_package (tx, INST_PK_INFO_AVAILABLE, id, "A sample app");
        }
      inst_dpkg_info_clear (&info);
    }
  finish (tx, INST_PK_EXIT_SUCCESS);
  return G_SOURCE_REMOVE;
}

static gboolean
demo_details (InstPkTx *tx)
{
  g_autofree const char **ids = NULL;

  g_variant_get (tx->params, "(^a&s)", &ids);
  for (guint i = 0; ids && ids[i]; i++)
    {
      GVariantDict dict;

      g_variant_dict_init (&dict, NULL);
      g_variant_dict_insert (&dict, "package-id", "s", ids[i]);
      g_variant_dict_insert (&dict, "summary", "s", "A sample app for trying App Installer");
      g_variant_dict_insert (&dict, "description", "s",
                             "This package is not real: App Installer's demo shows it to try its screens.\n\n"
                             "Nothing is installed or removed while the demo runs.");
      g_variant_dict_insert (&dict, "url", "s", "https://vibeos.top");
      g_variant_dict_insert (&dict, "size", "t", inst_demo_number ("installed-size", 9 * 1024 * 1024));
      g_variant_dict_insert (&dict, "download-size", "t", inst_demo_number ("package-size", 2600000));
      g_ptr_array_add (tx->details, g_variant_ref_sink (g_variant_dict_end (&dict)));
    }
  finish (tx, INST_PK_EXIT_SUCCESS);
  return G_SOURCE_REMOVE;
}

static gboolean
demo_refresh (InstPkTx *tx)
{
  if (tx->demo_step++ == 0)
    {
      demo_set (tx, INST_PK_STATUS_REFRESH_CACHE, 50, TRUE);
      return G_SOURCE_CONTINUE;
    }
  finish (tx, INST_PK_EXIT_SUCCESS);
  return G_SOURCE_REMOVE;
}

static gboolean
demo_tick (gpointer data)
{
  InstPkTx *tx = data;
  gboolean simulate = (tx->demo_flags & INST_PK_FLAG_SIMULATE) != 0;
  gboolean more;

  if (!strcmp (tx->method, "Resolve"))
    more = demo_resolve (tx);
  else if (!strcmp (tx->method, "GetDetails"))
    more = demo_details (tx);
  else if (!strcmp (tx->method, "RefreshCache"))
    more = demo_refresh (tx);
  else if (simulate)
    more = demo_check (tx);
  else
    more = demo_run (tx);
  if (!more)
    tx->demo_source = 0;
  return more;
}

static void
demo_start (InstPkTx *tx)
{
  /* the target: the file's package, or the first package id's */
  if (!strcmp (tx->method, "InstallFiles"))
    {
      g_autofree const char **paths = NULL;
      g_autoptr (InstDeb) deb = NULL;
      guint64 flags;

      g_variant_get (tx->params, "(t^a&s)", &flags, &paths);
      deb = paths && paths[0] ? inst_deb_read_control (paths[0], NULL, NULL) : NULL;
      tx->demo_target = g_strdup (deb ? deb->package : "demo");
      if (flags & INST_PK_FLAG_SIMULATE)
        demo_target (tx, tx->demo_target, deb ? deb->version : "1.0-1");
    }
  else if (!strcmp (tx->method, "InstallPackages") || !strcmp (tx->method, "RemovePackages"))
    {
      g_autofree const char **ids = NULL;
      g_autoptr (GVariant) first = g_variant_get_child_value (tx->params, 1);
      InstPkPackage *p;

      ids = g_variant_get_strv (first, NULL);
      p = ids && ids[0] ? inst_pk_package_new (0, ids[0], NULL) : NULL;
      tx->demo_target = g_strdup (p ? p->name : "demo");
      if (p && (tx->demo_flags & INST_PK_FLAG_SIMULATE))
        demo_target (tx, p->name, p->version);
      inst_pk_package_free (p);
    }
  tx->method_called = TRUE;
  tx->demo_source = g_timeout_add (demo_delay (), demo_tick, tx);
}

static void
demo_cancel (InstPkTx *tx)
{
  if (!tx->method_called || tx->allow_cancel || (tx->demo_flags & INST_PK_FLAG_SIMULATE)
      || strcmp (tx->method, "InstallFiles") * strcmp (tx->method, "InstallPackages")
             * strcmp (tx->method, "RemovePackages") != 0)
    demo_fail (tx, INST_PK_ERROR_TRANSACTION_CANCELLED, "The task was stopped successfully");
}
