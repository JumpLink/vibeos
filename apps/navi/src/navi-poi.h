/* navi-poi: the kinds of points of interest Navi shows on the map and searches by category.
 *
 * Each kind ("group") has a key ("fuel"), the Maki icon its marker shows, the marker's colour, and the Photon
 * categories a chip asks for. The map's tiles (OpenMapTiles' poi layer: class and subclass) and Photon's answers
 * (osm_key and osm_value) both map onto the same groups, so a fuel station found either way is a "fuel". */
#pragma once

#include "navi-search.h"

G_BEGIN_DECLS

typedef struct {
  const char *group;      /* "fuel": the place's category, the map's image "navi-poi-fuel" */
  NaviCategory category;  /* the chip that finds it */
  const char *icon;       /* the Maki icon (CC0) its marker shows: "fuel" */
  guint32 color;          /* the marker's colour, 0xRRGGBB */
  gboolean prominent;     /* drawn larger (fuel) */
} NaviPoiKind;

/* the kinds, in the order the chips show their categories */
const NaviPoiKind *navi_poi_kinds (guint *n_kinds);
/* the kind of a group ("fuel"), or NULL */
const NaviPoiKind *navi_poi_kind (const char *group);
/* the group of a tile's poi (class, subclass) or of an OSM tag (key, value), or NULL for one Navi does not show */
const char *navi_poi_group (const char *key, const char *value);
/* the Photon categories of a chip: "osm.amenity.fuel", several joined by commas; NULL for none */
const char *navi_poi_photon_categories (NaviCategory category);
/* whether `group` (a place's category) is one `category` finds */
gboolean navi_poi_in_category (NaviCategory category, const char *group);

G_END_DECLS
