/* navi-cameras: speed cameras for the approach alert, and where the alerts are allowed.
 *
 * The cameras come from Navi's overlay archive on our server (navi.vibeos.top/tiles/navi-overlay, built from
 * OpenStreetMap: highway=speed_camera nodes and type=enforcement relations), its "cameras" layer read at zoom 10
 * along the route or around the car. A camera has a kind (speed, red light, section), the direction of travel it
 * checks when OSM says (degrees), and its limit when tagged.
 *
 * Where they may be used is a per-country table (navi_cameras_policy): the one place to change it. The map's camera
 * icons and the alerts both follow it, from the country the road is in (Valhalla's admins, see navi-roadinfo.h). */
#pragma once

#include "navi-route.h"

G_BEGIN_DECLS

typedef enum {
  NAVI_CAMS_OFF,   /* no icons, no alerts */
  NAVI_CAMS_ZONES, /* no icons; "danger zones" that never say where the camera is (France) */
  NAVI_CAMS_ON,    /* icons and alerts */
} NaviCamPolicy;

/* the policy for a country ("DE"); NULL or "" (not known yet) is OFF */
NaviCamPolicy navi_cameras_policy (const char *country);

typedef enum {
  NAVI_CAM_SPEED,
  NAVI_CAM_RED_LIGHT,
  NAVI_CAM_SECTION,
} NaviCamKind;

typedef struct {
  double lat, lng;
  int dir;        /* the direction of travel it checks, degrees; -1 when not known (both ways) */
  int maxspeed;   /* km/h, 0 when not tagged */
  NaviCamKind kind;
  double s;       /* along a route: metres from its start (navi_cameras_on_route) */
} NaviSpeedCam;

/* adds the cameras of one overlay tile (z, x, y) to `cams` (NaviSpeedCam); FALSE when it is no vector tile */
gboolean navi_cameras_parse_tile (const guint8 *data, gsize length, int z, int x, int y, GArray *cams);

/* the cameras of `cams` on the route: within 35 m of it and, when their direction is known, facing the way the
 * route goes there (within 60 degrees); with `s` set, in order along it, each once */
GArray *navi_cameras_on_route (const NaviRoute *route, GArray *cams);

/* the overlay tiles (z10) along a route, or the 3 x 3 around a point; then their cameras */
void navi_cameras_fetch_route_async (NaviRoute *route, GCancellable *cancellable, GAsyncReadyCallback callback,
                                     gpointer user_data);
void navi_cameras_fetch_around_async (double lat, double lng, GCancellable *cancellable, GAsyncReadyCallback callback,
                                      gpointer user_data);
/* GArray of NaviSpeedCam (maybe empty: no camera there, or no overlay on the server) */
GArray *navi_cameras_fetch_finish (GAsyncResult *result, GError **error);

/* how far ahead the alert comes: about 12 s at the car's speed, 300 m at least and 800 m at most */
double navi_cameras_alert_distance (double speed);

/* France's danger zone for a camera `s` metres along: a stretch of 300 m (a limit of 50 or less), 2 km (up to 90) or
 * 4 km, that holds the camera somewhere the alert does not say (its place in the zone comes from the camera's
 * position, so it is the same every time) */
void navi_cameras_zone (const NaviSpeedCam *cam, int limit_kmh, double *from, double *to);

G_END_DECLS
