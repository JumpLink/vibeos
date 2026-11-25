/* Tests of Navi's logic: paths, Valhalla's answer, the guidance along a route and its prompts, the tile reader,
 * the search's folding and Photon's answer, the sun. No GTK, no network: the route is a saved answer (Bahnhof Zoo
 * to Potsdamer Platz in Berlin) and the drive is made-up fixes along it. */
#include "navi-compass.h"
#include "navi-geo.h"
#include "navi-guidance.h"
#include "navi-mvt.h"
#include "navi-route.h"
#include "navi-search.h"
#include "navi-poi.h"
#include "navi-sun.h"

#include <math.h>
#include <string.h>

static GPtrArray *
load_route_file (const char *name)
{
  g_autofree char *path = g_test_build_filename (G_TEST_DIST, "data", name, NULL);
  g_autofree char *json = NULL;
  gsize len;
  g_autoptr (GError) error = NULL;
  g_assert_true (g_file_get_contents (path, &json, &len, &error));
  GPtrArray *routes = navi_route_parse (json, len, &error);
  g_assert_no_error (error);
  return routes;
}

static GPtrArray *
load_routes (void)
{
  return load_route_file ("route-zoo-potsdamer-platz.json");
}

static void
test_geo (void)
{
  /* Berlin: Brandenburger Tor to the Siegessäule, about 1.8 km west */
  double d = navi_geo_distance (52.5163, 13.3777, 52.5145, 13.3501);
  g_assert_cmpfloat (fabs (d - 1880), <, 30);
  double b = navi_geo_bearing (52.5163, 13.3777, 52.5145, 13.3501);
  g_assert_cmpfloat (fabs (b - 263.5), <, 2);
  double lat, lng;
  navi_geo_offset (52.5, 13.4, 90, 1000, &lat, &lng);
  g_assert_cmpfloat (fabs (navi_geo_distance (52.5, 13.4, lat, lng) - 1000), <, 0.5);
  g_assert_cmpfloat (navi_angle_diff (10, 350), ==, 20);
  g_assert_cmpfloat (navi_angle_diff (350, 10), ==, -20);

  /* Google's example polyline, precision 5 */
  g_autoptr (GArray) pts = g_array_new (FALSE, FALSE, sizeof (NaviPathPoint));
  g_assert_true (navi_polyline_decode ("_p~iF~ps|U_ulLnnqC_mqNvxq`@", 5, pts));
  g_assert_cmpuint (pts->len, ==, 3);
  NaviPathPoint *p = (NaviPathPoint *) pts->data;
  g_assert_cmpfloat (fabs (p[0].lat - 38.5), <, 1e-9);
  g_assert_cmpfloat (fabs (p[0].lng + 120.2), <, 1e-9);
  g_assert_cmpfloat (fabs (p[2].lat - 43.252), <, 1e-9);
  g_assert_cmpfloat (fabs (p[2].lng + 126.453), <, 1e-9);
  g_assert_cmpfloat (p[2].s, >, p[1].s);

  char *s = navi_format_distance (112);
  g_assert_cmpstr (s, ==, "110 m");
  g_free (s);
  s = navi_format_distance (1234);
  g_assert_cmpstr (s, ==, "1.2 km");
  g_free (s);
  s = navi_format_distance (15400);
  g_assert_cmpstr (s, ==, "15 km");
  g_free (s);
  s = navi_format_duration (14 * 60 + 40);
  g_assert_cmpstr (s, ==, "15 min");
  g_free (s);
  s = navi_format_duration (65 * 60);
  g_assert_cmpstr (s, ==, "1 h 5 min");
  g_free (s);

  /* the spring gets there without passing it */
  double v = 0, rate = 0, maxv = 0;
  for (int i = 0; i < 180; i++) {
    v = navi_smooth_angle (v, 90, &rate, 0.6, 1 / 60.0);
    maxv = MAX (maxv, v);
  }
  g_assert_cmpfloat (fabs (v - 90), <, 0.5);
  g_assert_cmpfloat (maxv, <=, 90.01);
  v = 350, rate = 0;
  for (int i = 0; i < 180; i++)
    v = navi_smooth_angle (v, 10, &rate, 0.6, 1 / 60.0);
  g_assert_cmpfloat (fabs (navi_angle_diff (v, 10)), <, 0.5);
}

static void
test_route_errors (void)
{
  /* Valhalla's answers without a route: off the roads (or outside the area routed), and no way between */
  static const char no_road[] = "{\"error_code\":171,\"error\":\"No suitable edges near location\","
                                "\"status_code\":400,\"status\":\"Bad Request\"}";
  static const char no_path[] = "{\"error_code\":442,\"error\":\"No path could be found for input\","
                                "\"status_code\":400,\"status\":\"Bad Request\"}";
  g_autoptr (GError) error = NULL;
  g_assert_null (navi_route_parse (no_road, -1, &error));
  g_assert_error (error, NAVI_ROUTE_ERROR, NAVI_ROUTE_ERROR_NO_ROAD);
  g_clear_error (&error);
  g_assert_null (navi_route_parse (no_path, -1, &error));
  g_assert_error (error, NAVI_ROUTE_ERROR, NAVI_ROUTE_ERROR_NO_ROUTE);
}

static void
test_route_parse (void)
{
  g_autoptr (GPtrArray) routes = load_routes ();
  g_assert_cmpuint (routes->len, >=, 1);
  NaviRoute *r = routes->pdata[0];
  g_assert_cmpfloat (fabs (r->length - 5071), <, 60);
  g_assert_cmpfloat (r->time, >, 300);
  g_assert_cmpuint (r->n_maneuvers, ==, 10);
  g_assert_cmpint (r->maneuvers[0].type, ==, NAVI_TURN_START_RIGHT); /* the station is on the right */
  g_assert_cmpint (r->maneuvers[1].type, ==, NAVI_TURN_RIGHT);
  g_assert_cmpstr (r->maneuvers[1].street, ==, "Hardenbergstraße");
  g_assert_cmpstr (r->maneuvers[1].instruction, ==, "Turn right onto Hardenbergstraße.");
  g_assert_nonnull (r->maneuvers[1].verbal_pre);
  /* a name before the road number */
  g_assert_cmpstr (r->maneuvers[4].street, ==, "Straße des 17. Juni");
  g_assert_cmpstr (r->maneuvers[4].ref, ==, "B 2");
  g_assert_cmpint (r->maneuvers[9].type, ==, NAVI_TURN_DESTINATION_RIGHT);
  /* the manoeuvres in order along the path, their lengths adding up */
  double sum = 0;
  for (guint i = 0; i < r->n_maneuvers; i++) {
    if (i)
      g_assert_cmpfloat (r->maneuvers[i].s, >=, r->maneuvers[i - 1].s);
    sum += r->maneuvers[i].length;
  }
  g_assert_cmpfloat (fabs (sum - r->length), <, 1);
  g_assert_nonnull (r->via);
  g_assert_true (strstr (r->via, "Straße des 17. Juni") != NULL);
  g_assert_cmpstr (navi_route_street_at (r, r->maneuvers[2].s + 10), ==, "Fasanenstraße");
  g_assert_cmpfloat (fabs (navi_route_time_left (r, 0) - r->time), <, 5);
  g_assert_cmpfloat (navi_route_time_left (r, r->length), <, 1);

  g_autoptr (GError) error = NULL;
  GPtrArray *none = navi_route_parse ("{\"error_code\":171,\"error\":\"No suitable edges near location\"}", -1, &error);
  g_assert_null (none);
  g_assert_error (error, NAVI_ROUTE_ERROR, NAVI_ROUTE_ERROR_NO_ROAD);
}

