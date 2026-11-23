/* navi-search: the interface (see navi-search.h) and its vector-tile backend.
 *
 * The tile backend: a search loads the zoom-14 tiles around the point (5 x 5 of them, about 7 km across in Germany,
 * then 7 x 7 when that found little), decodes each once in a worker thread into an index (named streets with their
 * lines, house numbers, points of interest, places), and matches the folded text against the names, also in a
 * thread: the map keeps its frames. "Bergmannstraße 5" is a street and a number: the house numbers near that
 * street's lines. A tile's index stays in memory for later searches (the most recent 150). */
#include "navi-search.h"
#include "navi-geo.h"
#include "navi-mvt.h"
#include "navi-route.h"

#include <json-glib/json-glib.h>
#include <libsoup/soup.h>
#include <math.h>
#include <string.h>

#define TILE_Z       14
#define MAX_TILES    150
#define MAX_RESULTS  20
#define HOUSE_NEAR_M 45.0 /* a house number this close to a street's line is on it */

NaviPlace *
navi_place_new (NaviPlaceKind kind, const char *name, const char *detail, double lat, double lng)
{
  NaviPlace *p = g_new0 (NaviPlace, 1);
  p->kind = kind;
  p->name = g_strdup (name);
  p->detail = g_strdup (detail);
  p->lat = lat;
  p->lng = lng;
  return p;
}

NaviPlace *
navi_place_copy (const NaviPlace *place)
{
  NaviPlace *p = navi_place_new (place->kind, place->name, place->detail, place->lat, place->lng);
  p->category = g_strdup (place->category);
  return p;
}

void
navi_place_free (NaviPlace *place)
{
  if (!place)
    return;
  g_free (place->name);
  g_free (place->detail);
  g_free (place->category);
  g_free (place);
}

/* ---- the interface ---- */

G_DEFINE_INTERFACE (NaviSearch, navi_search, G_TYPE_OBJECT)

static void
navi_search_default_init (NaviSearchInterface *iface)
{
}

void
navi_search_search_async (NaviSearch *self, const char *text, NaviCategory category, double near_lat, double near_lng,
                          GCancellable *cancellable, GAsyncReadyCallback callback, gpointer user_data)
{
  NAVI_SEARCH_GET_IFACE (self)->search_async (self, text, category, near_lat, near_lng, cancellable, callback,
                                              user_data);
}

GPtrArray *
navi_search_search_finish (NaviSearch *self, GAsyncResult *result, GError **error)
{
  return NAVI_SEARCH_GET_IFACE (self)->search_finish (self, result, error);
}

void
navi_search_reverse_async (NaviSearch *self, double lat, double lng, GCancellable *cancellable,
                           GAsyncReadyCallback callback, gpointer user_data)
{
  NAVI_SEARCH_GET_IFACE (self)->reverse_async (self, lat, lng, cancellable, callback, user_data);
}

NaviPlace *
navi_search_reverse_finish (NaviSearch *self, GAsyncResult *result, GError **error)
{
  return NAVI_SEARCH_GET_IFACE (self)->reverse_finish (self, result, error);
}

/* ---- folding ---- */

char *
navi_search_fold (const char *text)
{
  if (!text)
    return g_strdup ("");
  g_autofree char *lower = g_utf8_casefold (text, -1);
  g_autofree char *nfkd = g_utf8_normalize (lower, -1, G_NORMALIZE_NFKD);
  GString *out = g_string_new (NULL);
  for (const char *p = nfkd; p && *p; p = g_utf8_next_char (p)) {
    gunichar c = g_utf8_get_char (p);
    GUnicodeType t = g_unichar_type (c);
    if (t == G_UNICODE_NON_SPACING_MARK || t == G_UNICODE_SPACING_MARK || t == G_UNICODE_ENCLOSING_MARK)
      continue; /* ä is a + ¨ by now: keep the a */
    if (c == 0x00DF) { /* ß (casefold makes it ss already; this is for safety) */
      g_string_append (out, "ss");
      continue;
    }
    if (g_unichar_isalnum (c))
      g_string_append_unichar (out, c);
    else if (c == '.' && out->len >= 3 && g_str_has_suffix (out->str, "str")) {
      g_string_append (out, "asse"); /* "Bergmannstr." */
    } else if (out->len && out->str[out->len - 1] != ' ')
      g_string_append_c (out, ' ');
  }
  while (out->len && out->str[out->len - 1] == ' ')
    g_string_truncate (out, out->len - 1);
  return g_string_free (out, FALSE);
}

/* ---- a tile's index ---- */

typedef struct {
  char *name, *folded, *cls;
  double lat, lng;
} Named;

typedef struct {
  char *name, *folded, *cls;
  double lat, lng;  /* the middle of its longest line in the tile */
  double *pts;      /* lat, lng pairs of all its lines here */
  guint *parts;     /* n_parts + 1 offsets (in points) */
  guint n_pts, n_parts;
} Street;

typedef struct {
  char *number;
  double lat, lng;
} House;

typedef struct {
  gatomicrefcount ref;
  int x, y;
  GArray *pois, *streets, *houses, *places; /* Named, Street, House, Named */
} TileIndex;

static void
named_clear (gpointer p)
{
  Named *n = p;
  g_free (n->name);
  g_free (n->folded);
  g_free (n->cls);
}

