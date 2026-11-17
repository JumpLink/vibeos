/* vibeos.Installer1 (inst-service.h; the interface is documented in data/vibeos.Installer1.xml) */
#include "inst-service.h"
#include "inst-dpkg.h"
#include "inst-job.h"
#include "inst-pk.h"
#include "inst-sheet.h"
#include "inst-version.h"

#include <glib/gi18n.h>
#include <string.h>

#define IFACE "vibeos.Installer1"

typedef struct {
  InstJob    *job;
  char       *caller;        /* the unique bus name that asked: the signals go to it alone (NULL: the demo) */
  char       *parent;        /* its window, for the sheet */
  GtkWidget  *sheet;
  int         percent;
  const char *status;
  gboolean    ended;
} ApiJob;

static GtkApplication *app;
static GDBusConnection *bus;
static char *object_path;
static guint registration;
static GHashTable *jobs;     /* job id -> ApiJob */

static void
api_job_free (gpointer data)
{
  ApiJob *api = data;

  g_signal_handlers_disconnect_by_data (api->job, api);
  g_object_unref (api->job);
  g_free (api->caller);
  g_free (api->parent);
  g_free (api);
}

static void
emit (ApiJob *api, const char *signal, GVariant *params)
{
  g_variant_ref_sink (params);
  if (api->caller && bus)
    g_dbus_connection_emit_signal (bus, api->caller, object_path, IFACE, signal, params, NULL);
  else
    {
      /* the demo: the signals as they would go out, to see them */
      g_autofree char *text = g_variant_print (params, FALSE);

      g_message ("%s %s", signal, text);
    }
  g_variant_unref (params);
}

static gboolean
drop_job (gpointer data)
{
  g_hash_table_remove (jobs, data);
  g_application_release (G_APPLICATION (app));
  return G_SOURCE_REMOVE;
}

/* Finished, once, and the job let go (from an idle: the job is still emitting) */
static void
end_job (ApiJob *api, const char *result, const char *message)
{
  if (api->ended)
    return;
  api->ended = TRUE;
  emit (api, "Finished", g_variant_new ("(uss)", inst_job_get_id (api->job), result, message ? message : ""));
  g_idle_add (drop_job, GUINT_TO_POINTER (inst_job_get_id (api->job)));
}

static void
on_finished (InstJob *job, ApiJob *api)
{
  const char *message = inst_job_get_text (job) ? inst_job_get_text (job) : inst_job_get_title (job);

  end_job (api, inst_job_get_result (job), message);
}

static void
on_changed (InstJob *job, ApiJob *api)
{
  InstPhase phase = inst_job_get_phase (job);
  const char *status = phase == INST_PHASE_READY ? "asking" : inst_job_get_status_id (job);
  int percent = inst_job_get_percent (job);

  if (api->ended || inst_job_is_finished (job))
    return;
  if (phase == INST_PHASE_READY && !api->sheet)
    {
      if (!inst_job_can_start (job))
        {
          /* the same version is installed: nothing to do, and nothing to ask */
          const InstPlan *plan = inst_job_get_plan (job);
          g_autofree char *message = g_strdup_printf (_("%s %s is installed already."),
                                                      inst_job_get_display_name (job), plan->version);

          end_job (api, "done", message);
          return;
        }
      if (!inst_job_needs_question (job))
        {
          inst_job_start (job);
          return;
        }
      api->sheet = inst_sheet_new (app, job, api->parent);
      g_object_add_weak_pointer (G_OBJECT (api->sheet), (gpointer *) &api->sheet);
      gtk_window_present (GTK_WINDOW (api->sheet));
    }
  if (percent != api->percent || g_strcmp0 (status, api->status))
    {
      api->percent = percent;
      api->status = status;
      emit (api, "Progress", g_variant_new ("(uis)", inst_job_get_id (job), percent, status ? status : ""));
    }
}

static gboolean
prepare_later (gpointer data)
{
  InstJob *job = data;

  inst_job_prepare (job);
  return G_SOURCE_REMOVE;
}