/* the route was asked with "turn_lanes", to a point off the road on its right: the lanes up to the turns, and the
 * destination's side said in its last metres */
static void
test_lanes_and_side (void)
{
  g_autoptr (GPtrArray) routes = load_routes ();
  NaviRoute *r = routes->pdata[0];
  g_assert_cmpuint (r->n_maneuvers, ==, 10);
  /* no lanes in the map, or none to take: nothing */
  g_assert_null (r->maneuvers[1].lanes);
  g_assert_cmpuint (r->maneuvers[1].n_lanes, ==, 0);
  /* Fasanenstraße: one lane left, one straight on, two straight on or right; the right two to take */
  const NaviManeuver *m = &r->maneuvers[2];
  g_assert_cmpstr (m->street, ==, "Fasanenstraße");
  g_assert_cmpuint (m->n_lanes, ==, 4);
  g_assert_cmpuint (m->lanes[0].directions, ==, NAVI_LANE_LEFT);
  g_assert_cmpuint (m->lanes[0].active, ==, 0);
  g_assert_cmpuint (m->lanes[2].directions, ==, NAVI_LANE_THROUGH | NAVI_LANE_RIGHT);
  g_assert_cmpuint (m->lanes[2].active, ==, NAVI_LANE_RIGHT);
  g_assert_cmpuint (m->lanes[3].active, ==, NAVI_LANE_RIGHT);
  /* the right turn onto Straße des 17. Juni from the rightmost lane */
  g_assert_cmpuint (r->maneuvers[4].n_lanes, ==, 2);
  g_assert_cmpuint (r->maneuvers[4].lanes[1].active, ==, NAVI_LANE_RIGHT);
  g_assert_cmpint (r->maneuvers[9].type, ==, NAVI_TURN_DESTINATION_RIGHT);

  /* driven at 10 m/s: the side is said once, in the instruction's metres before the end */
  g_autoptr (NaviGuidance) g = navi_guidance_new (r);
  gint64 t = 1000000;
  guint hint = 0, said = 0;
  double said_at = -1;
  for (double s = 0; s <= r->length + 20; s += 10, t += 1000000) {
    double lat, lng;
    navi_path_point_at (r->points, r->n_points, MIN (s, r->length), &lat, &lng, &hint);
    double heading = navi_path_heading_at (r->points, r->n_points, MIN (s, r->length), 10);
    const NaviProgress *p = navi_guidance_update (g, lat, lng, 5, 10, heading, t);
    char *words;
    while ((words = navi_guidance_take_prompt (g, 10))) {
      if (!strcmp (words, "Your destination is on the right.")) {
        said++;
        said_at = p->to_next;
      }
      g_free (words);
    }
  }
  g_assert_cmpuint (said, ==, 1);
  g_assert_cmpfloat (said_at, <=, navi_guidance_pre_distance (10));
  g_assert_cmpfloat (navi_guidance_pre_distance (10), ==, 60);
  g_assert_cmpfloat (navi_guidance_pre_distance (3), ==, 50);
}

/* drives the route at 10 m/s with a fix a second, `drift` metres to the side from `drift_from` on */
static void
test_guidance (void)
{
  g_autoptr (GPtrArray) routes = load_routes ();
  NaviRoute *r = routes->pdata[0];
  g_autoptr (NaviGuidance) g = navi_guidance_new (r);
  g_autoptr (GPtrArray) prompts = g_ptr_array_new_with_free_func (g_free);
  gint64 t = 1000000;
  guint hint = 0;
  gboolean arrived = FALSE;
  guint last_next = 0;
  /* what the app would have made into sound ahead, after the fix before: every prompt is among it */
  g_autoptr (GHashTable) foreseen = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  guint unforeseen = 0;
  for (double s = 0; s <= r->length + 20; s += 10, t += 1000000) {
    g_auto (GStrv) upcoming = navi_guidance_upcoming_prompts (g, 10);
    for (guint i = 0; upcoming[i]; i++)
      g_hash_table_add (foreseen, g_strdup (upcoming[i]));
    double lat, lng;
    navi_path_point_at (r->points, r->n_points, MIN (s, r->length), &lat, &lng, &hint);
    double heading = navi_path_heading_at (r->points, r->n_points, MIN (s, r->length), 10);
    const NaviProgress *p = navi_guidance_update (g, lat, lng, 5, 10, heading, t);
    g_assert_true (p->on_route);
    g_assert_false (p->off_route);
    g_assert_cmpfloat (fabs (p->s - MIN (s, r->length)), <, 2);
    g_assert_cmpuint (p->next, >=, last_next);
    last_next = p->next;
    char *words;
    while ((words = navi_guidance_take_prompt (g, 10))) {
      if (!g_hash_table_contains (foreseen, words) && !p->arrived) {
        g_test_message ("not made ahead: %s", words);
        unforeseen++;
      }
      g_ptr_array_add (prompts, words);
    }
    if (p->arrived)
      arrived = TRUE;
  }
  g_assert_true (arrived);
  g_assert_cmpuint (unforeseen, ==, 0);
  /* the start, each turn's instruction, the arrival: each once */
  g_assert_cmpuint (prompts->len, >=, 6);
  g_assert_cmpstr (prompts->pdata[0], ==, "Drive southwest on Hardenbergplatz. Then Turn right onto Hardenbergstraße.");
  guint mitte = 0;
  for (guint i = 0; i < prompts->len; i++)
    if (strstr (prompts->pdata[i], "toward Mitte"))
      mitte++;
  g_assert_cmpuint (mitte, ==, 2); /* the alert 400 m before it, then the instruction */
  gboolean alert = FALSE;
  for (guint i = 0; i < prompts->len; i++)
    if (g_str_has_prefix (prompts->pdata[i], "In 400 meters, turn right toward Mitte"))
      alert = TRUE;
  g_assert_true (alert);
  /* the destination is off the road on the right: the side is the last word */
  g_assert_cmpstr (prompts->pdata[prompts->len - 1], ==, "Your destination is on the right.");
}

