/* InstWindow: App Installer's window.
 *
 * Opened from the app drawer it starts at a page that says what the app does, with "Choose a package" (the GTK file
 * dialog, showing .deb files). A package (chosen there, tapped in Files, or named on the command line) opens its job
 * page over it: the app, the question, the progress and how it ended (inst-view.h).
 *
 * Back (the shell's gesture, Alt+Left) and Escape: from a job page, back to the start page, or out of the window when
 * the job was its first page; the job is cancelled on the way, unless it is running. A running install is never
 * stopped by Back: the window goes away (to the home screen) while it goes on, and a notification says how it ended.
 * Stop (while PackageKit can stop) is the way to stop it. */
#pragma once

#include "inst-job.h"

#include <gtk/gtk.h>

G_BEGIN_DECLS

GtkWidget *inst_window_new      (GtkApplication *app);
/* `job` on the window's job page (prepared here): over the start page, or as the window's only page */
void       inst_window_show_job (GtkWidget *window, InstJob *job, gboolean over_start);
/* the start page */
void       inst_window_show_start (GtkWidget *window);
/* a job of the window's is running: another one gets a window of its own */
gboolean   inst_window_is_busy  (GtkWidget *window);

G_END_DECLS
