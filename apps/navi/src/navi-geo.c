/* navi-geo: see navi-geo.h. */
#include "navi-geo.h"

#include <math.h>
#include <string.h>

#define EARTH_R 6371008.8
#define RAD (G_PI / 180.0)

double
navi_geo_distance (double lat1, double lng1, double lat2, double lng2)
{
  double p1 = lat1 * RAD, p2 = lat2 * RAD, dp = p2 - p1, dl = (lng2 - lng1) * RAD;
  double a = sin (dp / 2) * sin (dp / 2) + cos (p1) * cos (p2) * sin (dl / 2) * sin (dl / 2);
  return 2 * EARTH_R * atan2 (sqrt (a), sqrt (MAX (0.0, 1 - a)));
}

double
navi_geo_bearing (double lat1, double lng1, double lat2, double lng2)
{
  double p1 = lat1 * RAD, p2 = lat2 * RAD, dl = (lng2 - lng1) * RAD;
  double y = sin (dl) * cos (p2), x = cos (p1) * sin (p2) - sin (p1) * cos (p2) * cos (dl);
  double b = atan2 (y, x) / RAD;
  return b < 0 ? b + 360 : b;
}

void
navi_geo_offset (double lat, double lng, double bearing, double distance, double *lat2, double *lng2)
{
  double d = distance / EARTH_R, b = bearing * RAD, p1 = lat * RAD, l1 = lng * RAD;
  double p2 = asin (sin (p1) * cos (d) + cos (p1) * sin (d) * cos (b));
  double l2 = l1 + atan2 (sin (b) * sin (d) * cos (p1), cos (d) - sin (p1) * sin (p2));
  *lat2 = p2 / RAD;
  *lng2 = fmod (l2 / RAD + 540, 360) - 180;
}

double
navi_angle_diff (double a, double b)
{
  return remainder (a - b, 360);
}

/* ---- paths ---- */

gboolean
navi_polyline_decode (const char *encoded, int precision, GArray *points)
{
  double factor = pow (10, precision);
  gint64 lat = 0, lng = 0;
  const char *p = encoded;
  while (*p) {
    gint64 v[2];
    for (int k = 0; k < 2; k++) {
      gint64 result = 0;
      int shift = 0, b;
      do {
        if (!*p)
          return FALSE;
        b = *p++ - 63;
        if (b < 0 || b > 63)
          return FALSE;
        result |= (gint64) (b & 0x1f) << shift;
        shift += 5;
      } while (b >= 0x20 && shift < 64);
      v[k] = (result & 1) ? ~(result >> 1) : (result >> 1);
    }
    lat += v[0];
    lng += v[1];
    NaviPathPoint pt = { lat / factor, lng / factor, 0 };
    g_array_append_val (points, pt);
  }
  navi_path_measure ((NaviPathPoint *) points->data, points->len);
  return TRUE;
}

void
navi_path_measure (NaviPathPoint *points, guint n)
{
  if (n)
    points[0].s = 0;
  for (guint i = 1; i < n; i++)
    points[i].s = points[i - 1].s +
                  navi_geo_distance (points[i - 1].lat, points[i - 1].lng, points[i].lat, points[i].lng);
}

void
navi_path_point_at (const NaviPathPoint *points, guint n, double s, double *lat, double *lng, guint *hint)
{
  if (n == 0) {
    *lat = *lng = NAN;
    return;
  }
  if (n == 1 || s <= 0) {
    *lat = points[0].lat;
    *lng = points[0].lng;
    if (hint)
      *hint = 0;
    return;
  }
  if (s >= points[n - 1].s) {
    *lat = points[n - 1].lat;
    *lng = points[n - 1].lng;
    if (hint)
      *hint = n - 2;
    return;
  }
  /* the segment i .. i+1 with points[i].s <= s < points[i+1].s, from the hint either way */
  guint i = hint && *hint < n - 1 ? *hint : 0;
  while (i > 0 && points[i].s > s)
    i--;
  while (i < n - 2 && points[i + 1].s <= s)
    i++;
  if (hint)
    *hint = i;
  const NaviPathPoint *a = &points[i], *b = &points[i + 1];
  double f = b->s > a->s ? (s - a->s) / (b->s - a->s) : 0;
  *lat = a->lat + (b->lat - a->lat) * f;
  *lng = a->lng + (b->lng - a->lng) * f;
}