/* a drive at `speed` (m/s, a fix a second), the prompts made ahead at `ahead_speed`: how many said were not made */
static guint
drive_unforeseen (NaviRoute *r, double ahead_speed, double speed)
{
  g_autoptr (NaviGuidance) g = navi_guidance_new (r);
  g_autoptr (GHashTable) foreseen = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  guint hint = 0, unforeseen = 0;
  gint64 t = 1000000;
  for (double s = 0; s <= r->length + 20; s += speed, t += 1000000) {
    g_auto (GStrv) upcoming = navi_guidance_upcoming_prompts (g, ahead_speed);
    for (guint i = 0; upcoming[i]; i++)
      g_hash_table_add (foreseen, g_strdup (upcoming[i]));
    double lat, lng;
    navi_path_point_at (r->points, r->n_points, MIN (s, r->length), &lat, &lng, &hint);
    double heading = navi_path_heading_at (r->points, r->n_points, MIN (s, r->length), 10);
    const NaviProgress *p = navi_guidance_update (g, lat, lng, 5, speed, heading, t);
    char *words;
    while ((words = navi_guidance_take_prompt (g, speed))) {
      if (!g_hash_table_contains (foreseen, words) && !p->arrived) {
        g_test_message ("not made ahead at %.1f m/s (%.1f): %s", speed, ahead_speed, words);
        unforeseen++;
      }
      g_free (words);
    }
  }
  return unforeseen;
}

static void
test_upcoming (void)
{
  g_autoptr (GPtrArray) routes = load_routes ();
  NaviRoute *r = routes->pdata[0];
  /* at a speed's own bands, and with the speed a little different when the prompt comes, over a band's edge */
  g_assert_cmpuint (drive_unforeseen (r, 10, 10), ==, 0);
  g_assert_cmpuint (drive_unforeseen (r, 14.5, 15.5), ==, 0);
  g_assert_cmpuint (drive_unforeseen (r, 16, 14), ==, 0);
  g_assert_cmpuint (drive_unforeseen (r, 24, 26), ==, 0);
}

static void
test_off_route (void)
{
  g_autoptr (GPtrArray) routes = load_routes ();
  NaviRoute *r = routes->pdata[0];
  g_autoptr (NaviGuidance) g = navi_guidance_new (r);
  gint64 t = 0;
  guint hint = 0;
  const NaviProgress *p = NULL;
  /* on the route for 300 m */
  for (double s = 0; s < 300; s += 10, t += 1000000) {
    double lat, lng;
    navi_path_point_at (r->points, r->n_points, s, &lat, &lng, &hint);
    p = navi_guidance_update (g, lat, lng, 5, 10, -1, t);
  }
  g_assert_false (p->off_route);
  /* then 120 m off to one side: one fix is noise, three over three seconds is off the route */
  double lat, lng, olat, olng;
  navi_path_point_at (r->points, r->n_points, 310, &lat, &lng, &hint);
  double b = navi_path_heading_at (r->points, r->n_points, 310, 10);
  navi_geo_offset (lat, lng, b + 90, 120, &olat, &olng);
  p = navi_guidance_update (g, olat, olng, 5, 10, -1, t += 1000000);
  g_assert_false (p->on_route);
  g_assert_false (p->off_route);
  p = navi_guidance_update (g, olat, olng, 5, 10, -1, t += 1000000);
  g_assert_false (p->off_route);
  p = navi_guidance_update (g, olat, olng, 5, 10, -1, t += 1000000);
  p = navi_guidance_update (g, olat, olng, 5, 10, -1, t += 1000000);
  g_assert_true (p->off_route);
  /* a bad fix (300 m accuracy) 120 m off is still on it */
  g_autoptr (NaviGuidance) g2 = navi_guidance_new (r);
  p = navi_guidance_update (g2, olat, olng, 300, 10, -1, 0);
  g_assert_true (p->on_route);
}

static void
test_alert_text (void)
{
  char *s = navi_guidance_alert_text ("de-DE", 410, "Rechts auf Hardenbergstraße abbiegen.");
  g_assert_cmpstr (s, ==, "In 400 Metern: Rechts auf Hardenbergstraße abbiegen.");
  g_free (s);
  s = navi_guidance_alert_text ("de-DE", 1480, "Ausfahrt nehmen.");
  g_assert_cmpstr (s, ==, "In 1,5 Kilometern: Ausfahrt nehmen.");
  g_free (s);
  s = navi_guidance_alert_text ("en-US", 1000, "Turn right.");
  g_assert_cmpstr (s, ==, "In 1 kilometer, turn right.");
  g_free (s);
  /* the other languages Navi speaks: their words for the distance, the instruction after it in lower case */
  s = navi_guidance_alert_text ("fr-FR", 410, "Tournez à droite sur Rue de Rivoli.");
  g_assert_cmpstr (s, ==, "Dans 400 mètres, tournez à droite sur Rue de Rivoli.");
  g_free (s);
  s = navi_guidance_alert_text ("fr-FR", 1480, "Prenez la sortie.");
  g_assert_cmpstr (s, ==, "Dans 1,5 kilomètre, prenez la sortie.");
  g_free (s);
  s = navi_guidance_alert_text ("it-IT", 1000, "Svolta a sinistra.");
  g_assert_cmpstr (s, ==, "Tra 1 chilometro, svolta a sinistra.");
  g_free (s);
  s = navi_guidance_alert_text ("es-ES", 2020, "Gire a la izquierda.");
  g_assert_cmpstr (s, ==, "En 2 kilómetros, gire a la izquierda.");
  g_free (s);
  s = navi_guidance_alert_text ("ru-RU", 390, "Поверните направо.");
  g_assert_cmpstr (s, ==, "Через 400 метров поверните направо.");
  g_free (s);
  s = navi_guidance_alert_text ("ru-RU", 2100, "Поверните налево.");
  g_assert_cmpstr (s, ==, "Через 2 километра поверните налево.");
  g_free (s);
  s = navi_guidance_alert_text ("ru-RU", 1000, "Держитесь правее.");
  g_assert_cmpstr (s, ==, "Через 1 километр держитесь правее.");
  g_free (s);
  s = navi_guidance_alert_text ("ru-RU", 5100, "Съезд.");
  g_assert_cmpstr (s, ==, "Через 5 километров съезд.");
  g_free (s);
  /* a language Navi does not speak: English */
  s = navi_guidance_alert_text ("pl-PL", 200, "Turn left.");
  g_assert_cmpstr (s, ==, "In 200 meters, turn left.");
  g_free (s);
}

