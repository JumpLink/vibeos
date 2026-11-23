/* navi-window: the map and what moves on it. See navi-app.h.
 *
 * The car is drawn by the app, not the map: while the camera follows it, it is a widget fixed on the map's focus point
 * (half the width, three quarters down), and the map moves under it. So it cannot jump against the map, whatever
 * the GPS does: the camera's centre is the car's drawn position, which moves on every frame, ahead from the last fix
 * along the route at the car's speed, and never backwards. A fix that lands behind it only slows it down; one ahead
 * speeds it up until it is there. Its heading turns on a critically damped spring toward the route's direction
 * 20 m ahead, so the map turns smoothly into a bend and never overshoots it.
 *
 * Touching the map leaves the car where it is on the map (free); the re-centre button, or ten seconds of driving
 * untouched, flies back in 650 ms, landing on the car as it is in that frame so following takes over without a step. */
#include "navi-app.h"
#include "navi-alerts.h"
#include "navi-cameras.h"
#include "navi-draw.h"
#include "navi-poi.h"
#include "navi-sun.h"

#include <glib/gi18n.h>
#include <json-glib/json-glib.h>
#include <math.h>
#include <string.h>

#define DRIVE_PITCH      52.0
#define BROWSE_ZOOM      16.5
#define PLACE_ZOOM       16.5
#define RECENTER_MS      650
#define RECENTER_AFTER_S 10
#define CAR_DP           64
#define PIN_W            34
#define PIN_H            46
#define HEADING_SMOOTH_S 0.6
#define HEADING_AHEAD_M  20.0
#define ZOOM_SMOOTH_S    1.6
#define REROUTE_GAP_US   (6 * G_USEC_PER_SEC)

/* the route's colours: purple by day, cyan by night; the others lighter */
static const char *const route_fill[2] = { "#6C18D2", "#2CDDFF" };
static const char *const route_edge[2] = { "#48009C", "#0A8FB0" };
#define DONE_OPACITY "0.6" /* the road driven, under the route ahead */
static const char *const alt_fill[2] = { "#BBA3E8", "#A7B2BF" };
static const char *const alt_edge[2] = { "#8E72C8", "#5F6B79" };

static void update_route_layers (Navi *n);
static void ensure_tick (Navi *n);

/* ---- small things ---- */

static gboolean
heading_up (Navi *n)
{
  if (n->settings->north_up)
    return FALSE;
  return n->mode == NAVI_MODE_DRIVE || n->car_speed > 1.5 || n->car_heading != 0;
}

static double
target_pitch (Navi *n)
{
  if (n->settings->view == NAVI_VIEW_2D)
    return 0;
  if (n->settings->view == NAVI_VIEW_3D || n->mode == NAVI_MODE_DRIVE)
    return DRIVE_PITCH;
  return 0;
}

void
navi_apply_view (Navi *n)
{
  NaviCamera cam = NAVI_CAMERA_UNSET;
  cam.pitch = target_pitch (n);
  navi_map_ease_to (n->map, &cam, 400, NAVI_EASING_EASE_OUT_CUBIC);
}

/* the zoom while following: street level, out a little as the car gets faster */
static double
target_zoom (Navi *n)
{
  if (n->mode != NAVI_MODE_DRIVE)
    return BROWSE_ZOOM;
  if (!n->settings->auto_zoom)
    return 17.6;
  double kmh = n->car_speed * 3.6;
  double out = CLAMP ((kmh - 30) / 70, 0.0, 1.0);
  double z = 18.2 - 1.3 * out;
  /* nearer a turn, nearer the street */
  if (n->guidance) {
    const NaviProgress *p = navi_guidance_get_progress (n->guidance);
    if (p->on_route && p->to_next < 250 && kmh > 40)
      z += 0.5;
  }
  return z;
}

double
navi_distance_from_car (Navi *n, double lat, double lng)
{
  double clat, clng;
  gboolean approx;
  navi_where_am_i (n, &clat, &clng, &approx);
  return navi_geo_distance (clat, clng, lat, lng);
}

void
navi_where_am_i (Navi *n, double *lat, double *lng, gboolean *approximate)
{
  if (n->have_fix) {
    *lat = n->car_known ? n->car_lat : n->fix.lat;
    *lng = n->car_known ? n->car_lng : n->fix.lng;
    *approximate = n->fix.kind == NAVI_FIX_APPROXIMATE || n->fix.kind == NAVI_FIX_LAST_KNOWN;
    return;
  }
  NaviCamera cam;
  navi_map_get_camera (n->map, &cam);
  *lat = cam.latitude;
  *lng = cam.longitude;
  *approximate = TRUE;
}

static void
append_coord (GString *s, double lat, double lng)
{
  char a[G_ASCII_DTOSTR_BUF_SIZE], b[G_ASCII_DTOSTR_BUF_SIZE];
  g_string_append_printf (s, "[%s,%s]", g_ascii_formatd (a, sizeof a, "%.6f", lng),
                          g_ascii_formatd (b, sizeof b, "%.6f", lat));
}

static void
append_line (GString *s, const NaviPathPoint *p, guint n, guint step)
{
  g_string_append_c (s, '[');
  for (guint i = 0; i < n; i += step) {
    if (i)
      g_string_append_c (s, ',');
    append_coord (s, p[i].lat, p[i].lng);
  }
  if (n > 1 && (n - 1) % step) {
    g_string_append_c (s, ',');
    append_coord (s, p[n - 1].lat, p[n - 1].lng);
  }
  g_string_append_c (s, ']');
}

static NaviRoute *
current_route (Navi *n)
{
  if (n->guidance)
    return navi_guidance_get_route (n->guidance);
  if (n->routes && n->selected < n->routes->len)
    return n->routes->pdata[n->selected];
  return NULL;
}

/* ---- the frame clock: statistics ---- */

static void
count_shown (Navi *n, GdkFrameClock *clock)
{
  gint64 now = gdk_frame_clock_get_frame_counter (clock);
  for (gint64 i = MAX (n->timings_done + 1, gdk_frame_clock_get_history_start (clock)); i < now; i++) {
    GdkFrameTimings *t = gdk_frame_clock_get_timings (clock, i);
    if (!t || !gdk_frame_timings_get_complete (t))
      break;
    n->timings_done = i;
    gint64 shown = gdk_frame_timings_get_presentation_time (t), refresh = gdk_frame_timings_get_refresh_interval (t);
    if (refresh)
      n->refresh_ms = refresh / 1000.0;
    if (!shown)
      continue;
    if (n->last_shown_us) {
      double gap = (shown - n->last_shown_us) / 1000.0;
      n->shown++;
      n->gap_sum += gap;
      n->gap_max = MAX (n->gap_max, gap);
      if (n->refresh_ms > 0)
        n->vblanks[CLAMP ((int) lround (gap / n->refresh_ms), 1, 4) - 1]++;
      /* a late frame: what happened just before it */
      if (n->refresh_ms > 0 && gap > 2.5 * n->refresh_ms)
        g_print ("navi: late frame %.1f ms; the fix before it %.0f ms before (handled in %.1f ms)\n", gap,
                 (shown - n->fix_at) / 1000.0, n->fix_cost / 1000.0);
    }
    n->last_shown_us = shown;
  }
}

static void
on_frame_rendered (NaviMap *map, double render_ms, Navi *n)
{
  n->frames++;
  n->render_sum += render_ms;
  n->render_max = MAX (n->render_max, render_ms);
}

static void
print_drive_stats (Navi *n)
{
  if (!n->stats || !n->drive_seconds || !n->drive_shown)
    return;
  g_print ("navi: drive over %u s: %.1f fps, shown every %.2f ms (refresh %.2f ms), worst gap %.2f ms, mean jitter "
           "%.2f ms; gaps of 1/2/3/4+ refreshes: %u/%u/%u/%u\n",
           n->drive_seconds, (double) n->drive_frames / n->drive_seconds, n->drive_gap_sum / n->drive_shown,
           n->refresh_ms, n->drive_gap_max, n->drive_jitter_sum / n->drive_seconds, n->drive_vblanks[0],
           n->drive_vblanks[1], n->drive_vblanks[2], n->drive_vblanks[3]);
}

static gboolean
stats_second (gpointer data)
{
  Navi *n = data;
  double gap = n->shown ? n->gap_sum / n->shown : 0;
  static const char *const cams[] = { "following", "re-centring", "free", "fixed" };
  g_print ("navi: fps %3u render avg %5.2f max %6.2f ms shown %3u gap avg %5.2f max %6.2f ms refreshes "
           "1:%u 2:%u 3:%u 4+:%u zoom %.2f %s%s\n",
           n->frames, n->frames ? n->render_sum / n->frames : 0.0, n->render_max, n->shown, gap, n->gap_max,
           n->vblanks[0], n->vblanks[1], n->vblanks[2], n->vblanks[3], n->zoom, cams[n->cam],
           n->mode == NAVI_MODE_DRIVE ? ", driving" : "");
  /* the drive's figures: the seconds spent following, from the second one on */
  if (n->mode == NAVI_MODE_DRIVE && n->cam == NAVI_CAM_FOLLOW && n->steady && n->shown) {
    n->drive_seconds++;
    n->drive_frames += n->frames;
    n->drive_shown += n->shown;
    for (int i = 0; i < 4; i++)
      n->drive_vblanks[i] += n->vblanks[i];
    n->drive_gap_sum += n->gap_sum;
    n->drive_gap_max = MAX (n->drive_gap_max, n->gap_max);
    n->drive_jitter_sum += n->gap_max - gap;
  }
  n->steady = n->mode == NAVI_MODE_DRIVE && n->cam == NAVI_CAM_FOLLOW;
  n->frames = n->shown = 0;
  memset (n->vblanks, 0, sizeof n->vblanks);
  n->render_sum = n->render_max = n->gap_sum = n->gap_max = 0;
  return G_SOURCE_CONTINUE;
}

