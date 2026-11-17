/* SwInstaller: see sw-installer.h */
#include "sw-installer.h"
#include "sw-dpkg.h"

#include <gio/gdesktopappinfo.h>
#include <string.h>
#ifdef GDK_WINDOWING_WAYLAND
#include <gdk/wayland/gdkwayland.h>
#endif

#define BUS_NAME  "vibeos.Installer"
#define OBJ_PATH  "/vibeos/Installer"
#define IFACE     "vibeos.Installer1"
#define RELOAD_MS 400

struct _SwInstaller {
  GObject          parent_instance;
  GDBusConnection *bus;
  guint            signal_sub;
  guint            name_watch;
  gboolean         service_seen;  /* App Installer has been on the bus since we asked */

  GHashTable      *installed;     /* package -> version */
  gboolean         loaded;
  gboolean         reading, read_again;
  GFileMonitor    *monitor;
  guint            reload_timeout;

  GPtrArray       *jobs;          /* SwJob, the running one first */
  GHashTable      *parents;       /* package -> GtkWindow (weak), for the sheet */
};

G_DEFINE_FINAL_TYPE (SwInstaller, sw_installer, G_TYPE_OBJECT)

enum { CHANGED, FINISHED, PACKAGE_CHANGED, N_SIGNALS };
static guint signals[N_SIGNALS];

static void pump (SwInstaller *self);

static void
job_free (gpointer data)
{
  SwJob *job = data;

  g_free (job->package);
  g_free (job->name);
  g_free (job->status);
  g_free (job);
}

static SwJob *
find_job (SwInstaller *self, const char *package)
{
  for (guint i = 0; i < self->jobs->len; i++)
    {
      SwJob *job = g_ptr_array_index (self->jobs, i);

      if (!g_strcmp0 (job->package, package))
        return job;
    }
  return NULL;
}

static SwJob *
find_job_id (SwInstaller *self, guint id)
{
  for (guint i = 0; i < self->jobs->len; i++)
    {
      SwJob *job = g_ptr_array_index (self->jobs, i);

      if (job->id == id && id)
        return job;
    }
  return NULL;
}

/* ---- what is installed ----------------------------------------------------------------------------------------- */

static void read_status (SwInstaller *self);

static void
on_status_read (GObject *source, GAsyncResult *res, gpointer data)
{
  SwInstaller *self = data;
  g_autoptr (GError) error = NULL;
  g_autoptr (GHashTable) now = sw_dpkg_read_finish (res, &error);
  g_autoptr (GHashTable) before = NULL;
  GHashTableIter iter;
  gpointer key, value;
  gboolean first = !self->loaded;

  self->reading = FALSE;
  if (!now)
    {
      g_warning ("could not read %s: %s", sw_dpkg_status_path (), error->message);
      now = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
    }
  before = g_steal_pointer (&self->installed);
  self->installed = g_steal_pointer (&now);
  self->loaded = TRUE;

  /* what changed since the last reading (nothing to compare with the first time) */
  if (!first)
    {
      g_hash_table_iter_init (&iter, self->installed);
      while (g_hash_table_iter_next (&iter, &key, &value))
        {
          const char *old = g_hash_table_lookup (before, key);

          if (g_strcmp0 (old, value))
            g_signal_emit (self, signals[PACKAGE_CHANGED], 0, key, old, value);
        }
      g_hash_table_iter_init (&iter, before);
      while (g_hash_table_iter_next (&iter, &key, &value))
        if (!g_hash_table_contains (self->installed, key))
          g_signal_emit (self, signals[PACKAGE_CHANGED], 0, key, value, NULL);
    }
  g_signal_emit (self, signals[CHANGED], 0, NULL);
  if (self->read_again)
    {
      self->read_again = FALSE;
      read_status (self);
    }
  g_object_unref (self);
}

static void
read_status (SwInstaller *self)
{
  if (self->reading)
    {
      self->read_again = TRUE;
      return;
    }
  self->reading = TRUE;
  sw_dpkg_read_async (sw_dpkg_status_path (), NULL, on_status_read, g_object_ref (self));
}

