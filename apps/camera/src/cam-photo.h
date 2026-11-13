/* cam-photo: a photo from one frame of the stream.
 *
 * The frame is cropped to the centre 1/zoom (the digital zoom) and scaled back to the stream's size, turned upright
 * (its pixels: the file says Orientation 1, so every viewer shows it the same), mirrored for a front camera when
 * "Mirror front camera" is on, encoded as JPEG at quality 90 with its EXIF (time with sub-seconds and offset, make
 * and model, exposure and gain as libcamera reported them for the frame, the GPS fix), and written under a hidden
 * name, synced, then renamed: the gallery never sees half a file. All of it off the main thread. */
#pragma once

#include "cam-sensors.h"

#include <gst/gst.h>

G_BEGIN_DECLS

typedef struct {
  GstSample   *frame;           /* (owned) */
  int          rotation;        /* clockwise turn to upright */
  gboolean     mirror;          /* left-right, in the upright picture */
  double       zoom;            /* ≥ 1 */
  char        *path;            /* the final name */
  GDateTime   *time;            /* when the shutter fired */
  CamLocation  location;
  int          exposure_us;     /* 0 if unknown */
  double       gain;            /* analogue gain, 0 if unknown */
  gboolean     flash_fired, has_flash;
} CamPhoto;

void      cam_photo_free         (CamPhoto *photo);
G_DEFINE_AUTOPTR_CLEANUP_FUNC (CamPhoto, cam_photo_free)

/* takes `photo`; the result is the saved path */
void      cam_photo_save_async   (CamPhoto *photo, GCancellable *cancel, GAsyncReadyCallback done, gpointer data);
char     *cam_photo_save_finish  (GAsyncResult *result, GError **error);

G_END_DECLS