/* ---- the car between fixes ---- */

static void
advance_car (Navi *n, gint64 now, double dt)
{
  if (!n->have_fix)
    return;
  double age = CLAMP ((now - n->fix_time) / 1e6, 0.0, 1.5);
  double speed = MAX (n->fix.speed, 0.0);
  NaviRoute *route = n->guidance ? navi_guidance_get_route (n->guidance) : NULL;
  const NaviProgress *p = n->guidance ? navi_guidance_get_progress (n->guidance) : NULL;

  if (route && p->on_route) {
    /* along the route: toward where the car should be now, never back */
    double target = MIN (n->fix_s + speed * age, route->length);
    double gap = target - n->car_s;
    if (fabs (gap) > 300) {
      n->car_s = target;
    } else {
      double v = MAX (0.0, speed + gap * 1.2);
      n->car_s = MIN (n->car_s + v * dt, MAX (target, n->car_s));
    }
    guint hint = 0;
    navi_path_point_at (route->points, route->n_points, n->car_s, &n->car_lat, &n->car_lng, &hint);
    double want = navi_path_heading_at (route->points, route->n_points, n->car_s, HEADING_AHEAD_M);
    n->car_heading = fmod (navi_smooth_angle (n->car_heading, want, &n->heading_rate, HEADING_SMOOTH_S, dt) + 360,
                           360);
  } else {
    /* off any route: toward the fix moved on by its speed, easing over a third of a second */
    double tlat = n->fix.lat, tlng = n->fix.lng;
    if (speed > 0.5 && n->fix.heading >= 0)
      navi_geo_offset (n->fix.lat, n->fix.lng, n->fix.heading, speed * age, &tlat, &tlng);
    if (!n->car_known || navi_geo_distance (n->car_lat, n->car_lng, tlat, tlng) > 500) {
      n->car_lat = tlat;
      n->car_lng = tlng;
    } else {
      double k = 1 - exp (-dt / 0.3);
      n->car_lat += (tlat - n->car_lat) * k;
      n->car_lng += (tlng - n->car_lng) * k;
    }
    if (speed > 1.5 && n->fix.heading >= 0)
      n->car_heading = fmod (navi_smooth_angle (n->car_heading, n->fix.heading, &n->heading_rate,
                                                HEADING_SMOOTH_S, dt) + 360, 360);
  }
  n->car_speed = speed;
  n->car_known = TRUE;
}

/* The route ahead of the car is drawn in full colour from the car on; the whole route lies under it, a little
 * transparent: the road driven stays marked, subdued. Ten times a second, so the colours change under the car
 * (the S10e's drive: 58.5 frames a second with it, as with twice a second). */
static void
trim_ahead (Navi *n, gint64 now)
{
  if (!n->guidance || now - n->ahead_time < 100000 || fabs (n->car_s - n->ahead_s) < 0.5)
    return;
  NaviRoute *r = navi_guidance_get_route (n->guidance);
  n->ahead_time = now;
  n->ahead_s = n->car_s;
  guint i = 0;
  while (i + 1 < r->n_points && r->points[i + 1].s <= n->car_s)
    i++;
  guint count = r->n_points - i;
  double *pairs = g_new (double, 2 * (count + 1));
  pairs[0] = n->car_lat;
  pairs[1] = n->car_lng;
  for (guint k = 1; k < count; k++) {
    pairs[2 * k] = r->points[i + k].lat;
    pairs[2 * k + 1] = r->points[i + k].lng;
  }
  navi_map_set_geojson_line (n->map, "navi-route", pairs, MAX (count, 2u), NULL);
  g_free (pairs);
}

/* ---- the camera ---- */

static void
camera_frame (Navi *n, gint64 now, double dt)
{
  n->zoom = navi_smooth_value (n->zoom, target_zoom (n), &n->zoom_rate, ZOOM_SMOOTH_S, dt);
  double pitch = target_pitch (n);
  n->pitch += (pitch - n->pitch) * (1 - exp (-dt / 0.25));
  if (fabs (n->pitch - pitch) < 0.05)
    n->pitch = pitch;
  double bearing = heading_up (n) ? n->car_heading : 0;
  if (n->cam == NAVI_CAM_FOLLOW && n->car_known) {
    NaviCamera cam = { n->car_lat, n->car_lng, n->zoom, bearing, n->pitch };
    navi_map_jump_to (n->map, &cam);
  } else if (n->cam == NAVI_CAM_RECENTER) {
    /* a 650 ms ease-out from where the user left the camera to the car as it is in this frame: it lands exactly on
     * the moving car, and following takes over without a step */
    double k = MIN (1.0, (now - n->flight_t0) / (RECENTER_MS * 1000.0)), e = 1 - pow (1 - k, 3);
    const NaviCamera *f = &n->flight_from;
    NaviCamera cam = { f->latitude + (n->car_lat - f->latitude) * e, f->longitude + (n->car_lng - f->longitude) * e,
                       f->zoom + (n->zoom - f->zoom) * e, f->bearing + remainder (bearing - f->bearing, 360) * e,
                       f->pitch + (n->pitch - f->pitch) * e };
    navi_map_jump_to (n->map, &cam);
    if (k >= 1) {
      n->cam = NAVI_CAM_FOLLOW;
      navi_panels_set_cam (n->panels, n->cam);
    }
  }
}

static gboolean
frame_tick (GtkWidget *widget, GdkFrameClock *clock, gpointer data)
{
  Navi *n = data;
  gint64 now = gdk_frame_clock_get_frame_time (clock);
  double dt = n->last_frame_us ? CLAMP ((now - n->last_frame_us) / 1e6, 0.0, 0.25) : 0;
  n->last_frame_us = now;
  if (n->stats)
    count_shown (n, clock);
  advance_car (n, now, dt);
  trim_ahead (n, now);
  camera_frame (n, now, dt);
  gtk_widget_queue_allocate (n->map_overlay);
  gtk_widget_queue_draw (n->car);

  /* the clock keeps running while anything moves; a car standing still in browse lets it rest */
  gboolean moving = n->mode == NAVI_MODE_DRIVE || n->cam == NAVI_CAM_RECENTER || n->car_speed > 0.2 ||
                    fabs (n->zoom_rate) > 0.002 || fabs (n->heading_rate) > 0.05 ||
                    fabs (n->pitch - target_pitch (n)) > 0.05;
  if (moving)
    return G_SOURCE_CONTINUE;
  n->tick = 0;
  n->last_frame_us = 0;
  n->last_shown_us = 0;
  return G_SOURCE_REMOVE;
}

static void
ensure_tick (Navi *n)
{
  if (!n->tick)
    n->tick = gtk_widget_add_tick_callback (GTK_WIDGET (n->map), frame_tick, n, NULL);
}

static void
set_cam (Navi *n, NaviCam cam)
{
  n->cam = cam;
  if (cam != NAVI_CAM_FREE)
    g_clear_handle_id (&n->recenter_source, g_source_remove);
  navi_panels_set_cam (n->panels, cam);
  gtk_widget_queue_allocate (n->map_overlay);
}

void
navi_recenter (Navi *n)
{
  g_clear_handle_id (&n->recenter_source, g_source_remove);
  if (!n->car_known) {
    set_cam (n, NAVI_CAM_FOLLOW);
    return;
  }
  navi_map_cancel_transitions (n->map);
  navi_map_get_camera (n->map, &n->flight_from);
  if (isnan (n->flight_from.bearing))
    n->flight_from.bearing = 0;
  GdkFrameClock *clock = gtk_widget_get_frame_clock (GTK_WIDGET (n->map));
  n->flight_t0 = clock ? gdk_frame_clock_get_frame_time (clock) : g_get_monotonic_time ();
  n->last_shown_us = 0;
  set_cam (n, NAVI_CAM_RECENTER);
  ensure_tick (n);
}

static gboolean
recenter_later (gpointer data)
{
  Navi *n = data;
  n->recenter_source = 0;
  /* a drive goes back to following the car after a pan (FREE) or the overview (FIXED) */
  if (n->mode == NAVI_MODE_DRIVE && (n->cam == NAVI_CAM_FREE || n->cam == NAVI_CAM_FIXED))
    navi_recenter (n);
  return G_SOURCE_REMOVE;
}

static void
on_user_moved (NaviMap *map, Navi *n)
{
  set_cam (n, NAVI_CAM_FREE);
  g_clear_handle_id (&n->recenter_source, g_source_remove);
  if (n->mode == NAVI_MODE_DRIVE)
    n->recenter_source = g_timeout_add_seconds (RECENTER_AFTER_S, recenter_later, n);
}

/* the camera on a box (south-west, north-east), north up and flat, in the part of the map the panels leave: what
 * fitBounds does, with the focus point kept where it is */
