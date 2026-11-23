/* navi-route: see navi-route.h. */
#include "navi-route.h"

#include <json-glib/json-glib.h>
#include <libsoup/soup.h>
#include <math.h>
#include <string.h>

typedef struct {
  NaviRoute route;
  gatomicrefcount ref;
} RouteBox;

NaviRoute *
navi_route_ref (NaviRoute *route)
{
  g_atomic_ref_count_inc (&((RouteBox *) route)->ref);
  return route;
}

void
navi_route_unref (NaviRoute *route)
{
  if (!route)
    return;
  RouteBox *box = (RouteBox *) route;
  if (!g_atomic_ref_count_dec (&box->ref))
    return;
  for (guint i = 0; i < route->n_maneuvers; i++) {
    NaviManeuver *m = &route->maneuvers[i];
    g_free (m->instruction);
    g_free (m->verbal_alert);
    g_free (m->verbal_pre);
    g_free (m->verbal_post);
    g_free (m->street);
    g_free (m->ref);
    g_free (m->exit);
  }
  g_free (route->maneuvers);
  g_free (route->points);
  g_free (route->via);
  g_free (box);
}

guint
navi_route_maneuver_at (const NaviRoute *route, double s)
{
  guint i = 0;
  while (i + 1 < route->n_maneuvers && route->maneuvers[i + 1].s <= s)
    i++;
  return i;
}

const char *
navi_route_street_at (const NaviRoute *route, double s)
{
  if (!route->n_maneuvers)
    return NULL;
  const NaviManeuver *m = &route->maneuvers[navi_route_maneuver_at (route, s)];
  return m->street;
}

double
navi_route_time_left (const NaviRoute *route, double s)
{
  if (!route->n_maneuvers)
    return 0;
  guint i = navi_route_maneuver_at (route, s);
  const NaviManeuver *m = &route->maneuvers[i];
  double into = m->length > 0 ? CLAMP ((s - m->s) / m->length, 0.0, 1.0) : 1;
  double t = m->time * (1 - into);
  for (guint j = i + 1; j < route->n_maneuvers; j++)
    t += route->maneuvers[j].time;
  return t;
}

/* ---- parsing ---- */

static char *
dup_member (JsonObject *o, const char *name)
{
  const char *s = json_object_get_string_member_with_default (o, name, NULL);
  return s && *s ? g_strdup (s) : NULL;
}

/* the first text of a sign's elements ("exit_number_elements": [{"text": "12"}]) */
static char *
sign_text (JsonObject *sign, const char *name)
{
  if (!sign || !json_object_has_member (sign, name))
    return NULL;
  JsonArray *a = json_object_get_array_member (sign, name);
  if (!a || !json_array_get_length (a))
    return NULL;
  JsonObject *e = json_array_get_object_element (a, 0);
  return e ? dup_member (e, "text") : NULL;
}

/* a road number ("B 1", "A 100", "E 30") rather than a name */
static gboolean
is_ref (const char *s)
{
  const char *p = s;
  while (g_ascii_isupper (*p))
    p++;
  if (p == s || p - s > 3)
    return FALSE;
  if (*p == ' ')
    p++;
  if (!g_ascii_isdigit (*p))
    return FALSE;
  while (g_ascii_isalnum (*p))
    p++;
  return *p == '\0';
}

/* The street a manoeuvre leads onto, as its instruction names it: a name before a road number ("Potsdamer Straße",
 * not "B 1"), the names it begins on first. `ref`, when there is a number too, gets it. */
static char *
pick_street (JsonObject *m, char **ref)
{
  static const char *const members[] = { "begin_street_names", "street_names" };
  const char *name = NULL, *number = NULL;
  for (guint k = 0; k < G_N_ELEMENTS (members); k++) {
    JsonArray *a = json_object_has_member (m, members[k]) ? json_object_get_array_member (m, members[k]) : NULL;
    for (guint i = 0; a && i < json_array_get_length (a); i++) {
      const char *s = json_array_get_string_element (a, i);
      if (!s || !*s)
        continue;
      if (is_ref (s)) {
        if (!number)
          number = s;
      } else if (!name) {
        name = s;
      }
    }
    if (name)
      break;
  }
  if (ref)
    *ref = number && name ? g_strdup (number) : NULL;
  return g_strdup (name ? name : number);
}

