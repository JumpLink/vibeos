/* navi-sun: where the sun is, for "Dark mode: only at night". */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

/* The sun's elevation above the horizon in degrees at (lat, lng) at `unix_time` (seconds) */
double navi_sun_elevation (double lat, double lng, gint64 unix_time);
/* Night: the sun below the horizon (sunset to sunrise, the disc's upper edge with refraction, -0.833 degrees) */
gboolean navi_sun_is_night (double lat, double lng, gint64 unix_time);

G_END_DECLS