static void
fit_bounds (Navi *n, double south, double west, double north, double east, guint duration)
{
  int w = gtk_widget_get_width (GTK_WIDGET (n->map)), h = gtk_widget_get_height (GTK_WIDGET (n->map));
  if (w <= 0 || h <= 0)
    return;
  double top = navi_panels_top_inset (n->panels) + 48, bottom = navi_panels_bottom_inset (n->panels) + 40;
  double side = 44;
  double avail_w = MAX (w - 2 * side, 60.0), avail_h = MAX (h - top - bottom, 60.0);
  /* Web Mercator: x and y from 0 to 1 over the world */
  double x0 = (west + 180) / 360, x1 = (east + 180) / 360;
  double y0 = 0.5 - log (tan (G_PI / 4 + north * G_PI / 360)) / (2 * G_PI);
  double y1 = 0.5 - log (tan (G_PI / 4 + south * G_PI / 360)) / (2 * G_PI);
  double dx = MAX (x1 - x0, 1e-9), dy = MAX (y1 - y0, 1e-9);
  double zoom = log2 (MIN (avail_w / (dx * 512), avail_h / (dy * 512)));
  zoom = CLAMP (zoom, 3, 17);
  double world = 512 * pow (2, zoom);
  double fx, fy;
  navi_map_get_focus_point (n->map, &fx, &fy);
  /* the box's middle goes to the middle of the free part; the camera's centre is the focus point */
  double want_x = side + avail_w / 2, want_y = top + avail_h / 2;
  double cx = (x0 + x1) / 2 + (fx - want_x) / world, cy = (y0 + y1) / 2 + (fy - want_y) / world;
  double lng = cx * 360 - 180;
  double lat = atan (sinh (G_PI * (1 - 2 * cy))) * 180 / G_PI;
  NaviCamera cam = { lat, lng, zoom, 0, 0 };
  if (duration)
    navi_map_ease_to (n->map, &cam, duration, NAVI_EASING_EASE_OUT_CUBIC);
  else
    navi_map_jump_to (n->map, &cam);
}

static void
fit_point (Navi *n, double lat, double lng, double zoom, guint duration)
{
  int w = gtk_widget_get_width (GTK_WIDGET (n->map)), h = gtk_widget_get_height (GTK_WIDGET (n->map));
  double top = navi_panels_top_inset (n->panels), bottom = navi_panels_bottom_inset (n->panels);
  double fx, fy;
  navi_map_get_focus_point (n->map, &fx, &fy);
  double world = 512 * pow (2, zoom);
  double x = (lng + 180) / 360, y = 0.5 - log (tan (G_PI / 4 + lat * G_PI / 360)) / (2 * G_PI);
  double want_y = top + (h - top - bottom) * 0.5;
  x += (fx - w / 2.0) / world;
  y += (fy - want_y) / world;
  NaviCamera cam = { atan (sinh (G_PI * (1 - 2 * y))) * 180 / G_PI, x * 360 - 180, zoom, 0, 0 };
  navi_map_ease_to (n->map, &cam, duration, NAVI_EASING_EASE_OUT_CUBIC);
}

void
navi_overview (Navi *n)
{
  NaviRoute *r = current_route (n);
  if (!r)
    return;
  set_cam (n, NAVI_CAM_FIXED);
  /* the rest of the route, from the car */
  double s = n->car_lat, no = n->car_lat, w = n->car_lng, e = n->car_lng;
  for (guint i = 0; i < r->n_points; i++) {
    if (r->points[i].s < n->car_s)
      continue;
    s = MIN (s, r->points[i].lat);
    no = MAX (no, r->points[i].lat);
    w = MIN (w, r->points[i].lng);
    e = MAX (e, r->points[i].lng);
  }
  fit_bounds (n, s, w, no, e, RECENTER_MS);
  g_clear_handle_id (&n->recenter_source, g_source_remove);
  n->recenter_source = g_timeout_add_seconds (RECENTER_AFTER_S, recenter_later, n);
}

/* ---- the overlays: the car, the pin, the bubbles ---- */

static void
draw_car (GtkDrawingArea *area, cairo_t *cr, int w, int h, gpointer data)
{
  Navi *n = data;
  double rotation = 0, pitch = n->pitch;
  NaviCamera cam;
  navi_map_get_camera (n->map, &cam);
  if (!isnan (cam.pitch))
    pitch = cam.pitch;
  if (n->cam != NAVI_CAM_FOLLOW || !heading_up (n))
    rotation = remainder (n->car_heading - (isnan (cam.bearing) ? 0 : cam.bearing), 360);
  navi_draw_car (cr, MIN (w, h), rotation, 0.55 + 0.45 * cos (pitch * G_PI / 180), n->night);
}

static void
draw_pin (GtkDrawingArea *area, cairo_t *cr, int w, int h, gpointer data)
{
  navi_draw_pin (cr, w, h);
}

static gboolean
place_child (GtkOverlay *overlay, GtkWidget *child, GdkRectangle *alloc, gpointer data)
{
  Navi *n = data;
  double x, y;
  if (child == n->car) {
    if (!n->car_known) {
      x = y = -1000;
    } else if (n->cam == NAVI_CAM_FOLLOW) {
      navi_map_get_focus_point (n->map, &x, &y);
    } else {
      navi_map_geo_to_screen (n->map, n->car_lat, n->car_lng, &x, &y);
    }
    if (isnan (x) || isnan (y))
      x = y = -1000;
    alloc->width = alloc->height = CAR_DP;
    alloc->x = (int) lround (x) - CAR_DP / 2;
    alloc->y = (int) lround (y) - CAR_DP / 2;
    return TRUE;
  }
  if (child == n->pin) {
    if (!n->dest) {
      x = y = -1000;
    } else {
      navi_map_geo_to_screen (n->map, n->dest->lat, n->dest->lng, &x, &y);
    }
    if (isnan (x) || isnan (y))
      x = y = -1000;
    alloc->width = PIN_W;
    alloc->height = PIN_H;
    alloc->x = (int) lround (x) - PIN_W / 2;
    alloc->y = (int) lround (y) - PIN_H;
    return TRUE;
  }
  for (guint i = 0; n->bubbles && i < n->bubbles->len; i++) {
    if (child != n->bubbles->pdata[i])
      continue;
    double *at = g_object_get_data (G_OBJECT (child), "navi-at");
    navi_map_geo_to_screen (n->map, at[0], at[1], &x, &y);
    int mw, nw, mh, nh;
    gtk_widget_measure (child, GTK_ORIENTATION_HORIZONTAL, -1, &mw, &nw, NULL, NULL);
    gtk_widget_measure (child, GTK_ORIENTATION_VERTICAL, nw, &mh, &nh, NULL, NULL);
    if (isnan (x) || isnan (y))
      x = y = -1000;
    alloc->width = nw;
    alloc->height = nh;
    /* its corner on the route, the bubble up and to the side away from the middle */
    int mid = gtk_widget_get_width (GTK_WIDGET (overlay)) / 2;
    alloc->x = x < mid ? (int) lround (x) - nw + 6 : (int) lround (x) - 6;
    alloc->y = (int) lround (y) - nh + 4;
    return TRUE;
  }
  return FALSE;
}

static void
on_camera_changed (NaviMap *map, Navi *n)
{
  gtk_widget_queue_allocate (n->map_overlay);
  if (n->cam != NAVI_CAM_FOLLOW || !heading_up (n))
    gtk_widget_queue_draw (n->car);
  NaviCamera cam;
  navi_map_get_camera (map, &cam);
  navi_panels_set_compass (n->panels, isnan (cam.bearing) ? 0 : cam.bearing);
}

/* ---- the route on the map ---- */

static void
add_layer (Navi *n, const char *json, const char *below)
{
  g_autoptr (GError) error = NULL;
  if (!navi_map_add_layer_json (n->map, json, below, &error))
    g_message ("map layer: %s", error->message);
}

/* a cairo surface as the map's image `id` (and gone after) */
static void
add_surface_image (Navi *n, const char *id, cairo_surface_t *surface, int scale)
{
  int w = cairo_image_surface_get_width (surface), h = cairo_image_surface_get_height (surface);
  int stride = cairo_image_surface_get_stride (surface);
  const guint8 *argb = cairo_image_surface_get_data (surface);
  /* cairo's native-endian ARGB words to the map's RGBA bytes, both premultiplied */
  g_autofree guint8 *rgba = g_malloc ((gsize) w * h * 4);
  for (int y = 0; y < h; y++) {
    const guint32 *row = (const guint32 *) (argb + (gsize) y * stride);
    for (int x = 0; x < w; x++) {
      guint32 v = row[x];
      guint8 *o = rgba + ((gsize) y * w + x) * 4;
      o[0] = (v >> 16) & 0xff;
      o[1] = (v >> 8) & 0xff;
      o[2] = v & 0xff;
      o[3] = v >> 24;
    }
  }
  navi_map_add_image (n->map, id, rgba, w, h, w * 4, scale);
  cairo_surface_destroy (surface);
}

/* the style's point-of-interest markers ("navi-poi-fuel", ...), drawn at the screen's scale */
static void
add_poi_images (Navi *n)
{
  int scale = gtk_widget_get_scale_factor (GTK_WIDGET (n->map));
  if (n->poi_scale == scale)
    return; /* the map keeps them across style loads */
  guint count;
  const NaviPoiKind *kinds = navi_poi_kinds (&count);
  for (guint i = 0; i < count; i++) {
    g_autofree char *file = g_strdup_printf ("%s.svg", kinds[i].icon);
    g_autofree char *path = g_build_filename (n->data_dir, "poi", file, NULL);
    cairo_surface_t *marker = navi_draw_poi_marker (path, kinds[i].color, kinds[i].prominent ? 30 : 24, scale);
    if (!marker)
      continue;
    int w = cairo_image_surface_get_width (marker), h = cairo_image_surface_get_height (marker);
    int stride = cairo_image_surface_get_stride (marker);
    const guint8 *argb = cairo_image_surface_get_data (marker);
    /* cairo's native-endian ARGB words to the map's RGBA bytes, both premultiplied */
    g_autofree guint8 *rgba = g_malloc ((gsize) w * h * 4);
    for (int y = 0; y < h; y++) {
      const guint32 *row = (const guint32 *) (argb + (gsize) y * stride);
      for (int x = 0; x < w; x++) {
        guint32 v = row[x];
        guint8 *o = rgba + ((gsize) y * w + x) * 4;
        o[0] = (v >> 16) & 0xff;
        o[1] = (v >> 8) & 0xff;
        o[2] = v & 0xff;
        o[3] = v >> 24;
      }
    }
    g_autofree char *id = g_strconcat ("navi-poi-", kinds[i].group, NULL);
    navi_map_add_image (n->map, id, rgba, w, h, w * 4, scale);
    cairo_surface_destroy (marker);
  }
  /* the overlay's: speed cameras and traffic signals */
  add_surface_image (n, "navi-camera", navi_draw_camera_marker (26, scale), scale);
  add_surface_image (n, "navi-signal", navi_draw_signal_marker (22, scale), scale);
  n->poi_scale = scale;
}

