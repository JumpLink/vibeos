/* inst-service: the vibeos.Installer1 D-Bus interface, App Installer's API for other apps (Software first).
 *
 * The interface, its options, statuses and results are documented in data/vibeos.Installer1.xml, which is also its
 * introspection data (installed in /usr/share/dbus-1/interfaces). In short:
 *
 *   InstallPackage (s name, a{sv} options) -> u job
 *   InstallFile    (s path, a{sv} options) -> u job
 *   RemovePackage  (s name, a{sv} options) -> u job
 *   Cancel         (u job)
 *   GetState       (s name) -> (s state, s installed_version, s available_version)
 *   signal Progress (u job, i percent, s status)
 *   signal Finished (u job, s result, s message)
 *
 * Each job is an InstJob (inst-job.h): the same rules as the window's. A question (inst_job_needs_question) is put
 * to the user in an InstSheet over the caller's window; nothing else of App Installer's shows. The application is
 * held while a job runs. */
#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

/* exports the interface on `connection` at `path` (the application's own path) */
gboolean inst_service_register   (GtkApplication *app, GDBusConnection *connection, const char *path,
                                  GError **error);
void     inst_service_unregister (void);

/* the demo's api= setting: as if another app had called (install:NAME, file:PATH or remove:NAME) */
void     inst_service_demo_call  (GtkApplication *app, const char *call);

G_END_DECLS
