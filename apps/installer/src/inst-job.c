/* One install or uninstall, and the rules for it (inst-job.h) */
#include "inst-job.h"
#include "inst-catalog.h"
#include "inst-deb.h"
#include "inst-demo.h"
#include "inst-dpkg.h"
#include "inst-pk.h"
#include "inst-version.h"

#include <glib/gi18n.h>
#include <glib/gstdio.h>
#include <string.h>

/* the package lists are refreshed before a lookup when they are older than this */
#define LISTS_OLD      (6 * G_TIME_SPAN_HOUR)
/* and again when a package is not found, unless they were refreshed this recently */
#define LISTS_RECENT   (10 * G_TIME_SPAN_MINUTE)

typedef enum {
  STEP_NONE,
  STEP_REFRESH,       /* RefreshCache */
  STEP_RESOLVE,       /* Resolve: the installed and the candidate version */
  STEP_DETAILS,       /* GetDetails of the candidate: sizes and description */
  STEP_CHECK,         /* the dry run */
  STEP_EXTRAS,        /* GetDetails of the other packages: their download size */
  STEP_RUN,           /* the real transaction */
} Step;

struct _InstJob {
  GObject       parent_instance;

  guint         id;
  InstJobKind   kind;
  InstPhase     phase;
  InstPlan      plan;

  GFile        *file;          /* a file job's file, as given */
  char         *path;          /* the file PackageKit reads: a copy when root cannot read the original */
  gboolean      copied;
  char         *candidate_id;  /* a package job's package id in the software sources */
  char         *installed_id;  /* an uninstall's package id */
  char         *download;      /* a remote file's local copy, to delete at the end */
  GStrv         extras;        /* the other packages' ids, for their sizes */

  GCancellable *cancellable;
  InstPkTx     *tx;
  Step          step;
  gboolean      refreshed;     /* the lists were refreshed by this job */
  gboolean      catalog_done;

  int           percent;
  const char   *status_id;
  char         *status_text;
  gboolean      can_cancel;
  char         *title, *text, *details;
};

G_DEFINE_FINAL_TYPE (InstJob, inst_job, G_TYPE_OBJECT)

enum { SIGNAL_CHANGED, SIGNAL_FINISHED, N_SIGNALS };
static guint signals[N_SIGNALS];

static void
plan_clear (InstPlan *plan)
{
  g_clear_pointer (&plan->package, g_free);
  g_clear_pointer (&plan->version, g_free);
  g_clear_pointer (&plan->architecture, g_free);
  g_clear_pointer (&plan->installed_version, g_free);
  g_clear_pointer (&plan->name, g_free);
  g_clear_pointer (&plan->summary, g_free);
  g_clear_pointer (&plan->description, g_free);
  g_clear_pointer (&plan->homepage, g_free);
  g_clear_pointer (&plan->maintainer, g_free);
  g_clear_object (&plan->icon);
  g_clear_pointer (&plan->desktop_id, g_free);
  g_clear_pointer (&plan->also_install, g_ptr_array_unref);
  g_clear_pointer (&plan->also_remove, g_ptr_array_unref);
}

static void
inst_job_dispose (GObject *object)
{
  InstJob *self = INST_JOB (object);

  g_cancellable_cancel (self->cancellable);
  g_clear_pointer (&self->tx, inst_pk_tx_free);
  if (self->copied && self->path)
    g_unlink (self->path);
  self->copied = FALSE;
  if (self->download)
    g_unlink (self->download);
  g_clear_pointer (&self->download, g_free);
  G_OBJECT_CLASS (inst_job_parent_class)->dispose (object);
}

static void
inst_job_finalize (GObject *object)
{
  InstJob *self = INST_JOB (object);

  plan_clear (&self->plan);
  g_clear_object (&self->file);
  g_clear_object (&self->cancellable);
  g_free (self->path);
  g_free (self->candidate_id);
  g_free (self->installed_id);
  g_strfreev (self->extras);
  g_free (self->status_text);
  g_free (self->title);
  g_free (self->text);
  g_free (self->details);
  G_OBJECT_CLASS (inst_job_parent_class)->finalize (object);
}

static void
inst_job_class_init (InstJobClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = inst_job_dispose;
  object_class->finalize = inst_job_finalize;
  signals[SIGNAL_CHANGED] = g_signal_new ("changed", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL,
                                          NULL, G_TYPE_NONE, 0);
  signals[SIGNAL_FINISHED] = g_signal_new ("finished", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL,
                                           NULL, G_TYPE_NONE, 0);
}

static void
inst_job_init (InstJob *self)
{
  static guint next_id = 1;

  self->id = next_id++;
  self->cancellable = g_cancellable_new ();
  self->percent = -1;
  self->status_id = "reading";
  self->plan.also_install = g_ptr_array_new_with_free_func (g_free);
  self->plan.also_remove = g_ptr_array_new_with_free_func (g_free);
}

static InstJob *
job_new (InstJobKind kind, const char *package)
{
  InstJob *self = g_object_new (INST_TYPE_JOB, NULL);

  self->kind = kind;
  self->plan.change = kind == INST_JOB_REMOVE ? INST_CHANGE_REMOVE : INST_CHANGE_NEW;
  self->plan.package = g_strdup (package);
  return self;
}

InstJob *
inst_job_new_file (GFile *file)
{
  InstJob *self = job_new (INST_JOB_FILE, NULL);
  g_autofree char *base = g_file_get_basename (file);

  self->file = g_object_ref (file);
  /* until the file is read: its name, without the _version_arch.deb */
  if (base)
    {
      char *cut = strchr (base, '_');

      if (cut)
        *cut = '\0';
      else if (g_str_has_suffix (base, ".deb"))
        base[strlen (base) - 4] = '\0';
      self->plan.package = g_strdup (base);
    }
  return self;
}

InstJob *
inst_job_new_package (const char *package)
{
  return job_new (INST_JOB_PACKAGE, package);
}

InstJob *
inst_job_new_remove (const char *package)
{
  return job_new (INST_JOB_REMOVE, package);
}