static gboolean
on_reload_timeout (gpointer data)
{
  SwInstaller *self = data;

  self->reload_timeout = 0;
  read_status (self);
  return G_SOURCE_REMOVE;
}

static void
on_status_file_changed (GFileMonitor *monitor, GFile *file, GFile *other, GFileMonitorEvent event,
                        SwInstaller *self)
{
  /* dpkg writes the file many times in one run: read it once things have settled */
  g_clear_handle_id (&self->reload_timeout, g_source_remove);
  self->reload_timeout = g_timeout_add (RELOAD_MS, on_reload_timeout, self);
}

void
sw_installer_reload (SwInstaller *self)
{
  read_status (self);
}

gboolean
sw_installer_is_loaded (SwInstaller *self)
{
  return self->loaded;
}

const char *
sw_installer_get_installed (SwInstaller *self, const char *package)
{
  return package ? g_hash_table_lookup (self->installed, package) : NULL;
}

GHashTable *
sw_installer_get_installed_all (SwInstaller *self)
{
  return self->installed;
}

/* ---- jobs ------------------------------------------------------------------------------------------------------ */

const SwJob *
sw_installer_get_job (SwInstaller *self, const char *package)
{
  return find_job (self, package);
}

GPtrArray *
sw_installer_get_jobs (SwInstaller *self)
{
  return self->jobs;
}

gboolean
sw_job_can_cancel (const SwJob *job)
{
  static const char *const too_late[] = { "installing", "removing", "finishing", "stopping", NULL };

  if (!job || job->cancelling)
    return FALSE;
  return job->state != SW_JOB_RUNNING || !job->status || !g_strv_contains (too_late, job->status);
}

char *
sw_job_describe (const SwJob *job)
{
  const char *s = job->status;

  if (job->cancelling)
    return g_strdup ("Stopping…");
  if (job->state == SW_JOB_QUEUED)
    return g_strdup ("Queued");
  if (job->state == SW_JOB_STARTING || !s)
    return g_strdup ("Starting…");
  if (!strcmp (s, "downloading"))
    return job->percent >= 0 ? g_strdup_printf ("Downloading %d%%", job->percent) : g_strdup ("Downloading…");
  if (!strcmp (s, "installing"))
    return job->percent >= 0 ? g_strdup_printf ("Installing %d%%", job->percent) : g_strdup ("Installing…");
  if (!strcmp (s, "removing"))
    return g_strdup ("Uninstalling…");
  if (!strcmp (s, "waiting-for-auth"))
    return g_strdup ("Waiting for your password");
  if (!strcmp (s, "waiting-for-lock"))
    return g_strdup ("Waiting for another install to finish");
  if (!strcmp (s, "asking"))
    return g_strdup ("Waiting for your answer");
  if (!strcmp (s, "refreshing"))
    return g_strdup ("Refreshing the software sources…");
  if (!strcmp (s, "reading") || !strcmp (s, "checking"))
    return g_strdup ("Checking…");
  if (!strcmp (s, "finishing"))
    return g_strdup ("Finishing…");
  if (!strcmp (s, "stopping"))
    return g_strdup ("Stopping…");
  return g_strdup ("Starting…");
}

static void
end_job (SwInstaller *self, SwJob *job, const char *result, const char *message)
{
  g_autofree char *package = g_strdup (job->package);
  g_autofree char *name = g_strdup (job->name);
  SwJobKind kind = job->kind;

  g_hash_table_remove (self->parents, package);
  g_ptr_array_remove (self->jobs, job);
  g_signal_emit (self, signals[CHANGED], 0, package);
  g_signal_emit (self, signals[FINISHED], 0, package, name, kind, result, message ? message : "");
  /* what it changed shows in dpkg's list (its monitor may be slow, or missing in a test) */
  read_status (self);
  pump (self);
}

static void
on_cancel_reply (GObject *source, GAsyncResult *res, gpointer data)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (GVariant) reply = g_dbus_connection_call_finish (G_DBUS_CONNECTION (source), res, &error);

  if (!reply)
    g_debug ("Cancel: %s", error->message);
}