static void
street_clear (gpointer p)
{
  Street *s = p;
  g_free (s->name);
  g_free (s->folded);
  g_free (s->cls);
  g_free (s->pts);
  g_free (s->parts);
}

static void
house_clear (gpointer p)
{
  g_free (((House *) p)->number);
}

static TileIndex *
tile_index_new (int x, int y)
{
  TileIndex *t = g_new0 (TileIndex, 1);
  g_atomic_ref_count_init (&t->ref);
  t->x = x;
  t->y = y;
  t->pois = g_array_new (FALSE, TRUE, sizeof (Named));
  g_array_set_clear_func (t->pois, named_clear);
  t->places = g_array_new (FALSE, TRUE, sizeof (Named));
  g_array_set_clear_func (t->places, named_clear);
  t->streets = g_array_new (FALSE, TRUE, sizeof (Street));
  g_array_set_clear_func (t->streets, street_clear);
  t->houses = g_array_new (FALSE, TRUE, sizeof (House));
  g_array_set_clear_func (t->houses, house_clear);
  return t;
}

static TileIndex *
tile_index_ref (TileIndex *t)
{
  g_atomic_ref_count_inc (&t->ref);
  return t;
}

static void
tile_index_unref (gpointer p)
{
  TileIndex *t = p;
  if (!t || !g_atomic_ref_count_dec (&t->ref))
    return;
  g_array_unref (t->pois);
  g_array_unref (t->places);
  g_array_unref (t->streets);
  g_array_unref (t->houses);
  g_free (t);
}

static const char *
feature_name (const NaviMvtFeature *f)
{
  const char *n = navi_mvt_feature_get (f, "name:latin");
  if (!n || !*n)
    n = navi_mvt_feature_get (f, "name");
  return n && *n ? n : NULL;
}

/* a feature's representative point: a point's own, a line's middle point, a polygon's first vertex */
static void
feature_point (TileIndex *t, const NaviMvtFeature *f, double *lat, double *lng)
{
  guint n;
  const int *pts = navi_mvt_feature_points (f, &n, NULL, NULL);
  guint i = navi_mvt_feature_type (f) == NAVI_MVT_LINE ? n / 2 : 0;
  if (!n) {
    *lat = *lng = NAN;
    return;
  }
  navi_mvt_to_lat_lng (TILE_Z, t->x, t->y, navi_mvt_feature_extent (f), pts[2 * i], pts[2 * i + 1], lat, lng);
}

static void
index_feature (const char *layer, const NaviMvtFeature *f, gpointer data)
{
  TileIndex *t = data;
  if (!strcmp (layer, "housenumber")) {
    const char *num = navi_mvt_feature_get (f, "housenumber");
    if (!num || !*num)
      return;
    House h = { g_strdup (num) };
    feature_point (t, f, &h.lat, &h.lng);
    if (!isnan (h.lat))
      g_array_append_val (t->houses, h);
    return;
  }
  const char *name = feature_name (f);
  if (!name)
    return;
  if (!strcmp (layer, "transportation_name")) {
    const char *cls = navi_mvt_feature_get (f, "class");
    if (cls && (!strcmp (cls, "path") || !strcmp (cls, "rail") || !strcmp (cls, "transit") ||
                !strcmp (cls, "ferry") || !strcmp (cls, "aerialway")))
      return; /* streets for cars only */
    guint n, n_parts;
    const guint *parts;
    const int *pts = navi_mvt_feature_points (f, &n, &parts, &n_parts);
    if (n < 2)
      return;
    Street s = { g_strdup (name), navi_search_fold (name), g_strdup (cls) };
    s.n_pts = n;
    s.n_parts = n_parts;
    s.pts = g_new (double, 2 * n);
    s.parts = g_memdup2 (parts, (n_parts + 1) * sizeof (guint));
    guint extent = navi_mvt_feature_extent (f);
    for (guint i = 0; i < n; i++)
      navi_mvt_to_lat_lng (TILE_Z, t->x, t->y, extent, pts[2 * i], pts[2 * i + 1], &s.pts[2 * i], &s.pts[2 * i + 1]);
    s.lat = s.pts[2 * (n / 2)];
    s.lng = s.pts[2 * (n / 2) + 1];
    g_array_append_val (t->streets, s);
    return;
  }
  Named nm = { g_strdup (name), navi_search_fold (name), g_strdup (navi_mvt_feature_get (f, "class")) };
  feature_point (t, f, &nm.lat, &nm.lng);
  if (isnan (nm.lat)) {
    named_clear (&nm);
    return;
  }
  if (!strcmp (layer, "place"))
    g_array_append_val (t->places, nm);
  else
    g_array_append_val (t->pois, nm); /* poi, park, water_name, aerodrome_label, mountain_peak */
}

static TileIndex *
tile_index_build (int x, int y, GBytes *bytes)
{
  static const char *const layers[] = { "poi", "transportation_name", "housenumber", "place", "park", "water_name",
                                        "aerodrome_label", "mountain_peak", NULL };
  TileIndex *t = tile_index_new (x, y);
  gsize len;
  const guint8 *data = g_bytes_get_data (bytes, &len);
  navi_mvt_read (data, len, layers, index_feature, t);
  return t;
}

