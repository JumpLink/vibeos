/* navi-map-demo: the NaviMap widget at a navigation camera (tilted, the car low on the screen), with an
 * optional drive along a GeoJSON line. Prints frame statistics once a second.
 *
 *   navi-map-demo STYLE_URL [--route FILE.geojson] [--drive] [--speed KMH] [--zoom Z] [--pitch DEG]
 *                 [--render-scale S] [--tile-lod RADIUS,SCALE,PITCH,SHIFT] [--pace N] [--seconds N]
 *                 [--fullscreen] [--cache FILE]
 *
 * --pace N shows a frame every N display refreshes (default: the widget's automatic pacing).
 * NAVI_DEMO_TRACE=1 prints every frame-clock cycle's frame time and presentation time (TRACE lines).
 *
 * Driving runs on the display's frame clock: every frame the car moves on by speed x elapsed time along
 * the line, the camera is set straight to it (no easing between steps), and the camera's bearing turns
 * toward the route 20 m ahead through a critically damped spring, so a corner turns the map smoothly.
 * While following, the car is drawn fixed at the focus point, pointing up. Touching the map leaves follow
 * mode ("user-moved") and pins the car to its place on the map; 5 s later the camera flies back to it
 * (650 ms ease-out) and follows again. At the end of the line the drive starts over.
 * Keys: r re-centres, d starts or stops the drive, q quits.
 */
#include "navi-map.h"

#include <json-glib/json-glib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RECENTER_MS 650
#define RECENTER_AFTER_S 5
#define NORTH_UP_BELOW_ZOOM 13.0
#define LOOK_AHEAD_M 20.0 /* the camera turns toward the route this far ahead of the car */
#define TURN_SMOOTH_S 0.6 /* and gets there through a critically damped spring this slow */
#define CAR_PX 56

typedef struct {
  double lat, lng;
  double dist; /* metres from the start */
} RoutePoint;

typedef enum {
  MODE_FOLLOW,   /* the camera rides with the car; the car sits still at the focus point */
  MODE_RECENTER, /* the flight back to the car */
  MODE_FREE,     /* the user has the map; the car is pinned to its place on it */
} Mode;

typedef struct {
  NaviMap *map;
  GtkWidget *window, *overlay, *car;
  char *route_json;
  RoutePoint *route;
  guint n_route;
  double zoom, pitch, speed_mps;
  gboolean driving;
  Mode mode;
  double travelled; /* metres */
  double car_lat, car_lng;
  double heading, heading_rate; /* the smoothed heading (the camera's bearing) and its rate, deg and deg/s */
  double car_turn;              /* the arrow's rotation on screen, degrees */
  guint tick, recenter_source;
  gint64 last_frame_us, flight_t0;
  NaviCamera flight_from;
  /* frame statistics, per second */
  guint frames;
  double render_ms_sum, render_ms_max;
  gint64 timings_done, last_shown_us;
  double refresh_ms;
  guint shown, vblanks[4]; /* gaps of 1, 2, 3 and 4+ refresh intervals */
  double gap_sum, gap_max;
  gboolean steady;         /* following all through this second */
  /* and over the drive's steady seconds, after the first 5 s */
  guint seconds, drive_seconds, drive_frames, drive_shown, drive_vblanks[4];
  double drive_gap_sum, drive_gap_max, drive_jitter_sum;
  int quit_after;
} Demo;

/* ---- geometry on a small area: metres east/north of a reference ---- */

static double
dist_m (double lat1, double lng1, double lat2, double lng2)
{
  double k = cos ((lat1 + lat2) / 2 * G_PI / 180);
  double dy = (lat2 - lat1) * 111320.0, dx = (lng2 - lng1) * 111320.0 * k;
  return hypot (dx, dy);
}

static double
heading_deg (double lat1, double lng1, double lat2, double lng2)
{
  double k = cos ((lat1 + lat2) / 2 * G_PI / 180);
  double h = atan2 ((lng2 - lng1) * k, lat2 - lat1) * 180 / G_PI;
  return h < 0 ? h + 360 : h;
}

