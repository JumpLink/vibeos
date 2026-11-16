/* Compass: which way the phone points, on a drawn compass rose, and where you are. The C port of the GJS
 * vibeos.Compass, same dial.
 *
 * The heading comes from iio-sensor-proxy (net.hadess.SensorProxy.Compass, fed by the sensor hub's
 * bcm4775-compass); the dial turns so its N points at magnetic north. The position comes from GeoClue.
 *
 * Both are claimed only while the window is in front: in the background the sensor hub stops streaming (several
 * DSP and CPU wakeups a second) and the GPS is let go. iio-sensor-proxy crashes on a quick claim / release / claim
 * of the compass, so the sensor is released only after 3 s in the background, and coming back within that time
 * keeps it. */
#include <adwaita.h>
#include <math.h>

#define APP_ID      "vibeos.Compass"
#define SENSOR_BUS  "net.hadess.SensorProxy"
#define SENSOR_PATH "/net/hadess/SensorProxy/Compass"
#define SENSOR_IF   "net.hadess.SensorProxy.Compass"
#define GEO_BUS     "org.freedesktop.GeoClue2"
#define GEO_MANAGER "/org/freedesktop/GeoClue2/Manager"
#define RELEASE_S   3
/* the needle closes 15 % of the gap per 60 Hz frame in the GJS app; as a time constant it is the same at 120 Hz */
#define EASE_TAU_US 102600.0
#define GEO_EXACT   8          /* GClueAccuracyLevel: exact (the dial prints tenths of a second) */

static const char *const NAMES[] = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};

typedef struct {
  GtkWidget    *win, *area, *status;
  gboolean      active;         /* in front: the sensor and the GPS are wanted */
  guint         release_id;

  GDBusProxy   *sensor;
  gboolean      sensor_asked;   /* the proxy is being made */
  gboolean      claimed;
  double        heading;        /* degrees, < 0 = no reading yet */
  double        shown;          /* where the dial is drawn, easing towards heading */
  guint         tick_id;
  gint64        last_frame;

  GDBusConnection *system;
  char         *geo_client;     /* the GeoClue client object, while started */
  guint         geo_signal;
  gboolean      geo_asked;
  gboolean      have_position;
  double        lat, lon;
  char         *geo_error;
} Compass;

static void claim (Compass *c);
static void release (Compass *c);
static void geo_start (Compass *c);
static void geo_stop (Compass *c);

static void
set_status (Compass *c, const char *text)
{
  gtk_label_set_text (GTK_LABEL (c->status), text);
}

/* ---- the needle: eased towards the reading at the display's rate, only while it is still moving ---- */

static gboolean
on_tick (GtkWidget *w, GdkFrameClock *clock, gpointer data)
{
  Compass *c = data;
  gint64 now = gdk_frame_clock_get_frame_time (clock);
  double dt = c->last_frame ? (double) (now - c->last_frame) : 16667.0;
  c->last_frame = now;

  double diff = fmod (c->heading - c->shown + 540.0, 360.0) - 180.0;   /* the short way round */
  if (fabs (diff) <= 0.05) {
    c->shown = c->heading;
    gtk_widget_queue_draw (c->area);
    c->tick_id = 0;
    c->last_frame = 0;
    return G_SOURCE_REMOVE;
  }
  c->shown = fmod (c->shown + diff * (1.0 - exp (-dt / EASE_TAU_US)) + 360.0, 360.0);
  gtk_widget_queue_draw (c->area);
  return G_SOURCE_CONTINUE;
}

static void
start_tick (Compass *c)
{
  if (!c->tick_id && c->active && c->heading >= 0)
    c->tick_id = gtk_widget_add_tick_callback (c->area, on_tick, c, NULL);
}

static void
stop_tick (Compass *c)
{
  if (c->tick_id)
    gtk_widget_remove_tick_callback (c->area, c->tick_id);
  c->tick_id = 0;
  c->last_frame = 0;
}

/* ---- the sensor ---- */

static void
on_sensor_props (GDBusProxy *proxy, GVariant *changed, GStrv invalidated, gpointer data)
{
  Compass *c = data;
  double h;

  if (!g_variant_lookup (changed, "CompassHeading", "d", &h) || h < 0)   /* -1: not known yet */
    return;
  c->heading = h;
  start_tick (c);
}

