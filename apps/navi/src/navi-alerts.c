/* navi-alerts: the road's speed limit and the speed cameras ahead. See navi-alerts.h. */
#include "navi-alerts.h"

#include "navi-cameras.h"
#include "navi-geo.h"
#include "navi-mvt.h"
#include "navi-roadinfo.h"

#include <glib/gi18n.h>
#include <math.h>
#include <string.h>

#define TRACK        16           /* fixes kept for the map-match: one each 2 s, half a minute */
#define TRACK_GAP_US (2 * G_USEC_PER_SEC)
#define MATCH_GAP_US (12 * G_USEC_PER_SEC)
#define MATCH_KEEP_US (40 * G_USEC_PER_SEC) /* a matched limit this old is not trusted any more */
#define MOVING       2.0          /* m/s: slower than this, nothing is matched */
#define PASSED       20.0         /* metres past a camera: the alert goes */
/* without a route: the cameras of the 3 x 3 overlay tiles (z10, some 40 km a side) around the car, asked again in a
 * new tile; one ahead is within LANE_OFF of the line the car heads along, facing its way where OSM says */
#define AROUND_Z       10
#define AROUND_RETRY_US (60 * G_USEC_PER_SEC)
#define LANE_OFF       30.0
#define ZONE_LINGER_US (45 * G_USEC_PER_SEC) /* France's zone stays this long after the last camera ahead: its end
                                             * does not tell where the camera was */

struct _NaviAlerts {
  Navi *n;
  /* along the route */
  NaviRoute *route;
  NaviRoadInfo *road;
  GArray *cams;          /* NaviSpeedCam on the route, by s */
  GCancellable *cancel;
  guint next_cam;        /* the first camera not passed */
  guint alerted;         /* the cameras before this one were alerted */
  gboolean showing;      /* the card shows */
  /* where the car is, off a route */
  double lat[TRACK], lng[TRACK];
  gint64 when[TRACK];
  guint count, head;
  gint64 last_match;
  GCancellable *match_cancel;
  int match_kmh;
  gint64 match_time;
  char country[4];
  /* the cameras around the car, without a route */
  GArray *around;
  int around_x, around_y;
  gint64 around_asked;
  GCancellable *around_cancel;
  GHashTable *spoken;    /* cameras (cam_key) already said without a route: each is said once */
  gint64 zone_until;
};

/* a camera, the same from every tile */
static gint64
cam_key (const NaviSpeedCam *c)
{
  return (gint64) lround ((c->lat + 90) * 1e5) * 100000000 + lround ((c->lng + 180) * 1e5);
}

NaviAlerts *
navi_alerts_new (Navi *n)
{
  NaviAlerts *a = g_new0 (NaviAlerts, 1);
  a->n = n;
  a->match_kmh = NAVI_LIMIT_UNKNOWN;
  a->spoken = g_hash_table_new_full (g_int64_hash, g_int64_equal, g_free, NULL);
  return a;
}

static void
hide_card (NaviAlerts *a)
{
  if (a->showing)
    navi_panels_show_alert (a->n->panels, NULL, NULL);
  a->showing = FALSE;
}

void
navi_alerts_free (NaviAlerts *a)
{
  if (!a)
    return;
  g_cancellable_cancel (a->cancel);
  g_clear_object (&a->cancel);
  g_cancellable_cancel (a->match_cancel);
  g_clear_object (&a->match_cancel);
  g_cancellable_cancel (a->around_cancel);
  g_clear_object (&a->around_cancel);
  g_clear_pointer (&a->around, g_array_unref);
  g_clear_pointer (&a->spoken, g_hash_table_unref);
  g_clear_pointer (&a->road, navi_road_info_free);
  g_clear_pointer (&a->cams, g_array_unref);
  g_clear_pointer (&a->route, navi_route_unref);
  g_free (a);
}

const char *
navi_alerts_country (NaviAlerts *a)
{
  return a ? a->country : "";
}

static void
set_country (NaviAlerts *a, const char *code)
{
  if (!code || !*code || !strcmp (a->country, code))
    return;
  g_strlcpy (a->country, code, sizeof a->country);
  navi_panels_set_units (a->n->panels, code);
  navi_window_update_overlay (a->n);
}

/* ---- the route's ---- */