/* ---- distances to lines ---- */

/* metres from (lat, lng) to the street's nearest line here */
static double
street_distance (const Street *s, double lat, double lng)
{
  double best = G_MAXDOUBLE;
  double ky = 111320.0, kx = ky * cos (lat * G_PI / 180);
  for (guint p = 0; p < s->n_parts; p++) {
    for (guint i = s->parts[p]; i + 1 < s->parts[p + 1]; i++) {
      double ax = (s->pts[2 * i + 1] - lng) * kx, ay = (s->pts[2 * i] - lat) * ky;
      double bx = (s->pts[2 * i + 3] - lng) * kx, by = (s->pts[2 * i + 2] - lat) * ky;
      double dx = bx - ax, dy = by - ay, l2 = dx * dx + dy * dy;
      double u = l2 > 0 ? CLAMP (-(ax * dx + ay * dy) / l2, 0.0, 1.0) : 0;
      best = MIN (best, hypot (ax + dx * u, ay + dy * u));
    }
  }
  return best;
}

/* ---- matching ---- */

typedef struct {
  char *text;            /* folded, without the house number */
  char *number;          /* "5", "12a", or NULL */
  char **tokens;
  NaviCategory category;
  double lat, lng;
  GPtrArray *tiles;      /* TileIndex */
} Query;

static void
query_free (gpointer p)
{
  Query *q = p;
  g_free (q->text);
  g_free (q->number);
  g_strfreev (q->tokens);
  if (q->tiles)
    g_ptr_array_unref (q->tiles);
  g_free (q);
}

static gboolean
is_house_number (const char *t)
{
  /* 5, 12a, 7-9 */
  if (!g_ascii_isdigit (t[0]))
    return FALSE;
  for (const char *p = t; *p; p++)
    if (!g_ascii_isdigit (*p) && !(g_ascii_isalpha (*p) && !p[1]) && *p != '-')
      return FALSE;
  return TRUE;
}

/* How well `folded` matches the query's tokens: 0 for not at all. Every token has to start one of its words
 * (or, four letters and more, be inside one: "brauerei" in "kulturbrauerei"). */
static int
match_score (const Query *q, const char *folded)
{
  if (!q->tokens || !q->tokens[0])
    return 0;
  if (!strcmp (folded, q->text))
    return 100;
  int score = g_str_has_prefix (folded, q->text) ? 60 : 30;
  for (guint i = 0; q->tokens[i]; i++) {
    const char *tok = q->tokens[i];
    gsize tl = strlen (tok);
    gboolean word = FALSE;
    for (const char *w = folded; w && *w;) {
      if (!strncmp (w, tok, tl)) {
        word = TRUE;
        break;
      }
      w = strchr (w, ' ');
      if (w)
        w++;
    }
    if (!word) {
      if (tl >= 4 && strstr (folded, tok))
        score -= 10;
      else
        return 0;
    }
  }
  return MAX (score, 1);
}

static gboolean
in_category (NaviCategory c, const char *cls)
{
  if (!cls)
    return FALSE;
  switch (c) {
  case NAVI_CATEGORY_FUEL: return !strcmp (cls, "fuel");
  case NAVI_CATEGORY_FOOD:
    return !strcmp (cls, "restaurant") || !strcmp (cls, "fast_food") || !strcmp (cls, "cafe") ||
           !strcmp (cls, "ice_cream") || !strcmp (cls, "bakery");
  case NAVI_CATEGORY_PARKING: return !strcmp (cls, "parking");
  default: return FALSE;
  }
}

typedef struct {
  NaviPlace *place;
  double score;
  char *key; /* for dropping the same street found in several tiles */
} Hit;

static void
hit_clear (gpointer p)
{
  Hit *h = p;
  navi_place_free (h->place);
  g_free (h->key);
}

static int
hit_compare (gconstpointer a, gconstpointer b)
{
  double d = ((const Hit *) b)->score - ((const Hit *) a)->score;
  return d > 0 ? 1 : d < 0 ? -1 : 0;
}

/* the district or town a point is in: the nearest suburb-like place, else the nearest town */
static const char *
locality_near (const Query *q, double lat, double lng)
{
  static const char *const near_classes[] = { "suburb", "quarter", "neighbourhood", "borough", "village", "hamlet", NULL };
  const char *best = NULL, *town = NULL;
  double bd = 3000, td = 30000;
  for (guint t = 0; t < q->tiles->len; t++) {
    TileIndex *ti = q->tiles->pdata[t];
    for (guint i = 0; i < ti->places->len; i++) {
      Named *n = &g_array_index (ti->places, Named, i);
      double d = navi_geo_distance (lat, lng, n->lat, n->lng);
      gboolean nearby = n->cls && g_strv_contains (near_classes, n->cls);
      if (nearby && d < bd) {
        bd = d;
        best = n->name;
      } else if (!nearby && d < td) {
        td = d;
        town = n->name;
      }
    }
  }
  return best ? best : town;
}

