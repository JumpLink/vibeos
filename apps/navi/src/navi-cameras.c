/* navi-cameras: speed cameras for the approach alert. See navi-cameras.h. */
#include "navi-cameras.h"

#include "navi-mvt.h"

#include <libsoup/soup.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TILE_Z      10
#define MAX_TILES   96   /* a route of some 600 km */
#define NEAR_ROUTE  35.0 /* metres: a camera this close to the route is on it */
#define FACING      60.0 /* degrees between the camera's direction and the route's */

/* Where speed camera alerts may be used. The owner decides; this table is the one place to change it.
 * Countries not listed: on. */
static const struct {
  const char *country;
  NaviCamPolicy policy;
} policies[] = {
  /* owner 2026-11-23: on by default. §23 (1c) StVO bars the DRIVER from using one while driving (a passenger may):
   * the one-time notice says so */
  { "DE", NAVI_CAMS_ON },
  /* banned outright (Art. 98a SVG), as Waze has them off there */
  { "CH", NAVI_CAMS_OFF },
  /* only "zones de danger" (décret 2012-3), never the camera's place */
  { "FR", NAVI_CAMS_ZONES },
};

NaviCamPolicy
navi_cameras_policy (const char *country)
{
  if (!country || !*country)
    return NAVI_CAMS_OFF;
  for (guint i = 0; i < G_N_ELEMENTS (policies); i++)
    if (!g_ascii_strcasecmp (policies[i].country, country))
      return policies[i].policy;
  return NAVI_CAMS_ON;
}

/* ---- reading ---- */

typedef struct {
  int z, x, y;
  GArray *cams;
} Parse;

static void
camera_feature (const char *layer, const NaviMvtFeature *f, gpointer data)
{
  Parse *p = data;
  guint n;
  const int *pts = navi_mvt_feature_points (f, &n, NULL, NULL);
  if (navi_mvt_feature_type (f) != NAVI_MVT_POINT || !n)
    return;
  NaviSpeedCam c = { 0 };
  navi_mvt_to_lat_lng (p->z, p->x, p->y, navi_mvt_feature_extent (f), pts[0], pts[1], &c.lat, &c.lng);
  const char *kind = navi_mvt_feature_get (f, "kind");
  c.kind = !g_strcmp0 (kind, "red_light") ? NAVI_CAM_RED_LIGHT
           : !g_strcmp0 (kind, "section") ? NAVI_CAM_SECTION
                                          : NAVI_CAM_SPEED;
  const char *dir = navi_mvt_feature_get (f, "dir");
  c.dir = dir ? (int) g_ascii_strtoll (dir, NULL, 10) % 360 : -1;
  const char *ms = navi_mvt_feature_get (f, "maxspeed");
  c.maxspeed = ms ? (int) g_ascii_strtoll (ms, NULL, 10) : 0;
  g_array_append_val (p->cams, c);
}

gboolean
navi_cameras_parse_tile (const guint8 *data, gsize length, int z, int x, int y, GArray *cams)
{
  static const char *const layers[] = { "cameras", NULL };
  Parse p = { z, x, y, cams };
  return navi_mvt_read (data, length, layers, camera_feature, &p);
}

static int
by_s (gconstpointer a, gconstpointer b)
{
  double d = ((const NaviSpeedCam *) a)->s - ((const NaviSpeedCam *) b)->s;
  return d < 0 ? -1 : d > 0;
}

