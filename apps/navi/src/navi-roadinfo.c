/* navi-roadinfo: speed limits and traffic signals from Valhalla's trace_attributes. See navi-roadinfo.h. */
#include "navi-roadinfo.h"

#include <json-glib/json-glib.h>
#include <langinfo.h>
#include <libsoup/soup.h>
#include <locale.h>
#include <math.h>
#include <string.h>

#define PIECE_POINTS 4000 /* the server takes 5000 */

NaviRoadInfo *
navi_road_info_new (void)
{
  NaviRoadInfo *info = g_new0 (NaviRoadInfo, 1);
  info->limits = g_array_new (FALSE, FALSE, sizeof (NaviLimitSpan));
  info->signals = g_array_new (FALSE, FALSE, sizeof (NaviRoadMark));
  info->countries = g_array_new (FALSE, FALSE, sizeof (NaviCountryMark));
  return info;
}

void
navi_road_info_free (NaviRoadInfo *info)
{
  if (!info)
    return;
  g_array_unref (info->limits);
  g_array_unref (info->signals);
  g_array_unref (info->countries);
  g_free (info);
}

int
navi_road_info_limit_at (const NaviRoadInfo *info, double s)
{
  if (!info || !info->limits->len)
    return NAVI_LIMIT_UNKNOWN;
  /* the last span that starts at or before s */
  guint lo = 0, hi = info->limits->len;
  while (hi - lo > 1) {
    guint mid = (lo + hi) / 2;
    if (g_array_index (info->limits, NaviLimitSpan, mid).from <= s)
      lo = mid;
    else
      hi = mid;
  }
  const NaviLimitSpan *span = &g_array_index (info->limits, NaviLimitSpan, lo);
  return s >= span->from && s < span->to ? span->kmh : NAVI_LIMIT_UNKNOWN;
}

const char *
navi_road_info_country_at (const NaviRoadInfo *info, double s)
{
  const char *code = NULL;
  for (guint i = 0; info && i < info->countries->len; i++) {
    const NaviCountryMark *c = &g_array_index (info->countries, NaviCountryMark, i);
    if (c->from > s)
      break;
    code = c->code;
  }
  return code;
}

/* the country codes of an answer's "admins", by index */
static GPtrArray *
admin_codes (JsonObject *o)
{
  GPtrArray *codes = g_ptr_array_new ();
  JsonArray *admins = json_object_has_member (o, "admins") ? json_object_get_array_member (o, "admins") : NULL;
  for (guint i = 0; admins && i < json_array_get_length (admins); i++) {
    JsonObject *a = json_array_get_object_element (admins, i);
    g_ptr_array_add (codes, (gpointer) (a ? json_object_get_string_member_with_default (a, "country_code", "") : ""));
  }
  return codes;
}

/* an edge's country: its end node's admin */
static const char *
edge_country (JsonObject *e, GPtrArray *codes)
{
  JsonObject *node = json_object_has_member (e, "end_node") ? json_object_get_object_member (e, "end_node") : NULL;
  gint64 i = node ? json_object_get_int_member_with_default (node, "admin_index", -1) : -1;
  return i >= 0 && i < codes->len ? codes->pdata[i] : "";
}

/* a speed_limit member: a number of km/h, "unlimited", or none */
static int
edge_limit (JsonObject *e)
{
  if (!json_object_has_member (e, "speed_limit"))
    return NAVI_LIMIT_UNKNOWN;
  JsonNode *v = json_object_get_member (e, "speed_limit");
  if (JSON_NODE_HOLDS_VALUE (v) && json_node_get_value_type (v) == G_TYPE_STRING)
    return !g_strcmp0 (json_node_get_string (v), "unlimited") ? NAVI_LIMIT_NONE : NAVI_LIMIT_UNKNOWN;
  if (JSON_NODE_HOLDS_VALUE (v)) {
    gint64 kmh = json_node_get_int (v);
    return kmh > 0 && kmh < 250 ? (int) kmh : NAVI_LIMIT_UNKNOWN;
  }
  return NAVI_LIMIT_UNKNOWN;
}