/* the street a point is on: the nearest named line within `max_m` */
static const Street *
street_near (GPtrArray *tiles, double lat, double lng, double max_m, double *dist)
{
  const Street *best = NULL;
  double bd = max_m;
  for (guint t = 0; t < tiles->len; t++) {
    TileIndex *ti = tiles->pdata[t];
    for (guint i = 0; i < ti->streets->len; i++) {
      const Street *s = &g_array_index (ti->streets, Street, i);
      /* quick reject: a street whose middle is far off is not near either (lines in one tile are short) */
      if (navi_geo_distance (lat, lng, s->lat, s->lng) > 3000)
        continue;
      double d = street_distance (s, lat, lng);
      if (d < bd) {
        bd = d;
        best = s;
      }
    }
  }
  if (dist)
    *dist = bd;
  return best;
}

static char *
join_detail (const char *a, const char *b)
{
  if (a && b && strcmp (a, b))
    return g_strdup_printf ("%s, %s", a, b);
  return g_strdup (a ? a : b);
}

static void
add_hit (GArray *hits, GHashTable *seen, NaviPlace *place, double score, char *key)
{
  if (key && g_hash_table_contains (seen, key)) {
    navi_place_free (place);
    g_free (key);
    return;
  }
  if (key)
    g_hash_table_add (seen, g_strdup (key));
  Hit h = { place, score, key };
  g_array_append_val (hits, h);
}

static void
run_match (GTask *task, gpointer source, gpointer data, GCancellable *cancellable)
{
  Query *q = data;
  g_autoptr (GArray) hits = g_array_new (FALSE, TRUE, sizeof (Hit));
  g_array_set_clear_func (hits, hit_clear);
  g_autoptr (GHashTable) seen = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);

  for (guint t = 0; t < q->tiles->len; t++) {
    TileIndex *ti = q->tiles->pdata[t];
    /* points of interest: by name, or by category */
    for (guint i = 0; i < ti->pois->len; i++) {
      Named *n = &g_array_index (ti->pois, Named, i);
      int ms;
      if (q->category != NAVI_CATEGORY_NONE) {
        if (!in_category (q->category, n->cls))
          continue;
        ms = q->tokens && q->tokens[0] ? match_score (q, n->folded) : 50;
      } else {
        ms = match_score (q, n->folded);
      }
      if (!ms || q->number)
        continue;
      double d = navi_geo_distance (q->lat, q->lng, n->lat, n->lng);
      NaviPlace *p = navi_place_new (NAVI_PLACE_POI, n->name, NULL, n->lat, n->lng);
      p->category = g_strdup (n->cls);
      /* one name in one ~150 m cell is one place: a station's entrances, a shop drawn as a point and a building */
      add_hit (hits, seen, p, ms - d / 250.0,
               g_strdup_printf ("p:%s:%d:%d", n->folded, (int) floor (n->lat / 0.00135), (int) floor (n->lng / 0.0022)));
    }
    if (q->category != NAVI_CATEGORY_NONE)
      continue;
    /* streets, and with a number the house on it */
    for (guint i = 0; i < ti->streets->len; i++) {
      Street *s = &g_array_index (ti->streets, Street, i);
      int ms = match_score (q, s->folded);
      if (!ms)
        continue;
      double d = navi_geo_distance (q->lat, q->lng, s->lat, s->lng);
      /* the same street in the next tile (or the next segment) is one street: keyed by name and a 1.5 km cell */
      g_autofree char *cell = g_strdup_printf ("%s:%d:%d", s->folded, (int) floor (s->lat / 0.0135),
                                               (int) floor (s->lng / 0.022));
      if (q->number) {
        for (guint u = 0; u < q->tiles->len; u++) {
          TileIndex *tu = q->tiles->pdata[u];
          if (abs (tu->x - ti->x) > 1 || abs (tu->y - ti->y) > 1)
            continue;
          for (guint h = 0; h < tu->houses->len; h++) {
            House *ho = &g_array_index (tu->houses, House, h);
            if (g_ascii_strcasecmp (ho->number, q->number))
              continue;
            if (street_distance (s, ho->lat, ho->lng) > HOUSE_NEAR_M)
              continue;
            /* the nearest street of all is this one (a corner house belongs to one street) */
            const Street *own = street_near (q->tiles, ho->lat, ho->lng, HOUSE_NEAR_M, NULL);
            if (own && strcmp (own->folded, s->folded))
              continue;
            g_autofree char *name = g_strdup_printf ("%s %s", s->name, ho->number);
            NaviPlace *p = navi_place_new (NAVI_PLACE_ADDRESS, name, NULL, ho->lat, ho->lng);
            add_hit (hits, seen, p, ms + 20 - d / 250.0, g_strdup_printf ("a:%s", cell));
          }
        }
      } else {
        NaviPlace *p = navi_place_new (NAVI_PLACE_STREET, s->name, NULL, s->lat, s->lng);
        add_hit (hits, seen, p, ms - 5 - d / 250.0, g_strdup_printf ("s:%s", cell));
      }
    }
    if (q->number)
      continue;
    for (guint i = 0; i < ti->places->len; i++) {
      Named *n = &g_array_index (ti->places, Named, i);
      int ms = match_score (q, n->folded);
      if (!ms)
        continue;
      NaviPlace *p = navi_place_new (NAVI_PLACE_LOCALITY, n->name, NULL, n->lat, n->lng);
      add_hit (hits, seen, p, ms + 5, g_strdup_printf ("l:%s", n->folded));
    }
  }
  g_array_sort (hits, hit_compare);
  GPtrArray *out = g_ptr_array_new_with_free_func ((GDestroyNotify) navi_place_free);
  for (guint i = 0; i < hits->len && out->len < MAX_RESULTS; i++) {
    Hit *h = &g_array_index (hits, Hit, i);
    /* the same name again within 400 m (a station's stops on both sides of the road): the better one is shown */
    gboolean again = FALSE;
    for (guint k = 0; k < out->len && !again; k++) {
      const NaviPlace *o = out->pdata[k];
      again = o->kind == h->place->kind && !g_strcmp0 (o->name, h->place->name) &&
              navi_geo_distance (o->lat, o->lng, h->place->lat, h->place->lng) < 400;
    }
    if (again)
      continue;
    NaviPlace *p = g_steal_pointer (&h->place);
    /* where it is: the street a point of interest is on, and the district */
    const char *loc = locality_near (q, p->lat, p->lng);
    if (p->kind == NAVI_PLACE_POI) {
      const Street *s = street_near (q->tiles, p->lat, p->lng, 80, NULL);
      p->detail = join_detail (s ? s->name : NULL, loc);
    } else if (p->kind != NAVI_PLACE_LOCALITY) {
      p->detail = g_strdup (loc);
    }
    g_ptr_array_add (out, p);
  }
  g_task_return_pointer (task, out, (GDestroyNotify) g_ptr_array_unref);
}