static void
test_spoken_language (void)
{
  /* Navi's own words in the voice's language, English for any other */
  g_assert_cmpstr (navi_guidance_saying (NAVI_SAY_SPEED_CAMERA, "de-DE"), ==, "Blitzer voraus.");
  g_assert_cmpstr (navi_guidance_saying (NAVI_SAY_ARRIVED, "fr"), ==, "Vous êtes arrivé à destination.");
  g_assert_cmpstr (navi_guidance_saying (NAVI_SAY_DANGER_ZONE, "ru-RU"), ==, "Впереди опасный участок.");
  g_assert_cmpstr (navi_guidance_saying (NAVI_SAY_STOP_REACHED, "nl-NL"), ==, "You have reached your stop.");
  for (NaviSaying w = NAVI_SAY_ARRIVED; w <= NAVI_SAY_DANGER_ZONE; w++)
    for (guint i = 0; navi_spoken_language_code (i); i++)
      g_assert_nonnull (navi_guidance_saying (w, navi_spoken_language_code (i)));
  /* the six, each with Valhalla's tag once chosen */
  static const char *const tags[] = { "en-US", "de-DE", "fr-FR", "it-IT", "es-ES", "ru-RU" };
  guint n = 0;
  for (const char *code; (code = navi_spoken_language_code (n)); n++) {
    navi_route_set_language (code);
    g_assert_cmpstr (navi_route_spoken_code (), ==, code);
    if (n > 0) /* English may be British, as the system's is */
      g_assert_cmpstr (navi_route_language (), ==, tags[n]);
  }
  g_assert_cmpuint (n, ==, 6);
  g_assert_cmpstr (navi_spoken_language_name ("ru"), ==, "Русский");
  g_assert_null (navi_spoken_language_name ("pl"));
  /* none chosen, or one Navi does not speak: the system's (the tests' LANGUAGE is en_US) */
  navi_route_set_language ("pl");
  g_assert_cmpstr (navi_route_spoken_code (), ==, "en");
  navi_route_set_language (NULL);
  g_assert_cmpstr (navi_route_language (), ==, "en-US");
}

/* ---- a vector tile made by hand ---- */

static void
put_varint (GByteArray *b, guint64 v)
{
  while (v >= 0x80) {
    guint8 c = (v & 0x7f) | 0x80;
    g_byte_array_append (b, &c, 1);
    v >>= 7;
  }
  guint8 c = v;
  g_byte_array_append (b, &c, 1);
}

static void
put_bytes (GByteArray *b, guint field, const guint8 *data, gsize n)
{
  put_varint (b, (field << 3) | 2);
  put_varint (b, n);
  g_byte_array_append (b, data, n);
}

static guint
zz (int v)
{
  return (guint) ((v << 1) ^ (v >> 31));
}

typedef struct {
  guint count;
  gboolean saw_street, saw_house;
} Seen;

static void
on_feature (const char *layer, const NaviMvtFeature *f, gpointer data)
{
  Seen *seen = data;
  seen->count++;
  if (!strcmp (layer, "transportation_name")) {
    g_assert_cmpstr (navi_mvt_feature_get (f, "name"), ==, "Rathausstraße");
    g_assert_cmpint (navi_mvt_feature_type (f), ==, NAVI_MVT_LINE);
    guint n, n_parts;
    const guint *parts;
    const int *pts = navi_mvt_feature_points (f, &n, &parts, &n_parts);
    g_assert_cmpuint (n, ==, 2);
    g_assert_cmpuint (n_parts, ==, 1);
    g_assert_cmpint (pts[0], ==, 100);
    g_assert_cmpint (pts[1], ==, 200);
    g_assert_cmpint (pts[2], ==, 300);
    g_assert_cmpint (pts[3], ==, 150);
    seen->saw_street = TRUE;
  }
  if (!strcmp (layer, "housenumber")) {
    g_assert_cmpstr (navi_mvt_feature_get (f, "housenumber"), ==, "5");
    seen->saw_house = TRUE;
  }
}

static GByteArray *
make_layer (const char *name, const char *key, const char *value, int type, const guint *geom, guint n_geom)
{
  GByteArray *layer = g_byte_array_new ();
  put_bytes (layer, 1, (const guint8 *) name, strlen (name));
  GByteArray *feat = g_byte_array_new ();
  guint8 tags[] = { 0, 0 };
  put_bytes (feat, 2, tags, 2);
  put_varint (feat, (3 << 3) | 0);
  put_varint (feat, type);
  GByteArray *g = g_byte_array_new ();
  for (guint i = 0; i < n_geom; i++)
    put_varint (g, geom[i]);
  put_bytes (feat, 4, g->data, g->len);
  put_bytes (layer, 2, feat->data, feat->len);
  put_bytes (layer, 3, (const guint8 *) key, strlen (key));
  GByteArray *val = g_byte_array_new ();
  put_bytes (val, 1, (const guint8 *) value, strlen (value));
  put_bytes (layer, 4, val->data, val->len);
  put_varint (layer, (5 << 3) | 0);
  put_varint (layer, 4096);
  g_byte_array_unref (feat);
  g_byte_array_unref (g);
  g_byte_array_unref (val);
  return layer;
}

static void
test_mvt (void)
{
  /* a line from (100, 200) to (300, 150), and a point at (120, 210) */
  guint line[] = { (1 << 3) | 1, zz (100), zz (200), (1 << 3) | 2, zz (200), zz (-50) };
  guint point[] = { (1 << 3) | 1, zz (120), zz (210) };
  g_autoptr (GByteArray) tile = g_byte_array_new ();
  g_autoptr (GByteArray) l1 = make_layer ("transportation_name", "name", "Rathausstraße", 2, line, 6);
  g_autoptr (GByteArray) l2 = make_layer ("housenumber", "housenumber", "5", 1, point, 3);
  g_autoptr (GByteArray) l3 = make_layer ("building", "colour", "#fff", 1, point, 3);
  put_bytes (tile, 3, l1->data, l1->len);
  put_bytes (tile, 3, l2->data, l2->len);
  put_bytes (tile, 3, l3->data, l3->len);
  static const char *const layers[] = { "transportation_name", "housenumber", NULL };
  Seen seen = { 0 };
  g_assert_true (navi_mvt_read (tile->data, tile->len, layers, on_feature, &seen));
  g_assert_cmpuint (seen.count, ==, 2); /* the building layer was not asked for */
  g_assert_true (seen.saw_street && seen.saw_house);
  /* garbage is no tile */
  guint8 junk[] = { 0xff, 0xff, 0xff };
  g_assert_false (navi_mvt_read (junk, sizeof junk, NULL, on_feature, &seen));

  int x, y;
  navi_mvt_tile_at (14, 52.5186, 13.4083, &x, &y);
  g_assert_cmpint (x, ==, 8802);
  g_assert_cmpint (y, ==, 5373);
  double lat, lng;
  navi_mvt_to_lat_lng (14, x, y, 4096, 2048, 2048, &lat, &lng);
  g_assert_cmpfloat (fabs (lat - 52.5186), <, 0.02);
  g_assert_cmpfloat (fabs (lng - 13.4083), <, 0.02);
}