/* Navi's point overlay (navi.vibeos.top/tiles/navi-overlay, see navi-cameras.h): the signals while driving, the
 * cameras where the car's country lets them show and Settings have them on. The map keeps the layers until the
 * next style. */
void
navi_window_update_overlay (Navi *n)
{
  if (!n->map || !navi_map_is_style_loaded (n->map))
    return;
  gboolean signals = n->mode == NAVI_MODE_DRIVE || n->mode == NAVI_MODE_ARRIVED;
  gboolean cams = n->settings->cameras && navi_cameras_policy (navi_alerts_country (n->alerts)) == NAVI_CAMS_ON;
  if (signals != n->signals_shown) {
    if (signals)
      add_layer (n,
                 "{\"id\":\"navi-signals\",\"type\":\"symbol\",\"source\":\"navi-overlay\",\"source-layer\":"
                 "\"signals\",\"minzoom\":15,\"layout\":{\"icon-image\":\"navi-signal\",\"icon-allow-overlap\":true,"
                 "\"icon-ignore-placement\":true,\"icon-pitch-alignment\":\"viewport\",\"icon-size\":"
                 "[\"interpolate\",[\"linear\"],[\"zoom\"],15,0.7,18,1]}}", NULL);
    else
      navi_map_remove_layer (n->map, "navi-signals", NULL);
    n->signals_shown = signals;
  }
  if (cams != n->cams_shown) {
    if (cams)
      add_layer (n,
                 "{\"id\":\"navi-cameras\",\"type\":\"symbol\",\"source\":\"navi-overlay\",\"source-layer\":"
                 "\"cameras\",\"minzoom\":11,\"layout\":{\"icon-image\":\"navi-camera\",\"icon-allow-overlap\":true,"
                 "\"icon-ignore-placement\":true,\"icon-pitch-alignment\":\"viewport\",\"icon-size\":"
                 "[\"interpolate\",[\"linear\"],[\"zoom\"],11,0.6,15,1]}}", NULL);
    else
      navi_map_remove_layer (n->map, "navi-cameras", NULL);
    n->cams_shown = cams;
    /* the first time cameras show: where they are not allowed */
    if (cams)
      navi_panels_camera_notice (n->panels);
  }
}

static void
on_load_error (NaviMap *map, const char *message, Navi *n)
{
  g_message ("map: %s", message);
  n->map_failed = TRUE;
  navi_panels_set_note (n->panels, _("The map cannot be loaded. Check the connection."));
}

static void
on_style_loaded (NaviMap *map, Navi *n)
{
  n->map_failed = FALSE;
  g_auto (GStrv) symbols = navi_map_list_layers (map, "symbol");
  const char *below = symbols && symbols[0] ? symbols[0] : NULL;
  int night = n->style_night ? 1 : 0;
  navi_map_add_geojson_source (map, "navi-alts", NULL, NULL);
  navi_map_add_geojson_source (map, "navi-done", NULL, NULL);
  navi_map_add_geojson_source (map, "navi-route", NULL, NULL);
  navi_map_add_geojson_source (map, "navi-turn", NULL, NULL);
  navi_map_add_geojson_source (map, "navi-turn-head", NULL, NULL);
  navi_map_add_geojson_source (map, "navi-stop", NULL, NULL);
  const char *edge_w = "[\"interpolate\",[\"exponential\",1.6],[\"zoom\"],8,3,12,5,15,9,17,19,19,64]";
  const char *fill_w = "[\"interpolate\",[\"exponential\",1.6],[\"zoom\"],8,1.8,12,3.4,15,6.5,17,14.5,19,52]";
  g_autofree char *alt_e = g_strdup_printf (
    "{\"id\":\"navi-alts-edge\",\"type\":\"line\",\"source\":\"navi-alts\",\"layout\":{\"line-join\":\"round\","
    "\"line-cap\":\"round\"},\"paint\":{\"line-color\":\"%s\",\"line-width\":%s}}", alt_edge[night], edge_w);
  g_autofree char *alt_f = g_strdup_printf (
    "{\"id\":\"navi-alts-fill\",\"type\":\"line\",\"source\":\"navi-alts\",\"layout\":{\"line-join\":\"round\","
    "\"line-cap\":\"round\"},\"paint\":{\"line-color\":\"%s\",\"line-width\":%s}}", alt_fill[night], fill_w);
  g_autofree char *route_e = g_strdup_printf (
    "{\"id\":\"navi-route-edge\",\"type\":\"line\",\"source\":\"navi-route\",\"layout\":{\"line-join\":\"round\","
    "\"line-cap\":\"round\"},\"paint\":{\"line-color\":\"%s\",\"line-width\":%s}}", route_edge[night], edge_w);
  g_autofree char *route_f = g_strdup_printf (
    "{\"id\":\"navi-route-fill\",\"type\":\"line\",\"source\":\"navi-route\",\"layout\":{\"line-join\":\"round\","
    "\"line-cap\":\"round\"},\"paint\":{\"line-color\":\"%s\",\"line-width\":%s}}", route_fill[night], fill_w);
  /* the whole route while driving, under the route ahead: what is driven shows through, subdued */
  g_autofree char *done_e = g_strdup_printf (
    "{\"id\":\"navi-done-edge\",\"type\":\"line\",\"source\":\"navi-done\",\"layout\":{\"line-join\":\"round\","
    "\"line-cap\":\"round\"},\"paint\":{\"line-color\":\"%s\",\"line-width\":%s,\"line-opacity\":%s}}",
    route_edge[night], edge_w, DONE_OPACITY);
  g_autofree char *done_f = g_strdup_printf (
    "{\"id\":\"navi-done-fill\",\"type\":\"line\",\"source\":\"navi-done\",\"layout\":{\"line-join\":\"round\","
    "\"line-cap\":\"round\"},\"paint\":{\"line-color\":\"%s\",\"line-width\":%s,\"line-opacity\":%s}}",
    route_fill[night], fill_w, DONE_OPACITY);
  add_layer (n, alt_e, below);
  add_layer (n, alt_f, below);
  add_layer (n, done_e, below);
  add_layer (n, done_f, below);
  add_layer (n, route_e, below);
  add_layer (n, route_f, below);
  /* the arrow at the next turn: a white shaft and head on the route, edged in the route's dark colour */
  g_autofree char *turn_e = g_strdup_printf (
    "{\"id\":\"navi-turn-edge\",\"type\":\"line\",\"source\":\"navi-turn\",\"minzoom\":15,\"layout\":"
    "{\"line-join\":\"round\",\"line-cap\":\"butt\"},\"paint\":{\"line-color\":\"%s\",\"line-width\":"
    "[\"interpolate\",[\"exponential\",1.6],[\"zoom\"],15,4,17,9,19,30]}}", route_edge[night]);
  add_layer (n, turn_e, below);
  add_layer (n,
             "{\"id\":\"navi-turn-fill\",\"type\":\"line\",\"source\":\"navi-turn\",\"minzoom\":15,\"layout\":"
             "{\"line-join\":\"round\",\"line-cap\":\"butt\"},\"paint\":{\"line-color\":\"#FFFFFF\",\"line-width\":"
             "[\"interpolate\",[\"exponential\",1.6],[\"zoom\"],15,2.4,17,6,19,22]}}", below);
  add_layer (n,
             "{\"id\":\"navi-turn-head\",\"type\":\"fill\",\"source\":\"navi-turn-head\",\"minzoom\":15,"
             "\"paint\":{\"fill-color\":\"#FFFFFF\"}}", below);
  /* a stop on the way: a white-ringed dot in the route's colour */
  g_autofree char *stop = g_strdup_printf (
    "{\"id\":\"navi-stop\",\"type\":\"circle\",\"source\":\"navi-stop\",\"paint\":{\"circle-color\":\"%s\","
    "\"circle-radius\":9,\"circle-stroke-color\":\"#FFFFFF\",\"circle-stroke-width\":3,"
    "\"circle-pitch-alignment\":\"viewport\"}}", route_edge[night]);
  add_layer (n, stop, NULL);
  add_poi_images (n);
  g_clear_pointer (&n->poi_layers, g_strfreev);
  update_route_layers (n);
  n->signals_shown = n->cams_shown = FALSE; /* a new style has none of them */
  navi_window_update_overlay (n);
}

