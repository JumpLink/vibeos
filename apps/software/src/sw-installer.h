/* SwInstaller: what is installed, and installing, updating and uninstalling through App Installer.
 *
 * Software never installs anything itself. Every change goes to App Installer's D-Bus interface, vibeos.Installer1
 * (apps/installer/data/vibeos.Installer1.xml): InstallPackage and RemovePackage start a job, Progress and Finished
 * follow it, Cancel stops it. App Installer asks for the password (the shell's polkit dialog) and puts its own
 * questions in a sheet over Software's window (its "parent-window" option, an xdg-foreign handle). Software never
 * sees a password.
 *
 * Jobs run one at a time, in the order asked: each install takes PackageKit's lock anyway, and one after another the
 * polkit prompts come one after another too. The others wait as "queued". A job outlives the page that started it:
 * Back never cancels one, only the Cancel button does (and only while App Installer still can: not once dpkg runs).
 *
 * What is installed comes from dpkg's status file (sw-dpkg.h), read again whenever it changes. Each change to a
 * package's installed version is announced ("package-changed"), whoever made it. */
#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

typedef enum {
  SW_JOB_INSTALL,
  SW_JOB_UPDATE,
  SW_JOB_REMOVE,
} SwJobKind;

typedef enum {
  SW_JOB_QUEUED,                 /* waiting for the job before it */
  SW_JOB_STARTING,               /* asked App Installer, no answer yet */
  SW_JOB_RUNNING,                /* App Installer is on it: status and percent */
} SwJobState;

typedef struct {
  char       *package;
  char       *name;              /* the app's name, for messages */
  SwJobKind   kind;
  SwJobState  state;
  guint       id;                /* App Installer's job id, 0 until it answered */
  int         percent;           /* 0 to 100, -1 when not known */
  char       *status;            /* App Installer's status id ("downloading"), or NULL */
  gboolean    cancelling;
} SwJob;

#define SW_TYPE_INSTALLER (sw_installer_get_type ())
G_DECLARE_FINAL_TYPE (SwInstaller, sw_installer, SW, INSTALLER, GObject)

SwInstaller  *sw_installer_get_default   (void);

/* dpkg's list has been read at least once */
gboolean      sw_installer_is_loaded     (SwInstaller *self);
/* the installed version of `package`, or NULL */
const char   *sw_installer_get_installed (SwInstaller *self, const char *package);
/* package -> version, every installed package */
GHashTable   *sw_installer_get_installed_all (SwInstaller *self);
/* reads dpkg's list again (it is also read whenever it changes) */
void          sw_installer_reload        (SwInstaller *self);

/* the job for `package`, or NULL */
const SwJob  *sw_installer_get_job       (SwInstaller *self, const char *package);
/* every job, in the order they run */
GPtrArray    *sw_installer_get_jobs      (SwInstaller *self);

/* installs (or updates, when it is installed) `package`; `name` names it in messages; App Installer's questions go
 * over `parent`'s window */
void          sw_installer_install       (SwInstaller *self, const char *package, const char *name,
                                          GtkWidget *parent);
void          sw_installer_remove        (SwInstaller *self, const char *package, const char *name,
                                          GtkWidget *parent);
/* a queued job goes; a running one is asked to stop (Finished follows) */
void          sw_installer_cancel        (SwInstaller *self, const char *package);
/* whether a job can still be stopped (not once dpkg runs) */
gboolean      sw_job_can_cancel          (const SwJob *job);
/* one line for a job: "Queued", "Waiting for your password", "Downloading 45%", "Installing" */
char         *sw_job_describe            (const SwJob *job);

/* App Installer's GetState: "installed", "available" or "not-installed", and the versions */
typedef void (*SwStateCallback) (const char *state, const char *installed, const char *available,
                                 const char *error, gpointer data);
void          sw_installer_get_state     (SwInstaller *self, const char *package, GCancellable *cancellable,
                                          SwStateCallback callback, gpointer data);

/* The installed app's launcher: the desktop file `app_id` names (the store's ids are AppStream's, usually the
 * desktop file's), else the one the package installs. In a thread; `callback` gets a GAppInfo or NULL. */
typedef void (*SwAppInfoCallback) (GAppInfo *info, gpointer data);
void          sw_installer_find_app      (SwInstaller *self, const char *app_id, const char *package,
                                          GCancellable *cancellable, SwAppInfoCallback callback, gpointer data);
/* starts the app; FALSE if it could not */
gboolean      sw_installer_launch        (GAppInfo *info, GtkWidget *parent);

/* Signals:
 *   "changed" (const char *package)          a job of that package changed, or (NULL) the installed list
 *   "finished" (const char *package, const char *name, SwJobKind kind, const char *result, const char *message)
 *                                            a job ended: App Installer's result (done, cancelled, refused, failed)
 *   "package-changed" (const char *package, const char *old_version, const char *new_version)
 *                                            installed, updated (both versions) or uninstalled (new NULL) */

G_END_DECLS