gboolean
navi_road_info_add_answer (NaviRoadInfo *info, const NaviRoute *route, guint first, const char *json, gssize length,
                           GError **error)
{
  g_autoptr (JsonParser) parser = json_parser_new ();
  if (!json_parser_load_from_data (parser, json, length, error))
    return FALSE;
  JsonNode *root = json_parser_get_root (parser);
  JsonObject *o = root && JSON_NODE_HOLDS_OBJECT (root) ? json_node_get_object (root) : NULL;
  JsonArray *edges = o && json_object_has_member (o, "edges") ? json_object_get_array_member (o, "edges") : NULL;
  if (!edges) {
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "%s",
                 o && json_object_has_member (o, "error") ? json_object_get_string_member (o, "error")
                                                           : "no edges in the answer");
    return FALSE;
  }
  g_autoptr (GPtrArray) codes = admin_codes (o);
  for (guint i = 0; i < json_array_get_length (edges); i++) {
    JsonObject *e = json_array_get_object_element (edges, i);
    if (!e)
      continue;
    gint64 b = json_object_get_int_member_with_default (e, "begin_shape_index", -1);
    gint64 en = json_object_get_int_member_with_default (e, "end_shape_index", -1);
    if (b < 0 || en < b || first + en >= route->n_points)
      continue;
    double from = route->points[first + b].s, to = route->points[first + en].s;
    const char *cc = edge_country (e, codes);
    if (*cc && (!info->countries->len ||
                strcmp (g_array_index (info->countries, NaviCountryMark, info->countries->len - 1).code, cc))) {
      NaviCountryMark mark = { info->countries->len ? from : 0, "" };
      g_strlcpy (mark.code, cc, sizeof mark.code);
      g_array_append_val (info->countries, mark);
    }
    int kmh = edge_limit (e);
    if (kmh != NAVI_LIMIT_UNKNOWN && to > from) {
      NaviLimitSpan *last = info->limits->len ? &g_array_index (info->limits, NaviLimitSpan, info->limits->len - 1)
                                              : NULL;
      if (last && last->kmh == kmh && fabs (last->to - from) < 0.5)
        last->to = MAX (last->to, to); /* the same limit goes on */
      else if (!last || from >= last->to - 0.5) {
        NaviLimitSpan span = { last ? MAX (from, last->to) : from, to, kmh };
        g_array_append_val (info->limits, span);
      }
    }
    if (json_object_get_boolean_member_with_default (e, "traffic_signal", FALSE)) {
      const NaviPathPoint *pt = &route->points[first + en];
      NaviRoadMark m = { pt->s, pt->lat, pt->lng };
      if (!info->signals->len || g_array_index (info->signals, NaviRoadMark, info->signals->len - 1).s < m.s - 1)
        g_array_append_val (info->signals, m);
    }
  }
  return TRUE;
}

/* ---- asking ---- */

static void
encode_value (GString *out, gint64 v)
{
  guint64 u = v < 0 ? ~((guint64) v << 1) : (guint64) v << 1;
  while (u >= 0x20) {
    g_string_append_c (out, (char) ((0x20 | (u & 0x1f)) + 63));
    u >>= 5;
  }
  g_string_append_c (out, (char) (u + 63));
}

/* points [from, to) as Valhalla's polyline, six decimals */
static char *
encode_points (const NaviPathPoint *points, guint from, guint to)
{
  GString *out = g_string_new (NULL);
  gint64 plat = 0, plng = 0;
  for (guint i = from; i < to; i++) {
    gint64 lat = llround (points[i].lat * 1e6), lng = llround (points[i].lng * 1e6);
    encode_value (out, lat - plat);
    encode_value (out, lng - plng);
    plat = lat;
    plng = lng;
  }
  return g_string_free (out, FALSE);
}

static char *
attributes_body (const char *shape_member, const char *shape, JsonNode *shape_array, const char *match,
                 gboolean signals)
{
  g_autoptr (JsonBuilder) b = json_builder_new ();
  json_builder_begin_object (b);
  if (shape) {
    json_builder_set_member_name (b, shape_member);
    json_builder_add_string_value (b, shape);
  } else {
    json_builder_set_member_name (b, shape_member);
    json_builder_add_value (b, json_node_ref (shape_array)); /* the builder takes the reference it is given */
  }
  json_builder_set_member_name (b, "costing");
  json_builder_add_string_value (b, "auto");
  json_builder_set_member_name (b, "shape_match");
  json_builder_add_string_value (b, match);
  json_builder_set_member_name (b, "filters");
  json_builder_begin_object (b);
  json_builder_set_member_name (b, "attributes");
  json_builder_begin_array (b);
  json_builder_add_string_value (b, "edge.speed_limit");
  json_builder_add_string_value (b, "edge.begin_shape_index");
  json_builder_add_string_value (b, "edge.end_shape_index");
  json_builder_add_string_value (b, "node.admin_index");
  json_builder_add_string_value (b, "admin.country_code");
  if (signals)
    json_builder_add_string_value (b, "edge.traffic_signal");
  json_builder_end_array (b);
  json_builder_set_member_name (b, "action");
  json_builder_add_string_value (b, "include");
  json_builder_end_object (b);
  json_builder_end_object (b);
  g_autoptr (JsonGenerator) gen = json_generator_new ();
  g_autoptr (JsonNode) root = json_builder_get_root (b);
  json_generator_set_root (gen, root);
  return json_generator_to_data (gen, NULL);
}