static void
point_at (Demo *d, double s, double *lat, double *lng)
{
  if (s <= 0) {
    *lat = d->route[0].lat;
    *lng = d->route[0].lng;
    return;
  }
  for (guint i = 1; i < d->n_route; i++) {
    if (d->route[i].dist >= s) {
      const RoutePoint *a = &d->route[i - 1], *b = &d->route[i];
      double f = b->dist > a->dist ? (s - a->dist) / (b->dist - a->dist) : 0;
      *lat = a->lat + (b->lat - a->lat) * f;
      *lng = a->lng + (b->lng - a->lng) * f;
      return;
    }
  }
  *lat = d->route[d->n_route - 1].lat;
  *lng = d->route[d->n_route - 1].lng;
}

/* where the road goes: from the car to the point LOOK_AHEAD_M further on (near the end, the last stretch) */
static double
route_heading (Demo *d, double s)
{
  double total = d->route[d->n_route - 1].dist;
  double s1 = MIN (s + LOOK_AHEAD_M, total), s0 = MIN (s, s1 - 1), la, lo, lb, lob;
  point_at (d, s0, &la, &lo);
  point_at (d, s1, &lb, &lob);
  return dist_m (la, lo, lb, lob) < 0.5 ? d->heading : heading_deg (la, lo, lb, lob);
}

/* One step of a critically damped spring from `current` toward `target` (degrees, the short way round),
 * `rate` its speed in degrees a second; reaches the target in about `smooth_s` without overshooting.
 * The closed form of the spring (as Unity's SmoothDamp has it), so a long frame is as stable as a short. */
static double
smooth_angle (double current, double target, double *rate, double smooth_s, double dt)
{
  target = current + remainder (target - current, 360);
  double omega = 2 / smooth_s, x = omega * dt;
  double k = 1 / (1 + x + 0.48 * x * x + 0.235 * x * x * x);
  double change = current - target;
  double temp = (*rate + omega * change) * dt;
  *rate = (*rate - omega * temp) * k;
  return fmod (target + (change + temp) * k + 720, 360);
}

static gboolean
load_route (Demo *d, const char *path, GError **error)
{
  if (!g_file_get_contents (path, &d->route_json, NULL, error))
    return FALSE;
  g_autoptr (JsonParser) parser = json_parser_new ();
  if (!json_parser_load_from_data (parser, d->route_json, -1, error))
    return FALSE;
  /* the first LineString in a FeatureCollection, a Feature or a bare geometry */
  JsonObject *obj = json_node_get_object (json_parser_get_root (parser));
  if (obj && json_object_has_member (obj, "features")) {
    JsonArray *features = json_object_get_array_member (obj, "features");
    obj = json_array_get_length (features) ? json_array_get_object_element (features, 0) : NULL;
  }
  if (obj && json_object_has_member (obj, "geometry"))
    obj = json_object_get_object_member (obj, "geometry");
  if (!obj || g_strcmp0 (json_object_get_string_member_with_default (obj, "type", ""), "LineString") != 0) {
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "%s: no LineString", path);
    return FALSE;
  }
  JsonArray *coords = json_object_get_array_member (obj, "coordinates");
  d->n_route = json_array_get_length (coords);
  if (d->n_route < 2) {
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "%s: the line needs two points", path);
    return FALSE;
  }
  d->route = g_new0 (RoutePoint, d->n_route);
  for (guint i = 0; i < d->n_route; i++) {
    JsonArray *c = json_array_get_array_element (coords, i);
    d->route[i].lng = json_array_get_double_element (c, 0);
    d->route[i].lat = json_array_get_double_element (c, 1);
    if (i)
      d->route[i].dist = d->route[i - 1].dist +
                         dist_m (d->route[i - 1].lat, d->route[i - 1].lng, d->route[i].lat, d->route[i].lng);
  }
  return TRUE;
}

/* ---- the car ---- */

static gboolean
heading_up (Demo *d)
{
  return d->zoom >= NORTH_UP_BELOW_ZOOM;
}