/* ---- the backend ---- */

#define NAVI_TYPE_SEARCH_TILES (navi_search_tiles_get_type ())
G_DECLARE_FINAL_TYPE (NaviSearchTiles, navi_search_tiles, NAVI, SEARCH_TILES, GObject)

typedef struct {
  TileIndex *index;  /* NULL while it loads */
  GPtrArray *waiters; /* GTask, each with a Job */
  gint64 used;
  gboolean loading;
} TileState;

struct _NaviSearchTiles {
  GObject parent_instance;
  NaviTileSource *sources;
  guint n_sources;
  GHashTable *tiles; /* key x << 32 | y → TileState */
};

static void navi_search_tiles_iface_init (NaviSearchInterface *iface);
G_DEFINE_FINAL_TYPE_WITH_CODE (NaviSearchTiles, navi_search_tiles, G_TYPE_OBJECT,
                               G_IMPLEMENT_INTERFACE (NAVI_TYPE_SEARCH, navi_search_tiles_iface_init))

static void
tile_state_free (gpointer p)
{
  TileState *s = p;
  if (s->index)
    tile_index_unref (s->index);
  if (s->waiters)
    g_ptr_array_unref (s->waiters);
  g_free (s);
}

static void
navi_search_tiles_finalize (GObject *object)
{
  NaviSearchTiles *self = NAVI_SEARCH_TILES (object);
  for (guint i = 0; i < self->n_sources; i++)
    g_free (self->sources[i].url);
  g_free (self->sources);
  g_hash_table_unref (self->tiles);
  G_OBJECT_CLASS (navi_search_tiles_parent_class)->finalize (object);
}

static void
navi_search_tiles_class_init (NaviSearchTilesClass *klass)
{
  G_OBJECT_CLASS (klass)->finalize = navi_search_tiles_finalize;
}

static void
navi_search_tiles_init (NaviSearchTiles *self)
{
  self->tiles = g_hash_table_new_full (g_int64_hash, g_int64_equal, g_free, tile_state_free);
}

NaviSearch *
navi_search_tiles_new (const NaviTileSource *sources, guint n_sources)
{
  NaviSearchTiles *self = g_object_new (NAVI_TYPE_SEARCH_TILES, NULL);
  self->sources = g_new0 (NaviTileSource, n_sources);
  self->n_sources = n_sources;
  for (guint i = 0; i < n_sources; i++) {
    self->sources[i] = sources[i];
    self->sources[i].url = g_strdup (sources[i].url);
  }
  return NAVI_SEARCH (self);
}

/* A search (or reverse) waiting for its tiles: the tiles it needs, how many are still out */
typedef struct {
  NaviSearchTiles *self;
  GArray *keys;     /* gint64 */
  guint pending;
  gboolean reverse;
  Query *query;     /* search */
  double lat, lng;  /* reverse */
  int radius;       /* tiles around the centre */
  gboolean widened;
} Job;

static void
job_free (gpointer p)
{
  Job *j = p;
  g_array_unref (j->keys);
  if (j->query)
    query_free (j->query);
  g_free (j);
}

static gint64
tile_key (int x, int y)
{
  return ((gint64) x << 32) | (guint32) y;
}

static const char *
source_for (NaviSearchTiles *self, double lat, double lng)
{
  for (guint i = 0; i < self->n_sources; i++) {
    const NaviTileSource *s = &self->sources[i];
    if (lat >= s->south && lat <= s->north && lng >= s->west && lng <= s->east)
      return s->url;
  }
  return NULL;
}

static void job_tiles_ready (GTask *task);

static void
tile_done (NaviSearchTiles *self, gint64 key, TileIndex *index)
{
  TileState *st = g_hash_table_lookup (self->tiles, &key);
  if (!st)
    return;
  st->loading = FALSE;
  if (index)
    st->index = index;
  g_autoptr (GPtrArray) waiters = g_steal_pointer (&st->waiters);
  if (!index) /* failed: forget it, so a later search tries again */
    g_hash_table_remove (self->tiles, &key);
  for (guint i = 0; waiters && i < waiters->len; i++) {
    GTask *task = waiters->pdata[i];
    Job *j = g_task_get_task_data (task);
    if (j->pending && --j->pending == 0)
      job_tiles_ready (task);
  }
}

