/* cam-storage: where captures go, what they are called, and which one is newest (§21).
 *
 * Photos go to $XDG_PICTURES_DIR/Camera and videos to $XDG_VIDEOS_DIR/Camera (the owner's choice; Aperture puts
 * both in DCIM/Camera), named yyyy-MM-dd-HH-mm-ss-SSS in local time, with -1, -2 … on a collision. The newest
 * capture is the newest file of either folder with such a name (so only this app's captures, as Aperture's
 * thumbnail shows only its own), by modification time; both folders are watched. */
#pragma once

#include <gdk/gdk.h>

G_BEGIN_DECLS

#define CAM_TYPE_STORAGE (cam_storage_get_type ())
G_DECLARE_FINAL_TYPE (CamStorage, cam_storage, CAM, STORAGE, GObject)

CamStorage *cam_storage_new            (void);
/* a free name for a capture taken at `time` (made now, so two captures never get the same one) */
char       *cam_storage_new_path       (CamStorage *self, gboolean video, GDateTime *time);
/* the newest capture, or NULL */
const char *cam_storage_get_newest     (CamStorage *self);

/* its thumbnail: `size` px square, centre-cropped (a video's first frame) */
void        cam_storage_thumbnail_async  (const char *path, int size, GCancellable *cancel,
                                          GAsyncReadyCallback done, gpointer data);
GdkTexture *cam_storage_thumbnail_finish (GAsyncResult *result, GError **error);

/* opens the newest capture in the app that shows its kind, or the default image viewer when there is none;
 * FALSE if no app can */
gboolean    cam_storage_open_gallery   (CamStorage *self, GdkAppLaunchContext *context);

/* Signals: "changed" (): the newest capture is another one */

G_END_DECLS