/* the arrow at manoeuvre `m`: 25 m of the route before it and 14 m after, a head at its end */
static void
update_turn_arrow (Navi *n, NaviRoute *r, guint m)
{
  if (!r || m == 0 || m >= r->n_maneuvers || r->maneuvers[m].type == NAVI_TURN_DESTINATION ||
      r->maneuvers[m].type == NAVI_TURN_DESTINATION_LEFT || r->maneuvers[m].type == NAVI_TURN_DESTINATION_RIGHT) {
    navi_map_set_geojson (n->map, "navi-turn", "{\"type\":\"FeatureCollection\",\"features\":[]}", NULL);
    navi_map_set_geojson (n->map, "navi-turn-head", "{\"type\":\"FeatureCollection\",\"features\":[]}", NULL);
    return;
  }
  double s0 = MAX (0.0, r->maneuvers[m].s - 25), s1 = MIN (r->length, r->maneuvers[m].s + 14);
  GArray *pairs = g_array_new (FALSE, FALSE, sizeof (double));
  guint hint = 0;
  for (double s = s0; s < s1; s += 2) {
    double p[2];
    navi_path_point_at (r->points, r->n_points, s, &p[0], &p[1], &hint);
    g_array_append_vals (pairs, p, 2);
  }
  double end[2];
  navi_path_point_at (r->points, r->n_points, s1, &end[0], &end[1], &hint);
  g_array_append_vals (pairs, end, 2);
  navi_map_set_geojson_line (n->map, "navi-turn", (double *) pairs->data, pairs->len / 2, NULL);
  g_array_free (pairs, TRUE);
  /* the head: 9 m long, 12 m wide, along the route's last 4 m */
  double b = navi_path_heading_at (r->points, r->n_points, s1 - 4, 4);
  double tip_lat, tip_lng, l_lat, l_lng, r_lat, r_lng;
  navi_geo_offset (end[0], end[1], b, 8, &tip_lat, &tip_lng);
  navi_geo_offset (end[0], end[1], b - 90, 6, &l_lat, &l_lng);
  navi_geo_offset (end[0], end[1], b + 90, 6, &r_lat, &r_lng);
  g_autoptr (GString) s = g_string_new ("{\"type\":\"Polygon\",\"coordinates\":[[");
  append_coord (s, l_lat, l_lng);
  g_string_append_c (s, ',');
  append_coord (s, tip_lat, tip_lng);
  g_string_append_c (s, ',');
  append_coord (s, r_lat, r_lng);
  g_string_append_c (s, ',');
  append_coord (s, l_lat, l_lng);
  g_string_append (s, "]]}");
  navi_map_set_geojson (n->map, "navi-turn-head", s->str, NULL);
}

static void
update_route_layers (Navi *n)
{
  if (!navi_map_is_style_loaded (n->map))
    return;
  static const char *const empty = "{\"type\":\"FeatureCollection\",\"features\":[]}";
  NaviRoute *r = current_route (n);
  /* the others, in the preview */
  if (n->mode == NAVI_MODE_ROUTES && n->routes && n->routes->len > 1) {
    g_autoptr (GString) s = g_string_new ("{\"type\":\"MultiLineString\",\"coordinates\":[");
    gboolean first = TRUE;
    for (guint i = 0; i < n->routes->len; i++) {
      if (i == n->selected)
        continue;
      NaviRoute *a = n->routes->pdata[i];
      if (!first)
        g_string_append_c (s, ',');
      first = FALSE;
      append_line (s, a->points, a->n_points, 1);
    }
    g_string_append (s, "]}");
    navi_map_set_geojson (n->map, "navi-alts", s->str, NULL);
  } else {
    navi_map_set_geojson (n->map, "navi-alts", empty, NULL);
  }
  if (r && (n->mode == NAVI_MODE_ROUTES || n->mode == NAVI_MODE_DRIVE)) {
    g_autoptr (GString) s = g_string_new ("{\"type\":\"LineString\",\"coordinates\":");
    append_line (s, r->points, r->n_points, 1);
    g_string_append (s, "}");
    navi_map_set_geojson (n->map, "navi-route", s->str, NULL);
    /* driving: the whole route stays drawn under the route ahead, which trim_ahead () starts at the car */
    navi_map_set_geojson (n->map, "navi-done", n->mode == NAVI_MODE_DRIVE ? s->str : empty, NULL);
    n->ahead_s = -1;
    n->ahead_time = 0;
  } else {
    navi_map_set_geojson (n->map, "navi-route", empty, NULL);
    navi_map_set_geojson (n->map, "navi-done", empty, NULL);
  }
  if (n->stop && n->mode == NAVI_MODE_DRIVE) {
    g_autoptr (GString) pt = g_string_new ("{\"type\":\"Point\",\"coordinates\":");
    append_coord (pt, n->stop->lat, n->stop->lng);
    g_string_append_c (pt, '}');
    navi_map_set_geojson (n->map, "navi-stop", pt->str, NULL);
  } else {
    navi_map_set_geojson (n->map, "navi-stop", empty, NULL);
  }
  if (n->mode == NAVI_MODE_DRIVE && n->guidance)
    update_turn_arrow (n, r, navi_guidance_get_progress (n->guidance)->next);
  else
    update_turn_arrow (n, NULL, 0);
}

/* ---- the time bubbles of the preview ---- */

static void
clear_bubbles (Navi *n)
{
  for (guint i = 0; n->bubbles && i < n->bubbles->len; i++)
    gtk_overlay_remove_overlay (GTK_OVERLAY (n->map_overlay), n->bubbles->pdata[i]);
  if (n->bubbles)
    g_ptr_array_set_size (n->bubbles, 0);
}

static void
bubble_clicked (GtkWidget *bubble, Navi *n)
{
  navi_select_route (n, GPOINTER_TO_UINT (g_object_get_data (G_OBJECT (bubble), "navi-index")));
}

/* where a route's bubble goes: its point farthest from the other routes (its own stretch), else its middle */
static void
bubble_point (Navi *n, guint i, double *lat, double *lng)
{
  NaviRoute *r = n->routes->pdata[i];
  guint hint = 0;
  navi_path_point_at (r->points, r->n_points, r->length / 2, lat, lng, &hint);
  if (n->routes->len < 2)
    return;
  double best = -1;
  guint step = MAX (1u, r->n_points / 60);
  for (guint k = r->n_points / 10; k < r->n_points * 9 / 10; k += step) {
    double nearest = G_MAXDOUBLE;
    for (guint j = 0; j < n->routes->len; j++) {
      if (j == i)
        continue;
      NaviRoute *o = n->routes->pdata[j];
      guint ostep = MAX (1u, o->n_points / 300);
      for (guint q = 0; q < o->n_points; q += ostep)
        nearest = MIN (nearest, navi_geo_distance (r->points[k].lat, r->points[k].lng, o->points[q].lat,
                                                   o->points[q].lng));
    }
    if (nearest > best) {
      best = nearest;
      *lat = r->points[k].lat;
      *lng = r->points[k].lng;
    }
  }
}

static void
update_bubbles (Navi *n)
{
  clear_bubbles (n);
  if (n->mode != NAVI_MODE_ROUTES || !n->routes)
    return;
  if (!n->bubbles)
    n->bubbles = g_ptr_array_new ();
  for (guint i = 0; i < n->routes->len; i++) {
    NaviRoute *r = n->routes->pdata[i];
    GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
    g_autofree char *time = navi_format_duration (r->time);
    GtkWidget *label = gtk_label_new (time);
    gtk_widget_add_css_class (label, "navi-bubble-time");
    gtk_box_append (GTK_BOX (box), label);
    if (i == 0 && n->routes->len > 1) {
      GtkWidget *best = gtk_label_new (_("Best"));
      gtk_widget_add_css_class (best, "navi-bubble-note");
      gtk_label_set_xalign (GTK_LABEL (best), 0);
      gtk_box_append (GTK_BOX (box), best);
    }
    GtkWidget *bubble = vibeos_pressable_new (box);
    gtk_widget_add_css_class (bubble, "navi-bubble");
    if (i == n->selected)
      gtk_widget_add_css_class (bubble, "selected");
    double *at = g_new (double, 2);
    bubble_point (n, i, &at[0], &at[1]);
    g_object_set_data_full (G_OBJECT (bubble), "navi-at", at, g_free);
    g_object_set_data (G_OBJECT (bubble), "navi-index", GUINT_TO_POINTER (i));
    g_signal_connect (bubble, "clicked", G_CALLBACK (bubble_clicked), n);
    gtk_overlay_add_overlay (GTK_OVERLAY (n->map_overlay), bubble);
    g_ptr_array_add (n->bubbles, bubble);
  }
}

/* ---- modes ---- */

static void
fit_routes (Navi *n, guint duration)
{
  if (!n->routes || !n->routes->len)
    return;
  double s = 90, w = 180, no = -90, e = -180;
  for (guint i = 0; i < n->routes->len; i++) {
    NaviRoute *r = n->routes->pdata[i];
    s = MIN (s, r->min_lat);
    w = MIN (w, r->min_lng);
    no = MAX (no, r->max_lat);
    e = MAX (e, r->max_lng);
  }
  set_cam (n, NAVI_CAM_FIXED);
  fit_bounds (n, s, w, no, e, duration);
}

void
navi_window_set_mode (Navi *n, NaviMode mode)
{
  n->mode = mode;
  /* the screen stays on while a route is driven, as long as the window is shown */
  gboolean awake = mode == NAVI_MODE_DRIVE || mode == NAVI_MODE_ARRIVED;
  if (awake && !n->keep_awake)
    n->keep_awake = gtk_application_inhibit (n->app, GTK_WINDOW (n->win), GTK_APPLICATION_INHIBIT_IDLE,
                                             _("Navigating"));
  else if (!awake && n->keep_awake) {
    gtk_application_uninhibit (n->app, n->keep_awake);
    n->keep_awake = 0;
  }
  gtk_widget_set_visible (n->pin, n->dest && (mode == NAVI_MODE_PLACE || mode == NAVI_MODE_ROUTES ||
                                              mode == NAVI_MODE_DRIVE || mode == NAVI_MODE_ARRIVED));
  navi_panels_set_mode (n->panels, mode);
  navi_window_update_overlay (n);
  if (n->back)
    gtk_widget_set_visible (n->back, mode != NAVI_MODE_BROWSE);
  update_route_layers (n);
  update_bubbles (n);
  ensure_tick (n);
}

