/* the position a forecast is asked for, rounded before use (see wx-place.h) */
#include "wx-place.h"

#include <glib/gi18n.h>
#include <math.h>

double
wx_round_deg (double deg)
{
  double r = round (deg / WX_STEP_DEG) * WX_STEP_DEG;
  return r == 0 ? 0 : r;          /* -0.0 → 0.0 */
}

char *
wx_deg_text (double deg)
{
  char s[G_ASCII_DTOSTR_BUF_SIZE];
  g_ascii_formatd (s, sizeof s, "%.1f", wx_round_deg (deg));
  return g_strdup (s);
}

char *
wx_forecast_url (double lat, double lon)
{
  g_autofree char *la = wx_deg_text (lat), *lo = wx_deg_text (lon);
  return g_strdup_printf (
    "https://api.open-meteo.com/v1/forecast?latitude=%s&longitude=%s&timezone=auto&past_days=1&forecast_days=8"
    "&current=temperature_2m,apparent_temperature,weather_code,is_day,relative_humidity_2m,wind_speed_10m,pressure_msl"
    "&hourly=temperature_2m,weather_code,precipitation_probability,is_day"
    "&daily=weather_code,temperature_2m_max,temperature_2m_min,precipitation_probability_max,sunrise,sunset,"
    "uv_index_max", la, lo);
}

char *
wx_reverse_url (double lat, double lon, const char *lang)
{
  g_autofree char *la = wx_deg_text (lat), *lo = wx_deg_text (lon);
  g_autofree char *l = g_uri_escape_string (lang ? lang : "en", NULL, FALSE);
  return g_strdup_printf ("https://nominatim.openstreetmap.org/reverse?format=jsonv2&zoom=10"
                          "&lat=%s&lon=%s&accept-language=%s", la, lo, l);
}

const char *
wx_nowhere_message (WxNowhere why)
{
  switch (why) {
  case WX_NOWHERE_NO_FIX:
    return N_("No location yet. Set a city in the ⋮ menu, or tap reload under open sky.");
  case WX_NOWHERE_OFF:
  default:
    return N_("No location. Turn on Location in Settings, then tap reload, or set a city in the ⋮ menu.");
  }
}