static void
road_ready (GObject *src, GAsyncResult *res, gpointer data)
{
  g_autoptr (GError) error = NULL;
  NaviRoadInfo *road = navi_road_info_request_finish (res, &error);
  if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    return;
  NaviAlerts *a = data;
  if (!road) {
    g_message ("road info: %s", error->message);
    return;
  }
  g_clear_pointer (&a->road, navi_road_info_free);
  a->road = road;
  set_country (a, navi_road_info_country_at (road, 0));
}

static void
cams_ready (GObject *src, GAsyncResult *res, gpointer data)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (GArray) all = navi_cameras_fetch_finish (res, &error);
  if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    return;
  NaviAlerts *a = data;
  if (!all || !a->route) {
    if (error)
      g_message ("cameras: %s", error->message);
    return;
  }
  g_clear_pointer (&a->cams, g_array_unref);
  a->cams = navi_cameras_on_route (a->route, all);
  a->next_cam = a->alerted = 0;
  g_message ("cameras: %u on the route", a->cams->len);
}

void
navi_alerts_set_route (NaviAlerts *a, NaviRoute *route)
{
  g_cancellable_cancel (a->cancel);
  g_clear_object (&a->cancel);
  g_clear_pointer (&a->road, navi_road_info_free);
  g_clear_pointer (&a->cams, g_array_unref);
  g_clear_pointer (&a->route, navi_route_unref);
  a->next_cam = a->alerted = 0;
  hide_card (a);
  if (!route)
    return;
  a->route = navi_route_ref (route);
  a->cancel = g_cancellable_new ();
  navi_road_info_request_async (route, a->cancel, road_ready, a);
  navi_cameras_fetch_route_async (route, a->cancel, cams_ready, a);
}

/* ---- off a route: the road where the car is ---- */

static void
matched (GObject *src, GAsyncResult *res, gpointer data)
{
  g_autoptr (GError) error = NULL;
  int kmh;
  char country[4];
  gboolean ok = navi_road_limit_request_finish (res, &kmh, country, &error);
  if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    return;
  NaviAlerts *a = data;
  g_clear_object (&a->match_cancel);
  if (!ok) {
    /* no road there (a car park, a ferry), or no server: no sign rather than an old one */
    a->match_kmh = NAVI_LIMIT_UNKNOWN;
    return;
  }
  a->match_kmh = kmh;
  a->match_time = g_get_monotonic_time ();
  set_country (a, country);
}

static void
track_fix (NaviAlerts *a, const NaviFix *fix, gint64 now)
{
  if (fix->kind == NAVI_FIX_LAST_KNOWN || fix->kind == NAVI_FIX_APPROXIMATE)
    return;
  guint last = (a->head + TRACK - 1) % TRACK;
  if (a->count && now - a->when[last] < TRACK_GAP_US)
    return;
  a->lat[a->head] = fix->lat;
  a->lng[a->head] = fix->lng;
  a->when[a->head] = now;
  a->head = (a->head + 1) % TRACK;
  a->count = MIN (a->count + 1, TRACK);
}

static void
maybe_match (NaviAlerts *a, const NaviFix *fix, gint64 now)
{
  if (a->match_cancel || a->count < 3 || fix->speed < MOVING || now - a->last_match < MATCH_GAP_US)
    return;
  a->last_match = now;
  double lat[TRACK], lng[TRACK], sec[TRACK];
  guint first = (a->head + TRACK - a->count) % TRACK;
  for (guint i = 0; i < a->count; i++) {
    guint k = (first + i) % TRACK;
    lat[i] = a->lat[k];
    lng[i] = a->lng[k];
    sec[i] = (a->when[k] - a->when[first]) / (double) G_USEC_PER_SEC;
  }
  a->match_cancel = g_cancellable_new ();
  navi_road_limit_request_async (lat, lng, sec, a->count, a->match_cancel, matched, a);
}

/* ---- the cameras ahead ---- */

