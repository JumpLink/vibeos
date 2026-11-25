/* Where a forecast is asked for. The phone's own position is rounded to WX_STEP_DEG (0.1°, about 11 km north to
 * south) the moment GeoClue gives it, before it is named, kept or sent, so the fix itself never leaves the phone;
 * and every request that carries a position (Open-Meteo's forecast, Nominatim's name for it) carries it rounded so,
 * a chosen city's as well. */
#pragma once

#include <glib.h>

#define WX_STEP_DEG 0.1
/* how long the page waits for GeoClue's first fix: a GNSS fix takes seconds when it was on lately, under a minute
 * with fresh orbit data, minutes from cold */
#define WX_FIX_WAIT_S 120

/* why GeoClue gave the page no position */
typedef enum {
  WX_NOWHERE_OFF,       /* it could not be asked, or said no: Location off, refused, no agent, no system bus */
  WX_NOWHERE_NO_FIX,    /* it was asked and had no fix within WX_FIX_WAIT_S: indoors, or a GNSS starting cold */
} WxNowhere;

/* deg to the nearest 0.1° (halves away from zero), never -0 */
double wx_round_deg (double deg);
/* deg rounded, as the requests write it: one decimal, a point whatever the locale ("52.5", "-0.1", "0.0") */
char *wx_deg_text (double deg);
/* Open-Meteo's forecast for the rounded position: now, 24 hours, 8 days, and yesterday */
char *wx_forecast_url (double lat, double lon);
/* Nominatim's name for the rounded position, at city level (zoom 10: a suburb's name means nothing for a point
 * rounded to 0.1°), in language lang */
char *wx_reverse_url (double lat, double lon, const char *lang);
/* the page's message when it has no place at all, untranslated (the caller translates it): what to do next */
const char *wx_nowhere_message (WxNowhere why);
