/* navi-search: finding places, behind one small interface so the backend can change.
 *
 * NaviSearch is what the "Where to?" sheet, the category chips and a long press on the map ask: places matching
 * some text (or of a category) near a point, and the name of the place at a point. Two backends:
 *
 *   navi_search_photon_new ()  our own Photon geocoder behind navi.vibeos.top (/api as the text is typed,
 *                              /reverse), which keeps no logs. Never a third party's.
 *   navi_search_tiles_new ()   the map's own vector tiles around the point (street names, house numbers, points of
 *                              interest, places), plus Valhalla's /locate for a street where the tiles have none.
 *                              It sends no search text anywhere, only the tile requests the map makes for that area
 *                              anyway. Photon's fallback while Photon cannot be reached. */
#pragma once

#include <gio/gio.h>

G_BEGIN_DECLS

typedef enum {
  NAVI_PLACE_POINT,    /* a point on the map, unnamed */
  NAVI_PLACE_ADDRESS,  /* a street and house number */
  NAVI_PLACE_STREET,
  NAVI_PLACE_POI,      /* a shop, a fuel station, a restaurant, ... */
  NAVI_PLACE_LOCALITY, /* a city, a district, a village */
} NaviPlaceKind;

typedef struct {
  NaviPlaceKind kind;
  char *name;     /* "Kulturbrauerei", "Rathausstraße 15" */
  char *detail;   /* where it is: "Schönhauser Allee, Prenzlauer Berg" (may be NULL) */
  char *category; /* a point of interest's group (navi-poi.h: "fuel", "food"), or its OSM value; else NULL */
  double lat, lng;
} NaviPlace;

NaviPlace *navi_place_new (NaviPlaceKind kind, const char *name, const char *detail, double lat, double lng);
NaviPlace *navi_place_copy (const NaviPlace *place);
void navi_place_free (NaviPlace *place);
G_DEFINE_AUTOPTR_CLEANUP_FUNC (NaviPlace, navi_place_free)

/* The categories of the chips under the search field */
typedef enum {
  NAVI_CATEGORY_NONE,
  NAVI_CATEGORY_FUEL,
  NAVI_CATEGORY_CHARGING,
  NAVI_CATEGORY_PARKING,
  NAVI_CATEGORY_FOOD, /* restaurants and fast food */
  NAVI_CATEGORY_COFFEE,
  NAVI_CATEGORY_GROCERY,
  NAVI_CATEGORY_PHARMACY,
  NAVI_CATEGORY_HOSPITAL,
  NAVI_CATEGORY_ATM, /* ATMs and banks */
  NAVI_CATEGORY_HOTEL,
  NAVI_CATEGORY_TOILETS,
} NaviCategory;

#define NAVI_TYPE_SEARCH (navi_search_get_type ())
G_DECLARE_INTERFACE (NaviSearch, navi_search, NAVI, SEARCH, GObject)

struct _NaviSearchInterface {
  GTypeInterface parent;

  void (*search_async) (NaviSearch *self, const char *text, NaviCategory category, double near_lat, double near_lng,
                        GCancellable *cancellable, GAsyncReadyCallback callback, gpointer user_data);
  GPtrArray *(*search_finish) (NaviSearch *self, GAsyncResult *result, GError **error);
  void (*reverse_async) (NaviSearch *self, double lat, double lng, GCancellable *cancellable,
                         GAsyncReadyCallback callback, gpointer user_data);
  NaviPlace *(*reverse_finish) (NaviSearch *self, GAsyncResult *result, GError **error);
};

/* Places for `text` (or, with an empty text, of `category`) near (near_lat, near_lng), best first: a GPtrArray of
 * NaviPlace that frees them */
void navi_search_search_async (NaviSearch *self, const char *text, NaviCategory category, double near_lat,
                               double near_lng, GCancellable *cancellable, GAsyncReadyCallback callback,
                               gpointer user_data);
GPtrArray *navi_search_search_finish (NaviSearch *self, GAsyncResult *result, GError **error);

/* What is at (lat, lng): an address, a street, or a place; never fails for want of a name (a point then) */
void navi_search_reverse_async (NaviSearch *self, double lat, double lng, GCancellable *cancellable,
                                GAsyncReadyCallback callback, gpointer user_data);
NaviPlace *navi_search_reverse_finish (NaviSearch *self, GAsyncResult *result, GError **error);

/* A vector tile source: its tile URL template ({z}/{x}/{y}) and the area it covers */
typedef struct {
  char *url;
  double west, south, east, north;
} NaviTileSource;

/* The vector-tile backend, over these sources (the style's; it copies them), using their zoom-14 tiles */
NaviSearch *navi_search_tiles_new (const NaviTileSource *sources, guint n_sources);

/* The Photon backend at `base_url` (NULL: NAVI_PHOTON_URL, else the server's, navi_server_url () + "/photon"), which asks
 * `fallback` (may be NULL) whenever Photon cannot be reached or answers with an error. The language of the names is
 * the UI's where Photon has it (de, en, fr, it). */
NaviSearch *navi_search_photon_new (const char *base_url, NaviSearch *fallback);
/* the last answer of `search`, a Photon backend, was its fallback's: Photon is down, unreachable or failing */
gboolean navi_search_photon_is_down (NaviSearch *search);
/* Photon's GeoJSON answer as places (for the tests) */
GPtrArray *navi_search_photon_parse (const char *json, gssize length, GError **error);
/* a text search's places as shown: a settlement named as the text first, the rest as Photon ranked them */
void navi_search_photon_rank (GPtrArray *places, const char *text);

/* text folded for matching: lower case, no accents, ß as ss, "str." as "strasse" */
char *navi_search_fold (const char *text);

G_END_DECLS