static void
send_cancel (SwInstaller *self, SwJob *job)
{
  g_dbus_connection_call (self->bus, BUS_NAME, OBJ_PATH, IFACE, "Cancel", g_variant_new ("(u)", job->id), NULL,
                          G_DBUS_CALL_FLAGS_NONE, -1, NULL, on_cancel_reply, NULL);
}

/* what to tell the user when App Installer cannot be asked at all */
static char *
call_error_message (GError *error)
{
  g_autofree char *remote = g_dbus_error_get_remote_error (error);

  if (remote && (!strcmp (remote, "org.freedesktop.DBus.Error.ServiceUnknown")
                 || !strcmp (remote, "org.freedesktop.DBus.Error.UnknownObject")
                 || !strcmp (remote, "org.freedesktop.DBus.Error.UnknownInterface")))
    return g_strdup ("App Installer is not installed. Software installs and uninstalls apps through it.");
  g_dbus_error_strip_remote_error (error);
  return g_strdup_printf ("App Installer could not start: %s", error->message);
}

typedef struct {
  SwInstaller *self;
  char        *package;
} Call;

static void
on_job_reply (GObject *source, GAsyncResult *res, gpointer data)
{
  Call *call = data;
  SwInstaller *self = call->self;
  g_autoptr (GError) error = NULL;
  g_autoptr (GVariant) reply = g_dbus_connection_call_finish (G_DBUS_CONNECTION (source), res, &error);
  SwJob *job = find_job (self, call->package);

  if (job && job->state == SW_JOB_STARTING)
    {
      if (!reply)
        {
          g_autofree char *message = call_error_message (error);

          end_job (self, job, "failed", message);
        }
      else
        {
          g_variant_get (reply, "(u)", &job->id);
          job->state = SW_JOB_RUNNING;
          if (job->cancelling)
            send_cancel (self, job);
          g_signal_emit (self, signals[CHANGED], 0, job->package);
        }
    }
  g_object_unref (call->self);
  g_free (call->package);
  g_free (call);
}

static void
call_job (SwInstaller *self, SwJob *job, const char *handle)
{
  GVariantBuilder options;
  Call *call = g_new0 (Call, 1);

  g_variant_builder_init (&options, G_VARIANT_TYPE_VARDICT);
  if (handle)
    {
      g_autofree char *parent = g_strconcat ("wayland:", handle, NULL);

      g_variant_builder_add (&options, "{sv}", "parent-window", g_variant_new_string (parent));
    }
  call->self = g_object_ref (self);
  call->package = g_strdup (job->package);
  /* App Installer is D-Bus activatable: the call starts it */
  g_dbus_connection_call (self->bus, BUS_NAME, OBJ_PATH, IFACE,
                          job->kind == SW_JOB_REMOVE ? "RemovePackage" : "InstallPackage",
                          g_variant_new ("(sa{sv})", job->package, &options), G_VARIANT_TYPE ("(u)"),
                          G_DBUS_CALL_FLAGS_NONE, 60000, NULL, on_job_reply, call);
}

#ifdef GDK_WINDOWING_WAYLAND
static void
on_exported (GdkToplevel *toplevel, const char *handle, gpointer data)
{
  Call *call = data;
  SwJob *job = find_job (call->self, call->package);

  /* kept by GTK until the window goes: every later job reuses it */
  g_object_set_data_full (G_OBJECT (toplevel), "sw-export-handle", g_strdup (handle), g_free);
  if (job && job->state == SW_JOB_STARTING)
    call_job (call->self, job, handle);
  g_object_unref (call->self);
  g_free (call->package);
  g_free (call);
}
#endif

