/* cam-devices: the phone's cameras, as libcamera reports them, and the flash LED.
 *
 * The cameras come from libcamera's own GStreamer device provider ("libcameraprovider"), which gives each camera's
 * id (libcamerasrc's camera-name), its properties (Location, Rotation, PixelArraySize, Model) and its stream formats.
 * Nothing here knows a phone: front and back are properties::Location, the rotation is properties::Rotation, the
 * sizes are what libcamera lists. A wrong value there is the kernel's (the sensor's devicetree "orientation" and
 * "rotation") to fix, not the app's.
 *
 * Aperture shows the main camera of each facing, and the others only with the device overlay's
 * config_enableAuxCameras, and then by their zoom ratio from the lens' focal length. libcamera reports no focal
 * length, so there is no honest ratio to give a second back camera: this build shows the main camera of each facing
 * (the one with the most pixels), as Aperture does without the overlay. */
#pragma once

#include <gst/gst.h>

G_BEGIN_DECLS

typedef enum {
  CAM_FACING_BACK,
  CAM_FACING_FRONT,
  CAM_FACING_EXTERNAL,
} CamFacing;

typedef struct {
  char      *id;                /* libcamera's camera id: libcamerasrc's camera-name */
  char      *model;             /* properties::Model, or NULL */
  CamFacing  facing;            /* properties::Location */
  int        orientation;       /* clockwise degrees that turn a frame upright on the phone held upright: Android's
                                 * SENSOR_ORIENTATION, from properties::Rotation (anticlockwise) */
  int        pixels_w, pixels_h;/* properties::PixelArraySize, 0 if not reported */
  GstCaps   *caps;              /* the stream formats */
  gboolean   flash;             /* a flash LED to light for it (back-facing, and the phone has one) */
} CamCamera;

/* All cameras libcamera reports, in its order (CamCamera *, owned by the array). Never NULL; empty if none. */
GPtrArray *cam_devices_enumerate   (void);
/* The camera Aperture would take for the facing: the one with the largest pixel array; NULL if none faces so */
CamCamera *cam_devices_main_for    (GPtrArray *cameras, CamFacing facing);

/* A stream size of aspect aw:ah (4:3, 16:9) near `pixels` pixels among the sizes the camera lists (exact sizes
 * first, then its ranges), never larger than it can give. FALSE if it lists none of that aspect. */
gboolean   cam_camera_pick_size    (CamCamera *camera, int aw, int ah, int pixels, int *width, int *height);
/* TRUE if the camera lists exactly width × height */
gboolean   cam_camera_has_size     (CamCamera *camera, int width, int height);

typedef enum {
  CAM_LED_OFF,
  CAM_LED_TORCH,                /* a quarter of its range, never below gmobile's GM_TORCH_MIN_BRIGHTNESS */
  CAM_LED_FLASH,                /* all of it, for a photo's second */
} CamLed;

/* The kernel's flash LED: the LED class device whose function (the last ':' field of its name) is "torch" or
 * "flash"; NULL for a phone without one */
const char *cam_flash_led          (void);
/* Sets it through logind (org.freedesktop.login1.Session.SetBrightness: the active session may set its seat's LEDs)
 * and warns when sysfs then disagrees. The _sync one waits for logind: the last OFF before the app goes. */
void        cam_flash_led_set      (CamLed led);
void        cam_flash_led_set_sync (CamLed led);
/* sends what is asked for and not yet sent, before the app goes */
void        cam_flash_led_flush    (void);
/* what sysfs reads: TRUE while it is lit */
gboolean    cam_flash_led_is_lit   (void);

/* the phone's maker and model for the photos' EXIF (DMI, else the devicetree's model), never NULL */
const char *cam_device_make        (void);
const char *cam_device_model       (void);

G_END_DECLS