void
navi_back_to_browse (Navi *n)
{
  g_cancellable_cancel (n->route_cancel);
  g_clear_object (&n->route_cancel);
  g_clear_pointer (&n->routes, g_ptr_array_unref);
  g_clear_pointer (&n->dest, navi_place_free);
  g_clear_pointer (&n->stop, navi_place_free);
  navi_window_set_mode (n, NAVI_MODE_BROWSE);
  navi_recenter (n);
  navi_panels_rest_voice (n->panels);
}

void
navi_show_place (Navi *n, const NaviPlace *place)
{
  g_clear_pointer (&n->routes, g_ptr_array_unref);
  if (n->dest != place) {
    g_clear_pointer (&n->dest, navi_place_free);
    n->dest = navi_place_copy (place);
  }
  set_cam (n, NAVI_CAM_FIXED);
  navi_panels_show_place (n->panels, place);
  navi_window_set_mode (n, NAVI_MODE_PLACE);
  /* once the card's height is known */
  fit_point (n, place->lat, place->lng, PLACE_ZOOM, RECENTER_MS);
}

/* what the voice says first on the selected route, made into sound while the routes are looked at */
static void
prepare_route_speech (Navi *n)
{
  NaviRoute *r = current_route (n);
  if (!r)
    return;
  g_autoptr (NaviGuidance) g = navi_guidance_new (r);
  g_auto (GStrv) upcoming = navi_guidance_upcoming_prompts (g, -1);
  navi_panels_prepare_speech (n->panels, upcoming);
}

static void
routes_ready (GObject *src, GAsyncResult *res, gpointer data)
{
  Navi *n = data;
  g_autoptr (GError) error = NULL;
  GPtrArray *routes = navi_route_request_finish (res, &error);
  if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    return;
  g_clear_object (&n->route_cancel);
  if (!routes) {
    g_message ("route: %s", error->message);
    const char *why;
    if (g_error_matches (error, NAVI_ROUTE_ERROR, NAVI_ROUTE_ERROR_NO_ROAD))
      why = _("Navi cannot reach a road there right now. Outside Germany, routes may be unavailable for a while.");
    else if (g_error_matches (error, NAVI_ROUTE_ERROR, NAVI_ROUTE_ERROR_NO_ROUTE))
      why = _("No route found there. Choose a place on a road.");
    else
      why = _("The route server cannot be reached. Check the connection.");
    navi_panels_set_routes (n->panels, NULL, 0, why);
    return;
  }
  g_clear_pointer (&n->routes, g_ptr_array_unref);
  n->routes = routes;
  n->selected = 0;
  navi_window_set_mode (n, NAVI_MODE_ROUTES);
  navi_panels_set_routes (n->panels, routes, 0, NULL);
  navi_panels_warm_voice (n->panels);
  prepare_route_speech (n);
  fit_routes (n, RECENTER_MS);
  if (g_object_get_data (G_OBJECT (n->win), "navi-go")) {
    g_object_set_data (G_OBJECT (n->win), "navi-go", NULL);
    navi_start_drive (n);
  } else if (g_object_get_data (G_OBJECT (n->win), "navi-free") && n->simulate) {
    /* a test's drive without a route (--free): the simulated car drives the route's line, Navi stays browsing */
    g_object_set_data (G_OBJECT (n->win), "navi-free", NULL);
    navi_position_simulate (n->position, current_route (n), n->sim_speed, 1000);
    navi_back_to_browse (n);
  }
}

static void
request_routes (Navi *n, guint alternates, GAsyncReadyCallback done)
{
  g_cancellable_cancel (n->route_cancel);
  g_clear_object (&n->route_cancel);
  n->route_cancel = g_cancellable_new ();
  NaviRouteRequest req = { 0 };
  gboolean approx;
  navi_where_am_i (n, &req.from_lat, &req.from_lng, &approx);
  req.from_heading = n->car_speed > 2 ? n->car_heading : -1;
  req.to_lat = n->dest->lat;
  req.to_lng = n->dest->lng;
  if (n->stop) {
    req.have_via = TRUE;
    req.via_lat = n->stop->lat;
    req.via_lng = n->stop->lng;
  }
  req.alternates = alternates;
  navi_route_request_async (&req, n->route_cancel, done, n);
}

void
navi_preview_routes (Navi *n, const NaviPlace *place)
{
  if (n->dest != place) {
    g_clear_pointer (&n->dest, navi_place_free);
    n->dest = navi_place_copy (place);
  }
  navi_settings_add_recent (n->settings, place);
  g_clear_pointer (&n->routes, g_ptr_array_unref);
  navi_window_set_mode (n, NAVI_MODE_ROUTES);
  navi_panels_set_routes (n->panels, NULL, 0, NULL);
  request_routes (n, 2, routes_ready);
}

void
navi_select_route (Navi *n, guint index)
{
  if (!n->routes || index >= n->routes->len)
    return;
  n->selected = index;
  prepare_route_speech (n);
  navi_panels_set_routes (n->panels, n->routes, index, NULL);
  update_route_layers (n);
  update_bubbles (n);
}

/* ---- driving ---- */

static void
apply_progress (Navi *n, const NaviProgress *p)
{
  NaviRoute *r = navi_guidance_get_route (n->guidance);
  static guint last_next = G_MAXUINT;
  if (p->next != last_next) {
    last_next = p->next;
    update_turn_arrow (n, r, p->next);
  }
  /* what comes next is made into sound now, so that saying it is only playing it (each prompt is made once) */
  g_auto (GStrv) upcoming = navi_guidance_upcoming_prompts (n->guidance, n->have_fix ? n->fix.speed : -1);
  navi_panels_prepare_speech (n->panels, upcoming);
  navi_panels_update_drive (n->panels, p, r);
}

static void navi_reroute_now (Navi *n);

static void
reroute_ready (GObject *src, GAsyncResult *res, gpointer data)
{
  Navi *n = data;
  g_autoptr (GError) error = NULL;
  g_autoptr (GPtrArray) routes = navi_route_request_finish (res, &error);
  if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    return;
  g_clear_object (&n->route_cancel);
  n->rerouting = 0;
  navi_panels_set_rerouting (n->panels, FALSE);
  if (!routes || n->mode != NAVI_MODE_DRIVE) {
    if (error)
      g_message ("reroute: %s", error->message);
    /* said on the banner; the next fix off the route asks again (REROUTE_GAP_US on) */
    n->reroute_failed = n->mode == NAVI_MODE_DRIVE;
    return;
  }
  n->reroute_failed = FALSE;
  NaviRoute *r = routes->pdata[0];
  g_clear_pointer (&n->guidance, navi_guidance_free);
  n->guidance = navi_guidance_new (r);
  navi_alerts_set_route (n->alerts, r);
  n->car_s = n->fix_s = 0;
  if (n->have_fix) {
    const NaviProgress *p = navi_guidance_update (n->guidance, n->fix.lat, n->fix.lng, n->fix.accuracy, n->fix.speed,
                                                  n->fix.heading, n->fix_time);
    n->car_s = n->fix_s = p->on_route ? p->s : 0;
  }
  /* the voice says the new route's first instruction, as it says the drive's */
  char *words;
  while ((words = navi_guidance_take_prompt (n->guidance, n->car_speed))) {
    navi_panels_say (n->panels, words, TRUE);
    g_free (words);
  }
  if (n->simulate)
    navi_position_simulate (n->position, r, n->sim_speed, 1000);
  update_route_layers (n);
  apply_progress (n, navi_guidance_get_progress (n->guidance));
}

static void
reroute (Navi *n)
{
  gint64 now = g_get_monotonic_time ();
  if (n->rerouting || now - n->last_reroute < REROUTE_GAP_US)
    return;
  navi_reroute_now (n);
}

/* a new route from the car now (a stop added, or off the route) */
static void
navi_reroute_now (Navi *n)
{
  gint64 now = g_get_monotonic_time ();
  if (n->rerouting)
    g_cancellable_cancel (n->route_cancel);
  n->rerouting = 1;
  n->last_reroute = now;
  navi_panels_set_rerouting (n->panels, TRUE);
  request_routes (n, 0, reroute_ready);
}

void
navi_add_stop (Navi *n, const NaviPlace *place)
{
  if (n->mode != NAVI_MODE_DRIVE || !n->dest)
    return;
  g_clear_pointer (&n->stop, navi_place_free);
  n->stop = navi_place_copy (place);
  navi_settings_add_recent (n->settings, place);
  update_route_layers (n);
  navi_reroute_now (n);
}

/* the stop passed: from now on a new route goes straight to the destination */
static void
check_stop (Navi *n, NaviRoute *r)
{
  if (!n->stop || !r || r->stop_s < 0 || n->car_s < r->stop_s - 30)
    return;
  g_clear_pointer (&n->stop, navi_place_free);
  navi_panels_say (n->panels, _("You have reached your stop."), TRUE);
  update_route_layers (n);
}