static void
draw_car (GtkDrawingArea *area, cairo_t *cr, int w, int h, gpointer user_data)
{
  Demo *d = user_data;
  double r = MIN (w, h) / 2.0 - 2;
  cairo_translate (cr, w / 2.0, h / 2.0);
  /* flattened by the tilt, as a marker lying on the road would be */
  cairo_scale (cr, 1, 0.55 + 0.45 * cos (d->pitch * G_PI / 180));
  cairo_arc (cr, 0, 0, r, 0, 2 * G_PI);
  cairo_set_source_rgba (cr, 0.35, 0.37, 0.40, 0.16);
  cairo_fill (cr);
  cairo_rotate (cr, d->car_turn * G_PI / 180);
  double a = r * 0.78;
  cairo_move_to (cr, 0, -a);
  cairo_line_to (cr, a * 0.72, a * 0.8);
  cairo_line_to (cr, 0, a * 0.42);
  cairo_line_to (cr, -a * 0.72, a * 0.8);
  cairo_close_path (cr);
  cairo_set_source_rgb (cr, 0x3c / 255.0, 0xcf / 255.0, 0xea / 255.0);
  cairo_fill_preserve (cr);
  cairo_set_line_width (cr, 2.5);
  cairo_set_line_join (cr, CAIRO_LINE_JOIN_ROUND);
  cairo_set_source_rgb (cr, 0x1a / 255.0, 0x9c / 255.0, 0xb8 / 255.0);
  cairo_stroke (cr);
}

/* Following, the car is the focus point itself, so it cannot drift against the map. Otherwise it sits on
 * its place on the map, which the camera's own frame moves. */
static gboolean
place_car (GtkOverlay *overlay, GtkWidget *child, GdkRectangle *alloc, gpointer user_data)
{
  Demo *d = user_data;
  if (child != d->car)
    return FALSE;
  double x, y;
  if (d->mode == MODE_FOLLOW)
    navi_map_get_focus_point (d->map, &x, &y);
  else
    navi_map_geo_to_screen (d->map, d->car_lat, d->car_lng, &x, &y);
  if (isnan (x) || isnan (y))
    x = y = -1000;
  alloc->width = alloc->height = CAR_PX;
  alloc->x = (int) lround (x) - CAR_PX / 2;
  alloc->y = (int) lround (y) - CAR_PX / 2;
  return TRUE;
}

/* after the camera or the car moved: the arrow points along the smoothed heading, which is straight up
 * while the camera follows heading-up */
static void
update_car (Demo *d)
{
  double turn = 0;
  if (d->mode != MODE_FOLLOW || !heading_up (d)) {
    NaviCamera cam;
    navi_map_get_camera (d->map, &cam);
    turn = remainder (d->heading - (isnan (cam.bearing) ? 0 : cam.bearing), 360);
  }
  if (fabs (turn - d->car_turn) > 0.05) {
    d->car_turn = turn;
    gtk_widget_queue_draw (d->car);
  }
  if (d->mode != MODE_FOLLOW)
    gtk_widget_queue_allocate (d->overlay);
}

static void
on_camera_changed (NaviMap *map, Demo *d)
{
  update_car (d);
}

/* ---- the frame clock: the car, the camera and the statistics ---- */

/* the gaps between frames that reached the screen, from the compositor's presentation times */
static void
count_shown (Demo *d, GdkFrameClock *clock)
{
  gint64 now = gdk_frame_clock_get_frame_counter (clock);
  for (gint64 i = MAX (d->timings_done + 1, gdk_frame_clock_get_history_start (clock)); i < now; i++) {
    GdkFrameTimings *t = gdk_frame_clock_get_timings (clock, i);
    if (!t || !gdk_frame_timings_get_complete (t))
      break;
    d->timings_done = i;
    gint64 shown = gdk_frame_timings_get_presentation_time (t), refresh = gdk_frame_timings_get_refresh_interval (t);
    if (refresh)
      d->refresh_ms = refresh / 1000.0;
    static int trace = -1;
    if (trace < 0)
      trace = g_getenv ("NAVI_DEMO_TRACE") != NULL;
    if (trace)
      g_print ("TRACE %" G_GINT64_FORMAT " frame %.2f shown %.2f predicted %.2f\n", i,
               gdk_frame_timings_get_frame_time (t) / 1000.0, shown / 1000.0,
               gdk_frame_timings_get_predicted_presentation_time (t) / 1000.0);
    if (!shown)
      continue;
    if (d->last_shown_us) {
      double gap = (shown - d->last_shown_us) / 1000.0;
      d->shown++;
      d->gap_sum += gap;
      d->gap_max = MAX (d->gap_max, gap);
      if (d->refresh_ms > 0)
        d->vblanks[CLAMP ((int) lround (gap / d->refresh_ms), 1, 4) - 1]++;
    }
    d->last_shown_us = shown;
  }
}