static void
on_claimed (GObject *src, GAsyncResult *res, gpointer data)
{
  Compass *c = data;
  g_autoptr (GError) error = NULL;
  g_autoptr (GVariant) ret = g_dbus_proxy_call_finish (G_DBUS_PROXY (src), res, &error);

  if (!ret) {
    c->claimed = FALSE;
    g_message ("compass: claim failed: %s", error->message);
    g_autofree char *text = g_strdup_printf ("Could not use the compass: %s", error->message);
    set_status (c, text);
    return;
  }
  set_status (c, "Hold the phone flat, away from metal. Move it in a figure 8 to calibrate.");
}

static void
claim_call (Compass *c)
{
  if (!c->active || c->claimed || !c->sensor)
    return;
  c->claimed = TRUE;
  g_dbus_proxy_call (c->sensor, "ClaimCompass", NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, on_claimed, c);
}

static void
on_sensor_proxy (GObject *src, GAsyncResult *res, gpointer data)
{
  Compass *c = data;
  g_autoptr (GError) error = NULL;

  c->sensor = g_dbus_proxy_new_for_bus_finish (res, &error);
  c->sensor_asked = FALSE;
  if (!c->sensor) {
    g_autofree char *text = g_strdup_printf ("No sensor service: %s", error->message);
    set_status (c, text);
    return;
  }
  g_autoptr (GVariant) has = g_dbus_proxy_get_cached_property (c->sensor, "HasCompass");
  if (has && !g_variant_get_boolean (has)) {
    set_status (c, "This phone reports no compass.");
    g_clear_object (&c->sensor);
    return;
  }
  g_signal_connect (c->sensor, "g-properties-changed", G_CALLBACK (on_sensor_props), c);
  claim_call (c);
}

static void
claim (Compass *c)
{
  if (c->sensor) {
    claim_call (c);
    return;
  }
  if (c->sensor_asked)
    return;
  c->sensor_asked = TRUE;
  g_dbus_proxy_new_for_bus (G_BUS_TYPE_SYSTEM, G_DBUS_PROXY_FLAGS_NONE, NULL, SENSOR_BUS, SENSOR_PATH, SENSOR_IF,
                            NULL, on_sensor_proxy, c);
}

static void
release (Compass *c)
{
  if (!c->claimed || !c->sensor)
    return;
  c->claimed = FALSE;
  g_dbus_proxy_call (c->sensor, "ReleaseCompass", NULL, G_DBUS_CALL_FLAGS_NONE, 1000, NULL, NULL, NULL);
}

/* ---- where you are: one GeoClue client, started while in front ---- */

static void
geo_failed (Compass *c, const char *what, GError *error)
{
  g_message ("compass: %s: %s", what, error ? error->message : "?");
  g_free (c->geo_error);
  c->geo_error = g_strdup ("Location unavailable");
  gtk_widget_queue_draw (c->area);
}

static void
on_location (GDBusConnection *bus, const char *sender, const char *path, const char *iface, const char *signal,
             GVariant *params, gpointer data)
{
  Compass *c = data;
  const char *old_path, *new_path;
  g_autoptr (GError) error = NULL;

  g_variant_get (params, "(&o&o)", &old_path, &new_path);
  g_autoptr (GVariant) all = g_dbus_connection_call_sync (bus, GEO_BUS, new_path, "org.freedesktop.DBus.Properties",
                                                          "GetAll", g_variant_new ("(s)", "org.freedesktop.GeoClue2.Location"),
                                                          G_VARIANT_TYPE ("(a{sv})"), G_DBUS_CALL_FLAGS_NONE, 2000,
                                                          NULL, &error);
  if (!all) {
    geo_failed (c, "location", error);
    return;
  }
  g_autoptr (GVariant) props = g_variant_get_child_value (all, 0);
  if (g_variant_lookup (props, "Latitude", "d", &c->lat) && g_variant_lookup (props, "Longitude", "d", &c->lon)) {
    c->have_position = TRUE;
    g_clear_pointer (&c->geo_error, g_free);
    gtk_widget_queue_draw (c->area);
  }
}