typedef struct {
  NaviSearchTiles *self;
  gint64 key;
  int x, y;
  GBytes *bytes;
} Load;

static void
load_free (gpointer p)
{
  Load *l = p;
  g_object_unref (l->self);
  if (l->bytes)
    g_bytes_unref (l->bytes);
  g_free (l);
}

static void
build_thread (GTask *task, gpointer source, gpointer data, GCancellable *c)
{
  Load *l = data;
  g_task_return_pointer (task, tile_index_build (l->x, l->y, l->bytes), tile_index_unref);
}

static void
built (GObject *source, GAsyncResult *res, gpointer data)
{
  Load *l = g_task_get_task_data (G_TASK (res));
  TileIndex *index = g_task_propagate_pointer (G_TASK (res), NULL);
  tile_done (l->self, l->key, index);
}

static void
tile_received (GObject *source, GAsyncResult *res, gpointer data)
{
  Load *l = data;
  SoupMessage *msg = soup_session_get_async_result_message (SOUP_SESSION (source), res);
  g_autoptr (GError) error = NULL;
  GBytes *bytes = soup_session_send_and_read_finish (SOUP_SESSION (source), res, &error);
  if (!bytes || soup_message_get_status (msg) != 200) {
    if (bytes)
      g_bytes_unref (bytes);
    /* a 404 is a tile with nothing in it (the sea): an empty index, not a failure */
    gboolean empty = msg && soup_message_get_status (msg) == 404;
    tile_done (l->self, l->key, empty ? tile_index_new (l->x, l->y) : NULL);
    load_free (l);
    return;
  }
  l->bytes = bytes;
  GTask *task = g_task_new (NULL, NULL, built, NULL);
  g_task_set_task_data (task, l, load_free);
  g_task_run_in_thread (task, build_thread);
  g_object_unref (task);
}

static void
evict (NaviSearchTiles *self)
{
  while (g_hash_table_size (self->tiles) > MAX_TILES) {
    GHashTableIter it;
    gpointer k, v;
    gint64 *oldest = NULL, oldest_used = G_MAXINT64;
    g_hash_table_iter_init (&it, self->tiles);
    while (g_hash_table_iter_next (&it, &k, &v)) {
      TileState *s = v;
      if (!s->loading && s->used < oldest_used) {
        oldest_used = s->used;
        oldest = k;
      }
    }
    if (!oldest)
      break;
    g_hash_table_remove (self->tiles, oldest);
  }
}

/* asks for the job's tiles; job_tiles_ready runs once they are all in (at once when they already are) */
static void
job_load (GTask *task)
{
  Job *j = g_task_get_task_data (task);
  NaviSearchTiles *self = j->self;
  int cx, cy;
  navi_mvt_tile_at (TILE_Z, j->lat, j->lng, &cx, &cy);
  g_array_set_size (j->keys, 0);
  j->pending = 1; /* held until every tile is asked for */
  for (int dy = -j->radius; dy <= j->radius; dy++)
    for (int dx = -j->radius; dx <= j->radius; dx++) {
      int x = cx + dx, y = cy + dy;
      gint64 key = tile_key (x, y);
      g_array_append_val (j->keys, key);
      TileState *st = g_hash_table_lookup (self->tiles, &key);
      if (st && st->index) {
        st->used = g_get_monotonic_time ();
        continue;
      }
      if (!st) {
        double lat, lng;
        navi_mvt_to_lat_lng (TILE_Z, x, y, 1, 0.5, 0.5, &lat, &lng);
        const char *url = source_for (self, lat, lng);
        if (!url)
          continue;
        st = g_new0 (TileState, 1);
        st->used = g_get_monotonic_time ();
        st->loading = TRUE;
        g_hash_table_insert (self->tiles, g_memdup2 (&key, sizeof key), st);
        g_autofree char *zs = g_strdup_printf ("%d", TILE_Z), *xs = g_strdup_printf ("%d", x),
                        *ys = g_strdup_printf ("%d", y);
        g_autoptr (GString) u = g_string_new (url);
        g_string_replace (u, "{z}", zs, 0);
        g_string_replace (u, "{x}", xs, 0);
        g_string_replace (u, "{y}", ys, 0);
        Load *l = g_new0 (Load, 1);
        l->self = g_object_ref (self);
        l->key = key;
        l->x = x;
        l->y = y;
        g_autoptr (SoupMessage) msg = soup_message_new ("GET", u->str);
        if (!msg) {
          load_free (l);
          g_hash_table_remove (self->tiles, &key);
          continue;
        }
        soup_session_send_and_read_async (SOUP_SESSION (navi_http_session ()), msg, G_PRIORITY_LOW, NULL,
                                          tile_received, l);
      }
      if (!st->waiters)
        st->waiters = g_ptr_array_new_with_free_func (g_object_unref);
      g_ptr_array_add (st->waiters, g_object_ref (task));
      j->pending++;
    }
  if (--j->pending == 0)
    job_tiles_ready (task);
}