static void
set_camera (Demo *d, gint64 now)
{
  if (d->mode == MODE_FOLLOW) {
    NaviCamera cam = { d->car_lat, d->car_lng, NAN, heading_up (d) ? d->heading : 0, NAN };
    navi_map_jump_to (d->map, &cam);
  } else if (d->mode == MODE_RECENTER) {
    /* a 650 ms ease-out from where the user left the camera to the car as it is in this frame, so the
     * flight lands exactly on the moving car and following takes over without a step */
    double k = MIN (1.0, (now - d->flight_t0) / (RECENTER_MS * 1000.0)), e = 1 - pow (1 - k, 3);
    const NaviCamera *f = &d->flight_from;
    double bearing = heading_up (d) ? d->heading : 0;
    NaviCamera cam = { f->latitude + (d->car_lat - f->latitude) * e, f->longitude + (d->car_lng - f->longitude) * e,
                       f->zoom + (d->zoom - f->zoom) * e, f->bearing + remainder (bearing - f->bearing, 360) * e,
                       f->pitch + (d->pitch - f->pitch) * e };
    navi_map_jump_to (d->map, &cam);
    if (k >= 1) {
      d->mode = MODE_FOLLOW;
      gtk_widget_queue_allocate (d->overlay);
    }
    d->steady = FALSE;
  }
}

static gboolean
frame_tick (GtkWidget *widget, GdkFrameClock *clock, gpointer user_data)
{
  Demo *d = user_data;
  gint64 now = gdk_frame_clock_get_frame_time (clock);
  /* a stall (the window hidden) moves the car on by at most a quarter second */
  double dt = d->last_frame_us ? CLAMP ((now - d->last_frame_us) / 1e6, 0.0, 0.25) : 0;
  d->last_frame_us = now;
  count_shown (d, clock);

  if (d->driving) {
    double total = d->route[d->n_route - 1].dist;
    d->travelled += d->speed_mps * dt;
    gboolean wrapped = d->travelled >= total;
    if (wrapped)
      d->travelled = fmod (d->travelled, total);
    point_at (d, d->travelled, &d->car_lat, &d->car_lng);
    double target = route_heading (d, d->travelled);
    if (wrapped) { /* back at the start: no spin all the way round from the last heading */
      d->heading = target;
      d->heading_rate = 0;
    } else {
      d->heading = smooth_angle (d->heading, target, &d->heading_rate, TURN_SMOOTH_S, dt);
    }
  }
  set_camera (d, now);
  update_car (d);

  if (d->driving || d->mode == MODE_RECENTER)
    return G_SOURCE_CONTINUE;
  d->tick = 0;
  d->last_frame_us = d->last_shown_us = 0;
  return G_SOURCE_REMOVE;
}

static void
ensure_tick (Demo *d)
{
  if (!d->tick)
    d->tick = gtk_widget_add_tick_callback (GTK_WIDGET (d->map), frame_tick, d, NULL);
}

/* ---- following ---- */

static void
recenter (Demo *d)
{
  g_clear_handle_id (&d->recenter_source, g_source_remove);
  navi_map_cancel_transitions (d->map);
  navi_map_get_camera (d->map, &d->flight_from);
  GdkFrameClock *clock = gtk_widget_get_frame_clock (GTK_WIDGET (d->map));
  d->flight_t0 = clock ? gdk_frame_clock_get_frame_time (clock) : g_get_monotonic_time ();
  d->mode = MODE_RECENTER;
  d->last_shown_us = 0; /* the map stood still till now: that pause is no late frame */
  ensure_tick (d);
}

static gboolean
recenter_timeout (gpointer user_data)
{
  Demo *d = user_data;
  d->recenter_source = 0;
  recenter (d);
  return G_SOURCE_REMOVE;
}

