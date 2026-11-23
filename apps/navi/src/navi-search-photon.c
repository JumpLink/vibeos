/* navi-search-photon: NaviSearch over our own Photon geocoder (see navi-search.h).
 *
 * Photon is at navi.vibeos.top/photon (Germany for now; GET only, five requests a second from an address, a burst of
 * twenty: the sheet waits for a pause in the typing and cancels what it asked before).
 * Photon's API: GET /api?q=TEXT&lat=&lon=&limit=&lang= finds places as the text is typed, nearer ones first with
 * the position given; GET /reverse?lat=&lon= names a point. Both answer a GeoJSON FeatureCollection whose features
 * are points with the OSM object's name, street, housenumber, postcode, city, district, osm_key/osm_value (its
 * kind: amenity/fuel) and type (house, street, city, ...). A category chip is a search for the category's word with
 * Photon's osm_tag filter, inside a box around the position.
 *
 * Whatever Photon cannot answer (no connection, an error status such as its 503 "search is unavailable right now",
 * an answer that is no GeoJSON) goes to the fallback backend, so the sheet always shows something, and says that
 * search is down (navi_search_photon_is_down ()). Nothing is logged: the text and the position stay
 * between the app and our server. */
#include "navi-search.h"
#include "navi-route.h"

#include <json-glib/json-glib.h>
#include <libsoup/soup.h>
#include <string.h>

#define LIMIT 10
#define CATEGORY_BOX_DEG 0.06 /* about 6 km around the position */

#define NAVI_TYPE_SEARCH_PHOTON (navi_search_photon_get_type ())
G_DECLARE_FINAL_TYPE (NaviSearchPhoton, navi_search_photon, NAVI, SEARCH_PHOTON, GObject)

struct _NaviSearchPhoton {
  GObject parent_instance;
  char *base;
  NaviSearch *fallback;
  gboolean down; /* the last answer was the fallback's: Photon could not answer */
};

static void navi_search_photon_iface_init (NaviSearchInterface *iface);
G_DEFINE_FINAL_TYPE_WITH_CODE (NaviSearchPhoton, navi_search_photon, G_TYPE_OBJECT,
                               G_IMPLEMENT_INTERFACE (NAVI_TYPE_SEARCH, navi_search_photon_iface_init))

static void
navi_search_photon_finalize (GObject *object)
{
  NaviSearchPhoton *self = NAVI_SEARCH_PHOTON (object);
  g_free (self->base);
  g_clear_object (&self->fallback);
  G_OBJECT_CLASS (navi_search_photon_parent_class)->finalize (object);
}

static void
navi_search_photon_class_init (NaviSearchPhotonClass *klass)
{
  G_OBJECT_CLASS (klass)->finalize = navi_search_photon_finalize;
}

static void
navi_search_photon_init (NaviSearchPhoton *self)
{
}

NaviSearch *
navi_search_photon_new (const char *base_url, NaviSearch *fallback)
{
  NaviSearchPhoton *self = g_object_new (NAVI_TYPE_SEARCH_PHOTON, NULL);
  const char *env = g_getenv ("NAVI_PHOTON_URL");
  self->base = base_url ? g_strdup (base_url) : env && *env ? g_strdup (env)
                                                            : g_strconcat (navi_server_url (), "/photon", NULL);
  if (g_str_has_suffix (self->base, "/"))
    self->base[strlen (self->base) - 1] = '\0';
  self->fallback = fallback ? g_object_ref (fallback) : NULL;
  return NAVI_SEARCH (self);
}

/* Photon's languages: the UI's when it has it, else its default (the local names) */
static const char *
photon_language (void)
{
  const char *lang = navi_route_language ();
  static const char *const known[] = { "de", "en", "fr", "it", NULL };
  for (guint i = 0; known[i]; i++)
    if (!g_ascii_strncasecmp (lang, known[i], 2))
      return known[i];
  return "default";
}

/* ---- the answer ---- */

static const char *
prop (JsonObject *p, const char *name)
{
  const char *s = json_object_has_member (p, name) ? json_object_get_string_member_with_default (p, name, NULL) : NULL;
  return s && *s ? s : NULL;
}

static void
append (GString *s, const char *part)
{
  if (!part)
    return;
  if (s->len)
    g_string_append (s, ", ");
  g_string_append (s, part);
}