static gboolean
geo_set (Compass *c, const char *prop, GVariant *value, GError **error)
{
  g_autoptr (GVariant) ret = g_dbus_connection_call_sync (c->system, GEO_BUS, c->geo_client,
                                                          "org.freedesktop.DBus.Properties", "Set",
                                                          g_variant_new ("(ssv)", "org.freedesktop.GeoClue2.Client",
                                                                         prop, value),
                                                          NULL, G_DBUS_CALL_FLAGS_NONE, 2000, NULL, error);
  return ret != NULL;
}

static void
on_geo_started (GObject *src, GAsyncResult *res, gpointer data)
{
  Compass *c = data;
  g_autoptr (GError) error = NULL;
  g_autoptr (GVariant) ret = g_dbus_connection_call_finish (G_DBUS_CONNECTION (src), res, &error);

  if (!ret)
    geo_failed (c, "start", error);   /* refused, or no agent to ask */
}

static void
on_geo_client (GObject *src, GAsyncResult *res, gpointer data)
{
  Compass *c = data;
  g_autoptr (GError) error = NULL;
  g_autoptr (GVariant) ret = g_dbus_connection_call_finish (G_DBUS_CONNECTION (src), res, &error);

  c->geo_asked = FALSE;
  if (!ret) {
    geo_failed (c, "client", error);
    return;
  }
  g_variant_get (ret, "(o)", &c->geo_client);
  if (!geo_set (c, "DesktopId", g_variant_new_string (APP_ID), &error) ||
      !geo_set (c, "RequestedAccuracyLevel", g_variant_new_uint32 (GEO_EXACT), &error)) {
    geo_failed (c, "client setup", error);
    g_clear_pointer (&c->geo_client, g_free);
    return;
  }
  c->geo_signal = g_dbus_connection_signal_subscribe (c->system, GEO_BUS, "org.freedesktop.GeoClue2.Client",
                                                      "LocationUpdated", c->geo_client, NULL,
                                                      G_DBUS_SIGNAL_FLAGS_NONE, on_location, c, NULL);
  if (!c->active) {             /* backgrounded while the client was made */
    geo_stop (c);
    return;
  }
  g_dbus_connection_call (c->system, GEO_BUS, c->geo_client, "org.freedesktop.GeoClue2.Client", "Start", NULL,
                          NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, on_geo_started, c);
}

static void
geo_start (Compass *c)
{
  if (c->geo_client || c->geo_asked)
    return;
  if (!c->system) {
    g_autoptr (GError) error = NULL;
    c->system = g_bus_get_sync (G_BUS_TYPE_SYSTEM, NULL, &error);
    if (!c->system) {
      geo_failed (c, "system bus", error);
      return;
    }
  }
  c->geo_asked = TRUE;
  g_dbus_connection_call (c->system, GEO_BUS, GEO_MANAGER, "org.freedesktop.GeoClue2.Manager", "GetClient", NULL,
                          G_VARIANT_TYPE ("(o)"), G_DBUS_CALL_FLAGS_NONE, 5000, NULL, on_geo_client, c);
}

/* GeoClue drops a client whose owner leaves the bus, so this matters only while the app runs in the background */
static void
geo_stop (Compass *c)
{
  if (!c->geo_client)
    return;
  if (c->geo_signal)
    g_dbus_connection_signal_unsubscribe (c->system, c->geo_signal);
  c->geo_signal = 0;
  g_dbus_connection_call (c->system, GEO_BUS, c->geo_client, "org.freedesktop.GeoClue2.Client", "Stop", NULL, NULL,
                          G_DBUS_CALL_FLAGS_NONE, 1000, NULL, NULL, NULL);
  g_dbus_connection_call (c->system, GEO_BUS, GEO_MANAGER, "org.freedesktop.GeoClue2.Manager", "DeleteClient",
                          g_variant_new ("(o)", c->geo_client), NULL, G_DBUS_CALL_FLAGS_NONE, 1000, NULL, NULL, NULL);
  g_clear_pointer (&c->geo_client, g_free);
}

/* ---- in front or not ---- */

static gboolean
on_release_timeout (gpointer data)
{
  Compass *c = data;

  c->release_id = 0;
  if (!c->active) {
    release (c);
    geo_stop (c);
  }
  return G_SOURCE_REMOVE;
}