static void
alert_camera (NaviAlerts *a, const NaviSpeedCam *cam, double ahead, gboolean first)
{
  g_autofree char *dist = navi_format_distance (MAX (ahead, 0));
  const char *title = cam->kind == NAVI_CAM_RED_LIGHT ? _("Red light camera")
                      : cam->kind == NAVI_CAM_SECTION ? _("Section control")
                                                      : _("Speed camera");
  navi_panels_show_alert (a->n->panels, title, dist);
  a->showing = TRUE;
  if (!first)
    return;
  /* in the voice's language, which may not be the UI's */
  NaviSaying words = cam->kind == NAVI_CAM_RED_LIGHT ? NAVI_SAY_RED_LIGHT_CAMERA
                     : cam->kind == NAVI_CAM_SECTION ? NAVI_SAY_SECTION_CONTROL
                                                     : NAVI_SAY_SPEED_CAMERA;
  navi_panels_say_alert (a->n->panels, navi_guidance_saying (words, navi_route_language ()));
}

static void
check_cameras (NaviAlerts *a, const NaviFix *fix, double s)
{
  NaviSettings *st = a->n->settings;
  NaviCamPolicy policy = navi_cameras_policy (a->road ? navi_road_info_country_at (a->road, s) : a->country);
  if (!a->cams || !st->cameras || policy == NAVI_CAMS_OFF) {
    hide_card (a);
    return;
  }
  /* the ones passed */
  while (a->next_cam < a->cams->len) {
    const NaviSpeedCam *c = &g_array_index (a->cams, NaviSpeedCam, a->next_cam);
    double end = c->s + PASSED;
    if (policy == NAVI_CAMS_ZONES) {
      double from, to;
      navi_cameras_zone (c, a->road ? navi_road_info_limit_at (a->road, c->s) : 0, &from, &to);
      end = to;
    }
    if (s <= end)
      break;
    a->next_cam++;
    hide_card (a);
  }
  if (a->next_cam >= a->cams->len)
    return;
  const NaviSpeedCam *c = &g_array_index (a->cams, NaviSpeedCam, a->next_cam);
  gboolean first = a->alerted <= a->next_cam;
  if (policy == NAVI_CAMS_ZONES) {
    /* France: a zone, never the camera */
    double from, to;
    navi_cameras_zone (c, a->road ? navi_road_info_limit_at (a->road, c->s) : 0, &from, &to);
    if (s < from - 100)
      return;
    if (first && !st->cameras_notice_seen)
      navi_panels_camera_notice (a->n->panels);
    navi_panels_show_alert (a->n->panels, _("Danger zone"), NULL);
    a->showing = TRUE;
    if (first)
      navi_panels_say_alert (a->n->panels, navi_guidance_saying (NAVI_SAY_DANGER_ZONE, navi_route_language ()));
    a->alerted = a->next_cam + 1;
    return;
  }
  double ahead = c->s - s;
  if (ahead > navi_cameras_alert_distance (MAX (fix->speed, 0)) && !a->showing)
    return;
  if (first && !st->cameras_notice_seen)
    navi_panels_camera_notice (a->n->panels);
  alert_camera (a, c, ahead, first);
  a->alerted = a->next_cam + 1;
}

/* ---- the cameras ahead, without a route ---- */

static void
around_ready (GObject *src, GAsyncResult *res, gpointer data)
{
  g_autoptr (GError) error = NULL;
  GArray *cams = navi_cameras_fetch_finish (res, &error);
  if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    return;
  NaviAlerts *a = data;
  g_clear_object (&a->around_cancel);
  if (!cams)
    return; /* asked again in a minute */
  g_clear_pointer (&a->around, g_array_unref);
  a->around = cams;
}

static void
fetch_around (NaviAlerts *a, const NaviFix *fix, gint64 now)
{
  int x, y;
  navi_mvt_tile_at (AROUND_Z, fix->lat, fix->lng, &x, &y);
  if (a->around_cancel)
    return;
  if (a->around && x == a->around_x && y == a->around_y)
    return;
  if (!a->around && x == a->around_x && y == a->around_y && now - a->around_asked < AROUND_RETRY_US)
    return; /* that failed a moment ago */
  a->around_x = x;
  a->around_y = y;
  a->around_asked = now;
  a->around_cancel = g_cancellable_new ();
  navi_cameras_fetch_around_async (fix->lat, fix->lng, a->around_cancel, around_ready, a);
}

