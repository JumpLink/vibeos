/* NaviCompass: which way the screen's top points, from iio-sensor-proxy's compass.
 *
 * GPS gives a heading only while the car moves (GeoClue's Heading is the course over ground): standing, or slower
 * than a walk, Navi had nothing to turn the arrow with and it did not point ("the compass doesn't point", POCO
 * 2026-11-25). iio-sensor-proxy offers the magnetometer's heading (net.hadess.SensorProxy.Compass, CompassHeading,
 * degrees from magnetic north of the device's top edge as it stands in portrait); on a screen turned to landscape
 * the top edge is the device's left or right one, which its accelerometer's orientation tells.
 *
 * The compass is claimed only while Navi asks for it (its window is shown) and released after, as iio-sensor-proxy
 * wants: the sensor runs only while someone holds a claim. No sensor, no system bus (an app sandbox): no heading,
 * and Navi goes on with GPS's alone.
 *
 * Magnetic north, not true north: the declination (3-4 degrees in Germany) is not added. */
#pragma once

#include <gio/gio.h>

G_BEGIN_DECLS

#define NAVI_TYPE_COMPASS (navi_compass_get_type ())
G_DECLARE_FINAL_TYPE (NaviCompass, navi_compass, NAVI, COMPASS, GObject)

NaviCompass *navi_compass_new (void);

/* claims the compass (and the accelerometer, for the screen's turn) while TRUE, releases them when FALSE */
void     navi_compass_set_active (NaviCompass *self, gboolean active);

/* whether the screen is shown in landscape (Navi knows it from its monitor): then the top edge is a side one */
void     navi_compass_set_landscape (NaviCompass *self, gboolean landscape);

/* the screen top's heading in degrees [0, 360); FALSE while there is none */
gboolean navi_compass_get_heading (NaviCompass *self, double *heading);

/* Signals: "changed" (): the heading moved by a degree or more, or came or went. */

/* ---- the arithmetic, for the tests ---- */

/* clockwise degrees from the device's top edge to the edge at the top of the screen: 0 in portrait (a portrait
 * screen does not say which way up it is, and with the rotation locked it does not follow the accelerometer); in
 * landscape by iio-sensor-proxy's AccelerometerOrientation: "right-up" 90, "left-up" 270 (a phone on its side),
 * "normal" 0 and "bottom-up" 180 (a tablet whose own top is a long edge); -1 while it is "undefined" */
int      navi_compass_screen_turn (gboolean landscape, const char *orientation);

/* the heading of the screen's top: the device's turned by `turn`, in [0, 360) */
double   navi_compass_screen_heading (double device_heading, int turn);

typedef enum {
  NAVI_HEADING_NONE,
  NAVI_HEADING_COURSE,   /* GPS's: the car moves */
  NAVI_HEADING_COMPASS,  /* the compass's: it stands, or creeps */
} NaviHeadingSource;

/* Which way the arrow points: GPS's course while the car moves (faster than `moving` m/s, with a course >= 0), else
 * the compass's, else none. `heading` is set unless NONE. */
NaviHeadingSource navi_heading_pick (double speed, double course, double moving, gboolean have_compass,
                                     double compass, double *heading);

/* Whether Navi holds the compass (and the accelerometer): while its window is `shown`, except while the car drives
 * along the route (`on_route`, in drive mode) faster than a walk. The route and GPS's course turn the arrow then,
 * and the sensors only cost power. With a margin: let go above 3 m/s, taken again below 1.5 (or off the route), so
 * a car crawling in a queue does not claim and release it at every fix. `held`: whether it is held now. */
#define NAVI_COMPASS_RELEASE_SPEED 3.0
#define NAVI_COMPASS_CLAIM_SPEED   1.5
gboolean navi_compass_wanted (gboolean shown, gboolean on_route, double speed, gboolean held);

G_END_DECLS