static void
set_active (Compass *c, gboolean on)
{
  if (on == c->active)
    return;
  c->active = on;
  if (on) {
    g_clear_handle_id (&c->release_id, g_source_remove);
    claim (c);
    geo_start (c);
    start_tick (c);
  } else {
    stop_tick (c);
    if (!c->release_id)
      c->release_id = g_timeout_add_seconds (RELEASE_S, on_release_timeout, c);
  }
}

/* in front = focused and not suspended (the shell covers an app with another or with the lock screen) */
static void
sync_active (Compass *c)
{
  set_active (c, gtk_window_is_active (GTK_WINDOW (c->win)) && !gtk_window_is_suspended (GTK_WINDOW (c->win)));
}

static void
on_window_notify (GObject *win, GParamSpec *pspec, gpointer data)
{
  sync_active (data);
}

static gboolean
on_close (GtkWindow *win, gpointer data)
{
  Compass *c = data;

  g_clear_handle_id (&c->release_id, g_source_remove);
  stop_tick (c);
  release (c);
  geo_stop (c);
  return FALSE;
}

/* ---- the dial ---- */

static double
rad (double d)
{
  return d * G_PI / 180.0;
}

/* text in the dial's font, size in px; returns its ink extents through ink */
static PangoLayout *
text_layout (GtkWidget *w, const char *text, double size, gboolean bold, PangoRectangle *ink)
{
  PangoLayout *layout = gtk_widget_create_pango_layout (w, text);
  PangoFontDescription *font = pango_font_description_from_string ("Sans");

  pango_font_description_set_absolute_size (font, size * PANGO_SCALE);
  pango_font_description_set_weight (font, bold ? PANGO_WEIGHT_BOLD : PANGO_WEIGHT_NORMAL);
  pango_layout_set_font_description (layout, font);
  pango_font_description_free (font);
  pango_layout_get_pixel_extents (layout, ink, NULL);
  return layout;
}

/* centred on x, its ink's baseline-ish bottom at y (as cairo_show_text at y) */
static void
centred (cairo_t *cr, GtkWidget *w, const char *text, double size, gboolean bold, double x, double y)
{
  PangoRectangle ink;
  g_autoptr (PangoLayout) layout = text_layout (w, text, size, bold, &ink);
  int baseline = pango_layout_get_baseline (layout) / PANGO_SCALE;

  cairo_move_to (cr, x - ink.width / 2.0 - ink.x, y - baseline);
  pango_cairo_show_layout (cr, layout);
}

/* text along the dial: centred on its own middle at dist from the centre, turned to angle a */
static void
along (cairo_t *cr, GtkWidget *w, double a, double dist, const char *text, double size, gboolean bold,
       double r, double g, double b)
{
  PangoRectangle ink;
  g_autoptr (PangoLayout) layout = text_layout (w, text, size, bold, &ink);

  cairo_save (cr);
  cairo_rotate (cr, rad (a));
  cairo_translate (cr, 0, -dist);
  cairo_set_source_rgb (cr, r, g, b);
  cairo_move_to (cr, -ink.width / 2.0 - ink.x, -ink.height / 2.0 - ink.y);
  pango_cairo_show_layout (cr, layout);
  cairo_restore (cr);
}

static void
dms (char *out, gsize n, double v, const char *pos, const char *neg)
{
  double a = fabs (v);
  int d = (int) floor (a);
  int m = (int) floor ((a - d) * 60);
  double s = ((a - d) * 60 - m) * 60;

  g_snprintf (out, n, "%d°%02d'%.1f\"%s", d, m, s, v >= 0 ? pos : neg);
}

/* A round dark dial: ticks and degrees on the rim, N/E/S/W and the intercardinals inside, all turning with the
 * heading; a red pointer fixed at the top; the heading and where you are in the middle. */