static void
on_user_moved (NaviMap *map, Demo *d)
{
  d->mode = MODE_FREE;
  d->steady = FALSE;
  g_clear_handle_id (&d->recenter_source, g_source_remove);
  d->recenter_source = g_timeout_add_seconds (RECENTER_AFTER_S, recenter_timeout, d);
  update_car (d);
  g_print ("user moved the map: follow off, re-centre in %d s\n", RECENTER_AFTER_S);
}

static void
set_driving (Demo *d, gboolean on)
{
  if (!d->route || d->driving == on)
    return;
  d->driving = on;
  if (on)
    ensure_tick (d);
  g_print ("drive %s\n", on ? "on" : "off");
}

/* ---- the map's signals ---- */

static void
on_style_loaded (NaviMap *map, Demo *d)
{
  g_print ("style loaded\n");
  if (!d->route_json)
    return;
  /* under the first label layer: the route covers the roads, never the street names */
  g_auto (GStrv) symbols = navi_map_list_layers (map, "symbol");
  const char *below = symbols && symbols[0] ? symbols[0] : NULL;
  g_autoptr (GError) error = NULL;
  if (!navi_map_add_geojson_source (map, "navi-route", d->route_json, &error) ||
      !navi_map_add_layer_json (map,
                                "{\"id\":\"navi-route-edge\",\"type\":\"line\",\"source\":\"navi-route\","
                                "\"layout\":{\"line-join\":\"round\",\"line-cap\":\"round\"},"
                                "\"paint\":{\"line-color\":\"#48009C\",\"line-width\":"
                                "[\"interpolate\",[\"exponential\",1.6],[\"zoom\"],12,5,15,9,17,19,19,64]}}",
                                below, &error) ||
      !navi_map_add_layer_json (map,
                                "{\"id\":\"navi-route-fill\",\"type\":\"line\",\"source\":\"navi-route\","
                                "\"layout\":{\"line-join\":\"round\",\"line-cap\":\"round\"},"
                                "\"paint\":{\"line-color\":\"#6C18D2\",\"line-width\":"
                                "[\"interpolate\",[\"exponential\",1.6],[\"zoom\"],12,3.4,15,6.5,17,14.5,19,52]}}",
                                below, &error))
    g_printerr ("route: %s\n", error->message);
}

static void
on_load_error (NaviMap *map, const char *message, Demo *d)
{
  g_printerr ("map load error: %s\n", message);
}

static void
on_frame (NaviMap *map, double render_ms, Demo *d)
{
  d->frames++;
  d->render_ms_sum += render_ms;
  d->render_ms_max = MAX (d->render_ms_max, render_ms);
}

static gboolean
print_fps (gpointer user_data)
{
  Demo *d = user_data;
  NaviCamera cam;
  navi_map_get_camera (d->map, &cam);
  d->seconds++;
  double gap = d->shown ? d->gap_sum / d->shown : 0;
  static const char *const modes[] = { "following", "re-centring", "free" };
  /* "shown" counts the frames that reached the screen, by the compositor's presentation times; the gap
   * between two of them, in refresh intervals, is 1 when every refresh got a new frame */
  g_print ("fps %3u  render avg %5.2f max %6.2f ms  shown %3u gap avg %5.2f max %6.2f jitter %5.2f ms"
           "  refreshes 1:%u 2:%u 3:%u 4+:%u  zoom %.2f pitch %.0f bearing %.0f  %s%s\n",
           d->frames, d->frames ? d->render_ms_sum / d->frames : 0.0, d->render_ms_max, d->shown, gap, d->gap_max,
           d->shown ? d->gap_max - gap : 0.0, d->vblanks[0], d->vblanks[1], d->vblanks[2], d->vblanks[3], cam.zoom,
           cam.pitch, cam.bearing, modes[d->mode], d->driving ? ", driving" : "");
  /* the drive figures take the seconds spent following from start to end, after the first 5 s (style,
   * tiles and shaders arrive then) */
  if (d->driving && d->steady && d->mode == MODE_FOLLOW && d->seconds > 5 && d->shown) {
    d->drive_seconds++;
    d->drive_frames += d->frames;
    d->drive_shown += d->shown;
    for (int i = 0; i < 4; i++)
      d->drive_vblanks[i] += d->vblanks[i];
    d->drive_gap_sum += d->gap_sum;
    d->drive_gap_max = MAX (d->drive_gap_max, d->gap_max);
    d->drive_jitter_sum += d->gap_max - gap;
  }
  d->steady = d->mode == MODE_FOLLOW;
  d->frames = d->shown = 0;
  memset (d->vblanks, 0, sizeof d->vblanks);
  d->render_ms_sum = d->render_ms_max = d->gap_sum = d->gap_max = 0;
  if (d->quit_after > 0 && (int) d->seconds >= d->quit_after) {
    if (d->drive_seconds)
      g_print ("drive over %u s: %.1f fps, shown every %.2f ms (refresh %.2f ms), worst gap %.2f ms, mean jitter "
               "%.2f ms; gaps of 1/2/3/4+ refreshes: %u/%u/%u/%u\n",
               d->drive_seconds, (double) d->drive_frames / d->drive_seconds, d->drive_gap_sum / d->drive_shown,
               d->refresh_ms, d->drive_gap_max, d->drive_jitter_sum / d->drive_seconds, d->drive_vblanks[0],
               d->drive_vblanks[1], d->drive_vblanks[2], d->drive_vblanks[3]);
    gtk_window_destroy (GTK_WINDOW (d->window));
    return G_SOURCE_REMOVE;
  }
  return G_SOURCE_CONTINUE;
}

