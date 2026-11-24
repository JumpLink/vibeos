/* cam-widgets: the camera screen's small drawn pieces.
 *
 *   CamRotator      a child turned in place with the phone: 300 ms accelerate-decelerate along the shortest way
 *   CamIcon         a symbolic icon at a size, white, dark grey when insensitive (the bar buttons' tint list)
 *   CamShutter      the shutter: backplate, record disc → square, capture disc; its five morphs
 *   CamFlip         the flip-camera button: the arrows turn 0 → −180° in 500 ms accelerate-decelerate on each flip
 *   CamPause        pause / resume, its glyphs morphing into each other
 *   CamGallery      the gallery card: placeholder icon or the newest capture, 200 ms crossfade
 *   CamLens         one lens button
 *   CamCountdown    the timer's digit
 *   CamZoomSlider   the zoom slider
 *   CamIsland       the info island
 *   CamToast        a toast: the app icon and a message, at the bottom, for 2 s
 */
#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

/* the clockwise turn that keeps something upright on a phone turned by `orientation` */
#define CAM_COMPENSATION(orientation) ((360 - (orientation)) % 360)

#define CAM_TYPE_ROTATOR (cam_rotator_get_type ())
G_DECLARE_FINAL_TYPE (CamRotator, cam_rotator, CAM, ROTATOR, GtkWidget)
GtkWidget *cam_rotator_new          (GtkWidget *child);
GtkWidget *cam_rotator_get_child    (CamRotator *self);
/* degrees clockwise */
void       cam_rotator_set_angle    (CamRotator *self, double angle, gboolean animate);

#define CAM_TYPE_ICON (cam_icon_get_type ())
G_DECLARE_FINAL_TYPE (CamIcon, cam_icon, CAM, ICON, GtkWidget)
GtkWidget *cam_icon_new             (const char *icon_name, double size);
void       cam_icon_set_icon        (CamIcon *self, const char *icon_name);
/* NULL: white, the camera's dark grey when insensitive */
void       cam_icon_set_color       (CamIcon *self, const GdkRGBA *color);

#define CAM_TYPE_SHUTTER (cam_shutter_get_type ())
G_DECLARE_FINAL_TYPE (CamShutter, cam_shutter, CAM, SHUTTER, GtkWidget)
GtkWidget *cam_shutter_new          (void);
/* photo ↔ video */
void       cam_shutter_set_video    (CamShutter *self, gboolean video, gboolean animate);
/* a recording's start and end: the disc turns into a square and back */
void       cam_shutter_set_recording (CamShutter *self, gboolean recording, gboolean animate);
/* a photo taken: both discs shrink and grow back */
void       cam_shutter_pulse        (CamShutter *self);
/* Signals: "clicked" */

#define CAM_TYPE_FLIP (cam_flip_get_type ())
G_DECLARE_FINAL_TYPE (CamFlip, cam_flip, CAM, FLIP, GtkWidget)
GtkWidget *cam_flip_new             (void);
void       cam_flip_spin            (CamFlip *self);
/* Signals: "clicked" */

#define CAM_TYPE_PAUSE (cam_pause_get_type ())
G_DECLARE_FINAL_TYPE (CamPause, cam_pause, CAM, PAUSE, GtkWidget)
GtkWidget *cam_pause_new            (void);
void       cam_pause_set_paused     (CamPause *self, gboolean paused, gboolean animate);
/* Signals: "clicked" */

#define CAM_TYPE_GALLERY (cam_gallery_get_type ())
G_DECLARE_FINAL_TYPE (CamGallery, cam_gallery, CAM, GALLERY, GtkWidget)
GtkWidget *cam_gallery_new          (void);
/* NULL shows the placeholder icon */
void       cam_gallery_set_thumbnail (CamGallery *self, GdkTexture *texture);
/* Signals: "clicked" */

#define CAM_TYPE_LENS (cam_lens_get_type ())
G_DECLARE_FINAL_TYPE (CamLens, cam_lens, CAM, LENS, GtkWidget)
GtkWidget *cam_lens_new             (const char *text, gboolean selected);
/* Signals: "clicked" */

#define CAM_TYPE_COUNTDOWN (cam_countdown_get_type ())
G_DECLARE_FINAL_TYPE (CamCountdown, cam_countdown, CAM, COUNTDOWN, GtkWidget)
GtkWidget *cam_countdown_new        (void);
/* N, N-1 … 1, a second each; then "finished" */
void       cam_countdown_start      (CamCountdown *self, int seconds);
void       cam_countdown_cancel     (CamCountdown *self);
gboolean   cam_countdown_is_running (CamCountdown *self);
void       cam_countdown_set_angle  (CamCountdown *self, double angle, gboolean animate);
/* Signals: "finished" */

#define CAM_TYPE_ZOOM_SLIDER (cam_zoom_slider_get_type ())
G_DECLARE_FINAL_TYPE (CamZoomSlider, cam_zoom_slider, CAM, ZOOM_SLIDER, GtkWidget)
GtkWidget *cam_zoom_slider_new      (void);
/* the track position 0 … 1 and the thumb's text */
void       cam_zoom_slider_set      (CamZoomSlider *self, double progress, const char *text);
void       cam_zoom_slider_set_angle (CamZoomSlider *self, double angle);
/* Signals: "changed" (double progress): the user moved it */

typedef enum {
  CAM_ISLAND_THERMAL,
  CAM_ISLAND_THERMAL_ERROR,
  CAM_ISLAND_BATTERY,
  CAM_ISLAND_BATTERY_CHARGING,
  CAM_ISLAND_MIC_OFF,
} CamIslandItem;

#define CAM_TYPE_ISLAND (cam_island_get_type ())
G_DECLARE_FINAL_TYPE (CamIsland, cam_island, CAM, ISLAND, GtkWidget)
GtkWidget *cam_island_new           (void);
/* the items shown, a bit mask of 1 << CamIslandItem */
void       cam_island_set_items     (CamIsland *self, guint items);
/* the corner it sits in (the user's top-left) and its turn, for a phone turned by `orientation` */
void       cam_island_set_orientation (CamIsland *self, int orientation);
int        cam_island_get_orientation (CamIsland *self);

#define CAM_TYPE_TOAST (cam_toast_get_type ())
G_DECLARE_FINAL_TYPE (CamToast, cam_toast, CAM, TOAST, GtkWidget)
GtkWidget *cam_toast_new            (void);
void       cam_toast_show           (CamToast *self, const char *text);

G_END_DECLS