static void
test_fold (void)
{
  char *f = navi_search_fold ("Rathausstraße 15");
  g_assert_cmpstr (f, ==, "rathausstrasse 15");
  g_free (f);
  f = navi_search_fold ("Rathausstr. 15");
  g_assert_cmpstr (f, ==, "rathausstrasse 15");
  g_free (f);
  f = navi_search_fold ("  Café  Müller-Lüdenscheidt ");
  g_assert_cmpstr (f, ==, "cafe muller ludenscheidt");
  g_free (f);
}

static void
test_photon (void)
{
  const char *json =
    "{\"type\":\"FeatureCollection\",\"features\":["
    "{\"type\":\"Feature\",\"geometry\":{\"type\":\"Point\",\"coordinates\":[13.4128,52.5389]},"
    "\"properties\":{\"osm_key\":\"amenity\",\"osm_value\":\"theatre\",\"type\":\"house\",\"name\":\"Kulturbrauerei\","
    "\"street\":\"Schönhauser Allee\",\"housenumber\":\"36\",\"postcode\":\"10435\",\"city\":\"Berlin\","
    "\"district\":\"Prenzlauer Berg\"}},"
    "{\"type\":\"Feature\",\"geometry\":{\"type\":\"Point\",\"coordinates\":[13.4083,52.5186]},"
    "\"properties\":{\"osm_key\":\"building\",\"osm_value\":\"yes\",\"type\":\"house\",\"street\":\"Rathausstraße\","
    "\"housenumber\":\"15\",\"postcode\":\"10178\",\"city\":\"Berlin\",\"district\":\"Mitte\"}},"
    "{\"type\":\"Feature\",\"geometry\":{\"type\":\"Point\",\"coordinates\":[13.407,52.518]},"
    "\"properties\":{\"osm_key\":\"highway\",\"osm_value\":\"residential\",\"type\":\"street\","
    "\"name\":\"Rathausstraße\",\"city\":\"Berlin\",\"district\":\"Mitte\"}},"
    "{\"type\":\"Feature\",\"geometry\":{\"type\":\"Point\",\"coordinates\":[13.408,52.519]},"
    "\"properties\":{\"osm_key\":\"highway\",\"osm_value\":\"residential\",\"type\":\"street\","
    "\"name\":\"Rathausstraße\",\"city\":\"Berlin\",\"district\":\"Mitte\"}},"
    "{\"type\":\"Feature\",\"geometry\":{\"type\":\"Point\",\"coordinates\":[13.4,52.5]},"
    "\"properties\":{\"osm_key\":\"place\",\"osm_value\":\"city\",\"type\":\"city\",\"name\":\"Berlin\","
    "\"state\":\"Berlin\",\"country\":\"Deutschland\"}}]}";
  g_autoptr (GError) error = NULL;
  g_autoptr (GPtrArray) places = navi_search_photon_parse (json, -1, &error);
  g_assert_no_error (error);
  g_assert_cmpuint (places->len, ==, 4); /* the street's second way is the same street */
  NaviPlace *p = places->pdata[0];
  g_assert_cmpint (p->kind, ==, NAVI_PLACE_POI);
  g_assert_cmpstr (p->name, ==, "Kulturbrauerei");
  g_assert_cmpstr (p->detail, ==, "Schönhauser Allee 36, Prenzlauer Berg, Berlin");
  g_assert_cmpstr (p->category, ==, "theatre");
  g_assert_cmpfloat (fabs (p->lat - 52.5389), <, 1e-9);
  p = places->pdata[1];
  g_assert_cmpint (p->kind, ==, NAVI_PLACE_ADDRESS);
  g_assert_cmpstr (p->name, ==, "Rathausstraße 15");
  g_assert_cmpstr (p->detail, ==, "Mitte, 10178 Berlin");
  p = places->pdata[2];
  g_assert_cmpint (p->kind, ==, NAVI_PLACE_STREET);
  g_assert_cmpstr (p->detail, ==, "Mitte, Berlin");
  p = places->pdata[3];
  g_assert_cmpint (p->kind, ==, NAVI_PLACE_LOCALITY);
  g_assert_cmpstr (p->detail, ==, "Berlin");
  GPtrArray *bad = navi_search_photon_parse ("{\"message\":\"missing q\"}", -1, &error);
  g_assert_null (bad);
  g_clear_error (&error);

  /* "Paris" typed far from it: the city first, the rest as Photon had them */
  const char *paris =
    "{\"type\":\"FeatureCollection\",\"features\":["
    "{\"type\":\"Feature\",\"geometry\":{\"type\":\"Point\",\"coordinates\":[11.55,48.14]},"
    "\"properties\":{\"osm_key\":\"highway\",\"osm_value\":\"residential\",\"type\":\"street\","
    "\"name\":\"Pariser Straße\",\"city\":\"München\"}},"
    "{\"type\":\"Feature\",\"geometry\":{\"type\":\"Point\",\"coordinates\":[11.56,48.13]},"
    "\"properties\":{\"osm_key\":\"shop\",\"osm_value\":\"shoes\",\"type\":\"house\",\"name\":\"Paris\","
    "\"city\":\"München\"}},"
    "{\"type\":\"Feature\",\"geometry\":{\"type\":\"Point\",\"coordinates\":[2.3488,48.8534]},"
    "\"properties\":{\"osm_key\":\"place\",\"osm_value\":\"city\",\"type\":\"city\",\"name\":\"Paris\","
    "\"state\":\"Île-de-France\",\"country\":\"France\"}},"
    "{\"type\":\"Feature\",\"geometry\":{\"type\":\"Point\",\"coordinates\":[-95.55,33.66]},"
    "\"properties\":{\"osm_key\":\"place\",\"osm_value\":\"town\",\"type\":\"city\",\"name\":\"Paris\","
    "\"state\":\"Texas\",\"country\":\"United States\"}}]}";
  g_autoptr (GPtrArray) found = navi_search_photon_parse (paris, -1, &error);
  g_assert_no_error (error);
  navi_search_photon_rank (found, " paris ");
  g_assert_cmpuint (found->len, ==, 4);
  g_assert_cmpstr (((NaviPlace *) found->pdata[0])->detail, ==, "Île-de-France");
  g_assert_cmpstr (((NaviPlace *) found->pdata[1])->detail, ==, "Texas");
  g_assert_cmpstr (((NaviPlace *) found->pdata[2])->name, ==, "Pariser Straße");
  g_assert_cmpint (((NaviPlace *) found->pdata[3])->kind, ==, NAVI_PLACE_POI);
  /* no settlement of that name: Photon's order stands */
  navi_search_photon_rank (found, "Pariser");
  g_assert_cmpstr (((NaviPlace *) found->pdata[0])->detail, ==, "Île-de-France");
  /* Photon's index has Russia, Navi does not: its places are not in the answer, none left is no answer */
  const char *moskau =
    "{\"type\":\"FeatureCollection\",\"features\":["
    "{\"type\":\"Feature\",\"geometry\":{\"type\":\"Point\",\"coordinates\":[37.6175,55.7504]},"
    "\"properties\":{\"osm_key\":\"place\",\"osm_value\":\"city\",\"type\":\"city\",\"name\":\"Москва\","
    "\"countrycode\":\"RU\",\"country\":\"Россия\"}},"
    "{\"type\":\"Feature\",\"geometry\":{\"type\":\"Point\",\"coordinates\":[13.40,52.52]},"
    "\"properties\":{\"osm_key\":\"highway\",\"osm_value\":\"residential\",\"type\":\"street\","
    "\"name\":\"Moskauer Straße\",\"countrycode\":\"DE\",\"city\":\"Berlin\"}}]}";
  g_autoptr (GPtrArray) moscow = navi_search_photon_parse (moskau, -1, &error);
  g_assert_no_error (error);
  g_assert_cmpuint (moscow->len, ==, 1);
  g_assert_cmpstr (((NaviPlace *) moscow->pdata[0])->name, ==, "Moskauer Straße");
  const char *only =
    "{\"type\":\"FeatureCollection\",\"features\":["
    "{\"type\":\"Feature\",\"geometry\":{\"type\":\"Point\",\"coordinates\":[30.3141,59.9386]},"
    "\"properties\":{\"osm_key\":\"place\",\"osm_value\":\"city\",\"type\":\"city\","
    "\"name\":\"Санкт-Петербург\",\"countrycode\":\"RU\"}}]}";
  g_autoptr (GPtrArray) none = navi_search_photon_parse (only, -1, &error);
  g_assert_no_error (error);
  g_assert_cmpuint (none->len, ==, 0);
}