static guint
add_job (InstJob *job, const char *caller, GVariant *options)
{
  ApiJob *api = g_new0 (ApiJob, 1);
  const char *parent = NULL;

  api->job = job;
  api->caller = g_strdup (caller);
  api->percent = -2;
  if (options)
    g_variant_lookup (options, "parent-window", "&s", &parent);
  api->parent = g_strdup (parent);
  if (!jobs)
    jobs = g_hash_table_new_full (g_direct_hash, g_direct_equal, NULL, api_job_free);
  g_hash_table_insert (jobs, GUINT_TO_POINTER (inst_job_get_id (job)), api);
  g_application_hold (G_APPLICATION (app));
  g_signal_connect (job, "changed", G_CALLBACK (on_changed), api);
  g_signal_connect (job, "finished", G_CALLBACK (on_finished), api);
  /* after the reply with the job id has gone out: a job can end at once (no such package) */
  g_idle_add_full (G_PRIORITY_DEFAULT_IDLE, prepare_later, g_object_ref (job), g_object_unref);
  return inst_job_get_id (job);
}

/* a Debian package name: lower-case letters, digits, + - and ., two or more, starting with a letter or digit */
static gboolean
valid_package (const char *name)
{
  if (!name || strlen (name) < 2 || strlen (name) > 255 || !g_ascii_isalnum (name[0]))
    return FALSE;
  for (const char *p = name; *p; p++)
    if (!(g_ascii_islower (*p) || g_ascii_isdigit (*p) || *p == '+' || *p == '-' || *p == '.'))
      return FALSE;
  return TRUE;
}

/* ---- GetState: dpkg for the installed version, PackageKit's Resolve for the newest available ------------------- */

typedef struct {
  GDBusMethodInvocation *invocation;
  char                  *name;
  char                  *installed;
  InstPkTx              *tx;
} StateQuery;

static gboolean
free_state (gpointer data)
{
  StateQuery *q = data;

  inst_pk_tx_free (q->tx);
  g_free (q->name);
  g_free (q->installed);
  g_free (q);
  g_application_release (G_APPLICATION (app));
  return G_SOURCE_REMOVE;
}

static void
on_state_resolved (InstPkTx *tx, gpointer data)
{
  StateQuery *q = data;
  GPtrArray *packages = inst_pk_tx_get_packages (tx);
  const char *available = NULL;

  for (guint i = 0; inst_pk_tx_get_exit (tx) == INST_PK_EXIT_SUCCESS && i < packages->len; i++)
    {
      InstPkPackage *p = g_ptr_array_index (packages, i);

      if (!g_strcmp0 (p->name, q->name) && (!available || inst_version_compare (p->version, available) > 0))
        available = p->version;
    }
  g_dbus_method_invocation_return_value (q->invocation,
                                         g_variant_new ("(sss)",
                                                        q->installed ? "installed"
                                                        : available  ? "available" : "not-installed",
                                                        q->installed ? q->installed : "",
                                                        available ? available : ""));
  /* not from inside the transaction's own callback */
  g_idle_add (free_state, q);
}

static void
get_state (GDBusMethodInvocation *invocation, const char *name)
{
  StateQuery *q = g_new0 (StateQuery, 1);
  InstDpkgInfo info;
  const char *names[] = { name, NULL };

  inst_dpkg_query (name, NULL, &info, NULL);
  q->invocation = invocation;
  q->name = g_strdup (name);
  q->installed = g_steal_pointer (&info.version);
  inst_dpkg_info_clear (&info);
  /* a D-Bus activated App Installer would otherwise quit before PackageKit answers */
  g_application_hold (G_APPLICATION (app));
  q->tx = inst_pk_tx_new (NULL, on_state_resolved, q);
  inst_pk_tx_resolve (q->tx, INST_PK_FILTER_ARCH | INST_PK_FILTER_NEWEST, names);
}

/* ---- the method calls ----------------------------------------------------------------------------------------- */