/* ---- state ---------------------------------------------------------------------------------------------------- */

static void
changed (InstJob *self)
{
  g_signal_emit (self, signals[SIGNAL_CHANGED], 0);
}

static void
set_status (InstJob *self, const char *id, const char *text)
{
  self->status_id = id;
  g_free (self->status_text);
  self->status_text = g_strdup (text);
}

static void
set_message (InstJob *self, const char *title, const char *text, const char *details)
{
  g_free (self->title);
  g_free (self->text);
  g_free (self->details);
  self->title = g_strdup (title);
  self->text = g_strdup (text);
  self->details = details && *details ? g_strdup (details) : NULL;
}

static void
end (InstJob *self, InstPhase phase)
{
  g_clear_pointer (&self->tx, inst_pk_tx_free);
  self->step = STEP_NONE;
  self->phase = phase;
  self->can_cancel = FALSE;
  self->percent = -1;
  set_status (self, phase == INST_PHASE_DONE ? "done" : phase == INST_PHASE_REFUSED ? "refused"
                    : phase == INST_PHASE_CANCELLED ? "cancelled" : "failed", NULL);
  if (self->copied && self->path)
    g_unlink (self->path);
  self->copied = FALSE;
  changed (self);
  g_signal_emit (self, signals[SIGNAL_FINISHED], 0);
}

static void
refuse (InstJob *self, const char *title, const char *text, const char *details)
{
  set_message (self, title, text, details);
  end (self, INST_PHASE_REFUSED);
}

guint inst_job_get_id (InstJob *self) { return self->id; }
InstJobKind inst_job_get_kind (InstJob *self) { return self->kind; }
InstPhase inst_job_get_phase (InstJob *self) { return self->phase; }
const InstPlan *inst_job_get_plan (InstJob *self) { return &self->plan; }
int inst_job_get_percent (InstJob *self) { return self->percent; }
const char *inst_job_get_status_id (InstJob *self) { return self->status_id; }
const char *inst_job_get_status_text (InstJob *self) { return self->status_text; }
const char *inst_job_get_title (InstJob *self) { return self->title; }
const char *inst_job_get_text (InstJob *self) { return self->text; }
const char *inst_job_get_details (InstJob *self) { return self->details; }

const char *
inst_job_get_display_name (InstJob *self)
{
  return self->plan.name && *self->plan.name ? self->plan.name : self->plan.package ? self->plan.package : "";
}

gboolean
inst_job_is_finished (InstJob *self)
{
  return self->phase == INST_PHASE_REFUSED || self->phase == INST_PHASE_DONE || self->phase == INST_PHASE_FAILED
         || self->phase == INST_PHASE_CANCELLED;
}

gboolean
inst_job_can_start (InstJob *self)
{
  return self->phase == INST_PHASE_READY && self->plan.change != INST_CHANGE_SAME;
}

gboolean
inst_job_needs_question (InstJob *self)
{
  /* an archive package that is new, or newer than the installed one, needs nothing but the password */
  return self->kind != INST_JOB_PACKAGE
         || (self->plan.change != INST_CHANGE_NEW && self->plan.change != INST_CHANGE_UPGRADE)
         || self->plan.also_install->len > 0 || self->plan.also_remove->len > 0;
}

gboolean
inst_job_can_cancel (InstJob *self)
{
  if (self->phase == INST_PHASE_RUNNING)
    return self->can_cancel;
  return !inst_job_is_finished (self);
}

const char *
inst_job_get_result (InstJob *self)
{
  switch (self->phase)
    {
    case INST_PHASE_DONE: return "done";
    case INST_PHASE_REFUSED: return "refused";
    case INST_PHASE_CANCELLED: return "cancelled";
    case INST_PHASE_FAILED: return "failed";
    default: return NULL;
    }
}

/* ---- the rules: what must never be removed -------------------------------------------------------------------- */

/* the shell, the window manager and the session VibeOS runs in */
static const char *const session_packages[] = {
  "gnome-shell", "gnome-shell-common", "mutter", "mutter-common", "mutter-common-bin", "gnome-session",
  "gnome-session-bin", "gnome-session-common", "gdm3", "gnome-settings-daemon", "gnome-settings-daemon-common",
  "packagekit", NULL,
};

char *
inst_job_protected_reason (const char *package)
{
  InstDpkgInfo info;
  char *reason = NULL;

  if (!package)
    return NULL;
  if (!strcmp (package, "vibeos") || g_str_has_prefix (package, "vibeos-"))
    return g_strdup_printf (_("%s is part of VibeOS."), package);
  if (g_strv_contains (session_packages, package) || g_str_has_prefix (package, "libmutter-"))
    return g_strdup_printf (_("%s is part of the phone's shell and session."), package);
  if (!inst_dpkg_query (package, NULL, &info, NULL))
    return NULL;
  if (info.essential)
    reason = g_strdup_printf (_("%s is essential: the system cannot work without it."), package);
  else if (!g_strcmp0 (info.priority, "required"))
    reason = g_strdup_printf (_("%s is required by the system."), package);
  inst_dpkg_info_clear (&info);
  return reason;
}

/* the first package of `names` (and the job's own, for an uninstall) that must not go */
static gboolean
check_removals (InstJob *self)
{
  const char *first = NULL;
  g_autofree char *reason = NULL;

  if (self->kind == INST_JOB_REMOVE)
    {
      reason = inst_job_protected_reason (self->plan.package);
      first = self->plan.package;
    }
  for (guint i = 0; !reason && i < self->plan.also_remove->len; i++)
    {
      first = g_ptr_array_index (self->plan.also_remove, i);
      reason = inst_job_protected_reason (first);
    }
  if (!reason)
    return TRUE;
  if (self->kind == INST_JOB_REMOVE && !g_strcmp0 (first, self->plan.package))
    refuse (self, _("Cannot be uninstalled"), reason, NULL);
  else if (self->kind == INST_JOB_REMOVE)
    {
      g_autofree char *text = g_strdup_printf (_("Uninstalling it would also remove %s. %s"), first, reason);

      refuse (self, _("Cannot be uninstalled"), text, NULL);
    }
  else
    {
      g_autofree char *text = g_strdup_printf (_("Installing it would remove %s. %s"), first, reason);

      refuse (self, _("Cannot be installed"), text, NULL);
    }
  return FALSE;
}