static void
test_sun (void)
{
  /* Berlin, 2026-06-21: noon high (about 61 degrees at 11:10 UTC), midnight below */
  GDateTime *noon = g_date_time_new_utc (2026, 6, 21, 11, 10, 0);
  GDateTime *midnight = g_date_time_new_utc (2026, 6, 21, 23, 0, 0);
  double e = navi_sun_elevation (52.52, 13.40, g_date_time_to_unix (noon));
  g_assert_cmpfloat (fabs (e - 61), <, 1);
  g_assert_false (navi_sun_is_night (52.52, 13.40, g_date_time_to_unix (noon)));
  g_assert_true (navi_sun_is_night (52.52, 13.40, g_date_time_to_unix (midnight)));
  /* 2026-11-22: sunset in Berlin about 15:59 CET (14:59 UTC) */
  GDateTime *before = g_date_time_new_utc (2026, 11, 22, 14, 50, 0);
  GDateTime *after = g_date_time_new_utc (2026, 11, 22, 15, 8, 0);
  g_assert_false (navi_sun_is_night (52.52, 13.40, g_date_time_to_unix (before)));
  g_assert_true (navi_sun_is_night (52.52, 13.40, g_date_time_to_unix (after)));
  g_date_time_unref (noon);
  g_date_time_unref (midnight);
  g_date_time_unref (before);
  g_date_time_unref (after);
}

/* the map's poi classes and Photon's OSM tags land in the same groups, which the chips find */
static void
test_poi_groups (void)
{
  g_assert_cmpstr (navi_poi_group ("fuel", "fuel"), ==, "fuel");
  g_assert_cmpstr (navi_poi_group ("fuel", "charging_station"), ==, "charging");
  g_assert_cmpstr (navi_poi_group ("amenity", "fuel"), ==, "fuel");
  g_assert_cmpstr (navi_poi_group ("amenity", "charging_station"), ==, "charging");
  g_assert_cmpstr (navi_poi_group ("lodging", "guest_house"), ==, "hotel");
  g_assert_cmpstr (navi_poi_group ("tourism", "hotel"), ==, "hotel");
  g_assert_cmpstr (navi_poi_group ("grocery", "supermarket"), ==, "grocery");
  g_assert_cmpstr (navi_poi_group ("shop", "supermarket"), ==, "grocery");
  g_assert_null (navi_poi_group ("grocery", "deli"));
  g_assert_null (navi_poi_group ("bollard", "bollard"));
  g_assert_true (navi_poi_in_category (NAVI_CATEGORY_FOOD, "fast_food"));
  g_assert_true (navi_poi_in_category (NAVI_CATEGORY_ATM, "bank"));
  g_assert_false (navi_poi_in_category (NAVI_CATEGORY_FUEL, "charging"));
  g_assert_cmpstr (navi_poi_photon_categories (NAVI_CATEGORY_FUEL), ==, "osm.amenity.fuel");
  g_assert_null (navi_poi_photon_categories (NAVI_CATEGORY_NONE));
  /* every group has a marker, every chip a Photon category */
  guint n;
  const NaviPoiKind *kinds = navi_poi_kinds (&n);
  for (guint i = 0; i < n; i++) {
    g_assert_true (navi_poi_kind (kinds[i].group) == &kinds[i]);
    g_assert_nonnull (navi_poi_photon_categories (kinds[i].category));
  }
  /* Photon's fuel station is a "fuel" */
  const char *json = "{\"type\":\"FeatureCollection\",\"features\":[{\"type\":\"Feature\",\"geometry\":"
                     "{\"type\":\"Point\",\"coordinates\":[13.33,52.5]},\"properties\":{\"osm_key\":\"amenity\","
                     "\"osm_value\":\"fuel\",\"type\":\"house\",\"name\":\"Aral\",\"street\":\"Kantstra\u00dfe\"}}]}";
  g_autoptr (GPtrArray) places = navi_search_photon_parse (json, -1, NULL);
  g_assert_cmpuint (places->len, ==, 1);
  NaviPlace *p = places->pdata[0];
  g_assert_cmpint (p->kind, ==, NAVI_PLACE_POI);
  g_assert_cmpstr (p->category, ==, "fuel");
}