static void
on_method_call (GDBusConnection *connection, const char *sender, const char *path, const char *iface,
                const char *method, GVariant *params, GDBusMethodInvocation *invocation, gpointer data)
{
  if (!strcmp (method, "InstallPackage") || !strcmp (method, "RemovePackage"))
    {
      const char *name;
      g_autoptr (GVariant) options = NULL;
      InstJob *job;

      g_variant_get (params, "(&s@a{sv})", &name, &options);
      if (!valid_package (name))
        {
          g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS,
                                                 "“%s” is not a package name", name);
          return;
        }
      job = !strcmp (method, "InstallPackage") ? inst_job_new_package (name) : inst_job_new_remove (name);
      g_dbus_method_invocation_return_value (invocation, g_variant_new ("(u)", add_job (job, sender, options)));
    }
  else if (!strcmp (method, "InstallFile"))
    {
      const char *path;
      g_autoptr (GVariant) options = NULL;
      g_autoptr (GFile) file = NULL;

      g_variant_get (params, "(&s@a{sv})", &path, &options);
      if (!g_path_is_absolute (path) || !g_file_test (path, G_FILE_TEST_IS_REGULAR))
        {
          g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS,
                                                 "“%s” is not a file (an absolute path is needed)", path);
          return;
        }
      file = g_file_new_for_path (path);
      g_dbus_method_invocation_return_value (invocation,
                                             g_variant_new ("(u)", add_job (inst_job_new_file (file), sender,
                                                                            options)));
    }
  else if (!strcmp (method, "Cancel"))
    {
      guint id;
      ApiJob *api;

      g_variant_get (params, "(u)", &id);
      api = jobs ? g_hash_table_lookup (jobs, GUINT_TO_POINTER (id)) : NULL;
      if (!api || api->ended)
        g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS,
                                               "No job %u", id);
      else if (g_strcmp0 (api->caller, sender))
        g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR, G_DBUS_ERROR_ACCESS_DENIED,
                                               "Job %u is another app's", id);
      else
        {
          inst_job_cancel (api->job);
          g_dbus_method_invocation_return_value (invocation, NULL);
        }
    }
  else if (!strcmp (method, "GetState"))
    {
      const char *name;

      g_variant_get (params, "(&s)", &name);
      if (!valid_package (name))
        g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS,
                                               "“%s” is not a package name", name);
      else
        get_state (invocation, name);
    }
  else
    g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_METHOD, "No method %s",
                                           method);
}

static const GDBusInterfaceVTable vtable = { on_method_call, NULL, NULL, { 0 } };

gboolean
inst_service_register (GtkApplication *application, GDBusConnection *connection, const char *path, GError **error)
{
  static GDBusNodeInfo *node;

  if (!node)
    {
      g_autoptr (GBytes) xml = g_resources_lookup_data ("/vibeos/Installer/vibeos.Installer1.xml",
                                                        G_RESOURCE_LOOKUP_FLAGS_NONE, error);

      if (!xml || !(node = g_dbus_node_info_new_for_xml (g_bytes_get_data (xml, NULL), error)))
        return FALSE;
    }
  registration = g_dbus_connection_register_object (connection, path, node->interfaces[0], &vtable, NULL, NULL,
                                                    error);
  if (!registration)
    return FALSE;
  app = application;
  bus = g_object_ref (connection);
  object_path = g_strdup (path);
  return TRUE;
}

void
inst_service_unregister (void)
{
  if (bus && registration)
    g_dbus_connection_unregister_object (bus, registration);
  registration = 0;
  g_clear_object (&bus);
  g_clear_pointer (&object_path, g_free);
}

void
inst_service_demo_call (GtkApplication *application, const char *call)
{
  InstJob *job = NULL;

  app = application;
  if (g_str_has_prefix (call, "install:"))
    job = inst_job_new_package (call + strlen ("install:"));
  else if (g_str_has_prefix (call, "remove:"))
    job = inst_job_new_remove (call + strlen ("remove:"));
  else if (g_str_has_prefix (call, "file:"))
    {
      g_autoptr (GFile) file = g_file_new_for_path (call + strlen ("file:"));

      job = inst_job_new_file (file);
    }
  if (job)
    add_job (job, NULL, NULL);
}
