/* navi-route: a route by car from Valhalla (navi.vibeos.top/route), with its manoeuvres, ready to drive.
 *
 * A NaviRoute is the line to drive (a path, see navi-geo.h) and the manoeuvres along it, each with what the banner
 * shows (its written instruction and street) and what the voice says (Valhalla's verbal instructions, in the
 * language asked for), and where on the path it is. Routes are reference counted and never change once made.
 *
 * Privacy: neither the request nor the answer is logged anywhere (navi.vibeos.top keeps no logs either); only a
 * debug build prints them, to its own console. */
#pragma once

#include <gio/gio.h>

#include "navi-geo.h"

G_BEGIN_DECLS

/* Valhalla's manoeuvre types (odin/maneuver.h), the ones Navi draws differently */
typedef enum {
  NAVI_TURN_NONE = 0,
  NAVI_TURN_START = 1,
  NAVI_TURN_START_RIGHT = 2,
  NAVI_TURN_START_LEFT = 3,
  NAVI_TURN_DESTINATION = 4,
  NAVI_TURN_DESTINATION_RIGHT = 5,
  NAVI_TURN_DESTINATION_LEFT = 6,
  NAVI_TURN_BECOMES = 7,
  NAVI_TURN_CONTINUE = 8,
  NAVI_TURN_SLIGHT_RIGHT = 9,
  NAVI_TURN_RIGHT = 10,
  NAVI_TURN_SHARP_RIGHT = 11,
  NAVI_TURN_UTURN_RIGHT = 12,
  NAVI_TURN_UTURN_LEFT = 13,
  NAVI_TURN_SHARP_LEFT = 14,
  NAVI_TURN_LEFT = 15,
  NAVI_TURN_SLIGHT_LEFT = 16,
  NAVI_TURN_RAMP_STRAIGHT = 17,
  NAVI_TURN_RAMP_RIGHT = 18,
  NAVI_TURN_RAMP_LEFT = 19,
  NAVI_TURN_EXIT_RIGHT = 20,
  NAVI_TURN_EXIT_LEFT = 21,
  NAVI_TURN_STAY_STRAIGHT = 22,
  NAVI_TURN_STAY_RIGHT = 23,
  NAVI_TURN_STAY_LEFT = 24,
  NAVI_TURN_MERGE = 25,
  NAVI_TURN_ROUNDABOUT_ENTER = 26,
  NAVI_TURN_ROUNDABOUT_EXIT = 27,
  NAVI_TURN_FERRY_ENTER = 28,
  NAVI_TURN_FERRY_EXIT = 29,
  NAVI_TURN_MERGE_RIGHT = 37,
  NAVI_TURN_MERGE_LEFT = 38,
} NaviTurn;

/* A lane's arrows on the road (Valhalla's turn lanes, baldr/turnlanes.h), as bits */
enum {
  NAVI_LANE_NONE = 1 << 0, /* no arrows */
  NAVI_LANE_THROUGH = 1 << 1,
  NAVI_LANE_SHARP_LEFT = 1 << 2,
  NAVI_LANE_LEFT = 1 << 3,
  NAVI_LANE_SLIGHT_LEFT = 1 << 4,
  NAVI_LANE_SLIGHT_RIGHT = 1 << 5,
  NAVI_LANE_RIGHT = 1 << 6,
  NAVI_LANE_SHARP_RIGHT = 1 << 7,
  NAVI_LANE_REVERSE = 1 << 8,
  NAVI_LANE_MERGE_TO_LEFT = 1 << 9,
  NAVI_LANE_MERGE_TO_RIGHT = 1 << 10,
};

/* a lane up to a manoeuvre: its arrows, and of them the one this manoeuvre may take it for (`valid`) or should
 * (`active`: it suits the manoeuvre after too). Valhalla marks a lane one or the other, never both. */
typedef struct {
  guint16 directions, valid, active;
} NaviLane;

