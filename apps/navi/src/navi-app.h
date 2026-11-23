/* navi-app: the Navi window, shared by its parts.
 *
 *   navi-main.c     the application, its options and the window's frame (phone: the map with panels over it;
 *                   tablet: the rail and the map, the panels floating over it as cards)
 *   navi-window.c   the map: the car, the camera following it, the routes on the map, the drive and its guidance
 *   navi-panels.c   everything over the map: the "Where to?" sheet and search, a place's card, the routes' preview,
 *                   the drive's banner and bottom bar, the menu and its settings
 *
 * Navi is one window, in one of these modes, each with its panel (the bottom sheet on a phone, a card floating
 * over the map's bottom left on a tablet, where browsing has none: the rail has Search):
 *
 *   BROWSE   the map round the car, "Where to?"
 *   PLACE    a place's card: one found, a home, a long press on the map
 *   ROUTES   the routes there: "Your location -> destination", their times on the map, Leave later and Go now
 *   DRIVE    driving: the banner with the next turn, the car followed in 3D, the bar with the arrival time
 *   ARRIVED  "You have arrived"
 *
 * and the camera in one of these: following the car, flying back to it (re-centre), left where the user put it
 * (free), or showing a whole route (fixed: the preview, the overview). */
#pragma once

#include <gtk/gtk.h>
#include <vibeos.h>

#include "navi-geo.h"
#include "navi-guidance.h"
#include "navi-map.h"
#include "navi-position.h"
#include "navi-route.h"
#include "navi-search.h"
#include "navi-settings.h"

G_BEGIN_DECLS

typedef enum {
  NAVI_MODE_BROWSE,
  NAVI_MODE_PLACE,
  NAVI_MODE_ROUTES,
  NAVI_MODE_DRIVE,
  NAVI_MODE_ARRIVED,
} NaviMode;

typedef enum {
  NAVI_CAM_FOLLOW,
  NAVI_CAM_RECENTER,
  NAVI_CAM_FREE,
  NAVI_CAM_FIXED,
} NaviCam;

typedef struct _NaviPanels NaviPanels;

typedef struct {
  GtkApplication *app;
  GtkWidget *win;
  NaviSettings *settings;
  gboolean tablet;
  char *data_dir;           /* the styles */

  /* the frame */
  GtkWidget *rail;          /* the tablet's; NULL on a phone */
  GtkWidget *map_overlay;   /* the map, the car and whatever floats over the map */
  NaviMap *map;
  GtkWidget *car;           /* the car: fixed on the focus point while following */
  GtkWidget *pin;           /* the destination's pin */
  GPtrArray *bubbles;       /* the routes' time bubbles in the preview */
  NaviPanels *panels;

  NaviMode mode;
  NaviCam cam;

  /* the position */
  NaviPosition *position;
  NaviFix fix;
  gboolean have_fix;
  gboolean simulate;        /* --simulate: a drive along the route instead of GPS */
  double sim_speed;
  gboolean sim_detour;      /* --sim-detour: the simulator takes the second route, off the one being guided */

  /* what is drawn: the car where it is in this frame (between fixes, ahead along the route), and its heading */
  double car_lat, car_lng, car_heading, heading_rate;
  double car_s;             /* along the route, while driving */
  double car_speed;         /* m/s */
  double fix_s;
  gint64 fix_time;
  gboolean car_known;

  /* the camera */
  double zoom, zoom_rate, pitch;
  NaviCamera flight_from;
  gint64 flight_t0;
  guint tick, recenter_source;
  gint64 last_frame_us;
  char **poi_layers;        /* the style's point-of-interest layers, for a tap */
  int poi_scale;            /* the scale their markers were drawn at */
  guint poi_seq;            /* the point of interest tapped last, for its address coming back */
  gint64 poi_tapped_at;     /* when a tap last opened a place's card (on_double_tap ()) */
  guint keep_awake;         /* the idle inhibitor while a route is driven (gtk_application_inhibit), 0 when none */
  double ahead_s;           /* where the route ahead was last drawn from (trim_ahead ()) */
  gint64 ahead_time;

  /* the routes */
  NaviPlace *dest;
  NaviPlace *stop;          /* a stop on the way while driving, NULL when none (or once it is reached) */
  GtkWidget *back;          /* the unseen "Back" the shell's back gesture presses while there is a route or a card */
  GPtrArray *routes;        /* NaviRoute, best first */
  guint selected;
  GCancellable *route_cancel;
  GCancellable *place_cancel; /* the drive to the place card's place, asked for its time and length */
  NaviGuidance *guidance;
  guint rerouting;          /* a new route is on its way */
  gboolean reroute_failed;  /* off the route and the last new route failed: it is asked again */
  gboolean map_failed;      /* the map's style could not be loaded */
  struct _NaviAlerts *alerts; /* the road's limit and its speed cameras (navi-alerts.c) */
  gboolean signals_shown, cams_shown; /* the overlay's layers on the map now */
  gint64 last_reroute;
  gboolean night;
  gboolean style_night;     /* the style loaded */

  NaviSearch *search;

  /* NAVI_STATS=1: frame pacing, each second and over the drive */
  gboolean stats;
  gint64 timings_done, last_shown_us;
  double refresh_ms, gap_sum, gap_max, render_sum, render_max;
  guint frames, shown, vblanks[4];
  guint drive_seconds, drive_frames, drive_shown, drive_vblanks[4];
  double drive_gap_sum, drive_gap_max, drive_jitter_sum;
  gboolean steady;
  gint64 fix_at, fix_cost;  /* when the last fix was handled, and how long that took (--stats: late frames) */
} Navi;