double
navi_path_heading_at (const NaviPathPoint *points, guint n, double s, double ahead)
{
  if (n < 2)
    return 0;
  double total = points[n - 1].s;
  double s1 = MIN (s + ahead, total), s0 = MIN (s, s1 - MIN (ahead, 1.0));
  double la, lo, lb, lob;
  guint h = 0;
  navi_path_point_at (points, n, s0, &la, &lo, &h);
  navi_path_point_at (points, n, s1, &lb, &lob, &h);
  if (navi_geo_distance (la, lo, lb, lob) < 0.2) {
    /* a zero-length end: the last real segment */
    for (guint i = n - 1; i > 0; i--)
      if (points[i].s - points[i - 1].s > 0.2)
        return navi_geo_bearing (points[i - 1].lat, points[i - 1].lng, points[i].lat, points[i].lng);
    return 0;
  }
  return navi_geo_bearing (la, lo, lb, lob);
}

double
navi_path_project (const NaviPathPoint *points, guint n, double lat, double lng, double s_from, double s_to,
                   double *s, double *bearing)
{
  double best = G_MAXDOUBLE, best_s = 0, best_b = 0;
  /* metres east and north of the point, on a plane: exact enough within a few km */
  double ky = EARTH_R * RAD, kx = ky * cos (lat * RAD);
  for (guint i = 0; i + 1 < n; i++) {
    const NaviPathPoint *a = &points[i], *b = &points[i + 1];
    if (b->s < s_from || a->s > s_to)
      continue;
    double ax = (a->lng - lng) * kx, ay = (a->lat - lat) * ky;
    double bx = (b->lng - lng) * kx, by = (b->lat - lat) * ky;
    double dx = bx - ax, dy = by - ay, len2 = dx * dx + dy * dy;
    double t = len2 > 0 ? CLAMP (-(ax * dx + ay * dy) / len2, 0.0, 1.0) : 0;
    double px = ax + dx * t, py = ay + dy * t, d = hypot (px, py);
    if (d < best) {
      best = d;
      best_s = a->s + (b->s - a->s) * t;
      best_b = len2 > 0 ? fmod (atan2 (dx, dy) / RAD + 360, 360) : best_b;
    }
  }
  if (s)
    *s = best_s;
  if (bearing)
    *bearing = best_b;
  return best;
}

/* The closed form of a critically damped spring (as Unity's SmoothDamp has it) */
double
navi_smooth_value (double current, double target, double *rate, double smooth_s, double dt)
{
  double omega = 2 / MAX (smooth_s, 1e-3), x = omega * dt;
  double k = 1 / (1 + x + 0.48 * x * x + 0.235 * x * x * x);
  double change = current - target;
  double temp = (*rate + omega * change) * dt;
  *rate = (*rate - omega * temp) * k;
  return target + (change + temp) * k;
}

double
navi_smooth_angle (double current, double target, double *rate, double smooth_s, double dt)
{
  target = current + remainder (target - current, 360);
  return fmod (navi_smooth_value (current, target, rate, smooth_s, dt) + 720, 360);
}

/* ---- what the driver reads ---- */

void
navi_format_distance_parts (double m, char **number, const char **unit)
{
  if (m < 0)
    m = 0;
  if (m < 950) {
    /* 10 m steps under 300 m (the turns), 50 m above */
    long v = m < 300 ? lround (m / 10) * 10 : lround (m / 50) * 50;
    *number = g_strdup_printf ("%ld", v);
    *unit = "m";
  } else if (m < 9950) {
    *number = g_strdup_printf ("%.1f", m / 1000);
    *unit = "km";
  } else {
    *number = g_strdup_printf ("%ld", lround (m / 1000));
    *unit = "km";
  }
}

char *
navi_format_distance (double m)
{
  g_autofree char *number = NULL;
  const char *unit;
  navi_format_distance_parts (m, &number, &unit);
  return g_strdup_printf ("%s %s", number, unit);
}

char *
navi_format_duration (double seconds)
{
  long min = MAX (1L, lround (seconds / 60));
  if (min < 60)
    return g_strdup_printf ("%ld min", min);
  if (min % 60 == 0)
    return g_strdup_printf ("%ld h", min / 60);
  return g_strdup_printf ("%ld h %ld min", min / 60, min % 60);
}

static gboolean clock_24h = TRUE;

void
navi_format_set_24h (gboolean use_24h)
{
  clock_24h = use_24h;
}

char *
navi_format_eta (double seconds_from_now)
{
  g_autoptr (GDateTime) now = g_date_time_new_now_local ();
  g_autoptr (GDateTime) t = g_date_time_add_seconds (now, seconds_from_now);
  return g_date_time_format (t, clock_24h ? "%H:%M" : "%l:%M %p");
}