/* "Via A - B": the two streets the route drives longest on, in the order it meets them */
static char *
make_via (NaviRoute *r)
{
  g_autoptr (GHashTable) sums = g_hash_table_new (g_str_hash, g_str_equal);
  g_autoptr (GPtrArray) order = g_ptr_array_new ();
  for (guint i = 0; i < r->n_maneuvers; i++) {
    const NaviManeuver *m = &r->maneuvers[i];
    if (!m->street || m->type == NAVI_TURN_DESTINATION || m->type == NAVI_TURN_DESTINATION_LEFT ||
        m->type == NAVI_TURN_DESTINATION_RIGHT)
      continue;
    double *sum = g_hash_table_lookup (sums, m->street);
    if (!sum) {
      sum = g_new0 (double, 1);
      g_hash_table_insert (sums, m->street, sum);
      g_ptr_array_add (order, m->street);
    }
    *sum += m->length;
  }
  if (!order->len)
    return NULL;
  /* the two longest */
  const char *top[2] = { NULL, NULL };
  double len[2] = { -1, -1 };
  for (guint i = 0; i < order->len; i++) {
    double l = *(double *) g_hash_table_lookup (sums, order->pdata[i]);
    if (l > len[0]) {
      top[1] = top[0], len[1] = len[0];
      top[0] = order->pdata[i], len[0] = l;
    } else if (l > len[1]) {
      top[1] = order->pdata[i], len[1] = l;
    }
  }
  char *via;
  if (top[1] && len[1] > r->length * 0.08) {
    /* in the order the route meets them */
    gboolean swap = FALSE;
    for (guint i = 0; i < order->len; i++) {
      if (order->pdata[i] == top[0])
        break;
      if (order->pdata[i] == top[1]) {
        swap = TRUE;
        break;
      }
    }
    via = g_strdup_printf ("%s - %s", swap ? top[1] : top[0], swap ? top[0] : top[1]);
  } else {
    via = g_strdup (top[0]);
  }
  GHashTableIter it;
  gpointer v;
  g_hash_table_iter_init (&it, sums);
  while (g_hash_table_iter_next (&it, NULL, &v))
    g_free (v);
  return via;
}