static GPtrArray *
job_indexes (Job *j)
{
  GPtrArray *tiles = g_ptr_array_new_with_free_func (tile_index_unref);
  for (guint i = 0; i < j->keys->len; i++) {
    TileState *st = g_hash_table_lookup (j->self->tiles, &g_array_index (j->keys, gint64, i));
    if (st && st->index)
      g_ptr_array_add (tiles, tile_index_ref (st->index));
  }
  return tiles;
}

static void reverse_tiles_ready (GTask *task);

static void
matched (GObject *source, GAsyncResult *res, gpointer data)
{
  GTask *outer = data;
  Job *j = g_task_get_task_data (outer);
  GError *error = NULL;
  GPtrArray *places = g_task_propagate_pointer (G_TASK (res), &error);
  if (!places) {
    g_task_return_error (outer, error);
  } else if (places->len < 3 && !j->widened && j->query->category == NAVI_CATEGORY_NONE) {
    /* little nearby: look further */
    g_ptr_array_unref (places);
    j->widened = TRUE;
    j->radius = 3;
    g_ptr_array_unref (g_steal_pointer (&j->query->tiles));
    job_load (outer);
    return;
  } else {
    g_task_return_pointer (outer, places, (GDestroyNotify) g_ptr_array_unref);
  }
  g_object_unref (outer);
}

static void
job_tiles_ready (GTask *task)
{
  Job *j = g_task_get_task_data (task);
  if (j->reverse) {
    reverse_tiles_ready (task);
    return;
  }
  if (g_task_return_error_if_cancelled (task))
    return;
  evict (j->self);
  j->query->tiles = job_indexes (j);
  /* the matching runs on its own copy of the query, so a widened search can go on with this one */
  Query *q = g_new0 (Query, 1);
  q->text = g_strdup (j->query->text);
  q->number = g_strdup (j->query->number);
  q->tokens = g_strdupv (j->query->tokens);
  q->category = j->query->category;
  q->lat = j->query->lat;
  q->lng = j->query->lng;
  q->tiles = g_ptr_array_ref (j->query->tiles);
  GTask *inner = g_task_new (NULL, g_task_get_cancellable (task), matched, g_object_ref (task));
  g_task_set_task_data (inner, q, query_free);
  g_task_run_in_thread (inner, run_match);
  g_object_unref (inner);
}

static void
tiles_search_async (NaviSearch *search, const char *text, NaviCategory category, double near_lat, double near_lng,
                    GCancellable *cancellable, GAsyncReadyCallback callback, gpointer user_data)
{
  NaviSearchTiles *self = NAVI_SEARCH_TILES (search);
  GTask *task = g_task_new (self, cancellable, callback, user_data);
  g_task_set_source_tag (task, tiles_search_async);
  Query *q = g_new0 (Query, 1);
  q->category = category;
  q->lat = near_lat;
  q->lng = near_lng;
  g_autofree char *folded = navi_search_fold (text);
  /* a house number at either end of the words */
  g_auto (GStrv) words = g_strsplit (folded, " ", -1);
  guint n = g_strv_length (words);
  g_autoptr (GPtrArray) rest = g_ptr_array_new ();
  for (guint i = 0; i < n; i++) {
    if (!*words[i])
      continue;
    if (!q->number && n > 1 && (i == 0 || i == n - 1) && is_house_number (words[i]))
      q->number = g_strdup (words[i]);
    else
      g_ptr_array_add (rest, words[i]);
  }
  g_ptr_array_add (rest, NULL);
  q->tokens = g_strdupv ((char **) rest->pdata);
  q->text = g_strjoinv (" ", q->tokens);
  if (!*q->text && category == NAVI_CATEGORY_NONE) {
    query_free (q);
    g_task_return_pointer (task, g_ptr_array_new_with_free_func ((GDestroyNotify) navi_place_free),
                           (GDestroyNotify) g_ptr_array_unref);
    g_object_unref (task);
    return;
  }
  Job *j = g_new0 (Job, 1);
  j->self = self;
  j->keys = g_array_new (FALSE, FALSE, sizeof (gint64));
  j->query = q;
  j->lat = near_lat;
  j->lng = near_lng;
  j->radius = category != NAVI_CATEGORY_NONE ? 1 : 2;
  g_task_set_task_data (task, j, job_free);
  job_load (task);
  g_object_unref (task);
}

static GPtrArray *
tiles_search_finish (NaviSearch *self, GAsyncResult *result, GError **error)
{
  return g_task_propagate_pointer (G_TASK (result), error);
}

/* ---- what is here ---- */