/* ---- failures in plain words ---------------------------------------------------------------------------------- */

static void
fail_from_tx (InstJob *self, InstPkTx *tx)
{
  guint code = inst_pk_tx_get_error_code (tx);
  const char *details = inst_pk_tx_get_error_details (tx);
  gboolean removing = self->kind == INST_JOB_REMOVE;
  const char *title = removing ? _("Not uninstalled") : _("Not installed");
  const char *text;

  if (inst_pk_tx_get_exit (tx) == INST_PK_EXIT_CANCELLED || code == INST_PK_ERROR_TRANSACTION_CANCELLED
      || code == INST_PK_ERROR_PROCESS_KILL)
    {
      set_message (self, _("Stopped"), removing ? _("Nothing was uninstalled.") : _("Nothing was installed."), NULL);
      end (self, INST_PHASE_CANCELLED);
      return;
    }
  switch (code)
    {
    case INST_PK_ERROR_NOT_AUTHORIZED:
      /* the password dialog closed without the password: the user's no, not a failure */
      set_message (self, title, removing ? _("The password was not given, so nothing was uninstalled.")
                                         : _("The password was not given, so nothing was installed."), NULL);
      end (self, INST_PHASE_CANCELLED);
      return;
    case INST_PK_ERROR_CANNOT_GET_LOCK:
    case INST_PK_ERROR_LOCK_REQUIRED:
    case INST_PK_ERROR_UNFINISHED_TRANSACTION:
      text = _("Another app is installing or updating software right now. Try again once it has finished.");
      break;
    case INST_PK_ERROR_DEP_RESOLUTION_FAILED:
      text = _("Something it needs cannot be installed: it is not in the software sources, or not in a version "
               "that fits.");
      break;
    case INST_PK_ERROR_NO_NETWORK:
    case INST_PK_ERROR_PACKAGE_DOWNLOAD_FAILED:
    case INST_PK_ERROR_CANNOT_FETCH_SOURCES:
    case INST_PK_ERROR_NO_MORE_MIRRORS_TO_TRY:
    case INST_PK_ERROR_REPO_NOT_AVAILABLE:
    case INST_PK_ERROR_NO_CACHE:
      text = _("Packages it needs could not be downloaded. Check the internet connection and try again.");
      break;
    case INST_PK_ERROR_PACKAGE_CORRUPT:
    case INST_PK_ERROR_INVALID_PACKAGE_FILE:
    case INST_PK_ERROR_LOCAL_INSTALL_FAILED:
      text = _("The package is broken: the file is damaged, or it is not a proper package.");
      break;
    case INST_PK_ERROR_NO_SPACE_ON_DEVICE:
      text = _("There is not enough free storage for it.");
      break;
    case INST_PK_ERROR_FILE_CONFLICTS:
    case INST_PK_ERROR_PACKAGE_CONFLICTS:
      text = _("It clashes with software that is already installed: both have the same files.");
      break;
    case INST_PK_ERROR_PACKAGE_FAILED_TO_CONFIGURE:
    case INST_PK_ERROR_PACKAGE_FAILED_TO_INSTALL:
      text = _("The package's own setup failed. It may be only partly installed.");
      break;
    case INST_PK_ERROR_PACKAGE_FAILED_TO_REMOVE:
      text = _("The package's own removal script failed. It may be only partly uninstalled.");
      break;
    case INST_PK_ERROR_INCOMPATIBLE_ARCHITECTURE:
      text = _("It is made for another kind of device.");
      break;
    case INST_PK_ERROR_BAD_GPG_SIGNATURE:
    case INST_PK_ERROR_MISSING_GPG_SIGNATURE:
    case INST_PK_ERROR_CANNOT_INSTALL_REPO_UNSIGNED:
      text = _("The software sources could not be verified, so nothing was downloaded from them.");
      break;
    case INST_PK_ERROR_PACKAGE_NOT_FOUND:
      text = _("The software sources do not have it (any more).");
      break;
    case INST_PK_ERROR_PACKAGE_DATABASE_CHANGED:
      text = _("Software was installed or removed meanwhile. Try again.");
      break;
    case INST_PK_ERROR_NO_PACKAGEKIT:
      title = _("Cannot install software");
      text = _("PackageKit, the system service that installs software, is missing or not running.");
      break;
    default:
      text = removing ? _("Something went wrong while uninstalling.") : _("Something went wrong while installing.");
      break;
    }
  set_message (self, title, text, details);
  end (self, INST_PHASE_FAILED);
}

/* ---- the transactions ----------------------------------------------------------------------------------------- */

static void run_step (InstJob *self, Step step);
static void on_tx_finished (InstPkTx *tx, gpointer data);

static void
status_from_tx (InstJob *self, InstPkTx *tx)
{
  guint status = inst_pk_tx_get_status (tx), percent = inst_pk_tx_get_percentage (tx);
  gboolean removing = self->kind == INST_JOB_REMOVE;

  self->percent = percent <= 100 ? (int) percent : -1;
  self->can_cancel = inst_pk_tx_get_allow_cancel (tx);
  switch (status)
    {
    case INST_PK_STATUS_WAITING_FOR_AUTH:
      set_status (self, "waiting-for-auth", _("Waiting for your password"));
      break;
    case INST_PK_STATUS_WAIT:
    case INST_PK_STATUS_WAITING_FOR_LOCK:
      set_status (self, "waiting-for-lock", _("Waiting for another install to finish"));
      break;
    case INST_PK_STATUS_DOWNLOAD:
    case INST_PK_STATUS_DOWNLOAD_REPOSITORY:
    case INST_PK_STATUS_DOWNLOAD_UPDATEINFO:
      set_status (self, "downloading", _("Downloading"));
      break;
    case INST_PK_STATUS_INSTALL:
    case INST_PK_STATUS_UPDATE:
    case INST_PK_STATUS_COMMIT:
    case INST_PK_STATUS_REMOVE:
    case INST_PK_STATUS_TEST_COMMIT:
      set_status (self, removing ? "removing" : "installing", removing ? _("Uninstalling") : _("Installing"));
      break;
    case INST_PK_STATUS_CLEANUP:
    case INST_PK_STATUS_RUN_HOOK:
    case INST_PK_STATUS_FINISHED:
      set_status (self, "finishing", _("Finishing"));
      break;
    case INST_PK_STATUS_CANCEL:
      set_status (self, "stopping", _("Stopping"));
      break;
    default:
      set_status (self, "starting", _("Getting ready"));
      break;
    }
}