typedef struct {
  NaviRoute *route;
  NaviRoadInfo *info;
  guint next;    /* the first point of the piece to ask for next */
  guint asked;   /* the first point of the piece asked */
  int attempt;
} Walk;

static void
walk_free (gpointer data)
{
  Walk *w = data;
  navi_route_unref (w->route);
  navi_road_info_free (w->info);
  g_free (w);
}

static void ask_piece (GTask *task);

static gboolean
retry_piece (gpointer data)
{
  ask_piece (G_TASK (data));
  return G_SOURCE_REMOVE;
}

static void
piece_received (GObject *source, GAsyncResult *res, gpointer data)
{
  g_autoptr (GTask) task = data;
  Walk *w = g_task_get_task_data (task);
  SoupMessage *msg = soup_session_get_async_result_message (SOUP_SESSION (source), res);
  GError *error = NULL;
  g_autoptr (GBytes) body = soup_session_send_and_read_finish (SOUP_SESSION (source), res, &error);
  if (!body) {
    g_task_return_error (task, error);
    return;
  }
  guint status = soup_message_get_status (msg);
  if (status == 429 && w->attempt < 3) {
    /* the server's pace for an address: wait a little and ask again */
    w->attempt++;
    w->next = w->asked;
    g_timeout_add_full (G_PRIORITY_DEFAULT, 1100 * w->attempt, retry_piece, g_object_ref (task), g_object_unref);
    return;
  }
  gsize len;
  const char *text = g_bytes_get_data (body, &len);
  /* a piece the server cannot walk (a road missing from its graph) leaves that piece unknown, no more */
  if (status == 200 && !navi_road_info_add_answer (w->info, w->route, w->asked, text, len, &error)) {
    g_debug ("trace_attributes: %s", error->message);
    g_clear_error (&error);
  }
  w->attempt = 0;
  if (w->next + 1 >= w->route->n_points) {
    g_task_return_pointer (task, g_steal_pointer (&w->info), (GDestroyNotify) navi_road_info_free);
    return;
  }
  ask_piece (task);
}

static void
ask_piece (GTask *task)
{
  Walk *w = g_task_get_task_data (task);
  guint from = w->next, to = MIN (from + PIECE_POINTS, w->route->n_points);
  w->asked = from;
  w->next = to - 1; /* the pieces share a point */
  g_autofree char *shape = encode_points (w->route->points, from, to);
  g_autofree char *body = attributes_body ("encoded_polyline", shape, NULL, "edge_walk", TRUE);
  g_autofree char *url = g_strdup_printf ("%s/trace_attributes", navi_server_url ());
  g_autoptr (SoupMessage) msg = soup_message_new ("POST", url);
  g_autoptr (GBytes) bytes = g_bytes_new (body, strlen (body));
  soup_message_set_request_body_from_bytes (msg, "application/json", bytes);
  soup_session_send_and_read_async (SOUP_SESSION (navi_http_session ()), msg, G_PRIORITY_LOW,
                                    g_task_get_cancellable (task), piece_received, g_object_ref (task));
}

void
navi_road_info_request_async (NaviRoute *route, GCancellable *cancellable, GAsyncReadyCallback callback,
                              gpointer user_data)
{
  g_autoptr (GTask) task = g_task_new (NULL, cancellable, callback, user_data);
  g_task_set_source_tag (task, navi_road_info_request_async);
  Walk *w = g_new0 (Walk, 1);
  w->route = navi_route_ref (route);
  w->info = navi_road_info_new ();
  g_task_set_task_data (task, w, walk_free);
  if (route->n_points < 2) {
    g_task_return_pointer (task, g_steal_pointer (&w->info), (GDestroyNotify) navi_road_info_free);
    return;
  }
  ask_piece (task);
}

NaviRoadInfo *
navi_road_info_request_finish (GAsyncResult *result, GError **error)
{
  return g_task_propagate_pointer (G_TASK (result), error);
}

/* ---- the road where the car is ---- */

typedef struct {
  int kmh;
  char country[4];
} Here;

