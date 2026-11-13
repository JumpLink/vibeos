/* cam-dialogs: the two dialogs of the Camera.
 *
 *   cam_list_dialog       a Settings choice: a single-choice list and Cancel; a tap on a choice answers at once
 *   cam_location_dialog   "Add photo and video locations": asked once, when "Save location data" was never set
 *                         and the phone can locate at all */
#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

/* @chosen: the index picked, or -1 for Cancel (or Back, or a tap outside) */
typedef void (*CamChoiceDone) (int chosen, gpointer data);
void cam_list_dialog     (GtkWidget *parent, const char *title, const char *const *labels, int n_labels, int selected,
                          CamChoiceDone done, gpointer data);

/* @turn_on: "Turn on"; FALSE for "Later", Back or a tap outside */
typedef void (*CamLocationDone) (gboolean turn_on, gpointer data);
void cam_location_dialog (GtkWidget *parent, CamLocationDone done, gpointer data);

G_END_DECLS