static void
start_job (SwInstaller *self, SwJob *job)
{
  GtkWindow *window = g_hash_table_lookup (self->parents, job->package);
  GdkSurface *surface = window ? gtk_native_get_surface (GTK_NATIVE (window)) : NULL;

  job->state = SW_JOB_STARTING;
  g_signal_emit (self, signals[CHANGED], 0, job->package);
  if (!self->bus)
    {
      end_job (self, job, "failed", "Software cannot reach the session bus to ask App Installer.");
      return;
    }
#ifdef GDK_WINDOWING_WAYLAND
  if (surface && GDK_IS_WAYLAND_TOPLEVEL (surface))
    {
      const char *handle = g_object_get_data (G_OBJECT (surface), "sw-export-handle");
      Call *call;

      if (handle)
        {
          call_job (self, job, handle);
          return;
        }
      call = g_new0 (Call, 1);
      call->self = g_object_ref (self);
      call->package = g_strdup (job->package);
      if (gdk_wayland_toplevel_export_handle (GDK_TOPLEVEL (surface), on_exported, call, NULL))
        return;
      g_object_unref (call->self);
      g_free (call->package);
      g_free (call);
    }
#endif
  (void) surface;
  call_job (self, job, NULL);
}

static void
pump (SwInstaller *self)
{
  SwJob *first = self->jobs->len ? g_ptr_array_index (self->jobs, 0) : NULL;

  if (first && first->state == SW_JOB_QUEUED)
    start_job (self, first);
}

static void
add_job (SwInstaller *self, const char *package, const char *name, SwJobKind kind, GtkWidget *parent)
{
  SwJob *job;
  GtkRoot *root = parent ? gtk_widget_get_root (parent) : NULL;

  if (!package || find_job (self, package))
    return;
  job = g_new0 (SwJob, 1);
  job->package = g_strdup (package);
  job->name = g_strdup (name && *name ? name : package);
  job->kind = kind;
  job->state = SW_JOB_QUEUED;
  job->percent = -1;
  g_ptr_array_add (self->jobs, job);
  if (GTK_IS_WINDOW (root))
    g_hash_table_replace (self->parents, g_strdup (package), root);
  g_signal_emit (self, signals[CHANGED], 0, package);
  pump (self);
}

void
sw_installer_install (SwInstaller *self, const char *package, const char *name, GtkWidget *parent)
{
  add_job (self, package, name, sw_installer_get_installed (self, package) ? SW_JOB_UPDATE : SW_JOB_INSTALL, parent);
}

void
sw_installer_remove (SwInstaller *self, const char *package, const char *name, GtkWidget *parent)
{
  add_job (self, package, name, SW_JOB_REMOVE, parent);
}

void
sw_installer_cancel (SwInstaller *self, const char *package)
{
  SwJob *job = find_job (self, package);

  if (!job || !sw_job_can_cancel (job))
    return;
  if (job->state == SW_JOB_QUEUED)
    {
      end_job (self, job, "cancelled", "");
      return;
    }
  job->cancelling = TRUE;
  if (job->id)
    send_cancel (self, job);
  g_signal_emit (self, signals[CHANGED], 0, package);
}

/* ---- App Installer's signals ----------------------------------------------------------------------------------- */

static void
on_signal (GDBusConnection *bus, const char *sender, const char *path, const char *iface, const char *signal,
           GVariant *params, gpointer data)
{
  SwInstaller *self = data;

  if (!strcmp (signal, "Progress") && g_variant_is_of_type (params, G_VARIANT_TYPE ("(uis)")))
    {
      guint id;
      int percent;
      const char *status;
      SwJob *job;

      g_variant_get (params, "(ui&s)", &id, &percent, &status);
      if (!(job = find_job_id (self, id)))
        return;
      job->percent = percent;
      g_free (job->status);
      job->status = *status ? g_strdup (status) : NULL;
      g_signal_emit (self, signals[CHANGED], 0, job->package);
    }
  else if (!strcmp (signal, "Finished") && g_variant_is_of_type (params, G_VARIANT_TYPE ("(uss)")))
    {
      guint id;
      const char *result, *message;
      SwJob *job;

      g_variant_get (params, "(u&s&s)", &id, &result, &message);
      if ((job = find_job_id (self, id)))
        end_job (self, job, result, message);
    }
}

