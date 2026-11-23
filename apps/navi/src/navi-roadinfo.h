/* navi-roadinfo: what the road says along a route, and where the car is: speed limits and traffic signals.
 *
 * Both come from Valhalla's trace_attributes on our server (navi.vibeos.top/trace_attributes): along a route, its
 * own shape walked edge by edge (shape_match edge_walk), in pieces of at most 4000 points (the server takes 5000);
 * without a route, the last fixes map-matched (map_snap). The limits are OSM's maxspeed as Valhalla has it, in
 * km/h; a stretch without one stays unknown and shows no sign: Navi never guesses a limit. A traffic signal is
 * where an edge Valhalla flags ends (edge.traffic_signal: a signal at its end, for its direction). */
#pragma once

#include "navi-route.h"

G_BEGIN_DECLS

#define NAVI_LIMIT_UNKNOWN 0
#define NAVI_LIMIT_NONE (-1) /* no limit: a derestricted motorway */

typedef struct {
  double from, to; /* metres along the route */
  int kmh;         /* the limit, or NAVI_LIMIT_NONE */
} NaviLimitSpan;

typedef struct {
  double s;        /* metres along the route */
  double lat, lng;
} NaviRoadMark;

typedef struct {
  GArray *limits;  /* NaviLimitSpan, in order along the route; none where the limit is unknown */
  GArray *signals; /* NaviRoadMark, in order */
} NaviRoadInfo;

NaviRoadInfo *navi_road_info_new (void);
void navi_road_info_free (NaviRoadInfo *info);
G_DEFINE_AUTOPTR_CLEANUP_FUNC (NaviRoadInfo, navi_road_info_free)

/* the limit `s` metres along the route: km/h, NAVI_LIMIT_NONE, or NAVI_LIMIT_UNKNOWN. At an edge's end the next
 * edge's limit holds: a new limit starts at its sign. */
int navi_road_info_limit_at (const NaviRoadInfo *info, double s);

/* adds one trace_attributes answer for the route's points from `first` on (the piece that was asked) */
gboolean navi_road_info_add_answer (NaviRoadInfo *info, const NaviRoute *route, guint first, const char *json,
                                    gssize length, GError **error);

/* the route's limits and signals, asked of the server piece by piece */
void navi_road_info_request_async (NaviRoute *route, GCancellable *cancellable, GAsyncReadyCallback callback,
                                   gpointer user_data);
NaviRoadInfo *navi_road_info_request_finish (GAsyncResult *result, GError **error);

/* the limit on the road where a track of fixes ends (the last half minute or so: lat, lng and seconds, oldest
 * first), map-matched: km/h, NAVI_LIMIT_NONE, or NAVI_LIMIT_UNKNOWN */
void navi_road_limit_request_async (const double *lat, const double *lng, const double *seconds, guint n,
                                    GCancellable *cancellable, GAsyncReadyCallback callback, gpointer user_data);
int navi_road_limit_request_finish (GAsyncResult *result, GError **error);

/* a limit in the units the speedometer shows: km/h, or mph where speeds are in miles (rounded to 5 mph, as the signs
 * are) */
int navi_limit_display (int kmh, gboolean mph);
/* whether this locale's road speeds are in miles an hour (the United States, the United Kingdom, ...) */
gboolean navi_speed_in_mph (void);

G_END_DECLS