void
navi_start_drive (Navi *n)
{
  NaviRoute *r = current_route (n);
  if (!r)
    return;
  g_clear_pointer (&n->guidance, navi_guidance_free);
  n->guidance = navi_guidance_new (r);
  navi_alerts_set_route (n->alerts, r);
  n->car_s = n->fix_s = 0;
  n->reroute_failed = FALSE;
  n->drive_seconds = n->drive_frames = n->drive_shown = 0;
  memset (n->drive_vblanks, 0, sizeof n->drive_vblanks);
  n->drive_gap_sum = n->drive_gap_max = n->drive_jitter_sum = 0;
  clear_bubbles (n);
  navi_window_set_mode (n, NAVI_MODE_DRIVE);
  if (n->simulate) {
    NaviRoute *drive = r;
    if (n->sim_detour && n->routes && n->routes->len > 1)
      drive = n->routes->pdata[n->selected == 0 ? 1 : 0];
    navi_position_simulate (n->position, drive, n->sim_speed, 1000);
  } else if (n->have_fix) {
    navi_guidance_update (n->guidance, n->fix.lat, n->fix.lng, n->fix.accuracy, n->fix.speed, n->fix.heading,
                          n->fix_time);
  }
  char *words;
  while ((words = navi_guidance_take_prompt (n->guidance, n->car_speed))) {
    navi_panels_say (n->panels, words, TRUE);
    g_free (words);
  }
  apply_progress (n, navi_guidance_get_progress (n->guidance));
  navi_recenter (n);
}

void
navi_stop_drive (Navi *n)
{
  print_drive_stats (n);
  g_clear_pointer (&n->guidance, navi_guidance_free);
  navi_alerts_set_route (n->alerts, NULL);
  if (n->simulate)
    navi_position_simulate (n->position, NULL, 1, 0);
  n->rerouting = 0;
  navi_back_to_browse (n);
}

static gboolean
arrived_done (gpointer data)
{
  Navi *n = data;
  if (n->mode == NAVI_MODE_ARRIVED)
    navi_stop_drive (n);
  return G_SOURCE_REMOVE;
}

static void
arrive (Navi *n)
{
  print_drive_stats (n);
  if (n->simulate)
    navi_position_simulate (n->position, NULL, 1, 0);
  navi_window_set_mode (n, NAVI_MODE_ARRIVED);
  g_timeout_add_seconds (20, arrived_done, n);
}

/* ---- the position ---- */

static void
on_fix (NaviPosition *position, NaviFix *fix, Navi *n)
{
  gboolean first = !n->have_fix;
  gint64 t0 = g_get_monotonic_time ();
  n->fix = *fix;
  n->have_fix = TRUE;
  n->fix_time = g_get_monotonic_time ();
  if (first || !n->car_known) {
    n->car_lat = fix->lat;
    n->car_lng = fix->lng;
    if (fix->heading >= 0)
      n->car_heading = fix->heading;
    n->car_known = TRUE;
  }
  gboolean approximate = fix->kind == NAVI_FIX_APPROXIMATE || fix->kind == NAVI_FIX_LAST_KNOWN;
  navi_panels_set_note (n->panels, n->map_failed ? _("The map cannot be loaded. Check the connection.")
                                   : approximate ? _("No GPS – showing your approximate location")
                                                 : NULL);
  navi_panels_set_speed (n->panels, fix->kind == NAVI_FIX_LAST_KNOWN ? -1 : fix->speed);

  if (n->guidance && n->mode == NAVI_MODE_DRIVE) {
    const NaviProgress *p = navi_guidance_update (n->guidance, fix->lat, fix->lng, fix->accuracy, fix->speed,
                                                  fix->heading, n->fix_time);
    if (p->on_route)
      n->fix_s = p->s;
    char *words;
    while ((words = navi_guidance_take_prompt (n->guidance, MAX (fix->speed, 0.0)))) {
      navi_panels_say (n->panels, words, FALSE);
      g_free (words);
    }
    apply_progress (n, p);
    navi_alerts_fix (n->alerts, fix, p);
    check_stop (n, navi_guidance_get_route (n->guidance));
    if (p->arrived)
      arrive (n);
    else if (p->off_route)
      reroute (n);
  }
  if (!n->guidance || n->mode != NAVI_MODE_DRIVE)
    navi_alerts_fix (n->alerts, fix, NULL); /* the limit without a route */
  if (!n->guidance || !navi_guidance_get_progress (n->guidance)->on_route) {
    /* the street the car is on, off a route: none known */
    if (n->mode != NAVI_MODE_DRIVE)
      navi_panels_set_street (n->panels, NULL);
  }
  /* the map's starting view next time, to a kilometre */
  if (fix->kind != NAVI_FIX_LAST_KNOWN && fix->kind != NAVI_FIX_SIMULATED) {
    n->settings->have_last = TRUE;
    n->settings->last_lat = round (fix->lat * 100) / 100;
    n->settings->last_lng = round (fix->lng * 100) / 100;
    navi_settings_save (n->settings);
  }
  if (first) {
    navi_update_night (n);
    if (n->mode == NAVI_MODE_BROWSE && n->cam != NAVI_CAM_FREE)
      navi_recenter (n);
  }
  ensure_tick (n);
  n->fix_at = g_get_monotonic_time ();
  n->fix_cost = n->fix_at - t0;
}

static void
on_denied (NaviPosition *position, Navi *n)
{
  navi_panels_set_note (n->panels, _("No GPS – showing your approximate location"));
}

/* ---- day and night ---- */

void
navi_update_night (Navi *n)
{
  gboolean night;
  VibeosTheme *theme = vibeos_theme_get_default ();
  const char *force = g_getenv ("NAVI_NIGHT");
  if (force) {
    night = !strcmp (force, "1");
  } else if (n->settings->dark == NAVI_DARK_ALWAYS) {
    night = TRUE;
  } else if (n->settings->dark == NAVI_DARK_NEVER) {
    night = FALSE;
  } else if (n->settings->dark == NAVI_DARK_SYSTEM) {
    vibeos_theme_set_scheme (theme, VIBEOS_SCHEME_SYSTEM);
    night = vibeos_theme_get_dark (theme);
  } else {
    double lat, lng;
    gboolean approx;
    navi_where_am_i (n, &lat, &lng, &approx);
    night = navi_sun_is_night (lat, lng, g_get_real_time () / G_USEC_PER_SEC);
  }
  if (n->settings->dark != NAVI_DARK_SYSTEM || force)
    vibeos_theme_set_scheme (theme, night ? VIBEOS_SCHEME_DARK : VIBEOS_SCHEME_LIGHT);
  n->night = night;
  if (night)
    gtk_widget_add_css_class (n->win, "night");
  else
    gtk_widget_remove_css_class (n->win, "night");
  if (n->style_night != night || !g_object_get_data (G_OBJECT (n->map), "navi-style-set")) {
    n->style_night = night;
    g_object_set_data (G_OBJECT (n->map), "navi-style-set", GINT_TO_POINTER (1));
    const char *url = g_getenv ("NAVI_STYLE");
    g_autofree char *file = g_build_filename (n->data_dir, night ? "style-night.json" : "style-day.json", NULL);
    g_autofree char *uri = url ? NULL : g_filename_to_uri (file, NULL, NULL);
    navi_map_set_style_url (n->map, url ? url : uri);
  }
  gtk_widget_queue_draw (n->car);
}

static gboolean
night_check (gpointer data)
{
  navi_update_night (data);
  return G_SOURCE_CONTINUE;
}

static void
on_theme_changed (VibeosTheme *theme, Navi *n)
{
  if (n->settings->dark == NAVI_DARK_SYSTEM && vibeos_theme_get_dark (theme) != n->night)
    navi_update_night (n);
}

/* ---- touching the map ---- */

static void
reversed (GObject *src, GAsyncResult *res, gpointer data)
{
  Navi *n = data;
  g_autoptr (NaviPlace) place = navi_search_reverse_finish (NAVI_SEARCH (src), res, NULL);
  if (!place || !place->name || !*place->name || n->mode != NAVI_MODE_PLACE || !n->dest)
    return; /* no address: it stays a "Dropped pin" */
  /* still the point that was pressed */
  if (n->dest->kind != NAVI_PLACE_POINT)
    return;
  place->lat = n->dest->lat;
  place->lng = n->dest->lng;
  g_clear_pointer (&n->dest, navi_place_free);
  n->dest = navi_place_copy (place);
  navi_panels_show_place (n->panels, place);
}

static void
on_long_press (GtkGestureLongPress *gesture, double x, double y, Navi *n)
{
  if (n->mode == NAVI_MODE_DRIVE || n->mode == NAVI_MODE_ARRIVED)
    return;
  double lat, lng;
  navi_map_screen_to_geo (n->map, x, y, &lat, &lng);
  if (isnan (lat) || isnan (lng))
    return;
  g_autoptr (NaviPlace) point = navi_place_new (NAVI_PLACE_POINT, _("Dropped pin"), NULL, lat, lng);
  navi_show_place (n, point);
  navi_search_reverse_async (n->search, lat, lng, NULL, reversed, n);
}

/* a point of interest's address, once Photon (or the tiles) named the point */
typedef struct {
  Navi *n;
  guint seq;
} PoiAsk;

