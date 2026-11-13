/* cam-viewfinder: the preview and what is drawn over it.
 *
 * The camera's frames come as they leave the sensor; the viewfinder turns them upright by the sensor's orientation,
 * mirrors a front camera's (a selfie preview is a mirror), fits them (whole for 4:3 photos, filling the viewfinder
 * for 16:9 and video) and zooms into the middle of them. While
 * the camera is being reconfigured the last picture stays, blurred. Over it: the grid on the
 * picture's visible part, the leveler, the focus ring, the capture blink.
 *
 * Touch: a tap ("tapped"), a horizontal fling of more than 75 px ("fling", +1 for the next mode), a pinch
 * ("pinch-begin", "pinch" with the scale since it began, "pinch-end"). */
#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

typedef enum {
  CAM_GRID_OFF,
  CAM_GRID_3X3,
  CAM_GRID_4X4,
  CAM_GRID_GOLDEN,
} CamGrid;

#define CAM_TYPE_VIEWFINDER (cam_viewfinder_get_type ())
G_DECLARE_FINAL_TYPE (CamViewfinder, cam_viewfinder, CAM, VIEWFINDER, GtkWidget)

GtkWidget *cam_viewfinder_new          (GdkPaintable *paintable);
/* the frames' clockwise turn to upright, the mirror, contain or cover, and the frame size to expect */
void       cam_viewfinder_set_geometry (CamViewfinder *self, int orientation, gboolean mirror, gboolean cover,
                                        int width, int height);
void       cam_viewfinder_set_zoom     (CamViewfinder *self, double zoom);
/* keeps the picture now, blurred, until thaw */
void       cam_viewfinder_freeze       (CamViewfinder *self);
void       cam_viewfinder_thaw         (CamViewfinder *self);
gboolean   cam_viewfinder_is_frozen    (CamViewfinder *self);
void       cam_viewfinder_set_grid     (CamViewfinder *self, CamGrid grid);
/* the leveler, with the phone's tilt in degrees, NaN for none yet */
void       cam_viewfinder_set_leveler  (CamViewfinder *self, gboolean shown, double tilt);
/* the focus ring at a point: it shrinks while focusing, grows back on focus_done and goes 500 ms later */
void       cam_viewfinder_focus        (CamViewfinder *self, double x, double y);
void       cam_viewfinder_focus_done   (CamViewfinder *self);
void       cam_viewfinder_blink        (CamViewfinder *self);
/* where the picture shows, in the viewfinder's coordinates */
void       cam_viewfinder_get_picture  (CamViewfinder *self, graphene_rect_t *rect);

/* Signals: "tapped" (double x, double y), "fling" (int direction), "pinch-begin" (), "pinch" (double scale),
 * "pinch-end" () */

G_END_DECLS