static void
on_tx_changed (InstPkTx *tx, gpointer data)
{
  InstJob *self = data;

  /* the dry runs and lookups are "checking" as a whole; only the real thing shows PackageKit's own steps */
  if (self->step == STEP_RUN)
    {
      status_from_tx (self, tx);
      changed (self);
    }
  else if (self->step == STEP_REFRESH && inst_pk_tx_get_percentage (tx) <= 100)
    {
      self->percent = inst_pk_tx_get_percentage (tx);
      changed (self);
    }
}

static InstPkTx *
new_tx (InstJob *self, Step step)
{
  g_clear_pointer (&self->tx, inst_pk_tx_free);
  self->step = step;
  self->tx = inst_pk_tx_new (on_tx_changed, on_tx_finished, self);
  return self->tx;
}

/* the lists' age: since apt last updated them, or App Installer refreshed them, whichever is later */
static char *
stamp_path (void)
{
  return g_build_filename (g_get_user_cache_dir (), "vibeos-installer", "lists-refreshed", NULL);
}

static GTimeSpan
lists_age (void)
{
  g_autofree char *stamp = stamp_path ();
  const char *paths[] = { "/var/lib/apt/lists", stamp };
  gint64 newest = 0;

  for (guint i = 0; i < G_N_ELEMENTS (paths); i++)
    {
      GStatBuf st;

      if (g_stat (paths[i], &st) == 0)
        newest = MAX (newest, (gint64) st.st_mtime);
    }
  return g_get_real_time () - newest * G_USEC_PER_SEC;
}

static void
touch_stamp (void)
{
  g_autofree char *stamp = stamp_path ();
  g_autofree char *dir = g_path_get_dirname (stamp);

  g_mkdir_with_parents (dir, 0700);
  g_file_set_contents (stamp, "", 0, NULL);
}

static gboolean
should_refresh (GTimeSpan older_than)
{
  if (inst_demo_active ())
    return inst_demo_has ("refresh");
  return lists_age () > older_than;
}

static void
set_checking (InstJob *self, const char *id, const char *text)
{
  set_status (self, id, text);
  self->percent = -1;
  changed (self);
}

/* the catalog's (or the installed desktop file's) app, filled in when it comes */
static void
catalog_thread (GTask *task, gpointer source, gpointer data, GCancellable *cancellable)
{
  InstJob *self = source;
  InstAppInfo *info = self->kind == INST_JOB_REMOVE ? inst_catalog_installed (data) : inst_catalog_lookup (data);

  g_task_return_pointer (task, info, (GDestroyNotify) inst_app_info_free);
}

static void
on_catalog (GObject *source, GAsyncResult *result, gpointer data)
{
  InstJob *self = INST_JOB (source);
  g_autoptr (InstAppInfo) info = g_task_propagate_pointer (G_TASK (result), NULL);

  self->catalog_done = TRUE;
  if (!info || g_cancellable_is_cancelled (self->cancellable))
    return;
  if (info->name && !self->plan.name)
    self->plan.name = g_strdup (info->name);
  if (info->summary && *info->summary)
    {
      g_free (self->plan.summary);
      self->plan.summary = g_strdup (info->summary);
    }
  if (info->description && *info->description)
    {
      g_free (self->plan.description);
      self->plan.description = g_strdup (info->description);
    }
  if (info->homepage && !self->plan.homepage)
    self->plan.homepage = g_strdup (info->homepage);
  if (info->icon && !self->plan.icon)
    self->plan.icon = g_object_ref (info->icon);
  if (info->desktop_id && !self->plan.desktop_id)
    self->plan.desktop_id = g_strdup (info->desktop_id);
  changed (self);
}

static void
look_up_app (InstJob *self)
{
  g_autoptr (GTask) task = g_task_new (self, self->cancellable, on_catalog, NULL);

  g_task_set_task_data (task, g_strdup (self->plan.package), g_free);
  g_task_run_in_thread (task, catalog_thread);
}

static void
plan_set_change (InstJob *self)
{
  int c;

  if (self->kind == INST_JOB_REMOVE)
    return;
  if (!self->plan.installed_version)
    {
      self->plan.change = INST_CHANGE_NEW;
      return;
    }
  c = inst_version_compare (self->plan.version, self->plan.installed_version);
  self->plan.change = c > 0 ? INST_CHANGE_UPGRADE : c < 0 ? INST_CHANGE_DOWNGRADE : INST_CHANGE_REINSTALL;
}

static guint64
flags_for_change (InstJob *self)
{
  guint64 flags = INST_PK_FLAG_NONE;

  /*
   * Packages come from signed archives: asking for trusted ones only makes PackageKit (and polkit) treat the
   * install as trusted, instead of warning that the software is not from a trusted source. A local .deb file
   * is never trusted, so file jobs leave the flag out.
   */
  if (self->kind == INST_JOB_PACKAGE)
    flags |= INST_PK_FLAG_ONLY_TRUSTED;
  if (self->plan.change == INST_CHANGE_DOWNGRADE)
    flags |= INST_PK_FLAG_ALLOW_DOWNGRADE;
  if (self->plan.change == INST_CHANGE_REINSTALL)
    flags |= INST_PK_FLAG_ALLOW_REINSTALL;
  return flags;
}

