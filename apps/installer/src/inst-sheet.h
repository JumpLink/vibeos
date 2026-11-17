/* InstSheet: App Installer's question for another app's request (inst-service.h), as a bottom sheet over that app.
 *
 * A window of its own: a scrim over the caller's window and, sliding up from the bottom, a sheet with the job's
 * question (inst-view.h, as a sheet): replace, downgrade, other packages, uninstall. Given the caller's window as an
 * xdg-foreign handle ("wayland:..."), the window is made transient for it, so it shows over that app and goes with
 * it; without one it is a window of its own, over whatever is showing.
 *
 * The answer is the job's: Install (or Replace, Uninstall) starts it, Cancel, a tap on the scrim, Back and Escape
 * cancel it. Once the job has left READY, the sheet slides away and the window closes. */
#pragma once

#include "inst-job.h"

#include <gtk/gtk.h>

G_BEGIN_DECLS

GtkWidget *inst_sheet_new (GtkApplication *app, InstJob *job, const char *parent_window);

G_END_DECLS
