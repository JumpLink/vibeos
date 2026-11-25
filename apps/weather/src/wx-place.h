/* Where a forecast is asked for. The phone's own position is rounded to WX_STEP_DEG (0.1°, about 11 km north to
 * south) the moment GeoClue gives it, before it is named, kept or sent, so the fix itself never leaves the phone;
 * and every request that carries a position (Open-Meteo's forecast, Nominatim's name for it) carries it rounded so,
 * a chosen city's as well. */
#pragma once

#include <glib.h>

#define WX_STEP_DEG 0.1

/* deg to the nearest 0.1° (halves away from zero), never -0 */
double wx_round_deg (double deg);
/* deg rounded, as the requests write it: one decimal, a point whatever the locale ("52.5", "-0.1", "0.0") */
char *wx_deg_text (double deg);
/* Open-Meteo's forecast for the rounded position: now, 24 hours, 8 days, and yesterday */
char *wx_forecast_url (double lat, double lon);
/* Nominatim's name for the rounded position, at city level (zoom 10: a suburb's name means nothing for a point
 * rounded to 0.1°), in language lang */
char *wx_reverse_url (double lat, double lon, const char *lang);
