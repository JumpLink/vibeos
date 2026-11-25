/* wx-place: a position is rounded to 0.1° before anything uses it, and the requests carry only that (the
 * coordinates here are made up) */
#include <locale.h>
#include <math.h>
#include <string.h>

#include "../src/wx-place.h"

static void
test_round (void)
{
  g_assert_cmpfloat_with_epsilon (wx_round_deg (12.3456), 12.3, 1e-9);
  g_assert_cmpfloat_with_epsilon (wx_round_deg (12.36), 12.4, 1e-9);
  g_assert_cmpfloat_with_epsilon (wx_round_deg (-45.6789), -45.7, 1e-9);
  g_assert_cmpfloat_with_epsilon (wx_round_deg (179.96), 180.0, 1e-9);
  /* never -0 */
  double z = wx_round_deg (-0.04);
  g_assert_cmpfloat (z, ==, 0);
  g_assert_false (signbit (z));
  /* rounding a rounded position changes nothing */
  g_assert_cmpfloat (wx_round_deg (wx_round_deg (12.3456)), ==, wx_round_deg (12.3456));
}

static void
test_text (void)
{
  const struct { double in; const char *out; } c[] = {
    { 12.3456, "12.3" }, { -45.6789, "-45.7" }, { -0.04, "0.0" }, { 0.06, "0.1" }, { 7, "7.0" }, { -120.05001, "-120.1" },
  };
  for (guint i = 0; i < G_N_ELEMENTS (c); i++) {
    g_autofree char *t = wx_deg_text (c[i].in);
    g_assert_cmpstr (t, ==, c[i].out);
  }
}

/* a decimal comma in the locale changes nothing in a request */
static void
test_locale (void)
{
  if (!setlocale (LC_NUMERIC, "de_DE.UTF-8") && !setlocale (LC_NUMERIC, "de_DE.utf8")) {
    g_test_skip ("no de_DE locale");
    return;
  }
  g_autofree char *t = wx_deg_text (12.3456);
  setlocale (LC_NUMERIC, "C");
  g_assert_cmpstr (t, ==, "12.3");
}

/* every number after key= in url has one decimal */
static void
assert_one_decimal (const char *url, const char *key, const char *want)
{
  g_autofree char *k = g_strconcat (key, "=", NULL);
  const char *v = strstr (url, k);
  g_assert_nonnull (v);
  v += strlen (k);
  g_autofree char *num = g_strndup (v, strcspn (v, "&"));
  g_assert_cmpstr (num, ==, want);
  g_assert_true (g_regex_match_simple ("^-?[0-9]+\\.[0-9]$", num, 0, 0));
}

static void
test_forecast_url (void)
{
  g_autofree char *u = wx_forecast_url (12.3456, -45.6789);
  g_assert_true (g_str_has_prefix (u, "https://api.open-meteo.com/v1/forecast?"));
  assert_one_decimal (u, "latitude", "12.3");
  assert_one_decimal (u, "longitude", "-45.7");
  g_assert_null (strstr (u, "12.34"));
  g_assert_null (strstr (u, "45.67"));
  g_assert_nonnull (strstr (u, "&past_days=1&forecast_days=8"));
}

static void
test_reverse_url (void)
{
  g_autofree char *u = wx_reverse_url (12.3456, -45.6789, "de");
  g_assert_true (g_str_has_prefix (u, "https://nominatim.openstreetmap.org/reverse?"));
  g_assert_nonnull (strstr (u, "&zoom=10&"));
  assert_one_decimal (u, "lat", "12.3");
  assert_one_decimal (u, "lon", "-45.7");
  g_assert_true (g_str_has_suffix (u, "&accept-language=de"));
  g_autofree char *odd = wx_reverse_url (1, 2, "x&y");
  g_assert_true (g_str_has_suffix (odd, "&accept-language=x%26y"));
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/place/round", test_round);
  g_test_add_func ("/place/text", test_text);
  g_test_add_func ("/place/locale", test_locale);
  g_test_add_func ("/place/forecast-url", test_forecast_url);
  g_test_add_func ("/place/reverse-url", test_reverse_url);
  return g_test_run ();
}