static NaviRoute *
parse_trip (JsonObject *trip, GError **error)
{
  JsonArray *legs = json_object_has_member (trip, "legs") ? json_object_get_array_member (trip, "legs") : NULL;
  if (!legs || !json_array_get_length (legs)) {
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "the route has no legs");
    return NULL;
  }
  RouteBox *box = g_new0 (RouteBox, 1);
  g_atomic_ref_count_init (&box->ref);
  NaviRoute *r = &box->route;
  g_autoptr (GArray) points = g_array_new (FALSE, FALSE, sizeof (NaviPathPoint));
  g_autoptr (GArray) mans = g_array_new (FALSE, TRUE, sizeof (NaviManeuver));
  gint stop_point = -1; /* the point where the first of several legs ends: the stop */
  for (guint l = 0; l < json_array_get_length (legs); l++) {
    JsonObject *leg = json_array_get_object_element (legs, l);
    guint base = points->len ? points->len - 1 : 0; /* a leg starts where the one before ended */
    g_autoptr (GArray) leg_points = g_array_new (FALSE, FALSE, sizeof (NaviPathPoint));
    const char *shape = json_object_get_string_member_with_default (leg, "shape", "");
    if (!navi_polyline_decode (shape, 6, leg_points) || leg_points->len < 2) {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "the route's shape does not decode");
      navi_route_unref (r);
      return NULL;
    }
    g_array_append_vals (points, (NaviPathPoint *) leg_points->data + (points->len ? 1 : 0),
                         leg_points->len - (points->len ? 1 : 0));
    if (l == 0 && json_array_get_length (legs) > 1)
      stop_point = points->len - 1;
    JsonArray *ms = json_object_get_array_member (leg, "maneuvers");
    for (guint i = 0; ms && i < json_array_get_length (ms); i++) {
      JsonObject *mo = json_array_get_object_element (ms, i);
      NaviManeuver m = { 0 };
      m.type = json_object_get_int_member_with_default (mo, "type", 0);
      /* an intermediate leg's arrival is not the trip's: only the last leg keeps its destination */
      if (l + 1 < json_array_get_length (legs) &&
          (m.type == NAVI_TURN_DESTINATION || m.type == NAVI_TURN_DESTINATION_LEFT || m.type == NAVI_TURN_DESTINATION_RIGHT))
        continue;
      m.instruction = dup_member (mo, "instruction");
      m.verbal_alert = dup_member (mo, "verbal_transition_alert_instruction");
      m.verbal_pre = dup_member (mo, "verbal_pre_transition_instruction");
      m.verbal_post = dup_member (mo, "verbal_post_transition_instruction");
      m.street = pick_street (mo, &m.ref);
      JsonObject *sign = json_object_has_member (mo, "sign") ? json_object_get_object_member (mo, "sign") : NULL;
      m.exit = sign_text (sign, "exit_number_elements");
      if (!m.street)
        m.street = sign_text (sign, "exit_toward_elements");
      if (!m.street)
        m.street = sign_text (sign, "exit_branch_elements");
      m.roundabout_exit = json_object_get_int_member_with_default (mo, "roundabout_exit_count", 0);
      m.begin = base + json_object_get_int_member_with_default (mo, "begin_shape_index", 0);
      m.end = base + json_object_get_int_member_with_default (mo, "end_shape_index", 0);
      m.length = json_object_get_double_member_with_default (mo, "length", 0) * 1000; /* km */
      m.time = json_object_get_double_member_with_default (mo, "time", 0);
      g_array_append_val (mans, m);
    }
  }
  navi_path_measure ((NaviPathPoint *) points->data, points->len);
  r->n_points = points->len;
  r->points = (NaviPathPoint *) g_array_free (g_steal_pointer (&points), FALSE);
  r->n_maneuvers = mans->len;
  r->maneuvers = (NaviManeuver *) g_array_free (g_steal_pointer (&mans), FALSE);
  for (guint i = 0; i < r->n_maneuvers; i++) {
    NaviManeuver *m = &r->maneuvers[i];
    m->begin = MIN (m->begin, r->n_points - 1);
    m->end = MIN (m->end, r->n_points - 1);
    m->s = r->points[m->begin].s;
  }
  /* the manoeuvre lengths from the path itself, so they add up to where the next one is */
  for (guint i = 0; i < r->n_maneuvers; i++)
    r->maneuvers[i].length = (i + 1 < r->n_maneuvers ? r->maneuvers[i + 1].s : r->points[r->n_points - 1].s) -
                             r->maneuvers[i].s;
  r->length = r->points[r->n_points - 1].s;
  r->stop_s = stop_point >= 0 ? r->points[stop_point].s : -1;
  JsonObject *sum = json_object_has_member (trip, "summary") ? json_object_get_object_member (trip, "summary") : NULL;
  r->time = sum ? json_object_get_double_member_with_default (sum, "time", 0) : 0;
  if (sum) {
    r->toll = json_object_get_boolean_member_with_default (sum, "has_toll", FALSE);
    r->highway = json_object_get_boolean_member_with_default (sum, "has_highway", FALSE);
    r->ferry = json_object_get_boolean_member_with_default (sum, "has_ferry", FALSE);
  }
  if (r->time <= 0)
    for (guint i = 0; i < r->n_maneuvers; i++)
      r->time += r->maneuvers[i].time;
  r->min_lat = r->min_lng = G_MAXDOUBLE;
  r->max_lat = r->max_lng = -G_MAXDOUBLE;
  for (guint i = 0; i < r->n_points; i++) {
    r->min_lat = MIN (r->min_lat, r->points[i].lat);
    r->max_lat = MAX (r->max_lat, r->points[i].lat);
    r->min_lng = MIN (r->min_lng, r->points[i].lng);
    r->max_lng = MAX (r->max_lng, r->points[i].lng);
  }
  r->via = make_via (r);
  return r;
}

G_DEFINE_QUARK (navi-route-error-quark, navi_route_error)