static gboolean
on_key (GtkEventControllerKey *c, guint keyval, guint code, GdkModifierType state, Demo *d)
{
  switch (keyval) {
  case GDK_KEY_r: recenter (d); return TRUE;
  case GDK_KEY_d: set_driving (d, !d->driving); return TRUE;
  case GDK_KEY_q: gtk_window_destroy (GTK_WINDOW (d->window)); return TRUE;
  default: return FALSE;
  }
}

static void
print_display (GtkWidget *window, Demo *d)
{
  GdkSurface *surface = gtk_native_get_surface (GTK_NATIVE (window));
  GdkMonitor *monitor = gdk_display_get_monitor_at_surface (gtk_widget_get_display (window), surface);
  g_print ("window %dx%d, scale %.2f, display %.1f Hz\n", gtk_widget_get_width (window),
           gtk_widget_get_height (window), gdk_surface_get_scale (surface),
           monitor ? gdk_monitor_get_refresh_rate (monitor) / 1000.0 : 0.0);
}

int
main (int argc, char **argv)
{
  Demo d = { .zoom = 16.5, .pitch = 52, .speed_mps = 50 / 3.6, .mode = MODE_FOLLOW };
  const char *style = NULL, *route = NULL, *cache = NULL;
  gboolean drive = FALSE, fullscreen = FALSE;
  double render_scale = 1, lod[4] = { 3, 1, 60, 0 };
  int pace = 0;
  for (int i = 1; i < argc; i++) {
    const char *a = argv[i];
    const char *v = i + 1 < argc ? argv[i + 1] : NULL;
    if (!strcmp (a, "--route") && v) route = v, i++;
    else if (!strcmp (a, "--drive")) drive = TRUE;
    else if (!strcmp (a, "--speed") && v) d.speed_mps = g_ascii_strtod (v, NULL) / 3.6, i++;
    else if (!strcmp (a, "--zoom") && v) d.zoom = g_ascii_strtod (v, NULL), i++;
    else if (!strcmp (a, "--pitch") && v) d.pitch = g_ascii_strtod (v, NULL), i++;
    else if (!strcmp (a, "--seconds") && v) d.quit_after = atoi (v), i++;
    else if (!strcmp (a, "--render-scale") && v) render_scale = g_ascii_strtod (v, NULL), i++;
    else if (!strcmp (a, "--tile-lod") && v &&
             sscanf (v, "%lf,%lf,%lf,%lf", &lod[0], &lod[1], &lod[2], &lod[3]) == 4) i++;
    else if (!strcmp (a, "--fullscreen")) fullscreen = TRUE;
    else if (!strcmp (a, "--pace") && v) pace = atoi (v), i++;
    else if (!strcmp (a, "--cache") && v) cache = v, i++;
    else if (a[0] != '-' && !style) style = a;
    else {
      g_printerr ("usage: %s STYLE_URL [--route FILE.geojson] [--drive] [--speed KMH] [--zoom Z] "
                  "[--pitch DEG] [--render-scale S] [--tile-lod RADIUS,SCALE,PITCH,SHIFT] [--pace N] [--seconds N] "
                  "[--fullscreen] [--cache FILE]\n", argv[0]);
      return 2;
    }
  }
  if (!style) {
    g_printerr ("%s: a style URL is required\n", argv[0]);
    return 2;
  }
  if (route) {
    g_autoptr (GError) error = NULL;
    if (!load_route (&d, route, &error)) {
      g_printerr ("%s\n", error->message);
      return 1;
    }
    d.car_lat = d.route[0].lat;
    d.car_lng = d.route[0].lng;
    d.heading = route_heading (&d, 0);
    g_print ("route: %u points, %.0f m\n", d.n_route, d.route[d.n_route - 1].dist);
  } else {
    d.car_lat = 52.5009, d.car_lng = 13.3469; /* nowhere in particular: a Berlin junction */
    d.heading = 45;
  }

  gtk_init ();
  d.window = gtk_window_new ();
  gtk_window_set_title (GTK_WINDOW (d.window), "Navi map demo");
  gtk_window_set_default_size (GTK_WINDOW (d.window), 400, 800);
  d.overlay = gtk_overlay_new ();
  d.map = NAVI_MAP (navi_map_new ());
  gtk_overlay_set_child (GTK_OVERLAY (d.overlay), GTK_WIDGET (d.map));
  d.car = gtk_drawing_area_new ();
  gtk_drawing_area_set_draw_func (GTK_DRAWING_AREA (d.car), draw_car, &d, NULL);
  gtk_widget_set_can_target (d.car, FALSE); /* touches go through to the map */
  gtk_overlay_add_overlay (GTK_OVERLAY (d.overlay), d.car);
  g_signal_connect (d.overlay, "get-child-position", G_CALLBACK (place_car), &d);
  gtk_window_set_child (GTK_WINDOW (d.window), d.overlay);

  if (cache)
    navi_map_set_cache_path (d.map, cache);
  navi_map_set_focus (d.map, 0.5, 0.75); /* the car a quarter of the height above the bottom */
  navi_map_set_render_scale (d.map, render_scale);
  navi_map_set_frame_pacing (d.map, pace);
  navi_map_set_tile_lod (d.map, lod[0], lod[1], lod[2], lod[3]);
  NaviCamera cam = { d.car_lat, d.car_lng, d.zoom, d.zoom >= NORTH_UP_BELOW_ZOOM ? d.heading : 0, d.pitch };
  navi_map_jump_to (d.map, &cam);
  navi_map_set_style_url (d.map, style);
  g_signal_connect (d.map, "style-loaded", G_CALLBACK (on_style_loaded), &d);
  g_signal_connect (d.map, "load-error", G_CALLBACK (on_load_error), &d);
  g_signal_connect (d.map, "frame-rendered", G_CALLBACK (on_frame), &d);
  g_signal_connect (d.map, "camera-changed", G_CALLBACK (on_camera_changed), &d);
  g_signal_connect (d.map, "user-moved", G_CALLBACK (on_user_moved), &d);

  GtkEventController *keys = gtk_event_controller_key_new ();
  g_signal_connect (keys, "key-pressed", G_CALLBACK (on_key), &d);
  gtk_widget_add_controller (d.window, keys);
  g_signal_connect_after (d.window, "map", G_CALLBACK (print_display), &d);

  if (fullscreen)
    gtk_window_fullscreen (GTK_WINDOW (d.window));
  else
    gtk_window_maximize (GTK_WINDOW (d.window)); /* a tablet's shell may leave the default size */
  gtk_window_present (GTK_WINDOW (d.window));
  if (drive)
    set_driving (&d, TRUE);
  g_timeout_add_seconds (1, print_fps, &d);

  while (g_list_model_get_n_items (gtk_window_get_toplevels ()) > 0)
    g_main_context_iteration (NULL, TRUE);
  g_free (d.route);
  g_free (d.route_json);
  return 0;
}