static NaviPlace *
feature_place (JsonObject *f)
{
  JsonObject *g = json_object_has_member (f, "geometry") ? json_object_get_object_member (f, "geometry") : NULL;
  JsonObject *p = json_object_has_member (f, "properties") ? json_object_get_object_member (f, "properties") : NULL;
  JsonArray *c = g && json_object_has_member (g, "coordinates") ? json_object_get_array_member (g, "coordinates") : NULL;
  if (!p || !c || json_array_get_length (c) < 2)
    return NULL;
  double lng = json_array_get_double_element (c, 0), lat = json_array_get_double_element (c, 1);
  const char *type = prop (p, "type"), *key = prop (p, "osm_key"), *value = prop (p, "osm_value");
  const char *name = prop (p, "name"), *street = prop (p, "street"), *number = prop (p, "housenumber");
  const char *city = prop (p, "city"), *district = prop (p, "district"), *locality = prop (p, "locality");
  const char *postcode = prop (p, "postcode");

  NaviPlaceKind kind;
  if (!g_strcmp0 (type, "house") && !name)
    kind = NAVI_PLACE_ADDRESS;
  else if (!g_strcmp0 (type, "street") || (!g_strcmp0 (key, "highway") && !number))
    kind = NAVI_PLACE_STREET;
  else if (!g_strcmp0 (key, "place") || !g_strcmp0 (key, "boundary") || !g_strcmp0 (type, "city") ||
           !g_strcmp0 (type, "district") || !g_strcmp0 (type, "locality") || !g_strcmp0 (type, "county") ||
           !g_strcmp0 (type, "state") || !g_strcmp0 (type, "country"))
    kind = NAVI_PLACE_LOCALITY;
  else
    kind = NAVI_PLACE_POI;

  g_autofree char *title = NULL;
  if (name)
    title = g_strdup (name);
  else if (street && number)
    title = g_strdup_printf ("%s %s", street, number);
  else if (street)
    title = g_strdup (street);
  else
    return NULL;

  /* where it is, without repeating the title */
  g_autoptr (GString) detail = g_string_new (NULL);
  if (kind == NAVI_PLACE_POI && street) {
    g_autofree char *addr = number ? g_strdup_printf ("%s %s", street, number) : g_strdup (street);
    append (detail, addr);
  }
  if (kind == NAVI_PLACE_ADDRESS && postcode && city) {
    g_autofree char *pc = g_strdup_printf ("%s %s", postcode, city);
    append (detail, district ? district : NULL);
    append (detail, pc);
  } else {
    const char *near = district ? district : locality;
    if (near && g_strcmp0 (near, title))
      append (detail, near);
    if (city && g_strcmp0 (city, title) && g_strcmp0 (city, near))
      append (detail, city);
  }
  if (kind == NAVI_PLACE_LOCALITY) {
    const char *state = prop (p, "state"), *country = prop (p, "country");
    if (!detail->len)
      append (detail, state ? state : country);
  }
  NaviPlace *place = navi_place_new (kind, title, detail->len ? detail->str : NULL, lat, lng);
  if (kind == NAVI_PLACE_POI)
    place->category = g_strdup (value);
  return place;
}

GPtrArray *
navi_search_photon_parse (const char *json, gssize length, GError **error)
{
  g_autoptr (JsonParser) parser = json_parser_new ();
  if (!json_parser_load_from_data (parser, json, length, error))
    return NULL;
  JsonNode *root = json_parser_get_root (parser);
  JsonObject *o = root && JSON_NODE_HOLDS_OBJECT (root) ? json_node_get_object (root) : NULL;
  JsonArray *features = o && json_object_has_member (o, "features") ? json_object_get_array_member (o, "features") : NULL;
  if (!features) {
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "not a GeoJSON FeatureCollection");
    return NULL;
  }
  GPtrArray *places = g_ptr_array_new_with_free_func ((GDestroyNotify) navi_place_free);
  g_autoptr (GHashTable) seen = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  for (guint i = 0; i < json_array_get_length (features); i++) {
    JsonObject *f = json_array_get_object_element (features, i);
    NaviPlace *p = f ? feature_place (f) : NULL;
    if (!p)
      continue;
    /* one street is often several OSM ways: the same name and detail only once */
    g_autofree char *key = g_strdup_printf ("%d|%s|%s", p->kind, p->name, p->detail ? p->detail : "");
    if (p->kind != NAVI_PLACE_POI && g_hash_table_contains (seen, key)) {
      navi_place_free (p);
      continue;
    }
    g_hash_table_add (seen, g_steal_pointer (&key));
    g_ptr_array_add (places, p);
  }
  return places;
}

/* ---- asking ---- */

typedef struct {
  char *text;
  NaviCategory category;
  double lat, lng;
  gboolean reverse;
} Ask;