static void
draw (GtkDrawingArea *area, cairo_t *cr, int w, int h, gpointer data)
{
  Compass *c = data;
  GtkWidget *wd = GTK_WIDGET (area);
  double R = MIN (w, h) / 2.0 * 0.86, cx = w / 2.0, cy = h / 2.0;   /* a margin round the dial */
  double rim = R * 0.80;                                           /* where the ticks start */

  /* body: a dark disc with a lighter rim band */
  cairo_set_source_rgb (cr, 0.16, 0.16, 0.17);
  cairo_arc (cr, cx, cy, R, 0, 2 * G_PI);
  cairo_fill (cr);
  cairo_set_source_rgb (cr, 0.11, 0.11, 0.12);
  cairo_arc (cr, cx, cy, rim - 2, 0, 2 * G_PI);
  cairo_fill (cr);
  cairo_set_source_rgba (cr, 1, 1, 1, 0.08);
  cairo_set_line_width (cr, 1);
  cairo_arc (cr, cx, cy, rim * 0.72, 0, 2 * G_PI);
  cairo_stroke (cr);

  cairo_save (cr);
  cairo_translate (cr, cx, cy);
  cairo_rotate (cr, -rad (c->shown));
  /* rim ticks: every 2°, longer every 10°, white and longest at the cardinals */
  for (int a = 0; a < 360; a += 2) {
    gboolean card = a % 90 == 0, ten = a % 10 == 0;
    cairo_save (cr);
    cairo_rotate (cr, rad (a));
    cairo_set_source_rgba (cr, 1, 1, 1, card ? 1 : ten ? 0.85 : 0.45);
    cairo_set_line_width (cr, card ? 3 : ten ? 1.8 : 1);
    cairo_move_to (cr, 0, -R + 4);
    cairo_line_to (cr, 0, -R + (card ? 22 : ten ? 15 : 10));
    cairo_stroke (cr);
    cairo_restore (cr);
  }
  /* degrees every 30° just inside the ticks, letters further in (they turn with it) */
  for (int a = 0; a < 360; a += 30) {
    char deg[8];
    g_snprintf (deg, sizeof deg, "%d", a);
    along (cr, wd, a, R - 38, deg, R * 0.075, FALSE, 0.92, 0.92, 0.94);
  }
  static const struct { int a; const char *t; } cardinals[] = {{0, "N"}, {90, "E"}, {180, "S"}, {270, "W"}},
                                                 inter[] = {{45, "NE"}, {135, "SE"}, {225, "SW"}, {315, "NW"}};
  for (guint i = 0; i < G_N_ELEMENTS (cardinals); i++) {
    gboolean north = cardinals[i].a == 0;
    along (cr, wd, cardinals[i].a, rim * 0.80, cardinals[i].t, R * 0.14, TRUE, 1, north ? 0.18 : 1, north ? 0.15 : 1);
  }
  for (guint i = 0; i < G_N_ELEMENTS (inter); i++)
    along (cr, wd, inter[i].a, rim * 0.80, inter[i].t, R * 0.065, FALSE, 0.75, 0.75, 0.78);
  cairo_restore (cr);

  /* the fixed red pointer at the top: where the phone points */
  cairo_set_source_rgb (cr, 1, 0.18, 0.15);
  cairo_move_to (cr, cx, cy - R + 14);
  cairo_line_to (cr, cx - 10, cy - R - 6);
  cairo_line_to (cr, cx + 10, cy - R - 6);
  cairo_close_path (cr);
  cairo_fill (cr);
  cairo_set_line_width (cr, 2.5);
  cairo_move_to (cr, cx, cy - R + 10);
  cairo_line_to (cr, cx, cy - R + 30);
  cairo_stroke (cr);

  /* the middle: heading, a divider, where you are */
  char hd[32];
  if (c->heading < 0)
    g_strlcpy (hd, "—", sizeof hd);
  else
    g_snprintf (hd, sizeof hd, "%d°%s", (int) lround (c->heading) % 360, NAMES[(int) lround (c->heading / 45) % 8]);
  cairo_set_source_rgb (cr, 1, 1, 1);
  centred (cr, wd, hd, R * 0.24, FALSE, cx, cy - R * 0.04);
  cairo_set_source_rgba (cr, 1, 1, 1, 0.35);
  cairo_set_line_width (cr, 1);
  cairo_move_to (cr, cx - R * 0.30, cy + R * 0.06);
  cairo_line_to (cr, cx + R * 0.30, cy + R * 0.06);
  cairo_stroke (cr);

  /* a location pin */
  double py = cy + R * 0.17, pr = R * 0.045;
  cairo_set_source_rgb (cr, 0.92, 0.92, 0.94);
  cairo_arc (cr, cx, py, pr, G_PI, 0);
  cairo_line_to (cr, cx, py + pr * 2.2);
  cairo_close_path (cr);
  cairo_fill (cr);
  cairo_set_source_rgb (cr, 0.11, 0.11, 0.12);
  cairo_arc (cr, cx, py, pr * 0.42, 0, 2 * G_PI);
  cairo_fill (cr);
  cairo_set_source_rgb (cr, 0.92, 0.92, 0.94);
  centred (cr, wd, "Location", R * 0.07, TRUE, cx, cy + R * 0.38);
  if (c->have_position) {
    char lat[32], lon[32];
    dms (lat, sizeof lat, c->lat, "N", "S");
    dms (lon, sizeof lon, c->lon, "E", "W");
    centred (cr, wd, lat, R * 0.062, FALSE, cx, cy + R * 0.49);
    centred (cr, wd, lon, R * 0.062, FALSE, cx, cy + R * 0.59);
  } else {
    cairo_set_source_rgba (cr, 1, 1, 1, 0.5);
    centred (cr, wd, c->geo_error ? c->geo_error : "Locating…", R * 0.062, FALSE, cx, cy + R * 0.49);
  }
}