static void
start_check (InstJob *self)
{
  self->phase = INST_PHASE_CHECKING;
  set_checking (self, "checking", self->kind == INST_JOB_REMOVE ? _("Checking what else it takes along")
                                                                 : _("Checking what else it needs"));
  run_step (self, STEP_CHECK);
}

static void
ready (InstJob *self)
{
  self->step = STEP_NONE;
  g_clear_pointer (&self->tx, inst_pk_tx_free);
  if (!check_removals (self))
    return;
  self->phase = INST_PHASE_READY;
  set_status (self, "ready", NULL);
  changed (self);
}

/* the dry run's packages: what happens to the job's own package, and to every other one */
static void
read_check (InstJob *self, InstPkTx *tx)
{
  GPtrArray *packages = inst_pk_tx_get_packages (tx);
  gboolean target_seen = FALSE;

  g_ptr_array_set_size (self->plan.also_install, 0);
  g_ptr_array_set_size (self->plan.also_remove, 0);
  for (guint i = 0; i < packages->len; i++)
    {
      InstPkPackage *p = g_ptr_array_index (packages, i);
      gboolean target = !g_strcmp0 (p->name, self->plan.package);
      GPtrArray *list = NULL;

      if (target)
        {
          target_seen = TRUE;
          if (p->info == INST_PK_INFO_REINSTALLING)
            self->plan.change = INST_CHANGE_REINSTALL;
          continue;
        }
      switch (p->info)
        {
        case INST_PK_INFO_INSTALLING:
        case INST_PK_INFO_UPDATING:
        case INST_PK_INFO_DOWNGRADING:
        case INST_PK_INFO_REINSTALLING:
          list = self->plan.also_install;
          break;
        case INST_PK_INFO_REMOVING:
        case INST_PK_INFO_OBSOLETING:
          list = self->plan.also_remove;
          break;
        default:
          break;
        }
      if (list && !g_ptr_array_find_with_equal_func (list, p->name, g_str_equal, NULL))
        g_ptr_array_add (list, g_strdup (p->name));
    }
  /* The same version again: PackageKit's apt backend does not reinstall, and its dry run then leaves the package out
   * altogether (installing it would end in success having done nothing) */
  if (self->plan.change == INST_CHANGE_REINSTALL && !target_seen)
    self->plan.change = INST_CHANGE_SAME;
  self->plan.also_size = inst_pk_tx_get_download_size (tx);
}

/* the ids of the dry run's other packages that are downloaded */
static GStrv
extra_ids (InstPkTx *tx, const char *own)
{
  GStrvBuilder *ids = g_strv_builder_new ();
  GPtrArray *packages = inst_pk_tx_get_packages (tx);

  for (guint i = 0; i < packages->len; i++)
    {
      InstPkPackage *p = g_ptr_array_index (packages, i);

      if (g_strcmp0 (p->name, own) && p->info != INST_PK_INFO_REMOVING && p->info != INST_PK_INFO_OBSOLETING
          && !g_str_has_prefix (p->data, "installed"))
        g_strv_builder_add (ids, p->id);
    }
  return g_strv_builder_unref_to_strv (ids);
}

