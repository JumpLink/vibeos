/* navi-mvt: a small reader of Mapbox Vector Tiles (the protobuf the map's tiles are), for search.
 *
 * It walks the features of the named layers and hands each to a callback with its type, its geometry in tile units
 * (0 .. extent, as MVT's commands give it, rings and lines one after the other) and a way to read its properties.
 * Only what search needs: no clipping, no simplification, nothing kept. */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

typedef enum {
  NAVI_MVT_UNKNOWN = 0,
  NAVI_MVT_POINT = 1,
  NAVI_MVT_LINE = 2,
  NAVI_MVT_POLYGON = 3,
} NaviMvtType;

typedef struct _NaviMvtFeature NaviMvtFeature;

/* the feature's string property `key`, or NULL; numbers are given as text ("34") */
const char *navi_mvt_feature_get (const NaviMvtFeature *feature, const char *key);
NaviMvtType navi_mvt_feature_type (const NaviMvtFeature *feature);
/* its geometry: `n` (x, y) pairs in tile units; `parts` (n_parts + 1 offsets into the pairs) splits it into its
 * lines or rings */
const int *navi_mvt_feature_points (const NaviMvtFeature *feature, guint *n, const guint **parts, guint *n_parts);
guint navi_mvt_feature_extent (const NaviMvtFeature *feature);

typedef void (*NaviMvtFunc) (const char *layer, const NaviMvtFeature *feature, gpointer data);

/* Calls `func` for every feature of the layers named in `layers` (NULL-terminated; NULL for all). FALSE when the
 * data is not a vector tile. */
gboolean navi_mvt_read (const guint8 *data, gsize length, const char *const *layers, NaviMvtFunc func, gpointer data_);

/* tile units of tile (z, x, y) to degrees */
void navi_mvt_to_lat_lng (int z, int x, int y, guint extent, double px, double py, double *lat, double *lng);
/* the z-level tile that holds (lat, lng) */
void navi_mvt_tile_at (int z, double lat, double lng, int *x, int *y);

G_END_DECLS