static void
ask_free (gpointer p)
{
  Ask *a = p;
  g_free (a->text);
  g_free (a);
}

static void
fallback_searched (GObject *source, GAsyncResult *res, gpointer data)
{
  g_autoptr (GTask) task = data;
  GError *error = NULL;
  GPtrArray *places = navi_search_search_finish (NAVI_SEARCH (source), res, &error);
  if (places)
    g_task_return_pointer (task, places, (GDestroyNotify) g_ptr_array_unref);
  else
    g_task_return_error (task, error);
}

static void
fallback_reversed (GObject *source, GAsyncResult *res, gpointer data)
{
  g_autoptr (GTask) task = data;
  GError *error = NULL;
  NaviPlace *place = navi_search_reverse_finish (NAVI_SEARCH (source), res, &error);
  if (place)
    g_task_return_pointer (task, place, (GDestroyNotify) navi_place_free);
  else
    g_task_return_error (task, error);
}

/* Photon did not answer: the fallback does, or the error stands */
static void
use_fallback (GTask *task, GError *error)
{
  NaviSearchPhoton *self = g_task_get_source_object (task);
  Ask *a = g_task_get_task_data (task);
  if (!self->fallback || g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
    g_task_return_error (task, error);
    return;
  }
  g_debug ("Photon: %s; asking the fallback", error->message);
  g_error_free (error);
  self->down = TRUE;
  if (a->reverse)
    navi_search_reverse_async (self->fallback, a->lat, a->lng, g_task_get_cancellable (task), fallback_reversed,
                               g_object_ref (task));
  else
    navi_search_search_async (self->fallback, a->text, a->category, a->lat, a->lng, g_task_get_cancellable (task),
                              fallback_searched, g_object_ref (task));
}

static void
received (GObject *source, GAsyncResult *res, gpointer data)
{
  g_autoptr (GTask) task = data;
  Ask *a = g_task_get_task_data (task);
  SoupMessage *msg = soup_session_get_async_result_message (SOUP_SESSION (source), res);
  GError *error = NULL;
  g_autoptr (GBytes) body = soup_session_send_and_read_finish (SOUP_SESSION (source), res, &error);
  if (!body) {
    use_fallback (task, error);
    return;
  }
  guint status = soup_message_get_status (msg);
  if (status != 200) {
    use_fallback (task, g_error_new (G_IO_ERROR, G_IO_ERROR_FAILED, "Photon answered %u", status));
    return;
  }
  gsize len;
  const char *text = g_bytes_get_data (body, &len);
  GPtrArray *places = navi_search_photon_parse (text, len, &error);
  if (!places) {
    use_fallback (task, error);
    return;
  }
  NAVI_SEARCH_PHOTON (g_task_get_source_object (task))->down = FALSE;
  if (a->reverse) {
    NaviPlace *p = places->len ? navi_place_copy (places->pdata[0])
                               : navi_place_new (NAVI_PLACE_POINT, NULL, NULL, a->lat, a->lng);
    /* the point pressed, not the house's centre */
    p->lat = a->lat;
    p->lng = a->lng;
    g_ptr_array_unref (places);
    g_task_return_pointer (task, p, (GDestroyNotify) navi_place_free);
  } else {
    g_task_return_pointer (task, places, (GDestroyNotify) g_ptr_array_unref);
  }
}

static void
add_coord (GString *url, const char *name, double v)
{
  char buf[G_ASCII_DTOSTR_BUF_SIZE];
  g_string_append_printf (url, "&%s=%s", name, g_ascii_formatd (buf, sizeof buf, "%.5f", v));
}

static void
send (GTask *task, GString *url)
{
  NAVI_PRIVATE_DEBUG ("navi: photon %s\n", url->str);
  SoupMessage *msg = soup_message_new ("GET", url->str);
  if (!msg) {
    use_fallback (task, g_error_new (G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "bad Photon URL"));
    return;
  }
  soup_session_send_and_read_async (SOUP_SESSION (navi_http_session ()), msg, G_PRIORITY_DEFAULT,
                                    g_task_get_cancellable (task), received, g_object_ref (task));
  g_object_unref (msg);
}

