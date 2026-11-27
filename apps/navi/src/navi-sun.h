/* navi-sun: where the sun is, for "Dark mode: only at night". */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

/* the map's "Dark mode" setting */
typedef enum {
  NAVI_DARK_AT_NIGHT, /* Waze's default: dark from sunset to sunrise where the car is */
  NAVI_DARK_ALWAYS,
  NAVI_DARK_NEVER,
  NAVI_DARK_SYSTEM,   /* as the phone's own light or dark mode */
} NaviDarkMode;

/* The sun's elevation above the horizon in degrees at (lat, lng) at `unix_time` (seconds) */
double navi_sun_elevation (double lat, double lng, gint64 unix_time);
/* Night: the sun below the horizon (sunset to sunrise, the disc's upper edge with refraction, -0.833 degrees) */
gboolean navi_sun_is_night (double lat, double lng, gint64 unix_time);

/* Whether the map is dark: as `mode` says (`sun_night`: the sun is down where the car is, `system_dark`: the phone
 * is in dark mode), except while `driving` (a route's guidance running) with `drive_dark` on: dark then, day or
 * night, whatever the mode (the owner's choice, 2026-11-27: "dark map while driving"). */
gboolean navi_night_pick (NaviDarkMode mode, gboolean sun_night, gboolean system_dark, gboolean driving,
                          gboolean drive_dark);

G_END_DECLS