/* the compass's arithmetic: the screen's turn, the heading across north, and when the compass turns the arrow */
static void
test_compass (void)
{
  g_assert_cmpint (navi_compass_screen_turn (FALSE, "normal"), ==, 0);
  g_assert_cmpint (navi_compass_screen_turn (FALSE, "bottom-up"), ==, 0);
  g_assert_cmpint (navi_compass_screen_turn (FALSE, NULL), ==, 0);
  g_assert_cmpint (navi_compass_screen_turn (TRUE, "right-up"), ==, 90);
  g_assert_cmpint (navi_compass_screen_turn (TRUE, "left-up"), ==, 270);
  g_assert_cmpint (navi_compass_screen_turn (TRUE, "normal"), ==, 0);
  g_assert_cmpint (navi_compass_screen_turn (TRUE, "bottom-up"), ==, 180);
  g_assert_cmpint (navi_compass_screen_turn (TRUE, "undefined"), ==, -1);
  g_assert_cmpint (navi_compass_screen_turn (TRUE, NULL), ==, -1);

  /* the device's top at 24.9: its right edge (right-up) at 114.9, its left edge (left-up) at 294.9 */
  g_assert_cmpfloat_with_epsilon (navi_compass_screen_heading (24.9, 0), 24.9, 1e-9);
  g_assert_cmpfloat_with_epsilon (navi_compass_screen_heading (24.9, 90), 114.9, 1e-9);
  g_assert_cmpfloat_with_epsilon (navi_compass_screen_heading (24.9, 270), 294.9, 1e-9);
  g_assert_cmpfloat_with_epsilon (navi_compass_screen_heading (350, 90), 80, 1e-9);
  g_assert_cmpfloat_with_epsilon (navi_compass_screen_heading (360, 0), 0, 1e-9);

  double h = -1;
  /* moving: GPS's course, the compass aside */
  g_assert_cmpint (navi_heading_pick (12, 87, 1.5, TRUE, 10, &h), ==, NAVI_HEADING_COURSE);
  g_assert_cmpfloat (h, ==, 87);
  /* standing or creeping: the compass */
  g_assert_cmpint (navi_heading_pick (0, -1, 1.5, TRUE, 10, &h), ==, NAVI_HEADING_COMPASS);
  g_assert_cmpfloat (h, ==, 10);
  g_assert_cmpint (navi_heading_pick (1.2, 200, 1.5, TRUE, 10, &h), ==, NAVI_HEADING_COMPASS);
  g_assert_cmpfloat (h, ==, 10);
  /* moving without a course (GeoClue had none): the compass */
  g_assert_cmpint (navi_heading_pick (8, -1, 1.5, TRUE, 10, &h), ==, NAVI_HEADING_COMPASS);
  /* neither */
  g_assert_cmpint (navi_heading_pick (0, -1, 1.5, FALSE, 0, &h), ==, NAVI_HEADING_NONE);

  /* the arrow's turn toward the compass goes the short way across north */
  double rate = 0, a = 350;
  for (int i = 0; i < 120; i++)
    a = fmod (navi_smooth_angle (a, 10, &rate, 0.6, 1.0 / 60) + 360, 360);
  g_assert_cmpfloat (fabs (remainder (a - 10, 360)), <, 1);
}

/* ---- the compass's claims, against a stand-in iio-sensor-proxy on a bus of the test's own ---- */

static const char sensor_proxy_xml[] =
  "<node>"
  " <interface name='net.hadess.SensorProxy'>"
  "  <method name='ClaimAccelerometer'/><method name='ReleaseAccelerometer'/>"
  "  <property name='HasAccelerometer' type='b' access='read'/>"
  "  <property name='AccelerometerOrientation' type='s' access='read'/>"
  " </interface>"
  " <interface name='net.hadess.SensorProxy.Compass'>"
  "  <method name='ClaimCompass'/><method name='ReleaseCompass'/>"
  "  <property name='HasCompass' type='b' access='read'/>"
  "  <property name='CompassHeading' type='d' access='read'/>"
  " </interface>"
  "</node>";

typedef struct {
  gboolean has_compass;
  double   heading;
  int      gets;                 /* properties read (a proxy's GetAll reads each) */
  int      claims, releases;     /* ClaimCompass, ReleaseCompass */
} SensorStandIn;

static void
stand_in_call (GDBusConnection *c, const char *sender, const char *path, const char *iface, const char *method,
               GVariant *params, GDBusMethodInvocation *inv, gpointer data)
{
  SensorStandIn *s = data;
  if (!strcmp (method, "ClaimCompass"))
    s->claims++;
  else if (!strcmp (method, "ReleaseCompass"))
    s->releases++;
  g_dbus_method_invocation_return_value (inv, NULL);
}

static GVariant *
stand_in_get (GDBusConnection *c, const char *sender, const char *path, const char *iface, const char *prop,
              GError **error, gpointer data)
{
  SensorStandIn *s = data;
  s->gets++;
  if (!strcmp (prop, "HasCompass"))
    return g_variant_new_boolean (s->has_compass);
  if (!strcmp (prop, "CompassHeading"))
    return g_variant_new_double (s->heading);
  if (!strcmp (prop, "HasAccelerometer"))
    return g_variant_new_boolean (FALSE);
  if (!strcmp (prop, "AccelerometerOrientation"))
    return g_variant_new_string ("undefined");
  g_set_error (error, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_PROPERTY, "no property %s", prop);
  return NULL;
}