GPtrArray *
navi_route_parse (const char *json, gssize length, GError **error)
{
  g_autoptr (JsonParser) parser = json_parser_new ();
  if (!json_parser_load_from_data (parser, json, length, error))
    return NULL;
  JsonNode *root = json_parser_get_root (parser);
  JsonObject *o = root && JSON_NODE_HOLDS_OBJECT (root) ? json_node_get_object (root) : NULL;
  if (!o || !json_object_has_member (o, "trip")) {
    /* Valhalla's error: {"error_code": 171, "error": "No suitable edges near location", "status_code": 400} */
    const char *msg = o ? json_object_get_string_member_with_default (o, "error", NULL) : NULL;
    gint64 code = o ? json_object_get_int_member_with_default (o, "error_code", 0) : 0;
    NaviRouteError kind = code == 171 ? NAVI_ROUTE_ERROR_NO_ROAD
                          : code == 170 || code == 442 || code == 443 || !code ? NAVI_ROUTE_ERROR_NO_ROUTE
                                                                                 : NAVI_ROUTE_ERROR_SERVER;
    g_set_error (error, NAVI_ROUTE_ERROR, kind, "%s", msg ? msg : "no route");
    return NULL;
  }
  g_autoptr (GPtrArray) routes = g_ptr_array_new_with_free_func ((GDestroyNotify) navi_route_unref);
  NaviRoute *best = parse_trip (json_object_get_object_member (o, "trip"), error);
  if (!best)
    return NULL;
  g_ptr_array_add (routes, best);
  if (json_object_has_member (o, "alternates")) {
    JsonArray *alts = json_object_get_array_member (o, "alternates");
    for (guint i = 0; alts && i < json_array_get_length (alts); i++) {
      JsonObject *a = json_array_get_object_element (alts, i);
      if (!a || !json_object_has_member (a, "trip"))
        continue;
      NaviRoute *r = parse_trip (json_object_get_object_member (a, "trip"), NULL);
      if (r)
        g_ptr_array_add (routes, r);
    }
  }
  return g_steal_pointer (&routes);
}

/* ---- the server ---- */

const char *
navi_server_url (void)
{
  static const char *url;
  if (!url) {
    const char *env = g_getenv ("NAVI_SERVER");
    url = env && *env ? env : "https://navi.vibeos.top";
  }
  return url;
}

GObject *
navi_http_session (void)
{
  static SoupSession *session;
  if (!session)
    session = soup_session_new_with_options ("user-agent", "VibeOS-Navi/0.1", "timeout", 20, "idle-timeout", 60,
                                             "max-conns-per-host", 6, NULL);
  return G_OBJECT (session);
}

const char *
navi_route_language (void)
{
  /* the languages Valhalla's narrative has (valhalla/locales), by the UI's first language */
  static const char *const known[] = {
    "bg-BG", "ca-ES", "cs-CZ", "da-DK", "de-DE", "el-GR", "en-GB", "en-US", "eo", "es-ES", "et-EE", "fi-FI",
    "fr-FR", "hi-IN", "hu-HU", "it-IT", "ja-JP", "nb-NO", "nl-NL", "pl-PL", "pt-BR", "pt-PT", "ro-RO", "ru-RU",
    "sk-SK", "sl-SI", "sv-SE", "tr-TR", "uk-UA", NULL
  };
  static char *chosen;
  if (chosen)
    return chosen;
  const char *const *names = g_get_language_names ();
  for (guint i = 0; names[i] && !chosen; i++) {
    if (!strcmp (names[i], "C") || !strcmp (names[i], "POSIX"))
      continue;
    g_autofree char *tag = g_strdup (names[i]);
    g_strdelimit (tag, ".@", '\0');
    g_strdelimit (tag, "_", '-');
    /* the exact language and country first, then the language anywhere */
    for (guint k = 0; known[k] && !chosen; k++)
      if (!g_ascii_strcasecmp (known[k], tag))
        chosen = g_strdup (known[k]);
    for (guint k = 0; known[k] && !chosen; k++)
      if (!g_ascii_strncasecmp (known[k], tag, 2) && (tag[2] == '\0' || tag[2] == '-'))
        chosen = g_strdup (known[k]);
  }
  if (!chosen)
    chosen = g_strdup ("en-US");
  return chosen;
}

typedef struct {
  char *body;
  guint attempt;
} Request;

static void
request_free (gpointer p)
{
  Request *r = p;
  g_free (r->body);
  g_free (r);
}

static void send_route (GTask *task);

static gboolean
retry_route (gpointer data)
{
  send_route (data);
  return G_SOURCE_REMOVE;
}