/* the nearest camera on the way ahead within `reach` metres, or NULL */
static const NaviSpeedCam *
camera_ahead (NaviAlerts *a, const NaviFix *fix, double reach, double *distance)
{
  const NaviSpeedCam *best = NULL;
  *distance = G_MAXDOUBLE;
  for (guint i = 0; i < a->around->len; i++) {
    const NaviSpeedCam *c = &g_array_index (a->around, NaviSpeedCam, i);
    /* a degree of latitude is 111 km: most are ruled out without the trigonometry */
    if (fabs (c->lat - fix->lat) * 111000 > reach + 50)
      continue;
    double d = navi_geo_distance (fix->lat, fix->lng, c->lat, c->lng);
    if (d > reach || d >= *distance)
      continue;
    double off = fabs (navi_angle_diff (navi_geo_bearing (fix->lat, fix->lng, c->lat, c->lng), fix->heading));
    if (off > 90 || d * sin (off * G_PI / 180) > LANE_OFF)
      continue; /* behind, or off to the side */
    if (c->dir >= 0 && fabs (navi_angle_diff (c->dir, fix->heading)) > 60)
      continue; /* it checks the other way */
    best = c;
    *distance = d;
  }
  return best;
}

static void
check_cameras_around (NaviAlerts *a, const NaviFix *fix, gint64 now)
{
  NaviSettings *st = a->n->settings;
  NaviCamPolicy policy = navi_cameras_policy (a->country);
  if (!st->cameras || policy == NAVI_CAMS_OFF || fix->kind == NAVI_FIX_LAST_KNOWN ||
      fix->kind == NAVI_FIX_APPROXIMATE) {
    hide_card (a);
    return;
  }
  fetch_around (a, fix, now);
  if (!a->around || fix->speed < MOVING || fix->heading < 0)
    return; /* standing (the card stays as it was), or no way to tell ahead */
  double d;
  if (policy == NAVI_CAMS_ZONES) {
    /* France: "Danger zone" from up to the zone's length ahead of a camera, and a while after it */
    int limit = a->match_time && now - a->match_time < MATCH_KEEP_US ? a->match_kmh : 0;
    NaviSpeedCam probe = { 0 };
    double from, to;
    navi_cameras_zone (&probe, limit, &from, &to);
    const NaviSpeedCam *c = camera_ahead (a, fix, to - from, &d);
    if (c) {
      gint64 key = cam_key (c);
      if (!g_hash_table_contains (a->spoken, &key)) {
        g_hash_table_add (a->spoken, g_memdup2 (&key, sizeof key));
        if (!st->cameras_notice_seen)
          navi_panels_camera_notice (a->n->panels);
        navi_panels_say_alert (a->n->panels, navi_guidance_saying (NAVI_SAY_DANGER_ZONE, navi_route_language ()));
      }
      a->zone_until = now + ZONE_LINGER_US;
    }
    if (now < a->zone_until) {
      navi_panels_show_alert (a->n->panels, _("Danger zone"), NULL);
      a->showing = TRUE;
    } else {
      hide_card (a);
    }
    return;
  }
  const NaviSpeedCam *c = camera_ahead (a, fix, navi_cameras_alert_distance (fix->speed), &d);
  if (!c) {
    hide_card (a); /* passed (it is behind now), or none */
    return;
  }
  gint64 key = cam_key (c);
  gboolean first = !g_hash_table_contains (a->spoken, &key);
  if (first) {
    g_hash_table_add (a->spoken, g_memdup2 (&key, sizeof key));
    if (!st->cameras_notice_seen)
      navi_panels_camera_notice (a->n->panels);
  }
  alert_camera (a, c, d, first);
}

/* ---- each fix ---- */

void
navi_alerts_fix (NaviAlerts *a, const NaviFix *fix, const NaviProgress *p)
{
  gint64 now = g_get_monotonic_time ();
  track_fix (a, fix, now);
  int kmh = NAVI_LIMIT_UNKNOWN;
  if (p && p->on_route && a->road) {
    kmh = navi_road_info_limit_at (a->road, p->s);
    set_country (a, navi_road_info_country_at (a->road, p->s));
    check_cameras (a, fix, p->s);
  } else {
    if (p && !p->on_route)
      hide_card (a); /* off the route: its cameras are not ahead any more */
    maybe_match (a, fix, now);
    if (a->match_time && now - a->match_time < MATCH_KEEP_US)
      kmh = a->match_kmh;
    if (!p)
      check_cameras_around (a, fix, now); /* no route: the cameras on the way ahead */
  }
  navi_panels_set_limit (a->n->panels, kmh);
}