/* the category's word in the UI's language and its OSM tags */
static const char *
category_query (NaviCategory c, const char *const **tags)
{
  static const char *const fuel[] = { "amenity:fuel", NULL };
  static const char *const food[] = { "amenity:restaurant", "amenity:fast_food", "amenity:cafe", NULL };
  static const char *const parking[] = { "amenity:parking", NULL };
  gboolean de = !g_ascii_strncasecmp (navi_route_language (), "de", 2);
  switch (c) {
  case NAVI_CATEGORY_FUEL: *tags = fuel; return de ? "Tankstelle" : "fuel";
  case NAVI_CATEGORY_FOOD: *tags = food; return de ? "Restaurant" : "restaurant";
  case NAVI_CATEGORY_PARKING: *tags = parking; return de ? "Parkplatz" : "parking";
  default: *tags = NULL; return NULL;
  }
}

static void
photon_search_async (NaviSearch *search, const char *text, NaviCategory category, double near_lat, double near_lng,
                     GCancellable *cancellable, GAsyncReadyCallback callback, gpointer user_data)
{
  NaviSearchPhoton *self = NAVI_SEARCH_PHOTON (search);
  g_autoptr (GTask) task = g_task_new (self, cancellable, callback, user_data);
  g_task_set_source_tag (task, photon_search_async);
  Ask *a = g_new0 (Ask, 1);
  a->text = g_strstrip (g_strdup (text ? text : ""));
  a->category = category;
  a->lat = near_lat;
  a->lng = near_lng;
  g_task_set_task_data (task, a, ask_free);
  const char *const *tags = NULL;
  const char *word = category_query (category, &tags);
  const char *q = *a->text ? a->text : word;
  if (!q) {
    g_task_return_pointer (task, g_ptr_array_new_with_free_func ((GDestroyNotify) navi_place_free),
                           (GDestroyNotify) g_ptr_array_unref);
    return;
  }
  g_autofree char *escaped = g_uri_escape_string (q, NULL, FALSE);
  g_autoptr (GString) url = g_string_new (NULL);
  g_string_printf (url, "%s/api?q=%s&limit=%d&lang=%s", self->base, escaped, LIMIT, photon_language ());
  add_coord (url, "lat", near_lat);
  add_coord (url, "lon", near_lng);
  if (tags) {
    for (guint i = 0; tags[i]; i++)
      g_string_append_printf (url, "&osm_tag=%s", tags[i]);
    /* nearby ones only: minLon,minLat,maxLon,maxLat */
    char b[4][G_ASCII_DTOSTR_BUF_SIZE];
    g_string_append_printf (url, "&bbox=%s,%s,%s,%s",
                            g_ascii_formatd (b[0], sizeof b[0], "%.4f", near_lng - CATEGORY_BOX_DEG * 1.6),
                            g_ascii_formatd (b[1], sizeof b[1], "%.4f", near_lat - CATEGORY_BOX_DEG),
                            g_ascii_formatd (b[2], sizeof b[2], "%.4f", near_lng + CATEGORY_BOX_DEG * 1.6),
                            g_ascii_formatd (b[3], sizeof b[3], "%.4f", near_lat + CATEGORY_BOX_DEG));
  }
  send (task, url);
}

static GPtrArray *
photon_search_finish (NaviSearch *self, GAsyncResult *result, GError **error)
{
  return g_task_propagate_pointer (G_TASK (result), error);
}

static void
photon_reverse_async (NaviSearch *search, double lat, double lng, GCancellable *cancellable,
                      GAsyncReadyCallback callback, gpointer user_data)
{
  NaviSearchPhoton *self = NAVI_SEARCH_PHOTON (search);
  g_autoptr (GTask) task = g_task_new (self, cancellable, callback, user_data);
  g_task_set_source_tag (task, photon_reverse_async);
  Ask *a = g_new0 (Ask, 1);
  a->reverse = TRUE;
  a->lat = lat;
  a->lng = lng;
  g_task_set_task_data (task, a, ask_free);
  g_autoptr (GString) url = g_string_new (NULL);
  g_string_printf (url, "%s/reverse?limit=1&lang=%s", self->base, photon_language ());
  add_coord (url, "lat", lat);
  add_coord (url, "lon", lng);
  send (task, url);
}

static NaviPlace *
photon_reverse_finish (NaviSearch *self, GAsyncResult *result, GError **error)
{
  return g_task_propagate_pointer (G_TASK (result), error);
}

static void
navi_search_photon_iface_init (NaviSearchInterface *iface)
{
  iface->search_async = photon_search_async;
  iface->search_finish = photon_search_finish;
  iface->reverse_async = photon_reverse_async;
  iface->reverse_finish = photon_reverse_finish;
}

gboolean
navi_search_photon_is_down (NaviSearch *search)
{
  return G_TYPE_CHECK_INSTANCE_TYPE (search, NAVI_TYPE_SEARCH_PHOTON) && NAVI_SEARCH_PHOTON (search)->down;
}