static void
stand_in_changed (GDBusConnection *c, const char *path, const char *iface, const char *prop, GVariant *value)
{
  GVariantBuilder b;
  const char *none[] = { NULL };
  g_variant_builder_init (&b, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_add (&b, "{sv}", prop, value);
  g_dbus_connection_emit_signal (c, NULL, path, "org.freedesktop.DBus.Properties", "PropertiesChanged",
                                 g_variant_new ("(sa{sv}^as)", iface, &b, none), NULL);
  g_dbus_connection_flush_sync (c, NULL, NULL);
}

/* runs the main loop until *count reaches `want` or `ms` pass; then on for `settle` ms */
static void
spin_until (const int *count, int want, int ms, int settle)
{
  gint64 end = g_get_monotonic_time () + ms * 1000;
  while ((!count || *count < want) && g_get_monotonic_time () < end)
    g_main_context_iteration (NULL, FALSE) || (g_usleep (1000), TRUE);
  end = g_get_monotonic_time () + settle * 1000;
  while (g_get_monotonic_time () < end)
    g_main_context_iteration (NULL, FALSE) || (g_usleep (1000), TRUE);
}

/* iio-sensor-proxy can take its name before a sensor behind a sensor hub is open: HasCompass is false when Navi
 * starts and turns true later. The claim follows it then (not only at the start), the heading comes, and the claim
 * goes back when Navi stops asking. */
static void
test_compass_late_sensor (void)
{
  g_autofree char *daemon = g_find_program_in_path ("dbus-daemon");
  if (!daemon) {
    g_test_skip ("no dbus-daemon");
    return;
  }
  g_autoptr (GTestDBus) bus = g_test_dbus_new (G_TEST_DBUS_NONE);
  g_test_dbus_up (bus);
  /* the compass looks for iio-sensor-proxy on the system bus: here, the test's own */
  g_setenv ("DBUS_SYSTEM_BUS_ADDRESS", g_test_dbus_get_bus_address (bus), TRUE);

  g_autoptr (GError) error = NULL;
  /* the compass's connection (the system bus singleton), held here so that the bus going away at the end does not
   * end the test's process */
  g_autoptr (GDBusConnection) sys = g_bus_get_sync (G_BUS_TYPE_SYSTEM, NULL, &error);
  g_assert_no_error (error);
  g_dbus_connection_set_exit_on_close (sys, FALSE);
  g_autoptr (GDBusConnection) c = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, &error);
  g_assert_no_error (error);
  g_autoptr (GDBusNodeInfo) node = g_dbus_node_info_new_for_xml (sensor_proxy_xml, &error);
  g_assert_no_error (error);
  SensorStandIn s = { .has_compass = FALSE, .heading = 24.9 };
  static const GDBusInterfaceVTable vtable = { stand_in_call, stand_in_get, NULL, { 0 } };
  guint ids[2];
  ids[0] = g_dbus_connection_register_object (c, "/net/hadess/SensorProxy", node->interfaces[0], &vtable, &s, NULL,
                                              &error);
  g_assert_no_error (error);
  ids[1] = g_dbus_connection_register_object (c, "/net/hadess/SensorProxy/Compass", node->interfaces[1], &vtable, &s,
                                              NULL, &error);
  g_assert_no_error (error);
  g_autoptr (GVariant) owned = g_dbus_connection_call_sync (c, "org.freedesktop.DBus", "/org/freedesktop/DBus",
                                                            "org.freedesktop.DBus", "RequestName",
                                                            g_variant_new ("(su)", "net.hadess.SensorProxy", 0),
                                                            G_VARIANT_TYPE ("(u)"), G_DBUS_CALL_FLAGS_NONE, -1, NULL,
                                                            &error);
  g_assert_no_error (error);

  NaviCompass *nc = navi_compass_new ();
  navi_compass_set_active (nc, TRUE);
  /* both proxies have read their properties (2 each); no compass yet, so no claim and no heading */
  spin_until (&s.gets, 4, 5000, 200);
  g_assert_cmpint (s.gets, >=, 4);
  g_assert_cmpint (s.claims, ==, 0);
  double h = -1;
  g_assert_false (navi_compass_get_heading (nc, &h));

  /* the sensor opens: HasCompass turns true, and the compass is claimed */
  s.has_compass = TRUE;
  stand_in_changed (c, "/net/hadess/SensorProxy/Compass", "net.hadess.SensorProxy.Compass", "HasCompass",
                    g_variant_new_boolean (TRUE));
  spin_until (&s.claims, 1, 5000, 100);
  g_assert_cmpint (s.claims, ==, 1);

  /* a reading after the claim is the heading (portrait: the device's top) */
  stand_in_changed (c, "/net/hadess/SensorProxy/Compass", "net.hadess.SensorProxy.Compass", "CompassHeading",
                    g_variant_new_double (24.9));
  spin_until (NULL, 0, 0, 300);
  g_assert_true (navi_compass_get_heading (nc, &h));
  g_assert_cmpfloat_with_epsilon (h, 24.9, 1e-9);
  /* another property's change neither claims again nor releases */
  stand_in_changed (c, "/net/hadess/SensorProxy/Compass", "net.hadess.SensorProxy.Compass", "CompassHeading",
                    g_variant_new_double (30.0));
  spin_until (NULL, 0, 0, 200);
  g_assert_cmpint (s.claims, ==, 1);
  g_assert_cmpint (s.releases, ==, 0);

  /* Navi stops asking: the claim goes back */
  navi_compass_set_active (nc, FALSE);
  spin_until (&s.releases, 1, 5000, 100);
  g_assert_cmpint (s.releases, ==, 1);

  g_object_unref (nc);
  spin_until (NULL, 0, 0, 100);
  g_dbus_connection_unregister_object (c, ids[0]);
  g_dbus_connection_unregister_object (c, ids[1]);
  g_clear_object (&c);
  g_dbus_connection_close_sync (sys, NULL, NULL);
  g_clear_object (&sys);
  g_test_dbus_down (bus);   /* also unsets DBUS_SYSTEM_BUS_ADDRESS */
}

int
main (int argc, char **argv)
{
  g_setenv ("LANGUAGE", "en_US", TRUE);
  g_setenv ("LANG", "en_US.UTF-8", TRUE);
  /* English's decimal point (navi_format_distance writes the user's) */
  g_unsetenv ("LC_ALL");
  g_unsetenv ("LC_NUMERIC");
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/navi/geo", test_geo);
  g_test_add_func ("/navi/compass", test_compass);
  g_test_add_func ("/navi/route/parse", test_route_parse);
  g_test_add_func ("/navi/route/errors", test_route_errors);
  g_test_add_func ("/navi/route/lanes-and-side", test_lanes_and_side);
  g_test_add_func ("/navi/guidance/drive", test_guidance);
  g_test_add_func ("/navi/guidance/upcoming", test_upcoming);
  g_test_add_func ("/navi/guidance/off-route", test_off_route);
  g_test_add_func ("/navi/guidance/alert-text", test_alert_text);
  g_test_add_func ("/navi/guidance/spoken-language", test_spoken_language);
  g_test_add_func ("/navi/mvt", test_mvt);
  g_test_add_func ("/navi/search/fold", test_fold);
  g_test_add_func ("/navi/search/photon", test_photon);
  g_test_add_func ("/navi/search/poi-groups", test_poi_groups);
  g_test_add_func ("/navi/sun", test_sun);
  g_test_add_func ("/navi/compass/late-sensor", test_compass_late_sensor);
  return g_test_run ();
}