static void
on_tx_finished (InstPkTx *tx, gpointer data)
{
  InstJob *self = data;
  Step step = self->step;
  gboolean ok = inst_pk_tx_get_exit (tx) == INST_PK_EXIT_SUCCESS;

  switch (step)
    {
    case STEP_REFRESH:
      /* a failed refresh is not the end: the lists there are may still have it */
      if (ok)
        touch_stamp ();
      self->refreshed = TRUE;
      set_checking (self, "reading", _("Looking it up"));
      run_step (self, STEP_RESOLVE);
      return;

    case STEP_RESOLVE:
      {
        GPtrArray *packages = inst_pk_tx_get_packages (tx);
        InstPkPackage *installed = NULL, *candidate = NULL;

        if (!ok)
          {
            fail_from_tx (self, tx);
            return;
          }
        for (guint i = 0; i < packages->len; i++)
          {
            InstPkPackage *p = g_ptr_array_index (packages, i);

            if (g_strcmp0 (p->name, self->plan.package))
              continue;
            if (p->info == INST_PK_INFO_INSTALLED && !installed)
              installed = p;
            else if (p->info == INST_PK_INFO_AVAILABLE
                     && (!candidate || inst_version_compare (p->version, candidate->version) > 0))
              candidate = p;
          }
        if (self->kind == INST_JOB_REMOVE)
          {
            if (installed)
              {
                g_free (self->installed_id);
                self->installed_id = g_strdup (installed->id);
              }
            start_check (self);
            return;
          }
        /* what is installed now, to compare the sources' version with */
        g_free (self->plan.installed_version);
        self->plan.installed_version = installed ? g_strdup (installed->version) : NULL;
        /* the version in the sources is the installed one: Resolve says so only as "installed" */
        if (!candidate && installed)
          candidate = installed;
        if (!candidate)
          {
            g_autofree char *text = NULL;

            if (!self->refreshed && should_refresh (LISTS_RECENT))
              {
                set_checking (self, "refreshing", _("Checking the software sources"));
                run_step (self, STEP_REFRESH);
                return;
              }
            text = g_strdup_printf (_("The software sources have no package called “%s”."), self->plan.package);
            refuse (self, _("Not found"), text, NULL);
            return;
          }
        g_free (self->candidate_id);
        self->candidate_id = g_strdup (candidate->id);
        g_free (self->plan.version);
        self->plan.version = g_strdup (candidate->version);
        g_free (self->plan.architecture);
        self->plan.architecture = g_strdup (candidate->arch);
        if (candidate->summary && *candidate->summary && !self->plan.summary)
          self->plan.summary = g_strdup (candidate->summary);
        plan_set_change (self);
        changed (self);
        run_step (self, STEP_DETAILS);
        return;
      }

    case STEP_DETAILS:
      {
        GPtrArray *details = inst_pk_tx_get_details_list (tx);

        for (guint i = 0; ok && i < details->len; i++)
          {
            GVariantDict dict;
            const char *text;
            guint64 size;

            g_variant_dict_init (&dict, g_ptr_array_index (details, i));
            if (g_variant_dict_lookup (&dict, "download-size", "t", &size))
              self->plan.download_size = size;
            if (g_variant_dict_lookup (&dict, "size", "t", &size))
              self->plan.installed_size = size;
            if (!self->plan.description && g_variant_dict_lookup (&dict, "description", "&s", &text) && *text)
              self->plan.description = g_strdup (text);
            if (!self->plan.summary && g_variant_dict_lookup (&dict, "summary", "&s", &text) && *text)
              self->plan.summary = g_strdup (text);
            if (!self->plan.homepage && g_variant_dict_lookup (&dict, "url", "&s", &text) && *text)
              self->plan.homepage = g_strdup (text);
            g_variant_dict_clear (&dict);
          }
        /* the details only add to the page: without them the check still says what matters */
        start_check (self);
        return;
      }

    case STEP_CHECK:
      if (!ok && (self->plan.change == INST_CHANGE_REINSTALL)
          && (inst_pk_tx_get_error_code (tx) == INST_PK_ERROR_PACKAGE_ALREADY_INSTALLED
              || inst_pk_tx_get_error_code (tx) == INST_PK_ERROR_ALL_PACKAGES_ALREADY_INSTALLED))
        {
          /* the same version, which PackageKit will not install again */
          self->plan.change = INST_CHANGE_SAME;
          ready (self);
          return;
        }
      if (!ok)
        {
          if (inst_pk_tx_get_error_code (tx) == INST_PK_ERROR_DEP_RESOLUTION_FAILED)
            {
              refuse (self, _("Cannot be installed"),
                      self->kind == INST_JOB_REMOVE
                          ? _("Removing it would leave other software with something it needs missing.")
                          : _("Something it needs cannot be installed: it is not in the software sources, or not in "
                              "a version that fits."),
                      inst_pk_tx_get_error_details (tx));
              return;
            }
          fail_from_tx (self, tx);
          return;
        }
      read_check (self, tx);
      if (self->kind == INST_JOB_REMOVE)
        {
          InstDpkgInfo info;

          self->plan.freed_size = self->plan.installed_size;
          for (guint i = 0; i < self->plan.also_remove->len; i++)
            if (inst_dpkg_query (g_ptr_array_index (self->plan.also_remove, i), NULL, &info, NULL))
              {
                self->plan.freed_size += info.installed_size;
                inst_dpkg_info_clear (&info);
              }
          ready (self);
          return;
        }
      if (self->plan.also_install->len > 0 && self->plan.also_size == 0 && !inst_demo_active ())
        {
          g_strfreev (self->extras);
          self->extras = extra_ids (tx, self->plan.package);
          if (self->extras[0])
            {
              run_step (self, STEP_EXTRAS);
              return;
            }
        }
      ready (self);
      return;

    case STEP_EXTRAS:
      {
        GPtrArray *details = inst_pk_tx_get_details_list (tx);

        for (guint i = 0; ok && i < details->len; i++)
          {
            GVariantDict dict;
            guint64 size;

            g_variant_dict_init (&dict, g_ptr_array_index (details, i));
            if (g_variant_dict_lookup (&dict, "download-size", "t", &size))
              self->plan.also_size += size;
            g_variant_dict_clear (&dict);
          }
        ready (self);
        return;
      }

    case STEP_RUN:
      if (!ok)
        {
          fail_from_tx (self, tx);
          return;
        }
      {
        g_autofree char *text = NULL;
        g_autoptr (InstAppInfo) app = NULL;

        if (self->kind == INST_JOB_REMOVE)
          {
            text = g_strdup_printf (_("%s is uninstalled."), inst_job_get_display_name (self));
            set_message (self, _("Uninstalled"), text, NULL);
          }
        else
          {
            /* the app it put in the drawer, to open */
            app = inst_catalog_installed (self->plan.package);
            if (app && app->desktop_id)
              {
                g_free (self->plan.desktop_id);
                self->plan.desktop_id = g_strdup (app->desktop_id);
              }
            if (app && app->name && !self->plan.name)
              self->plan.name = g_strdup (app->name);
            text = g_strdup_printf (_("%s %s is installed."), inst_job_get_display_name (self), self->plan.version);
            set_message (self, _("Installed"), text, NULL);
          }
        end (self, INST_PHASE_DONE);
        return;
      }

    case STEP_NONE:
      break;
    }
}

static void
run_step (InstJob *self, Step step)
{
  InstPkTx *tx = new_tx (self, step);

  switch (step)
    {
    case STEP_REFRESH:
      inst_pk_tx_refresh_cache (tx, FALSE);
      break;
    case STEP_RESOLVE:
      {
        const char *names[] = { self->plan.package, NULL };

        /* the installed version and the newest in the sources, of this device's architecture */
        inst_pk_tx_resolve (tx, INST_PK_FILTER_ARCH | (self->kind == INST_JOB_REMOVE ? INST_PK_FILTER_INSTALLED : 0)
                                    | INST_PK_FILTER_NEWEST, names);
        break;
      }
    case STEP_DETAILS:
      {
        const char *ids[] = { self->candidate_id, NULL };

        inst_pk_tx_get_details (tx, ids);
        break;
      }
    case STEP_CHECK:
    case STEP_RUN:
      {
        guint64 flags = flags_for_change (self) | (step == STEP_CHECK ? INST_PK_FLAG_SIMULATE : 0);

        if (step == STEP_RUN)
          {
            self->phase = INST_PHASE_RUNNING;
            set_status (self, "starting", _("Getting ready"));
            self->percent = -1;
            self->can_cancel = FALSE;
            changed (self);
          }
        if (self->kind == INST_JOB_FILE)
          inst_pk_tx_install_files (tx, flags, self->path);
        else if (self->kind == INST_JOB_PACKAGE)
          {
            const char *ids[] = { self->candidate_id, NULL };

            inst_pk_tx_install_packages (tx, flags, ids);
          }
        else
          {
            g_autofree char *fallback = NULL;
            const char *ids[] = { self->installed_id, NULL };

            /* a package PackageKit's Resolve did not list (dpkg knows it): its id from dpkg's fields */
            if (!ids[0])
              ids[0] = fallback = g_strdup_printf ("%s;%s;%s;installed", self->plan.package, self->plan.version,
                                                   self->plan.architecture ? self->plan.architecture : "");
            /* the packages that depend on it go too (the check listed them, the question shows them); nothing
             * else: no autoremove of what it pulled in once */
            inst_pk_tx_remove_packages (tx, step == STEP_CHECK ? INST_PK_FLAG_SIMULATE : 0, ids, TRUE, FALSE);
          }
        break;
      }
    case STEP_EXTRAS:
      inst_pk_tx_get_details (tx, (const char *const *) self->extras);
      break;
    case STEP_NONE:
      break;
    }
}