static void
limit_received (GObject *source, GAsyncResult *res, gpointer data)
{
  g_autoptr (GTask) task = data;
  SoupMessage *msg = soup_session_get_async_result_message (SOUP_SESSION (source), res);
  GError *error = NULL;
  g_autoptr (GBytes) body = soup_session_send_and_read_finish (SOUP_SESSION (source), res, &error);
  if (!body) {
    g_task_return_error (task, error);
    return;
  }
  if (soup_message_get_status (msg) != 200) {
    g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_FAILED, "trace_attributes answered %u",
                             soup_message_get_status (msg));
    return;
  }
  gsize len;
  const char *text = g_bytes_get_data (body, &len);
  g_autoptr (JsonParser) parser = json_parser_new ();
  if (!json_parser_load_from_data (parser, text, len, &error)) {
    g_task_return_error (task, error);
    return;
  }
  JsonNode *root = json_parser_get_root (parser);
  JsonObject *o = root && JSON_NODE_HOLDS_OBJECT (root) ? json_node_get_object (root) : NULL;
  JsonArray *edges = o && json_object_has_member (o, "edges") ? json_object_get_array_member (o, "edges") : NULL;
  guint n = edges ? json_array_get_length (edges) : 0;
  /* the edge the track ends on */
  Here *here = g_new0 (Here, 1);
  here->kmh = NAVI_LIMIT_UNKNOWN;
  if (n) {
    JsonObject *last = json_array_get_object_element (edges, n - 1);
    g_autoptr (GPtrArray) codes = admin_codes (o);
    here->kmh = edge_limit (last);
    g_strlcpy (here->country, edge_country (last, codes), sizeof here->country);
  }
  g_task_return_pointer (task, here, g_free);
}

void
navi_road_limit_request_async (const double *lat, const double *lng, const double *seconds, guint n,
                               GCancellable *cancellable, GAsyncReadyCallback callback, gpointer user_data)
{
  g_autoptr (GTask) task = g_task_new (NULL, cancellable, callback, user_data);
  g_task_set_source_tag (task, navi_road_limit_request_async);
  g_autoptr (JsonBuilder) b = json_builder_new ();
  json_builder_begin_array (b);
  for (guint i = 0; i < n; i++) {
    json_builder_begin_object (b);
    json_builder_set_member_name (b, "lat");
    json_builder_add_double_value (b, lat[i]);
    json_builder_set_member_name (b, "lon");
    json_builder_add_double_value (b, lng[i]);
    json_builder_set_member_name (b, "time");
    json_builder_add_int_value (b, llround (seconds[i]));
    json_builder_end_object (b);
  }
  json_builder_end_array (b);
  g_autoptr (JsonNode) shape = json_builder_get_root (b);
  g_autofree char *body = attributes_body ("shape", NULL, shape, "map_snap", FALSE);
  g_autofree char *url = g_strdup_printf ("%s/trace_attributes", navi_server_url ());
  g_autoptr (SoupMessage) msg = soup_message_new ("POST", url);
  g_autoptr (GBytes) bytes = g_bytes_new (body, strlen (body));
  soup_message_set_request_body_from_bytes (msg, "application/json", bytes);
  soup_session_send_and_read_async (SOUP_SESSION (navi_http_session ()), msg, G_PRIORITY_LOW, cancellable,
                                    limit_received, g_object_ref (task));
}

gboolean
navi_road_limit_request_finish (GAsyncResult *result, int *kmh, char country[4], GError **error)
{
  g_autofree Here *here = g_task_propagate_pointer (G_TASK (result), error);
  *kmh = here ? here->kmh : NAVI_LIMIT_UNKNOWN;
  g_strlcpy (country, here ? here->country : "", 4);
  return here != NULL;
}

/* ---- units ---- */

int
navi_limit_display (int kmh, gboolean mph)
{
  if (kmh <= 0 || !mph)
    return kmh;
  return (int) lround (kmh / 1.609344 / 5) * 5;
}

gboolean
navi_country_uses_mph (const char *cc)
{
  static const char *const mph[] = { "GB", "IM", "JE", "GG", "US", "PR", "GU", "VI", "AS", "MP", "LR", "MM", NULL };
  return cc && g_strv_contains (mph, cc);
}

gboolean
navi_country_uses_us_signs (const char *cc)
{
  static const char *const us[] = { "US", "PR", "GU", "VI", "AS", "MP", NULL };
  return cc && g_strv_contains (us, cc);
}

gboolean
navi_speed_in_mph (void)
{
  const char *env = g_getenv ("NAVI_SPEED_UNITS");
  if (env)
    return !g_ascii_strcasecmp (env, "mph");
  /* the US measure everything in miles; the UK and a few others measure roads in miles but much else metric */
  const char *lc = setlocale (LC_MEASUREMENT, NULL);
  if (lc && (strstr (lc, "_GB") || strstr (lc, "_US") || strstr (lc, "_LR") || strstr (lc, "_MM")))
    return TRUE;
#ifdef _NL_MEASUREMENT_MEASUREMENT
  const char *m = nl_langinfo (_NL_MEASUREMENT_MEASUREMENT);
  if (m && m[0] == 2)
    return TRUE;
#endif
  return FALSE;
}