typedef struct {
  NaviTurn type;
  char *instruction;   /* written: "Turn right onto Hardenbergstraße." */
  char *verbal_alert;  /* spoken well before it: "Turn right onto Hardenbergstraße." (Navi adds the distance) */
  char *verbal_pre;    /* spoken right before it, with "Then ..." when the next one follows at once */
  char *verbal_post;   /* spoken right after it: "Continue for 800 meters." */
  char *street;        /* the street it leads onto (or the exit's toward), NULL when none */
  char *ref;           /* that street's road number ("B 1") when it has a name too, else NULL */
  char *exit;          /* a motorway exit's number, NULL when none */
  int roundabout_exit; /* the exit to take, 0 when it is no roundabout */
  guint begin, end;    /* its stretch of the path: point indices */
  double s;            /* where it is: metres from the start (the path's point `begin`) */
  double length;       /* metres from it to the next manoeuvre */
  double time;         /* seconds from it to the next manoeuvre */
  NaviLane *lanes;     /* the lanes up to it, left to right; NULL where the map has none, or none to take */
  guint n_lanes;
} NaviManeuver;

typedef struct {
  NaviPathPoint *points;
  guint n_points;
  NaviManeuver *maneuvers;
  guint n_maneuvers;
  double length; /* metres */
  double time;   /* seconds */
  char *via;     /* "Via Straße des 17. Juni - B 2": the streets most of it runs on */
  gboolean toll, highway, ferry;
  double stop_s;  /* where the route reaches its stop (the first leg's end): metres from the start; < 0 without one */
  double min_lat, min_lng, max_lat, max_lng;
} NaviRoute;

NaviRoute *navi_route_ref (NaviRoute *route);
void navi_route_unref (NaviRoute *route);
G_DEFINE_AUTOPTR_CLEANUP_FUNC (NaviRoute, navi_route_unref)

/* the manoeuvre whose stretch `s` is on (the one being driven), and the next one (the one to do) */
guint navi_route_maneuver_at (const NaviRoute *route, double s);
/* the street being driven at `s`, or NULL */
const char *navi_route_street_at (const NaviRoute *route, double s);
/* seconds left from `s` to the end, at the route's own speeds */
double navi_route_time_left (const NaviRoute *route, double s);

/* Valhalla's answer to /route: the trip first, then its alternates, as NaviRoutes in a GPtrArray that frees them */
GPtrArray *navi_route_parse (const char *json, gssize length, GError **error);

/* Why there is no route, when the server answered (a connection's failure is a G_IO_ERROR, or libsoup's) */
#define NAVI_ROUTE_ERROR (navi_route_error_quark ())
typedef enum {
  NAVI_ROUTE_ERROR_NO_ROAD,  /* no road the routes reach near the start or the destination (Valhalla's 171): off the
                              * roads, or outside the area the server routes in (Germany, while Europe is down) */
  NAVI_ROUTE_ERROR_NO_ROUTE, /* roads at both ends, no way between them */
  NAVI_ROUTE_ERROR_SERVER,   /* the server failed */
} NaviRouteError;
GQuark navi_route_error_quark (void);

/* ---- asking Valhalla ---- */

typedef struct {
  double from_lat, from_lng;
  double from_heading; /* the car's heading, or < 0 when unknown: Valhalla then starts in any direction */
  double to_lat, to_lng;
  gboolean have_via;   /* a stop on the way (Valhalla routes no alternates then) */
  double via_lat, via_lng;
  guint alternates;    /* 0 to 2 */
  gboolean motorbike;  /* Valhalla's motorcycle costing, else auto */
  gboolean avoid_tolls, avoid_highways, avoid_ferries; /* kept off where there is a way round */
  gboolean miles;      /* Valhalla's spoken distances in miles ("Continue for 2 miles") */
  const char *language; /* Valhalla's: "de-DE", "en-US"; NULL picks it from the UI language */
} NaviRouteRequest;

/* The routing server, https://navi.vibeos.top unless NAVI_SERVER says otherwise (a test server). */
const char *navi_server_url (void);
/* The one HTTP session the app uses for the server: its tiles' searches, routes and locate */
GObject *navi_http_session (void);

void navi_route_request_async (const NaviRouteRequest *request, GCancellable *cancellable, GAsyncReadyCallback callback,
                               gpointer user_data);
/* the routes, best first (a GPtrArray of NaviRoute), or NULL with the error */
GPtrArray *navi_route_request_finish (GAsyncResult *result, GError **error);

/* Valhalla's language for the UI's: "de-DE" for German, "en-US" for English, ... */
const char *navi_route_language (void);

/* a debug build prints these to its console; a release build never says where anyone is or goes */
#ifdef NAVI_DEBUG
#define NAVI_PRIVATE_DEBUG(...) g_printerr (__VA_ARGS__)
#else
#define NAVI_PRIVATE_DEBUG(...) G_STMT_START { } G_STMT_END
#endif

G_END_DECLS