static void
poi_reversed (GObject *src, GAsyncResult *res, gpointer data)
{
  PoiAsk *ask = data;
  Navi *n = ask->n;
  gboolean same = ask->seq == n->poi_seq;
  g_free (ask);
  g_autoptr (NaviPlace) found = navi_search_reverse_finish (NAVI_SEARCH (src), res, NULL);
  if (!found || !found->name || !same || n->mode != NAVI_MODE_PLACE || !n->dest || n->dest->kind != NAVI_PLACE_POI || n->dest->detail)
    return; /* another place by now */
  /* the street and number with where it is; for the point of interest itself, where it is */
  g_autofree char *address = NULL;
  if (found->kind == NAVI_PLACE_ADDRESS || found->kind == NAVI_PLACE_STREET)
    address = found->detail ? g_strdup_printf ("%s, %s", found->name, found->detail) : g_strdup (found->name);
  else
    address = g_strdup (found->detail);
  if (!address)
    return;
  n->dest->detail = g_steal_pointer (&address);
  navi_panels_show_place (n->panels, n->dest);
}

/* the point of interest under a tap, the nearest one in reach */
static gboolean
tap_poi (Navi *n, double x, double y)
{
  if (!n->poi_layers) {
    g_auto (GStrv) symbols = navi_map_list_layers (n->map, "symbol");
    g_autoptr (GStrvBuilder) b = g_strv_builder_new ();
    for (guint i = 0; symbols && symbols[i]; i++)
      if (g_str_has_prefix (symbols[i], "poi-"))
        g_strv_builder_add (b, symbols[i]);
    n->poi_layers = g_strv_builder_end (b);
  }
  if (!n->poi_layers[0])
    return FALSE;
  g_autofree char *hit = navi_map_query_features (n->map, x, y, 18, (const char *const *) n->poi_layers);
  if (!hit)
    return FALSE;
  g_autoptr (JsonParser) parser = json_parser_new ();
  if (!json_parser_load_from_data (parser, hit, -1, NULL))
    return FALSE;
  JsonNode *root = json_parser_get_root (parser);
  JsonArray *features = JSON_NODE_HOLDS_OBJECT (root) && json_object_has_member (json_node_get_object (root), "features")
                          ? json_object_get_array_member (json_node_get_object (root), "features") : NULL;
  JsonObject *best = NULL;
  double best_d = G_MAXDOUBLE, best_lat = 0, best_lng = 0;
  for (guint i = 0; features && i < json_array_get_length (features); i++) {
    JsonObject *f = json_array_get_object_element (features, i);
    JsonObject *g = f && json_object_has_member (f, "geometry") ? json_object_get_object_member (f, "geometry") : NULL;
    JsonArray *c = g && json_object_has_member (g, "coordinates") ? json_object_get_array_member (g, "coordinates")
                                                                    : NULL;
    if (!c || json_array_get_length (c) < 2 || !json_object_has_member (f, "properties"))
      continue;
    double lng = json_array_get_double_element (c, 0), lat = json_array_get_double_element (c, 1), sx, sy;
    navi_map_geo_to_screen (n->map, lat, lng, &sx, &sy);
    double d = hypot (sx - x, sy - y);
    if (d < best_d) {
      best_d = d;
      best = json_object_get_object_member (f, "properties");
      best_lat = lat;
      best_lng = lng;
    }
  }
  if (!best)
    return FALSE;
  const char *cls = json_object_get_string_member_with_default (best, "class", NULL);
  const char *group = navi_poi_group (cls, json_object_get_string_member_with_default (best, "subclass", NULL));
  const char *name = json_object_get_string_member_with_default (best, "name:latin", NULL);
  if (!name)
    name = json_object_get_string_member_with_default (best, "name", NULL);
  const char *title = name ? name : navi_poi_label (group);
  g_autoptr (NaviPlace) place = navi_place_new (NAVI_PLACE_POI, title ? title : _("Place"), NULL, best_lat, best_lng);
  place->category = g_strdup (group ? group : cls);
  navi_show_place (n, place);
  PoiAsk *ask = g_new (PoiAsk, 1);
  ask->n = n;
  ask->seq = ++n->poi_seq;
  navi_search_reverse_async (n->search, best_lat, best_lng, NULL, poi_reversed, ask);
  return TRUE;
}

static void
on_tap (GtkGestureClick *gesture, int n_press, double x, double y, Navi *n)
{
  if (n_press == 1 && (n->mode == NAVI_MODE_BROWSE || n->mode == NAVI_MODE_PLACE)) {
    tap_poi (n, x, y);
    return;
  }
  if (n_press != 1 || n->mode != NAVI_MODE_ROUTES || !n->routes || n->routes->len < 2)
    return;
  /* a tap on another route selects it */
  static const char *const layers[] = { "navi-alts-edge", NULL };
  g_autofree char *hit = navi_map_query_features (n->map, x, y, 14, layers);
  if (!hit)
    return;
  double lat, lng, best = G_MAXDOUBLE;
  guint pick = n->selected;
  navi_map_screen_to_geo (n->map, x, y, &lat, &lng);
  for (guint i = 0; i < n->routes->len; i++) {
    if (i == n->selected)
      continue;
    NaviRoute *r = n->routes->pdata[i];
    double s, b;
    double d = navi_path_project (r->points, r->n_points, lat, lng, 0, r->length, &s, &b);
    if (d < best) {
      best = d;
      pick = i;
    }
  }
  navi_select_route (n, pick);
}

/* ---- building ---- */

void
navi_window_build_map (Navi *n)
{
  n->map = NAVI_MAP (navi_map_new ());
  n->alerts = navi_alerts_new (n);
  gtk_widget_set_hexpand (GTK_WIDGET (n->map), TRUE);
  gtk_widget_set_vexpand (GTK_WIDGET (n->map), TRUE);
  n->map_overlay = gtk_overlay_new ();
  gtk_overlay_set_child (GTK_OVERLAY (n->map_overlay), GTK_WIDGET (n->map));
  g_autofree char *cache = g_build_filename (g_get_user_cache_dir (), "vibeos-navi", "map-cache.db", NULL);
  g_autofree char *cache_dir = g_path_get_dirname (cache);
  g_mkdir_with_parents (cache_dir, 0700);
  navi_map_set_cache_path (n->map, g_getenv ("NAVI_CACHE") ? g_getenv ("NAVI_CACHE") : cache);
  navi_map_set_focus (n->map, 0.5, 0.68); /* the car a third of the way up, above the speed and street pills */
  navi_map_set_max_pitch (n->map, 60);
  navi_map_set_zoom_range (n->map, 2, 19.5);
  const char *v;
  if ((v = g_getenv ("NAVI_RENDER_SCALE")))
    navi_map_set_render_scale (n->map, g_ascii_strtod (v, NULL));
  if ((v = g_getenv ("NAVI_PACE")))
    navi_map_set_frame_pacing (n->map, atoi (v));
  double lod[4];
  if ((v = g_getenv ("NAVI_TILE_LOD")) && sscanf (v, "%lf,%lf,%lf,%lf", &lod[0], &lod[1], &lod[2], &lod[3]) == 4)
    navi_map_set_tile_lod (n->map, lod[0], lod[1], lod[2], lod[3]);

  /* where the map opens: where it was, else Berlin's middle */
  NaviCamera cam = { 52.5163, 13.3777, 12, 0, 0 };
  if (n->settings->have_last)
    cam = (NaviCamera) { n->settings->last_lat, n->settings->last_lng, BROWSE_ZOOM, 0, 0 };
  navi_map_jump_to (n->map, &cam);
  n->zoom = cam.zoom;
  n->pitch = 0;

  n->car = gtk_drawing_area_new ();
  gtk_drawing_area_set_draw_func (GTK_DRAWING_AREA (n->car), draw_car, n, NULL);
  gtk_widget_set_can_target (n->car, FALSE);
  gtk_overlay_add_overlay (GTK_OVERLAY (n->map_overlay), n->car);
  n->pin = gtk_drawing_area_new ();
  gtk_drawing_area_set_draw_func (GTK_DRAWING_AREA (n->pin), draw_pin, n, NULL);
  gtk_widget_set_can_target (n->pin, FALSE);
  gtk_widget_set_visible (n->pin, FALSE);
  gtk_overlay_add_overlay (GTK_OVERLAY (n->map_overlay), n->pin);
  g_signal_connect (n->map_overlay, "get-child-position", G_CALLBACK (place_child), n);

  g_signal_connect (n->map, "style-loaded", G_CALLBACK (on_style_loaded), n);
  g_signal_connect (n->map, "load-error", G_CALLBACK (on_load_error), n);
  g_signal_connect (n->map, "user-moved", G_CALLBACK (on_user_moved), n);
  g_signal_connect (n->map, "camera-changed", G_CALLBACK (on_camera_changed), n);
  g_signal_connect (n->map, "frame-rendered", G_CALLBACK (on_frame_rendered), n);

  GtkGesture *press = gtk_gesture_long_press_new ();
  gtk_gesture_single_set_touch_only (GTK_GESTURE_SINGLE (press), FALSE);
  g_signal_connect (press, "pressed", G_CALLBACK (on_long_press), n);
  gtk_widget_add_controller (GTK_WIDGET (n->map), GTK_EVENT_CONTROLLER (press));
  GtkGesture *tap = gtk_gesture_click_new ();
  g_signal_connect (tap, "released", G_CALLBACK (on_tap), n);
  gtk_widget_add_controller (GTK_WIDGET (n->map), GTK_EVENT_CONTROLLER (tap));

  n->position = navi_position_new ();
  g_signal_connect (n->position, "fix", G_CALLBACK (on_fix), n);
  g_signal_connect (n->position, "denied", G_CALLBACK (on_denied), n);
  g_signal_connect (vibeos_theme_get_default (), "changed", G_CALLBACK (on_theme_changed), n);
  g_timeout_add_seconds (60, night_check, n);
  if (n->stats)
    g_timeout_add_seconds (1, stats_second, n);
}