GArray *
navi_cameras_on_route (const NaviRoute *r, GArray *cams)
{
  GArray *out = g_array_new (FALSE, FALSE, sizeof (NaviSpeedCam));
  for (guint i = 0; cams && i < cams->len; i++) {
    NaviSpeedCam c = g_array_index (cams, NaviSpeedCam, i);
    /* a quick box test first: most of a tile's cameras are nowhere near the route */
    if (c.lat < r->min_lat - 0.001 || c.lat > r->max_lat + 0.001 || c.lng < r->min_lng - 0.0015 ||
        c.lng > r->max_lng + 0.0015)
      continue;
    double s, bearing;
    double off = navi_path_project (r->points, r->n_points, c.lat, c.lng, 0, G_MAXDOUBLE, &s, &bearing);
    if (off > NEAR_ROUTE)
      continue;
    if (c.dir >= 0 && fabs (navi_angle_diff (c.dir, bearing)) > FACING)
      continue; /* it checks the other way */
    c.s = s;
    g_array_append_val (out, c);
  }
  g_array_sort (out, by_s);
  /* the same camera from two tiles (or a relation's device and its node) once */
  for (guint i = 1; i < out->len;) {
    NaviSpeedCam *a = &g_array_index (out, NaviSpeedCam, i - 1), *b = &g_array_index (out, NaviSpeedCam, i);
    if (b->s - a->s < 15)
      g_array_remove_index (out, i);
    else
      i++;
  }
  return out;
}

double
navi_cameras_alert_distance (double speed)
{
  return CLAMP (speed * 12, 300, 800);
}

void
navi_cameras_zone (const NaviSpeedCam *cam, int limit_kmh, double *from, double *to)
{
  double len = limit_kmh > 0 && limit_kmh <= 50 ? 300 : limit_kmh > 0 && limit_kmh <= 90 ? 2000 : 4000;
  /* the camera's place in the zone: 20 to 80 % of the way in, from its coordinates */
  guint h = (guint) (fabs (cam->lat) * 1e5) * 2654435761u ^ (guint) (fabs (cam->lng) * 1e5);
  double at = 0.2 + 0.6 * (h % 1000) / 1000.0;
  *from = cam->s - len * at;
  *to = *from + len;
}

/* ---- fetching ---- */

typedef struct {
  GArray *cams;
  GPtrArray *tiles; /* "x/y" still to ask */
  guint pending;
} Fetch;

static void
fetch_free (gpointer data)
{
  Fetch *f = data;
  g_array_unref (f->cams);
  g_ptr_array_unref (f->tiles);
  g_free (f);
}

static void ask_next (GTask *task);

static void
tile_received (GObject *source, GAsyncResult *res, gpointer data)
{
  g_autoptr (GTask) task = data;
  Fetch *f = g_task_get_task_data (task);
  SoupMessage *msg = soup_session_get_async_result_message (SOUP_SESSION (source), res);
  g_autoptr (GError) error = NULL;
  g_autoptr (GBytes) bytes = soup_session_send_and_read_finish (SOUP_SESSION (source), res, &error);
  f->pending--;
  if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
    if (!f->pending)
      g_task_return_error (task, g_steal_pointer (&error));
    return;
  }
  /* a 404 is a tile without cameras (or a server without the overlay): nothing to add */
  if (bytes && soup_message_get_status (msg) == 200) {
    int x, y;
    const char *key = g_object_get_data (G_OBJECT (msg), "navi-tile");
    if (key && sscanf (key, "%d/%d", &x, &y) == 2) {
      gsize len;
      const guint8 *d = g_bytes_get_data (bytes, &len);
      navi_cameras_parse_tile (d, len, TILE_Z, x, y, f->cams);
    }
  }
  ask_next (task);
  if (!f->pending && !f->tiles->len && !g_task_get_completed (task))
    g_task_return_pointer (task, g_array_ref (f->cams), (GDestroyNotify) g_array_unref);
}