static void
on_name_vanished (GDBusConnection *bus, const char *name, gpointer data)
{
  SwInstaller *self = data;

  /* App Installer went with jobs of ours running: they will not finish */
  for (guint i = self->jobs->len; i > 0; i--)
    {
      SwJob *job = g_ptr_array_index (self->jobs, i - 1);

      if (job->state == SW_JOB_RUNNING && self->service_seen)
        end_job (self, job, "failed", "App Installer stopped before it was done. Try again.");
    }
  self->service_seen = FALSE;
}

static void
on_name_appeared (GDBusConnection *bus, const char *name, const char *owner, gpointer data)
{
  SW_INSTALLER (data)->service_seen = TRUE;
}

/* ---- GetState -------------------------------------------------------------------------------------------------- */

typedef struct {
  SwStateCallback callback;
  gpointer        data;
  GCancellable   *cancellable;
} StateCall;

static void
on_state_reply (GObject *source, GAsyncResult *res, gpointer data)
{
  StateCall *call = data;
  g_autoptr (GError) error = NULL;
  g_autoptr (GVariant) reply = g_dbus_connection_call_finish (G_DBUS_CONNECTION (source), res, &error);

  if (!g_cancellable_is_cancelled (call->cancellable))
    {
      if (reply)
        {
          const char *state, *installed, *available;

          g_variant_get (reply, "(&s&s&s)", &state, &installed, &available);
          call->callback (state, *installed ? installed : NULL, *available ? available : NULL, NULL, call->data);
        }
      else
        {
          g_autofree char *message = call_error_message (error);

          call->callback (NULL, NULL, NULL, message, call->data);
        }
    }
  g_clear_object (&call->cancellable);
  g_free (call);
}

void
sw_installer_get_state (SwInstaller *self, const char *package, GCancellable *cancellable, SwStateCallback callback,
                        gpointer data)
{
  StateCall *call = g_new0 (StateCall, 1);

  call->callback = callback;
  call->data = data;
  call->cancellable = cancellable ? g_object_ref (cancellable) : NULL;
  if (!self->bus)
    {
      callback (NULL, NULL, NULL, "No session bus.", data);
      g_clear_object (&call->cancellable);
      g_free (call);
      return;
    }
  g_dbus_connection_call (self->bus, BUS_NAME, OBJ_PATH, IFACE, "GetState", g_variant_new ("(s)", package),
                          G_VARIANT_TYPE ("(sss)"), G_DBUS_CALL_FLAGS_NONE, 60000, cancellable, on_state_reply,
                          call);
}

/* ---- the installed app's launcher ------------------------------------------------------------------------------ */

typedef struct {
  char *app_id, *package;
} FindData;

static void
find_data_free (gpointer data)
{
  FindData *find = data;

  g_free (find->app_id);
  g_free (find->package);
  g_free (find);
}

static GAppInfo *
desktop (const char *id)
{
  GDesktopAppInfo *info = id ? g_desktop_app_info_new (id) : NULL;

  return info ? G_APP_INFO (info) : NULL;
}

static void
find_thread (GTask *task, gpointer source, gpointer data, GCancellable *cancellable)
{
  FindData *find = data;
  GAppInfo *info = NULL;

  if (find->app_id)
    {
      g_autofree char *with = g_str_has_suffix (find->app_id, ".desktop")
                              ? g_strdup (find->app_id) : g_strconcat (find->app_id, ".desktop", NULL);
      g_autofree char *lower = g_ascii_strdown (with, -1);

      if (!(info = desktop (with)))
        info = desktop (lower);
    }
  if (!info && find->package)
    {
      g_autofree char *id = sw_dpkg_desktop_id (find->package);

      info = desktop (id);
    }
  g_task_return_pointer (task, info, g_object_unref);
}

typedef struct {
  SwAppInfoCallback callback;
  gpointer          data;
} FindCall;

static void
on_found (GObject *source, GAsyncResult *res, gpointer data)
{
  FindCall *call = data;
  GCancellable *cancellable = g_task_get_cancellable (G_TASK (res));
  g_autoptr (GAppInfo) info = g_task_propagate_pointer (G_TASK (res), NULL);

  if (!cancellable || !g_cancellable_is_cancelled (cancellable))
    call->callback (info, call->data);
  g_free (call);
}