/* ---- READING -------------------------------------------------------------------------------------------------- */

/* a file job: its fields first (quick), the installed version, then whether root can read the file */
typedef struct {
  InstDeb      *deb;
  InstDpkgInfo  installed;
  gboolean      arch_ok;
} FileFacts;

static void
file_facts_free (FileFacts *facts)
{
  inst_deb_free (facts->deb);
  inst_dpkg_info_clear (&facts->installed);
  g_free (facts);
}

static void
file_facts_thread (GTask *task, gpointer source, gpointer data, GCancellable *cancellable)
{
  const char *path = data;
  GError *error = NULL;
  FileFacts *facts = g_new0 (FileFacts, 1);

  facts->deb = inst_deb_read_control (path, cancellable, &error);
  if (!facts->deb)
    {
      file_facts_free (facts);
      g_task_return_error (task, error);
      return;
    }
  inst_dpkg_query (facts->deb->package, facts->deb->architecture, &facts->installed, NULL);
  facts->arch_ok = inst_dpkg_arch_supported (facts->deb->architecture);
  g_task_return_pointer (task, facts, (GDestroyNotify) file_facts_free);
}

/* the app in the file: its desktop file and icon, read from the data archive (slower: the whole archive) */
static void
file_app_thread (GTask *task, gpointer source, gpointer data, GCancellable *cancellable)
{
  InstDeb *deb = g_new0 (InstDeb, 1);
  GError *error = NULL;

  deb->package = g_strdup (INST_JOB (source)->plan.package);
  if (!inst_deb_read_files (deb, data, cancellable, &error))
    {
      inst_deb_free (deb);
      g_task_return_error (task, error);
      return;
    }
  g_task_return_pointer (task, deb, (GDestroyNotify) inst_deb_free);
}

static void
on_file_app (GObject *source, GAsyncResult *result, gpointer data)
{
  InstJob *self = INST_JOB (source);
  g_autoptr (InstDeb) deb = g_task_propagate_pointer (G_TASK (result), NULL);

  if (!deb || g_cancellable_is_cancelled (self->cancellable))
    return;
  if (deb->app_name && *deb->app_name)
    {
      g_free (self->plan.name);
      self->plan.name = g_strdup (deb->app_name);
    }
  if (deb->icon)
    {
      g_clear_object (&self->plan.icon);
      self->plan.icon = g_bytes_icon_new (deb->icon);
    }
  changed (self);
}

/* Root cannot read every file the user can: a FUSE mount (gvfs's, the document portal's) is the user's alone. A file
 * there, or not a local file at all, is copied to the user's cache first. */
static gboolean
root_can_read (GFile *file)
{
  g_autofree char *path = g_file_get_path (file);
  g_autoptr (GFileInfo) info = NULL;
  const char *fs;

  if (!path)
    return FALSE;
  if (g_str_has_prefix (path, g_get_user_runtime_dir ()))
    return FALSE;
  info = g_file_query_filesystem_info (file, G_FILE_ATTRIBUTE_FILESYSTEM_TYPE, NULL, NULL);
  fs = info ? g_file_info_get_attribute_string (info, G_FILE_ATTRIBUTE_FILESYSTEM_TYPE) : NULL;
  return !fs || !g_str_has_prefix (fs, "fuse");
}

static void file_ready (InstJob *self);

static void
on_copied (GObject *source, GAsyncResult *result, gpointer data)
{
  g_autoptr (InstJob) self = data;
  g_autoptr (GError) error = NULL;

  if (!g_file_copy_finish (G_FILE (source), result, &error))
    {
      if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        return;
      g_clear_pointer (&self->path, g_free);
      set_message (self, _("Cannot open the file"), _("The file could not be read."), error->message);
      end (self, INST_PHASE_FAILED);
      return;
    }
  self->copied = TRUE;
  file_ready (self);
}