static void
route_received (GObject *source, GAsyncResult *res, gpointer data)
{
  g_autoptr (GTask) task = data;
  Request *r = g_task_get_task_data (task);
  SoupMessage *msg = soup_session_get_async_result_message (SOUP_SESSION (source), res);
  GError *error = NULL;
  g_autoptr (GBytes) body = soup_session_send_and_read_finish (SOUP_SESSION (source), res, &error);
  if (!body) {
    g_task_return_error (task, error);
    return;
  }
  guint status = soup_message_get_status (msg);
  if (status == 429 && r->attempt < 3) {
    /* the server lets each address ask once a second, a burst of ten: wait a little and ask again */
    r->attempt++;
    g_timeout_add_full (G_PRIORITY_DEFAULT, 1100 * r->attempt, retry_route, g_object_ref (task), g_object_unref);
    return;
  }
  gsize len;
  const char *text = g_bytes_get_data (body, &len);
  NAVI_PRIVATE_DEBUG ("navi: route answer %u, %zu bytes\n", status, len);
  GPtrArray *routes = navi_route_parse (text, len, &error);
  if (!routes) {
    if (status != 200 && error && error->domain == JSON_PARSER_ERROR) {
      g_clear_error (&error);
      error = g_error_new (NAVI_ROUTE_ERROR, NAVI_ROUTE_ERROR_SERVER, "the routing server answered %u", status);
    }
    g_task_return_error (task, error);
    return;
  }
  g_task_return_pointer (task, routes, (GDestroyNotify) g_ptr_array_unref);
}

static void
send_route (GTask *task)
{
  Request *r = g_task_get_task_data (task);
  g_autofree char *url = g_strdup_printf ("%s/route", navi_server_url ());
  g_autoptr (SoupMessage) msg = soup_message_new ("POST", url);
  g_autoptr (GBytes) bytes = g_bytes_new (r->body, strlen (r->body));
  soup_message_set_request_body_from_bytes (msg, "application/json", bytes);
  soup_session_send_and_read_async (SOUP_SESSION (navi_http_session ()), msg, G_PRIORITY_DEFAULT,
                                    g_task_get_cancellable (task), route_received, g_object_ref (task));
}

void
navi_route_request_async (const NaviRouteRequest *rq, GCancellable *cancellable, GAsyncReadyCallback callback,
                          gpointer user_data)
{
  GTask *task = g_task_new (NULL, cancellable, callback, user_data);
  g_task_set_source_tag (task, navi_route_request_async);
  g_autoptr (JsonBuilder) b = json_builder_new ();
  json_builder_begin_object (b);
  json_builder_set_member_name (b, "locations");
  json_builder_begin_array (b);
  json_builder_begin_object (b);
  json_builder_set_member_name (b, "lat");
  json_builder_add_double_value (b, rq->from_lat);
  json_builder_set_member_name (b, "lon");
  json_builder_add_double_value (b, rq->from_lng);
  if (rq->from_heading >= 0) {
    /* the way the car points: no route that starts with a U-turn on a road it is already driving */
    json_builder_set_member_name (b, "heading");
    json_builder_add_int_value (b, lround (rq->from_heading) % 360);
    json_builder_set_member_name (b, "heading_tolerance");
    json_builder_add_int_value (b, 45);
  }
  json_builder_end_object (b);
  if (rq->have_via) {
    /* a stop: a break, where the route may turn round */
    json_builder_begin_object (b);
    json_builder_set_member_name (b, "lat");
    json_builder_add_double_value (b, rq->via_lat);
    json_builder_set_member_name (b, "lon");
    json_builder_add_double_value (b, rq->via_lng);
    json_builder_set_member_name (b, "type");
    json_builder_add_string_value (b, "break");
    json_builder_end_object (b);
  }
  json_builder_begin_object (b);
  json_builder_set_member_name (b, "lat");
  json_builder_add_double_value (b, rq->to_lat);
  json_builder_set_member_name (b, "lon");
  json_builder_add_double_value (b, rq->to_lng);
  json_builder_end_object (b);
  json_builder_end_array (b);
  json_builder_set_member_name (b, "costing");
  json_builder_add_string_value (b, "auto");
  json_builder_set_member_name (b, "units");
  json_builder_add_string_value (b, "kilometers");
  json_builder_set_member_name (b, "language");
  json_builder_add_string_value (b, rq->language ? rq->language : navi_route_language ());
  /* the default directions type, "instructions": "maneuvers" leaves every text out */
  if (rq->alternates && !rq->have_via) {
    json_builder_set_member_name (b, "alternates");
    json_builder_add_int_value (b, MIN (rq->alternates, 2u));
  }
  json_builder_end_object (b);
  g_autoptr (JsonGenerator) gen = json_generator_new ();
  g_autoptr (JsonNode) root = json_builder_get_root (b);
  json_generator_set_root (gen, root);
  Request *r = g_new0 (Request, 1);
  r->body = json_generator_to_data (gen, NULL);
  g_task_set_task_data (task, r, request_free);
  send_route (task);
  g_object_unref (task);
}

GPtrArray *
navi_route_request_finish (GAsyncResult *result, GError **error)
{
  return g_task_propagate_pointer (G_TASK (result), error);
}