void
sw_installer_find_app (SwInstaller *self, const char *app_id, const char *package, GCancellable *cancellable,
                       SwAppInfoCallback callback, gpointer data)
{
  FindCall *call = g_new0 (FindCall, 1);
  FindData *find = g_new0 (FindData, 1);
  g_autoptr (GTask) task = NULL;

  call->callback = callback;
  call->data = data;
  find->app_id = g_strdup (app_id);
  find->package = g_strdup (package);
  task = g_task_new (NULL, cancellable, on_found, call);
  g_task_set_task_data (task, find, find_data_free);
  /* answered even when cancelled: on_found checks */
  g_task_set_check_cancellable (task, FALSE);
  g_task_run_in_thread (task, find_thread);
}

gboolean
sw_installer_launch (GAppInfo *info, GtkWidget *parent)
{
  g_autoptr (GdkAppLaunchContext) context = gdk_display_get_app_launch_context (gtk_widget_get_display (parent));
  g_autoptr (GError) error = NULL;

  if (!g_app_info_launch (info, NULL, G_APP_LAUNCH_CONTEXT (context), &error))
    {
      g_warning ("could not start %s: %s", g_app_info_get_id (info), error->message);
      return FALSE;
    }
  return TRUE;
}

/* ---- the object ------------------------------------------------------------------------------------------------ */

static void
sw_installer_finalize (GObject *object)
{
  SwInstaller *self = SW_INSTALLER (object);

  if (self->bus && self->signal_sub)
    g_dbus_connection_signal_unsubscribe (self->bus, self->signal_sub);
  if (self->name_watch)
    g_bus_unwatch_name (self->name_watch);
  g_clear_handle_id (&self->reload_timeout, g_source_remove);
  g_clear_object (&self->monitor);
  g_clear_object (&self->bus);
  g_hash_table_unref (self->installed);
  g_ptr_array_unref (self->jobs);
  g_hash_table_unref (self->parents);
  G_OBJECT_CLASS (sw_installer_parent_class)->finalize (object);
}

static void
sw_installer_class_init (SwInstallerClass *klass)
{
  G_OBJECT_CLASS (klass)->finalize = sw_installer_finalize;
  signals[CHANGED] = g_signal_new ("changed", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                                   G_TYPE_NONE, 1, G_TYPE_STRING);
  signals[FINISHED] = g_signal_new ("finished", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                                    G_TYPE_NONE, 5, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_INT, G_TYPE_STRING,
                                    G_TYPE_STRING);
  signals[PACKAGE_CHANGED] = g_signal_new ("package-changed", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL,
                                           NULL, NULL, G_TYPE_NONE, 3, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING);
}

static void
sw_installer_init (SwInstaller *self)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (GFile) status = g_file_new_for_path (sw_dpkg_status_path ());

  self->installed = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
  self->jobs = g_ptr_array_new_with_free_func (job_free);
  self->parents = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  self->bus = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, &error);
  if (!self->bus)
    g_warning ("no session bus: %s", error->message);
  else
    {
      /* App Installer sends a job's signals to its caller only */
      self->signal_sub = g_dbus_connection_signal_subscribe (self->bus, BUS_NAME, IFACE, NULL, OBJ_PATH, NULL,
                                                             G_DBUS_SIGNAL_FLAGS_NONE, on_signal, self, NULL);
      self->name_watch = g_bus_watch_name_on_connection (self->bus, BUS_NAME, G_BUS_NAME_WATCHER_FLAGS_NONE,
                                                         on_name_appeared, on_name_vanished, self, NULL);
    }
  self->monitor = g_file_monitor_file (status, G_FILE_MONITOR_WATCH_MOVES, NULL, NULL);
  if (self->monitor)
    g_signal_connect (self->monitor, "changed", G_CALLBACK (on_status_file_changed), self);
  read_status (self);
}

SwInstaller *
sw_installer_get_default (void)
{
  static SwInstaller *installer;

  if (!installer)
    installer = g_object_new (SW_TYPE_INSTALLER, NULL);
  return installer;
}
