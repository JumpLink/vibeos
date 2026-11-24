/* navi-sun: the sun's position from the low-precision formulas of the Astronomical Almanac (about 0.01 degree from
 * 1950 to 2050), which is minutes at sunrise and sunset: plenty for a map's colours. */
#include "navi-sun.h"

#include <math.h>

#define RAD (G_PI / 180.0)

double
navi_sun_elevation (double lat, double lng, gint64 unix_time)
{
  double d = unix_time / 86400.0 + 2440587.5 - 2451545.0; /* days since J2000.0 */
  double g = fmod (357.529 + 0.98560028 * d, 360) * RAD;  /* mean anomaly */
  double q = fmod (280.459 + 0.98564736 * d, 360);        /* mean longitude */
  double l = (q + 1.915 * sin (g) + 0.020 * sin (2 * g)) * RAD;
  double e = (23.439 - 0.00000036 * d) * RAD;              /* obliquity */
  double ra = atan2 (cos (e) * sin (l), cos (l));
  double dec = asin (sin (e) * sin (l));
  double gmst = fmod (18.697374558 + 24.06570982441908 * d, 24); /* hours */
  double ha = gmst * 15 * RAD + lng * RAD - ra;
  double phi = lat * RAD;
  return asin (sin (phi) * sin (dec) + cos (phi) * cos (dec) * cos (ha)) / RAD;
}

gboolean
navi_sun_is_night (double lat, double lng, gint64 unix_time)
{
  return navi_sun_elevation (lat, lng, unix_time) < -0.833;
}
