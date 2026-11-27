/* navi-geo: distances, bearings and paths on the ground, and the numbers Navi shows (distances, durations).
 *
 * Distances are metres, bearings degrees clockwise from north, latitudes and longitudes degrees. A path is an array
 * of NaviPathPoint, each with its distance from the path's start, so a position on it is one number (s, metres from
 * the start) and stays cheap to find again. Nothing here needs GTK. */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

typedef struct {
  double lat, lng;
  double s; /* metres from the path's start */
} NaviPathPoint;

/* great-circle distance (haversine) and initial bearing */
double navi_geo_distance (double lat1, double lng1, double lat2, double lng2);
double navi_geo_bearing (double lat1, double lng1, double lat2, double lng2);
/* the point `distance` metres from (lat, lng) toward `bearing` */
void navi_geo_offset (double lat, double lng, double bearing, double distance, double *lat2, double *lng2);
/* a - b, the short way round: -180..180 */
double navi_angle_diff (double a, double b);

/* Decodes an encoded polyline (Google's format; Valhalla's shapes have `precision` 6) into path points with their
 * distances filled in. FALSE on malformed input. */
gboolean navi_polyline_decode (const char *encoded, int precision, GArray *points);
/* fills in the points' distances from the first */
void navi_path_measure (NaviPathPoint *points, guint n);

/* The point `s` metres along the path. `hint`, when not NULL, is a segment index to start looking from (it is
 * updated), which makes walking along a path in small steps O(1). */
void navi_path_point_at (const NaviPathPoint *points, guint n, double s, double *lat, double *lng, guint *hint);
/* the direction of the path at `s`: from the point at s to the one `ahead` metres further (the last stretch near the
 * end) */
double navi_path_heading_at (const NaviPathPoint *points, guint n, double s, double ahead);

/* The point on the path nearest to (lat, lng), among the segments that lie between `s_from` and `s_to` (metres along
 * the path; pass 0 and G_MAXDOUBLE for all). Returns the distance to it in metres and sets *s to its place on the
 * path and *bearing to the segment's direction. */
double navi_path_project (const NaviPathPoint *points, guint n, double lat, double lng, double s_from, double s_to,
                          double *s, double *bearing);

/* ---- the route near the car, while driving ----
 *
 * The route ahead is drawn in full colour from the car on, over the whole route drawn subdued. Cutting the route at
 * the car ten times a second made the map cut its source into tiles again ten times a second; instead the stretch
 * around the car is a line of its own, which the map measures (its line-progress), and a paint property, which
 * costs no new tiles, moves the colour's start to the car along it. The rest of the route ahead lies in another
 * line, made again only when the car nears the stretch's end. A gradient's colours are a ramp of 256, so the
 * stretch is short: 1.2 km, a ramp step under 5 m. */
#define NAVI_NEAR_BEHIND_M 100.0  /* the stretch starts this far behind the car */
#define NAVI_NEAR_AHEAD_M  1100.0 /* and ends this far ahead of it */
#define NAVI_NEAR_MOVE_M   500.0  /* a new stretch once the car is this far past where the last was made */

typedef struct {
  double s0, s1;    /* metres along the route it spans */
  guint n;
  double *lat_lng;  /* its n points, latitude and longitude, its ends cut where s0 and s1 fall */
  double *s;        /* each point's metres along the route */
  double *m;        /* each point's distance from the first along the Web Mercator projection, as MapLibre
                     * measures a line (its line-progress is m / m[n - 1]) */
} NaviPathSlice;

/* the stretch of the path between s0 and s1 metres along it (at least two points; NULL for an empty path) */
NaviPathSlice *navi_path_slice_new (const NaviPathPoint *points, guint n, double s0, double s1);
void navi_path_slice_free (NaviPathSlice *slice);
/* where the point `s` metres along the route is on the slice, as MapLibre's line-progress: 0 at its start, 1 at its
 * end */
double navi_path_slice_progress (const NaviPathSlice *slice, double s);
/* where the near stretch goes for a car `car_s` metres along a route `length` metres long */
void navi_near_span (double length, double car_s, double *s0, double *s1);
/* whether a stretch made as navi_near_span () makes it, spanning s0 to s1, has to be made again for the car at
 * car_s: the car is past NAVI_NEAR_MOVE_M into it (the ahead shrinks toward its end), or before its start */
gboolean navi_near_stale (double length, double s0, double s1, double car_s);

/* ---- when the drive loop can rest ----
 *
 * While driving the map is drawn every frame only while something on it moves: the car, the camera's springs, a
 * recentring flight. A car standing at a light with the camera settled draws nothing until the next fix. */
typedef struct {
  double speed;         /* the car's, m/s */
  double car_gap_m;     /* how far the car is drawn from where it should be */
  double zoom_rate, heading_rate, arrow_rate; /* the springs' speeds */
  double arrow_gap;     /* degrees from the arrow to the compass it follows (0 when it does not) */
  double pitch_gap;     /* degrees from the camera's pitch to the one it goes to */
  gboolean flight;      /* a recentring flight is under way */
  gboolean pending;     /* something waits for a frame (the route's colour not yet at the car) */
} NaviMotion;

/* TRUE when nothing in `m` moves: the frame clock can stop */
gboolean navi_motion_settled (const NaviMotion *m);

/* The camera's turn toward a heading: one step of a critically damped spring from `current` toward `target`
 * (degrees, the short way round), `rate` its speed in degrees a second, reaching the target in about `smooth_s`
 * without overshooting. Stable for long frames too. */
double navi_smooth_angle (double current, double target, double *rate, double smooth_s, double dt);
/* the same for a plain number */
double navi_smooth_value (double current, double target, double *rate, double smooth_s, double dt);

/* "110 m", "450 m", "1.2 km", "15 km": as a driver reads it, rounded more the farther it is; in miles "500 ft",
 * "0.3 mi", "15 mi" */
char *navi_format_distance (double metres);
/* the number and unit apart, for a banner that sets them in two sizes */
void navi_format_distance_parts (double metres, char **number, const char **unit);
/* "1 min", "15 min", "1 h 5 min" */
char *navi_format_duration (double seconds);
/* the clock time `seconds` from now, "21:18" (or "9:18 PM" where the clock is 12-hour) */
char *navi_format_eta (double seconds_from_now);
/* the clock navi_format_eta writes: 24-hour (the default) or 12-hour */
void navi_format_set_24h (gboolean use_24h);
/* distances in miles and feet (the UK's and the US's roads, or the units setting), else metres and kilometres */
void navi_format_set_miles (gboolean miles);
gboolean navi_format_uses_miles (void);

G_END_DECLS
