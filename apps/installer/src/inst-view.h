/* InstView: one job on screen, following it as it goes: the app (icon, name, version), what is happening or the
 * question, what else it takes along, its description, and the buttons for the moment.
 *
 * As a page (the App Installer window) it shows everything, scrolling, with its buttons in a bar at the bottom. As a
 * sheet (another app asked through the D-Bus API, inst-sheet.h) it shows the question only: the app, the versions
 * and what else is installed or removed, the buttons under them.
 *
 * The buttons act on the job (inst_job_start, inst_job_cancel) and open the installed app; "done" says the user is
 * finished with the view (Cancel, Done, Close, or Open once the app is opening), for whoever shows it to close it. */
#pragma once

#include "inst-job.h"

#include <gtk/gtk.h>

G_BEGIN_DECLS

typedef enum {
  INST_VIEW_PAGE,
  INST_VIEW_SHEET,
} InstViewMode;

#define INST_TYPE_VIEW (inst_view_get_type ())
G_DECLARE_FINAL_TYPE (InstView, inst_view, INST, VIEW, GtkWidget)

GtkWidget *inst_view_new     (InstJob *job, InstViewMode mode);
InstJob   *inst_view_get_job (InstView *self);

/* the app's icon at `size` px: the job's (a theme name, a file, the bytes from the .deb) or the generic package */
GtkWidget *inst_view_icon_new (GIcon *icon, int size);

/* "2.6 MB" */
char      *inst_format_size  (guint64 bytes);

/* Signals: "done" () */

G_END_DECLS