static void
ask_next (GTask *task)
{
  Fetch *f = g_task_get_task_data (task);
  /* four at a time */
  while (f->tiles->len && f->pending < 4 && !g_cancellable_is_cancelled (g_task_get_cancellable (task))) {
    g_autofree char *key = g_ptr_array_steal_index (f->tiles, f->tiles->len - 1);
    g_autofree char *url = g_strdup_printf ("%s/tiles/navi-overlay/%d/%s.mvt", navi_server_url (), TILE_Z, key);
    g_autoptr (SoupMessage) msg = soup_message_new ("GET", url);
    g_object_set_data_full (G_OBJECT (msg), "navi-tile", g_strdup (key), g_free);
    f->pending++;
    soup_session_send_and_read_async (SOUP_SESSION (navi_http_session ()), msg, G_PRIORITY_LOW,
                                      g_task_get_cancellable (task), tile_received, g_object_ref (task));
  }
}

static void
add_tile (GHashTable *seen, GPtrArray *tiles, double lat, double lng)
{
  int x, y;
  navi_mvt_tile_at (TILE_Z, lat, lng, &x, &y);
  char *key = g_strdup_printf ("%d/%d", x, y);
  if (g_hash_table_contains (seen, key) || tiles->len >= MAX_TILES) {
    g_free (key);
    return;
  }
  g_hash_table_add (seen, key);
  g_ptr_array_add (tiles, g_strdup (key));
}

static void
start (GTask *task, GPtrArray *tiles)
{
  Fetch *f = g_new0 (Fetch, 1);
  f->cams = g_array_new (FALSE, FALSE, sizeof (NaviSpeedCam));
  f->tiles = tiles;
  g_task_set_task_data (task, f, fetch_free);
  if (!tiles->len) {
    g_task_return_pointer (task, g_array_ref (f->cams), (GDestroyNotify) g_array_unref);
    return;
  }
  ask_next (task);
}

void
navi_cameras_fetch_route_async (NaviRoute *route, GCancellable *cancellable, GAsyncReadyCallback callback,
                                gpointer user_data)
{
  g_autoptr (GTask) task = g_task_new (NULL, cancellable, callback, user_data);
  g_task_set_source_tag (task, navi_cameras_fetch_route_async);
  g_autoptr (GHashTable) seen = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  GPtrArray *tiles = g_ptr_array_new_with_free_func (g_free);
  /* every 300 m, and a little to each side, so that a camera just over a tile's edge is not missed */
  double next = 0;
  for (guint i = 0; i < route->n_points; i++) {
    const NaviPathPoint *pt = &route->points[i];
    if (pt->s < next && i + 1 < route->n_points)
      continue;
    next = pt->s + 300;
    for (int dy = -1; dy <= 1; dy++)
      for (int dx = -1; dx <= 1; dx++)
        add_tile (seen, tiles, pt->lat + dy * 0.0005, pt->lng + dx * 0.0008);
  }
  /* the start's first: the order they are asked in is from the end of the array */
  for (guint i = 0, j = tiles->len ? tiles->len - 1 : 0; i < j; i++, j--) {
    gpointer t = tiles->pdata[i];
    tiles->pdata[i] = tiles->pdata[j];
    tiles->pdata[j] = t;
  }
  start (task, tiles);
}

void
navi_cameras_fetch_around_async (double lat, double lng, GCancellable *cancellable, GAsyncReadyCallback callback,
                                 gpointer user_data)
{
  g_autoptr (GTask) task = g_task_new (NULL, cancellable, callback, user_data);
  g_task_set_source_tag (task, navi_cameras_fetch_around_async);
  g_autoptr (GHashTable) seen = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  GPtrArray *tiles = g_ptr_array_new_with_free_func (g_free);
  int x, y;
  navi_mvt_tile_at (TILE_Z, lat, lng, &x, &y);
  for (int dy = -1; dy <= 1; dy++)
    for (int dx = -1; dx <= 1; dx++) {
      char *key = g_strdup_printf ("%d/%d", x + dx, y + dy);
      g_hash_table_add (seen, key);
      g_ptr_array_add (tiles, g_strdup (key));
    }
  start (task, tiles);
}

GArray *
navi_cameras_fetch_finish (GAsyncResult *result, GError **error)
{
  return g_task_propagate_pointer (G_TASK (result), error);
}