/* navi-window.c */
void navi_window_build_map (Navi *navi);
void navi_window_set_mode (Navi *navi, NaviMode mode);
/* the place's card, the camera on it */
void navi_show_place (Navi *navi, const NaviPlace *place);
/* routes from the car to `place`, and their preview */
void navi_preview_routes (Navi *navi, const NaviPlace *place);
void navi_select_route (Navi *navi, guint index);
/* Go now: drive the selected route */
void navi_start_drive (Navi *navi);
/* Stop, or the end of the drive: back to browsing */
void navi_stop_drive (Navi *navi);
/* while driving: the route goes on to the destination through `place` */
void navi_add_stop (Navi *navi, const NaviPlace *place);
/* the Map view setting, applied now */
void navi_apply_view (Navi *navi);
/* the overlay's icons on the map: traffic signals while driving, speed cameras where they may be shown */
void navi_window_update_overlay (Navi *navi);
void navi_back_to_browse (Navi *navi);
void navi_recenter (Navi *navi);
void navi_overview (Navi *navi);
/* the car's position for a route's start and the searches' "near": the fix, the last known, or the map's centre */
void navi_where_am_i (Navi *navi, double *lat, double *lng, gboolean *approximate);
/* the map's day or night style, from the settings, the sun where the car is, or the system */
void navi_update_night (Navi *navi);
double navi_distance_from_car (Navi *navi, double lat, double lng);

/* navi-panels.c */
/* a point of interest's kind in words, "Gas station", from its group (navi-poi.h); NULL for an unknown one */
const char *navi_poi_label (const char *group);
NaviPanels *navi_panels_new (Navi *navi);
/* the voice and the countdowns stop: the window is going */
void navi_panels_quiet (NaviPanels *panels);
void navi_panels_free (NaviPanels *panels);
void navi_panels_set_mode (NaviPanels *panels, NaviMode mode);
void navi_panels_set_cam (NaviPanels *panels, NaviCam cam);
void navi_panels_show_place (NaviPanels *panels, const NaviPlace *place);
/* the place card's drive there by road, once a route to it is known: its chip then shows that */
void navi_panels_set_place_drive (NaviPanels *panels, double metres, double seconds);
void navi_panels_set_routes (NaviPanels *panels, GPtrArray *routes, guint selected, const char *error);
void navi_panels_update_drive (NaviPanels *panels, const NaviProgress *progress, NaviRoute *route);
void navi_panels_set_speed (NaviPanels *panels, double speed);
/* the road's limit on the sign by the speedometer: km/h, NAVI_LIMIT_NONE, or NAVI_LIMIT_UNKNOWN (no sign) */
void navi_panels_set_limit (NaviPanels *panels, int kmh);
/* the units of the road's country ("DE": km/h, "GB": mph) */
void navi_panels_set_units (NaviPanels *panels, const char *country);
/* the card over the speedometer for a camera ahead: "Speed camera", "300 m"; a NULL title hides it */
void navi_panels_show_alert (NaviPanels *panels, const char *title, const char *detail);
/* once ever: where speed camera alerts are not allowed (Settings' cameras_notice_seen) */
void navi_panels_camera_notice (NaviPanels *panels);
void navi_panels_set_street (NaviPanels *panels, const char *street);
void navi_panels_set_note (NaviPanels *panels, const char *note);
void navi_panels_set_rerouting (NaviPanels *panels, gboolean rerouting);
void navi_panels_set_compass (NaviPanels *panels, double bearing);
/* the height the bottom sheet (a phone's) or card (a tablet's) covers, and the banner's at the top */
int navi_panels_bottom_inset (NaviPanels *panels);
int navi_panels_top_inset (NaviPanels *panels);
/* speak a prompt, as the voice setting allows */
void navi_panels_say (NaviPanels *panels, const char *text, gboolean alert);
/* the prompts likely to be said soon (navi_guidance_upcoming_prompts ()), made into sound ahead */
void navi_panels_prepare_speech (NaviPanels *panels, char **texts);
/* the voice loaded now: routes are being looked at, a drive may follow */
void navi_panels_warm_voice (NaviPanels *panels);
/* the voice let go of: no drive is near */
void navi_panels_rest_voice (NaviPanels *panels);
/* the menu (Settings, About) over `anchor`: the rail's menu button */
void navi_panels_open_menu (NaviPanels *panels, GtkWidget *anchor);
/* the search, opened with this text (for tests: --search) */
void navi_panels_open_search (NaviPanels *panels, const char *text);
/* the sound button, after the Spoken directions setting changed */
void navi_panels_sync_sound (NaviPanels *panels);
/* a road alert (a speed camera ahead): spoken unless the sound is off, also in "Alerts only" */
void navi_panels_say_alert (NaviPanels *panels, const char *text);
/* a route is being previewed or driven: asks whether to cancel it. With `place` (a new destination chosen while
 * driving) the answers are Add a stop and New destination (a tap outside keeps the drive); without (Back), Keep
 * driving (Keep route) and Cancel route, which then opens the search for a new address */
void navi_panels_ask_route_change (NaviPanels *panels, const NaviPlace *place);
/* Back for what the panels show over the map (the search): TRUE when it closed something */
gboolean navi_panels_back (NaviPanels *panels);

G_END_DECLS