static void
locate_received (GObject *source, GAsyncResult *res, gpointer data)
{
  g_autoptr (GTask) task = data;
  Job *j = g_task_get_task_data (task);
  g_autoptr (GBytes) bytes = soup_session_send_and_read_finish (SOUP_SESSION (source), res, NULL);
  char *street = NULL;
  if (bytes) {
    /* [{"edges": [{"edge_info": {"names": ["Fuggerstraße"]}}]}] */
    g_autoptr (JsonParser) parser = json_parser_new ();
    gsize len;
    const char *text = g_bytes_get_data (bytes, &len);
    if (json_parser_load_from_data (parser, text, len, NULL)) {
      JsonNode *root = json_parser_get_root (parser);
      JsonArray *locs = JSON_NODE_HOLDS_ARRAY (root) ? json_node_get_array (root) : NULL;
      JsonObject *loc = locs && json_array_get_length (locs) ? json_array_get_object_element (locs, 0) : NULL;
      JsonArray *edges = loc && json_object_has_member (loc, "edges") ? json_object_get_array_member (loc, "edges") : NULL;
      for (guint i = 0; edges && i < json_array_get_length (edges) && !street; i++) {
        JsonObject *e = json_array_get_object_element (edges, i);
        JsonObject *info = e && json_object_has_member (e, "edge_info") ? json_object_get_object_member (e, "edge_info") : NULL;
        JsonArray *names = info && json_object_has_member (info, "names") ? json_object_get_array_member (info, "names") : NULL;
        if (names && json_array_get_length (names))
          street = g_strdup (json_array_get_string_element (names, 0));
      }
    }
  }
  NaviPlace *p = navi_place_new (street ? NAVI_PLACE_STREET : NAVI_PLACE_POINT, street, NULL, j->lat, j->lng);
  g_free (street);
  g_task_return_pointer (task, p, (GDestroyNotify) navi_place_free);
}

/* no name in the tiles: Valhalla knows the street of the road nearest the point */
static void
ask_locate (GTask *task)
{
  Job *j = g_task_get_task_data (task);
  g_autofree char *url = g_strdup_printf ("%s/locate", navi_server_url ());
  g_autofree char *body = g_strdup_printf ("{\"locations\":[{\"lat\":%.6f,\"lon\":%.6f}],\"costing\":\"auto\","
                                           "\"verbose\":true}", j->lat, j->lng);
  for (char *c = body; *c; c++)
    if (*c == ',' && g_ascii_isdigit (c[-1]) && g_ascii_isdigit (c[1]))
      *c = '.'; /* a locale with decimal commas */
  g_autoptr (SoupMessage) msg = soup_message_new ("POST", url);
  g_autoptr (GBytes) bytes = g_bytes_new (body, strlen (body));
  soup_message_set_request_body_from_bytes (msg, "application/json", bytes);
  soup_session_send_and_read_async (SOUP_SESSION (navi_http_session ()), msg, G_PRIORITY_DEFAULT,
                                    g_task_get_cancellable (task), locate_received, g_object_ref (task));
}

static void
reverse_tiles_ready (GTask *task)
{
  Job *j = g_task_get_task_data (task);
  if (g_task_return_error_if_cancelled (task))
    return;
  g_autoptr (GPtrArray) tiles = job_indexes (j);
  double sd;
  const Street *s = street_near (tiles, j->lat, j->lng, 80, &sd);
  /* the nearest house number, if it is on that street */
  const House *house = NULL;
  double hd = 40;
  for (guint t = 0; t < tiles->len; t++) {
    TileIndex *ti = tiles->pdata[t];
    for (guint i = 0; i < ti->houses->len; i++) {
      const House *h = &g_array_index (ti->houses, House, i);
      double d = navi_geo_distance (j->lat, j->lng, h->lat, h->lng);
      if (d < hd) {
        hd = d;
        house = h;
      }
    }
  }
  Query q = { .tiles = tiles };
  if (house) {
    const Street *hs = street_near (tiles, house->lat, house->lng, HOUSE_NEAR_M, NULL);
    if (hs)
      s = hs;
  }
  if (!s) {
    ask_locate (task);
    return;
  }
  const char *loc = locality_near (&q, j->lat, j->lng);
  NaviPlace *p;
  if (house) {
    g_autofree char *name = g_strdup_printf ("%s %s", s->name, house->number);
    p = navi_place_new (NAVI_PLACE_ADDRESS, name, loc, j->lat, j->lng);
  } else {
    p = navi_place_new (NAVI_PLACE_STREET, s->name, loc, j->lat, j->lng);
  }
  g_task_return_pointer (task, p, (GDestroyNotify) navi_place_free);
}

static void
tiles_reverse_async (NaviSearch *search, double lat, double lng, GCancellable *cancellable,
                     GAsyncReadyCallback callback, gpointer user_data)
{
  NaviSearchTiles *self = NAVI_SEARCH_TILES (search);
  GTask *task = g_task_new (self, cancellable, callback, user_data);
  g_task_set_source_tag (task, tiles_reverse_async);
  Job *j = g_new0 (Job, 1);
  j->self = self;
  j->keys = g_array_new (FALSE, FALSE, sizeof (gint64));
  j->reverse = TRUE;
  j->lat = lat;
  j->lng = lng;
  j->radius = 0;
  g_task_set_task_data (task, j, job_free);
  if (!source_for (self, lat, lng))
    ask_locate (task);
  else
    job_load (task);
  g_object_unref (task);
}

static NaviPlace *
tiles_reverse_finish (NaviSearch *self, GAsyncResult *result, GError **error)
{
  return g_task_propagate_pointer (G_TASK (result), error);
}

static void
navi_search_tiles_iface_init (NaviSearchInterface *iface)
{
  iface->search_async = tiles_search_async;
  iface->search_finish = tiles_search_finish;
  iface->reverse_async = tiles_reverse_async;
  iface->reverse_finish = tiles_reverse_finish;
}
