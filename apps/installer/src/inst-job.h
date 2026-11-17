/* InstJob: one install or uninstall, from looking at the package to the end, and the rules for it.
 *
 * Three kinds: a .deb file, a package from the software sources by name, and an uninstall by name. Every job goes
 * the same way, whoever asked for it (a tap on a file in Files, the app's own file chooser, the command line, or
 * another app through the D-Bus API in inst-service.h):
 *
 *   READING   the file's fields, or the package looked up in the software sources (refreshed first when their
 *             lists are old), or the installed package, and the app in it (name, icon, description)
 *   CHECKING  PackageKit's dry run (the SIMULATE flag: no password, nothing changes), which says what else would be
 *             installed or removed with it, and whether its dependencies can be met at all
 *   READY     the plan is known and waits for a yes: inst_job_start. (Or there is nothing to do: the same version
 *             is installed and cannot be installed over itself; inst_job_can_start is FALSE.)
 *   REFUSED   it must not or cannot be done (it would remove part of VibeOS or something the system needs, a
 *             dependency cannot be met, no such package): inst_job_get_text says why
 *   RUNNING   the real transaction: polkit asks for the password (the shell's agent), then the download, then dpkg
 *   DONE, FAILED, CANCELLED
 *
 * This file is the one place that decides: what counts as a newer or older version (dpkg's rules, inst-version.h),
 * which packages must never be removed, what needs an explicit yes (inst_job_needs_question), and what each failure
 * means in plain words. The window and the sheet only show it.
 *
 * Nothing here handles a password: PackageKit asks polkit, polkit asks the session's agent. ONLY_TRUSTED is never
 * set, so a local .deb file is installed with polkit's "install untrusted" action (the administrator's password
 * every time), which is right for a file from anywhere. */
#pragma once

#include <gio/gio.h>

G_BEGIN_DECLS

typedef enum {
  INST_JOB_FILE,          /* install a .deb file */
  INST_JOB_PACKAGE,       /* install a package from the software sources */
  INST_JOB_REMOVE,        /* uninstall an installed package */
} InstJobKind;

typedef enum {
  INST_PHASE_READING,
  INST_PHASE_CHECKING,
  INST_PHASE_READY,
  INST_PHASE_REFUSED,
  INST_PHASE_RUNNING,
  INST_PHASE_DONE,
  INST_PHASE_FAILED,
  INST_PHASE_CANCELLED,
} InstPhase;

typedef enum {
  INST_CHANGE_NEW,        /* not installed yet */
  INST_CHANGE_UPGRADE,    /* replaces an older installed version */
  INST_CHANGE_DOWNGRADE,  /* replaces a NEWER installed version with this older one */
  INST_CHANGE_REINSTALL,  /* the same version again */
  INST_CHANGE_SAME,       /* the same version is installed, and PackageKit cannot install it over itself */
  INST_CHANGE_REMOVE,     /* uninstall */
} InstChange;

/* What the job will do, as far as it knows so far (fields fill in during READING and CHECKING) */
typedef struct {
  InstChange  change;
  char       *package;            /* the package's name */
  char       *version;            /* the version it installs (the file's, the software sources'), or removes */
  char       *architecture;
  char       *installed_version;  /* what is installed now, or NULL */
  char       *name;               /* the app's name, or NULL: show the package's name */
  char       *summary;            /* one line */
  char       *description;        /* paragraphs */
  char       *homepage, *maintainer;
  GIcon      *icon;               /* the app's icon, or NULL */
  char       *desktop_id;         /* once installed, the app to open (NULL: nothing to open) */
  guint64     download_size;      /* bytes to download for the package itself (0 for a file) */
  guint64     file_size;          /* the .deb file's size */
  guint64     installed_size;     /* the package's size once installed */
  guint64     freed_size;         /* an uninstall: the space it gives back, the other packages' included */
  GPtrArray  *also_install;       /* char *: other packages it installs or updates */
  guint64     also_size;          /* their download */
  GPtrArray  *also_remove;        /* char *: other packages it removes */
} InstPlan;

#define INST_TYPE_JOB (inst_job_get_type ())
G_DECLARE_FINAL_TYPE (InstJob, inst_job, INST, JOB, GObject)

InstJob    *inst_job_new_file    (GFile *file);
InstJob    *inst_job_new_package (const char *package);
InstJob    *inst_job_new_remove  (const char *package);

/* READING and CHECKING, up to READY or REFUSED (or FAILED, when PackageKit cannot even be asked) */
void        inst_job_prepare     (InstJob *job);
/* the yes: RUNNING, up to DONE, FAILED or CANCELLED */
void        inst_job_start       (InstJob *job);
/* no, or stop: before RUNNING always; while RUNNING only while inst_job_can_cancel (PackageKit cannot stop dpkg) */
void        inst_job_cancel      (InstJob *job);

guint       inst_job_get_id      (InstJob *job);
InstJobKind inst_job_get_kind    (InstJob *job);
InstPhase   inst_job_get_phase   (InstJob *job);
const InstPlan *inst_job_get_plan (InstJob *job);
/* the name to call it by: the app's, else the package's */
const char *inst_job_get_display_name (InstJob *job);
gboolean    inst_job_is_finished (InstJob *job);
/* READY with something to do */
gboolean    inst_job_can_start   (InstJob *job);
/* Whether the user must say yes to this plan explicitly, beyond asking for it: a file (anything can be in it), a
 * replacement (upgrade, downgrade, reinstall), other packages installed or removed with it, an uninstall. Another
 * app's InstallPackage of a new package with nothing else to install goes straight to polkit. */
gboolean    inst_job_needs_question (InstJob *job);
gboolean    inst_job_can_cancel  (InstJob *job);

/* progress: 0 to 100, or -1 when not known */
int         inst_job_get_percent (InstJob *job);
/* what is happening, as the screen says it ("Waiting for your password") and as the API names it
 * ("waiting-for-auth"; the list is in inst-service.h) */
const char *inst_job_get_status_text (InstJob *job);
const char *inst_job_get_status_id   (InstJob *job);

/* REFUSED, FAILED, CANCELLED and DONE: a title ("Not installed"), a sentence saying why or what now, and
 * PackageKit's own words for the curious (or NULL) */
const char *inst_job_get_title   (InstJob *job);
const char *inst_job_get_text    (InstJob *job);
const char *inst_job_get_details (InstJob *job);
/* finished: "done", "refused", "failed" or "cancelled" */
const char *inst_job_get_result  (InstJob *job);

/* Signals: "changed" () on any change; "finished" () once, when the job is finished (also READY with nothing to
 * do is not finished: the window shows it; inst-service.c ends such a job itself) */

/* why `package` must never be removed by App Installer (part of VibeOS, of the shell or the session, or something
 * the system needs), or NULL */
char       *inst_job_protected_reason (const char *package);

G_END_DECLS