/* ---- the window ---- */

static const char CSS[] =
  "window.compass, window.compass .compass-bg { background: #000; color: #fff; }"
  ".compass-status { color: #8e8e93; font-size: 13px; }";

static void
on_activate (GtkApplication *app, gpointer data)
{
  Compass *c = data;

  if (c->win) {
    gtk_window_present (GTK_WINDOW (c->win));
    return;
  }
  adw_style_manager_set_color_scheme (adw_style_manager_get_default (), ADW_COLOR_SCHEME_FORCE_DARK);
  GtkCssProvider *css = gtk_css_provider_new ();
  gtk_css_provider_load_from_string (css, CSS);
  gtk_style_context_add_provider_for_display (gdk_display_get_default (), GTK_STYLE_PROVIDER (css),
                                              GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
  g_object_unref (css);

  c->win = adw_application_window_new (app);
  gtk_window_set_title (GTK_WINDOW (c->win), "Compass");
  gtk_window_set_default_size (GTK_WINDOW (c->win), 360, 720);
  gtk_widget_add_css_class (c->win, "compass");

  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_add_css_class (box, "compass-bg");
  c->area = gtk_drawing_area_new ();
  gtk_widget_set_hexpand (c->area, TRUE);
  gtk_widget_set_vexpand (c->area, TRUE);
  gtk_drawing_area_set_draw_func (GTK_DRAWING_AREA (c->area), draw, c, NULL);
  c->status = gtk_label_new ("");
  gtk_widget_add_css_class (c->status, "compass-status");
  gtk_label_set_wrap (GTK_LABEL (c->status), TRUE);
  gtk_label_set_justify (GTK_LABEL (c->status), GTK_JUSTIFY_CENTER);
  gtk_widget_set_margin_start (c->status, 24);
  gtk_widget_set_margin_end (c->status, 24);
  gtk_widget_set_margin_bottom (c->status, 40);
  gtk_box_append (GTK_BOX (box), c->area);
  gtk_box_append (GTK_BOX (box), c->status);
  adw_application_window_set_content (ADW_APPLICATION_WINDOW (c->win), box);

  g_signal_connect (c->win, "close-request", G_CALLBACK (on_close), c);
  g_signal_connect (c->win, "notify::is-active", G_CALLBACK (on_window_notify), c);
  g_signal_connect (c->win, "notify::suspended", G_CALLBACK (on_window_notify), c);
  gtk_window_present (GTK_WINDOW (c->win));
  sync_active (c);
}

int
main (int argc, char **argv)
{
  Compass c = {.heading = -1};
  g_autoptr (AdwApplication) app = adw_application_new (APP_ID, G_APPLICATION_DEFAULT_FLAGS);

  g_signal_connect (app, "activate", G_CALLBACK (on_activate), &c);
  int status = g_application_run (G_APPLICATION (app), argc, argv);
  g_clear_object (&c.sensor);
  g_clear_object (&c.system);
  g_free (c.geo_error);
  return status;
}
