/* cam-sensors: what the camera screen reads from the phone besides the camera.
 *
 *   orientation  iio-sensor-proxy's AccelerometerOrientation, as Aperture's OrientationEventListener quantised to
 *                0 / 90 / 180 / 270 (the clockwise turn of the phone: 90 is its left edge up)        §17
 *   display-rotation  the turn of the phone the screen is laid out for, in the same unit: 0 while the shell keeps
 *                it upright, the orientation while it follows the phone (Mutter's DisplayConfig)
 *   tilt         the continuous roll angle for the leveler, from a raw IIO accelerometer if the session may read
 *                one (NaN otherwise, and the Leveler setting is left out)                             §14
 *   battery      UPower's display device: low (≤ 15 %) and charging                                   §13
 *   thermal      the worst of the thermal zones, each against its own trip points                     §13, §19.4
 *   location     GeoClue2, while "Save location data" is on                                           §21.3
 *
 * All of it is plain D-Bus and sysfs, nothing that knows a phone. Each part is quiet where its service is missing. */
#pragma once

#include <gio/gio.h>

G_BEGIN_DECLS

/* Android's PowerManager thermal status, which the island and the snackbars follow */
typedef enum {
  CAM_THERMAL_NONE,
  CAM_THERMAL_LIGHT,
  CAM_THERMAL_MODERATE,
  CAM_THERMAL_SEVERE,
  CAM_THERMAL_CRITICAL,
  CAM_THERMAL_EMERGENCY,
  CAM_THERMAL_SHUTDOWN,
} CamThermal;

typedef struct {
  gboolean valid;
  double   latitude, longitude;     /* degrees */
  double   altitude;                /* metres, or -G_MAXDOUBLE if unknown */
  gint64   time;                    /* unix seconds of the fix */
} CamLocation;

#define CAM_TYPE_SENSORS (cam_sensors_get_type ())
G_DECLARE_FINAL_TYPE (CamSensors, cam_sensors, CAM, SENSORS, GObject)

CamSensors *cam_sensors_new              (void);
/* claims the accelerometer and polls the rest while the camera screen is up; lets go otherwise */
void        cam_sensors_set_active       (CamSensors *self, gboolean active);
void        cam_sensors_set_locating     (CamSensors *self, gboolean locating);
/* reads the tilt continuously while the leveler shows */
void        cam_sensors_set_leveling     (CamSensors *self, gboolean leveling);

int         cam_sensors_get_orientation  (CamSensors *self);
int         cam_sensors_get_display_rotation (CamSensors *self);
gboolean    cam_sensors_has_tilt         (CamSensors *self);
double      cam_sensors_get_tilt         (CamSensors *self);
gboolean    cam_sensors_get_battery_low  (CamSensors *self);
gboolean    cam_sensors_get_charging     (CamSensors *self);
CamThermal  cam_sensors_get_thermal      (CamSensors *self);
/* GeoClue is there to ask (running or startable) */
gboolean    cam_sensors_can_locate       (CamSensors *self);
/* the last fix, if there is one */
CamLocation cam_sensors_get_location     (CamSensors *self);

/* Properties (notify): "orientation", "display-rotation", "tilt", "battery", "thermal"
 * Signals: "location-refused" (): GeoClue would not locate the app (no agent's yes, or no service) */

G_END_DECLS