static void
on_file_facts (GObject *source, GAsyncResult *result, gpointer data)
{
  InstJob *self = INST_JOB (source);
  g_autoptr (GError) error = NULL;
  FileFacts *facts = g_task_propagate_pointer (G_TASK (result), &error);
  InstDeb *deb;

  if (g_cancellable_is_cancelled (self->cancellable))
    {
      g_clear_pointer (&facts, file_facts_free);
      return;
    }
  if (!facts)
    {
      if (g_error_matches (error, INST_DEB_ERROR, INST_DEB_ERROR_NO_DPKG))
        set_message (self, _("Cannot open the file"), _("dpkg, which reads packages, is missing."), error->message);
      else
        set_message (self, _("Not a package"),
                     _("This file is not a software package that can be installed here, or it is damaged."),
                     error ? error->message : NULL);
      end (self, INST_PHASE_FAILED);
      return;
    }
  deb = facts->deb;
  g_free (self->plan.package);
  self->plan.package = g_strdup (deb->package);
  self->plan.version = g_strdup (deb->version);
  self->plan.architecture = g_strdup (deb->architecture);
  self->plan.summary = g_strdup (deb->summary);
  self->plan.description = g_strdup (deb->description);
  self->plan.homepage = g_strdup (deb->homepage);
  self->plan.maintainer = g_strdup (deb->maintainer);
  self->plan.installed_size = deb->installed_size;
  self->plan.file_size = deb->file_size;
  self->plan.installed_version = g_strdup (facts->installed.version);
  plan_set_change (self);
  if (!facts->arch_ok)
    {
      g_autofree char *text = g_strdup_printf (_("It is made for %s devices; this one is %s."), deb->architecture,
                                               inst_dpkg_native_arch ());

      file_facts_free (facts);
      changed (self);
      refuse (self, _("Made for another kind of device"), text, NULL);
      return;
    }
  file_facts_free (facts);
  changed (self);

  /* the app's name and icon come when the archive is read, meanwhile the check runs */
  {
    g_autoptr (GTask) task = g_task_new (self, self->cancellable, on_file_app, NULL);

    g_task_set_task_data (task, g_file_get_path (self->file), g_free);
    g_task_run_in_thread (task, file_app_thread);
  }

  if (root_can_read (self->file))
    {
      self->path = g_file_get_path (self->file);
      file_ready (self);
    }
  else
    {
      g_autofree char *dir = g_build_filename (g_get_user_cache_dir (), "vibeos-installer", NULL);
      g_autofree char *name = g_strdup_printf ("%u-%s_%s.deb", self->id, self->plan.package, self->plan.version);
      g_autoptr (GFile) copy = NULL;

      g_mkdir_with_parents (dir, 0700);
      self->path = g_build_filename (dir, name, NULL);
      copy = g_file_new_for_path (self->path);
      g_file_copy_async (self->file, copy, G_FILE_COPY_OVERWRITE, G_PRIORITY_DEFAULT, self->cancellable, NULL, NULL,
                         on_copied, g_object_ref (self));
    }
}

static void
file_ready (InstJob *self)
{
  start_check (self);
}

void
inst_job_prepare (InstJob *self)
{
  g_return_if_fail (self->phase == INST_PHASE_READING && self->step == STEP_NONE);

  switch (self->kind)
    {
    case INST_JOB_FILE:
      {
        g_autoptr (GTask) task = g_task_new (self, self->cancellable, on_file_facts, NULL);
        g_autofree char *path = g_file_get_path (self->file);

        set_checking (self, "reading", _("Reading the package"));
        if (!path)
          {
            /* a remote file: copied first, then read from the copy */
            g_autofree char *dir = g_build_filename (g_get_user_cache_dir (), "vibeos-installer", NULL);
            g_autofree char *name = g_strdup_printf ("%u-download.deb", self->id);
            g_autoptr (GFile) copy = NULL;
            g_autoptr (GError) error = NULL;

            g_mkdir_with_parents (dir, 0700);
            path = g_build_filename (dir, name, NULL);
            copy = g_file_new_for_path (path);
            if (!g_file_copy (self->file, copy, G_FILE_COPY_OVERWRITE, self->cancellable, NULL, NULL, &error))
              {
                set_message (self, _("Cannot open the file"), _("The file could not be read."), error->message);
                end (self, INST_PHASE_FAILED);
                return;
              }
            g_set_object (&self->file, copy);
            self->download = g_strdup (path);
          }
        g_task_set_task_data (task, g_steal_pointer (&path), g_free);
        g_task_run_in_thread (task, file_facts_thread);
        break;
      }

    case INST_JOB_PACKAGE:
      look_up_app (self);
      if (should_refresh (LISTS_OLD))
        {
          set_checking (self, "refreshing", _("Checking the software sources"));
          run_step (self, STEP_REFRESH);
        }
      else
        {
          set_checking (self, "reading", _("Looking it up"));
          run_step (self, STEP_RESOLVE);
        }
      break;

    case INST_JOB_REMOVE:
      {
        InstDpkgInfo info;
        g_autoptr (GError) error = NULL;

        set_checking (self, "reading", _("Looking it up"));
        if (!inst_dpkg_query (self->plan.package, NULL, &info, &error))
          {
            set_message (self, _("Cannot uninstall software"), _("dpkg, which keeps track of the installed "
                                                                 "software, is missing."), error->message);
            end (self, INST_PHASE_FAILED);
            return;
          }
        if (!info.version)
          {
            g_autofree char *text = g_strdup_printf (_("“%s” is not installed."), self->plan.package);

            refuse (self, _("Not installed"), text, NULL);
            return;
          }
        self->plan.version = g_strdup (info.version);
        self->plan.installed_version = g_strdup (info.version);
        self->plan.architecture = g_strdup (info.architecture);
        self->plan.installed_size = info.installed_size;
        inst_dpkg_info_clear (&info);
        if (!check_removals (self))
          return;
        look_up_app (self);
        run_step (self, STEP_RESOLVE);
        break;
      }
    }
}

void
inst_job_start (InstJob *self)
{
  g_return_if_fail (inst_job_can_start (self));
  run_step (self, STEP_RUN);
}

void
inst_job_cancel (InstJob *self)
{
  if (inst_job_is_finished (self))
    return;
  if (self->phase == INST_PHASE_RUNNING)
    {
      /* PackageKit says when it can stop: until dpkg starts. It then finishes as cancelled. */
      if (self->tx && self->can_cancel)
        {
          inst_pk_tx_cancel (self->tx);
          set_status (self, "stopping", _("Stopping"));
          self->can_cancel = FALSE;
          changed (self);
        }
      return;
    }
  /* before the real thing nothing has changed: a dry run in PackageKit is left to end by itself */
  g_cancellable_cancel (self->cancellable);
  if (self->tx)
    inst_pk_tx_cancel (self->tx);
  set_message (self, _("Stopped"), self->kind == INST_JOB_REMOVE ? _("Nothing was uninstalled.")
                                                                  : _("Nothing was installed."), NULL);
  end (self, INST_PHASE_CANCELLED);
}
