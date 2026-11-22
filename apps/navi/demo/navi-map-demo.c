/* navi-map-demo: the NaviMap widget at a navigation camera (tilted, the car low on the screen), with an
 * optional drive along a GeoJSON line. Prints frames per second once a second.
 *
 *   navi-map-demo STYLE_URL [--route FILE.geojson] [--drive] [--speed KMH] [--zoom Z] [--pitch DEG]
 *                 [--seconds N] [--fullscreen] [--cache FILE]
 *
 * Driving, the camera eases to the car's position and heading every 100 ms with a linear curve, so the
 * eases join into one steady motion; at the end of the line it starts over. Touching the map leaves
 * follow mode ("user-moved"); 5 s later the camera flies back to the car (650 ms ease-out) and follows
 * again. Keys: r re-centres, d starts or stops the drive, q quits.
 */
#include "navi-map.h"

#include <json-glib/json-glib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TICK_MS 100
#define EASE_MS 120 /* a little longer than a tick: a late tick never leaves the camera standing */
#define RECENTER_MS 650
#define RECENTER_AFTER_S 5
#define NORTH_UP_BELOW_ZOOM 13.0
#define CAR_PX 56

typedef struct {
  double lat, lng;
  double dist; /* metres from the start */
} RoutePoint;

typedef struct {
  NaviMap *map;
  GtkWidget *window, *overlay, *car;
  char *route_json;
  RoutePoint *route;
  guint n_route;
  double zoom, pitch, speed_mps;
  gboolean driving, following;
  double travelled; /* metres */
  gint64 last_tick_us;
  double car_lat, car_lng, car_heading;
  double cam_bearing;
  guint drive_source, recenter_source, follow_again_source;
  /* frame statistics */
  guint frames;
  double render_ms_sum, render_ms_max;
  guint seconds;
  guint drive_frames, drive_seconds;
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

/* the heading a driver looks along: from the car to a point 12 m further on the route */
static double
heading_at (Demo *d, double s)
{
  double la, lo, lb, lob;
  point_at (d, s, &la, &lo);
  point_at (d, s + 12, &lb, &lob);
  if (dist_m (la, lo, lb, lob) < 0.5)
    return d->car_heading;
  return heading_deg (la, lo, lb, lob);
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

static void
draw_car (GtkDrawingArea *area, cairo_t *cr, int w, int h, gpointer user_data)
{
  Demo *d = user_data;
  NaviCamera cam;
  navi_map_get_camera (d->map, &cam);
  double turn = (d->car_heading - (isnan (cam.bearing) ? 0 : cam.bearing)) * G_PI / 180;
  double r = MIN (w, h) / 2.0 - 2;
  cairo_translate (cr, w / 2.0, h / 2.0);
  /* flattened by the tilt, as a marker lying on the road would be */
  cairo_scale (cr, 1, isnan (cam.pitch) ? 1 : 0.55 + 0.45 * cos (cam.pitch * G_PI / 180));
  cairo_arc (cr, 0, 0, r, 0, 2 * G_PI);
  cairo_set_source_rgba (cr, 0.35, 0.37, 0.40, 0.16);
  cairo_fill (cr);
  cairo_rotate (cr, turn);
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

static gboolean
place_car (GtkOverlay *overlay, GtkWidget *child, GdkRectangle *alloc, gpointer user_data)
{
  Demo *d = user_data;
  if (child != d->car)
    return FALSE;
  double x, y;
  navi_map_geo_to_screen (d->map, d->car_lat, d->car_lng, &x, &y);
  if (isnan (x) || isnan (y))
    x = y = -1000;
  alloc->width = alloc->height = CAR_PX;
  alloc->x = (int) lround (x) - CAR_PX / 2;
  alloc->y = (int) lround (y) - CAR_PX / 2;
  return TRUE;
}

static void
on_camera_changed (NaviMap *map, Demo *d)
{
  gtk_widget_queue_allocate (d->overlay);
  gtk_widget_queue_draw (d->car);
}

/* ---- following ---- */

static double
follow_bearing (Demo *d, double zoom)
{
  return zoom < NORTH_UP_BELOW_ZOOM ? 0 : d->car_heading;
}

static gboolean
follow_again (gpointer user_data)
{
  Demo *d = user_data;
  d->follow_again_source = 0;
  d->following = TRUE;
  return G_SOURCE_REMOVE;
}

static void
recenter (Demo *d)
{
  g_clear_handle_id (&d->recenter_source, g_source_remove);
  g_clear_handle_id (&d->follow_again_source, g_source_remove);
  /* fly to where the car will be when the flight ends, then take up the 100 ms eases from there */
  double s = d->travelled + (d->driving ? d->speed_mps * RECENTER_MS / 1000.0 : 0), lat, lng;
  if (d->route)
    point_at (d, s, &lat, &lng);
  else
    lat = d->car_lat, lng = d->car_lng;
  NaviCamera cam = { lat, lng, d->zoom, follow_bearing (d, d->zoom), d->pitch };
  navi_map_ease_to (d->map, &cam, RECENTER_MS, NAVI_EASING_EASE_OUT_CUBIC);
  d->follow_again_source = g_timeout_add (RECENTER_MS, follow_again, d);
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
  d->following = FALSE;
  g_clear_handle_id (&d->follow_again_source, g_source_remove);
  g_clear_handle_id (&d->recenter_source, g_source_remove);
  d->recenter_source = g_timeout_add_seconds (RECENTER_AFTER_S, recenter_timeout, d);
  g_print ("user moved the map: follow off, re-centre in %d s\n", RECENTER_AFTER_S);
}

static gboolean
drive_tick (gpointer user_data)
{
  Demo *d = user_data;
  gint64 now = g_get_monotonic_time ();
  double dt = d->last_tick_us ? (now - d->last_tick_us) / 1e6 : TICK_MS / 1000.0;
  d->last_tick_us = now;
  d->travelled += d->speed_mps * dt;
  gboolean wrapped = d->travelled > d->route[d->n_route - 1].dist;
  if (wrapped) { /* round again from the start */
    d->travelled = 0;
    d->car_heading = heading_at (d, 0);
  }
  point_at (d, d->travelled, &d->car_lat, &d->car_lng);
  /* the car turns at most 90 degrees a second, and its heading leads the corner a little */
  double target = heading_at (d, d->travelled);
  double diff = remainder (target - d->car_heading, 360);
  double max = 90 * dt;
  d->car_heading = fmod (d->car_heading + CLAMP (diff, -max, max) + 360, 360);

  if (d->following) {
    NaviCamera cam = NAVI_CAMERA_UNSET, now_cam;
    navi_map_get_camera (d->map, &now_cam);
    cam.latitude = d->car_lat;
    cam.longitude = d->car_lng;
    cam.bearing = follow_bearing (d, now_cam.zoom);
    if (wrapped)
      navi_map_jump_to (d->map, &cam);
    else
      navi_map_ease_to (d->map, &cam, EASE_MS, NAVI_EASING_LINEAR);
  } else {
    gtk_widget_queue_allocate (d->overlay); /* the car moves on screen */
    gtk_widget_queue_draw (d->car);
  }
  return G_SOURCE_CONTINUE;
}

static void
set_driving (Demo *d, gboolean on)
{
  if (!d->route || d->driving == on)
    return;
  d->driving = on;
  if (on) {
    d->last_tick_us = 0;
    d->drive_source = g_timeout_add (TICK_MS, drive_tick, d);
  } else {
    g_clear_handle_id (&d->drive_source, g_source_remove);
  }
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
  g_print ("fps %3u  render avg %5.2f ms max %6.2f ms  zoom %.2f pitch %.0f bearing %.0f%s\n", d->frames,
           d->frames ? d->render_ms_sum / d->frames : 0.0, d->render_ms_max, cam.zoom, cam.pitch, cam.bearing,
           d->driving ? (d->following ? "  driving" : "  driving, not following") : "");
  /* the drive average leaves out the first 5 s (style, tiles and shaders arrive then) */
  if (d->driving && d->following && d->seconds > 5) {
    d->drive_frames += d->frames;
    d->drive_seconds++;
  }
  d->frames = 0;
  d->render_ms_sum = d->render_ms_max = 0;
  if (d->quit_after > 0 && (int) d->seconds >= d->quit_after) {
    if (d->drive_seconds)
      g_print ("drive average: %.1f fps over %u s\n", (double) d->drive_frames / d->drive_seconds,
               d->drive_seconds);
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
  Demo d = { .zoom = 16.5, .pitch = 52, .speed_mps = 50 / 3.6, .following = TRUE };
  const char *style = NULL, *route = NULL, *cache = NULL;
  gboolean drive = FALSE, fullscreen = FALSE;
  for (int i = 1; i < argc; i++) {
    const char *a = argv[i];
    const char *v = i + 1 < argc ? argv[i + 1] : NULL;
    if (!strcmp (a, "--route") && v) route = v, i++;
    else if (!strcmp (a, "--drive")) drive = TRUE;
    else if (!strcmp (a, "--speed") && v) d.speed_mps = g_ascii_strtod (v, NULL) / 3.6, i++;
    else if (!strcmp (a, "--zoom") && v) d.zoom = g_ascii_strtod (v, NULL), i++;
    else if (!strcmp (a, "--pitch") && v) d.pitch = g_ascii_strtod (v, NULL), i++;
    else if (!strcmp (a, "--seconds") && v) d.quit_after = atoi (v), i++;
    else if (!strcmp (a, "--fullscreen")) fullscreen = TRUE;
    else if (!strcmp (a, "--cache") && v) cache = v, i++;
    else if (a[0] != '-' && !style) style = a;
    else {
      g_printerr ("usage: %s STYLE_URL [--route FILE.geojson] [--drive] [--speed KMH] [--zoom Z] "
                  "[--pitch DEG] [--seconds N] [--fullscreen] [--cache FILE]\n", argv[0]);
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
    d.car_heading = heading_at (&d, 0);
    g_print ("route: %u points, %.0f m\n", d.n_route, d.route[d.n_route - 1].dist);
  } else {
    d.car_lat = 52.5009, d.car_lng = 13.3469; /* nowhere in particular: a Berlin junction */
    d.car_heading = 45;
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
  NaviCamera cam = { d.car_lat, d.car_lng, d.zoom, d.car_heading, d.pitch };
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
