/* NaviPosition: see navi-position.h. */
#include "navi-position.h"
#include "navi-geo.h"

#include <math.h>
#include <string.h>

#define GC_BUS      "org.freedesktop.GeoClue2"
#define GC_MANAGER  "/org/freedesktop/GeoClue2/Manager"
#define GC_IMGR     "org.freedesktop.GeoClue2.Manager"
#define GC_ICLIENT  "org.freedesktop.GeoClue2.Client"
#define GC_ILOC     "org.freedesktop.GeoClue2.Location"
#define PROPS       "org.freedesktop.DBus.Properties"
#define DESKTOP_ID  "vibeos.Navi"
#define GC_EXACT    8

#define PORTAL       "org.freedesktop.portal.Desktop"
#define PORTAL_PATH  "/org/freedesktop/portal/desktop"
#define PORTAL_ILOC  "org.freedesktop.portal.Location"
#define PORTAL_EXACT 5

#define APPROXIMATE_M 100.0
#define SIM_STEP_MS   50

struct _NaviPosition {
  GObject parent_instance;
  NaviFix fix;
  gboolean have_fix;
  /* the real source */
  gboolean running;
  GCancellable *cancel;
  GDBusConnection *bus;
  char *client;          /* GeoClue */
  guint gc_sub, step;
  char *session, *request, *token; /* the portal */
  guint portal_fix_sub, portal_answer_sub;
  /* the simulated drive */
  NaviRoute *sim;
  double sim_factor, sim_s, sim_v;
  guint sim_interval, sim_timer, sim_since_fix;
  guint sim_hint;
};

G_DEFINE_FINAL_TYPE (NaviPosition, navi_position, G_TYPE_OBJECT)

enum { SIGNAL_FIX, SIGNAL_DENIED, N_SIGNALS };
static guint signals[N_SIGNALS];

static void
emit_fix (NaviPosition *self, const NaviFix *fix)
{
  self->fix = *fix;
  self->have_fix = TRUE;
  NAVI_PRIVATE_DEBUG ("navi: fix %.6f %.6f +-%.0f m, %.1f m/s, %.0f deg, kind %d\n", fix->lat, fix->lng,
                      fix->accuracy, fix->speed, fix->heading, fix->kind);
  g_signal_emit (self, signals[SIGNAL_FIX], 0, &self->fix);
}

static void
real_fix (NaviPosition *self, double lat, double lng, double accuracy, double speed, double heading)
{
  if (self->sim)
    return; /* the drive has the position */
  NaviFix f = { lat, lng, accuracy, speed, heading, g_get_monotonic_time (),
                accuracy <= APPROXIMATE_M ? NAVI_FIX_GPS : NAVI_FIX_APPROXIMATE };
  /* how good the fixes are, never where: their accuracy, speed, and how far and how long from the last one */
  if (self->have_fix && self->fix.kind != NAVI_FIX_SIMULATED && self->fix.kind != NAVI_FIX_LAST_KNOWN)
    g_debug ("position: +-%.0f m, %.1f m/s, %.1f m and %.1f s from the last fix", accuracy, speed,
             navi_geo_distance (self->fix.lat, self->fix.lng, lat, lng), (f.time_us - self->fix.time_us) / 1e6);
  else
    g_debug ("position: first fix, +-%.0f m, %.1f m/s", accuracy, speed);
  emit_fix (self, &f);
}

static gboolean
sandboxed (void)
{
  static int known = -1;
  if (known < 0)
    known = g_file_test ("/.flatpak-info", G_FILE_TEST_EXISTS);
  return known;
}

/* ---- GeoClue ---- */

static void gc_step (NaviPosition *self);

static void
ignore_reply (GObject *src, GAsyncResult *res, gpointer data)
{
  g_autoptr (GVariant) v = g_dbus_connection_call_finish (G_DBUS_CONNECTION (src), res, NULL);
}

static void
gc_props (GObject *src, GAsyncResult *res, gpointer data)
{
  NaviPosition *self = data;
  g_autoptr (GVariant) props = g_dbus_connection_call_finish (G_DBUS_CONNECTION (src), res, NULL);
  if (props && self->running) {
    g_autoptr (GVariant) dict = g_variant_get_child_value (props, 0);
    double lat, lng, acc = 1e9, speed = -1, heading = -1;
    if (g_variant_lookup (dict, "Latitude", "d", &lat) && g_variant_lookup (dict, "Longitude", "d", &lng)) {
      g_variant_lookup (dict, "Accuracy", "d", &acc);
      g_variant_lookup (dict, "Speed", "d", &speed);
      g_variant_lookup (dict, "Heading", "d", &heading);
      real_fix (self, lat, lng, acc, speed, heading);
    }
  }
  g_object_unref (self);
}

static void
gc_updated (GDBusConnection *bus, const char *sender, const char *path, const char *iface, const char *signal,
            GVariant *params, gpointer data)
{
  NaviPosition *self = data;
  const char *old_path, *new_path;
  if (!self->running || !g_variant_is_of_type (params, G_VARIANT_TYPE ("(oo)")))
    return;
  g_variant_get (params, "(&o&o)", &old_path, &new_path);
  g_dbus_connection_call (bus, GC_BUS, new_path, PROPS, "GetAll", g_variant_new ("(s)", GC_ILOC), NULL,
                          G_DBUS_CALL_FLAGS_NONE, 15000, self->cancel, gc_props, g_object_ref (self));
}

static void
gc_reply (GObject *src, GAsyncResult *res, gpointer data)
{
  NaviPosition *self = data;
  g_autoptr (GError) error = NULL;
  g_autoptr (GVariant) v = g_dbus_connection_call_finish (G_DBUS_CONNECTION (src), res, &error);
  if (!self->running) {
    g_object_unref (self);
    return;
  }
  if (!v) {
    if (self->step == 0) {
      self->step = 1; /* an older GeoClue without CreateClient: the shared client */
      gc_step (self);
    } else if (self->step == 5) {
      self->step++; /* TimeThreshold needs GeoClue 2.4: on without it */
      gc_step (self);
    } else {
      g_message ("location: %s", error->message);
      if (g_error_matches (error, G_DBUS_ERROR, G_DBUS_ERROR_ACCESS_DENIED))
        g_signal_emit (self, signals[SIGNAL_DENIED], 0);
    }
    g_object_unref (self);
    return;
  }
  if (self->step <= 1) {
    g_variant_get (v, "(o)", &self->client);
    self->step = 2;
  } else {
    self->step++;
  }
  gc_step (self);
  g_object_unref (self);
}

static void
gc_set (NaviPosition *self, const char *name, GVariant *value)
{
  g_dbus_connection_call (self->bus, GC_BUS, self->client, PROPS, "Set",
                          g_variant_new ("(ssv)", GC_ICLIENT, name, value), NULL, G_DBUS_CALL_FLAGS_NONE, 15000,
                          self->cancel, gc_reply, g_object_ref (self));
}

static void
gc_step (NaviPosition *self)
{
  switch (self->step) {
  case 0:
    g_dbus_connection_call (self->bus, GC_BUS, GC_MANAGER, GC_IMGR, "CreateClient", NULL, G_VARIANT_TYPE ("(o)"),
                            G_DBUS_CALL_FLAGS_NONE, 15000, self->cancel, gc_reply, g_object_ref (self));
    break;
  case 1:
    g_dbus_connection_call (self->bus, GC_BUS, GC_MANAGER, GC_IMGR, "GetClient", NULL, G_VARIANT_TYPE ("(o)"),
                            G_DBUS_CALL_FLAGS_NONE, 15000, self->cancel, gc_reply, g_object_ref (self));
    break;
  case 2: gc_set (self, "DesktopId", g_variant_new_string (DESKTOP_ID)); break;
  case 3: gc_set (self, "RequestedAccuracyLevel", g_variant_new_uint32 (GC_EXACT)); break;
  case 4: gc_set (self, "DistanceThreshold", g_variant_new_uint32 (0)); break;
  case 5: gc_set (self, "TimeThreshold", g_variant_new_uint32 (0)); break;
  case 6:
    self->gc_sub = g_dbus_connection_signal_subscribe (self->bus, GC_BUS, GC_ICLIENT, "LocationUpdated", self->client,
                                                       NULL, G_DBUS_SIGNAL_FLAGS_NONE, gc_updated, self, NULL);
    g_dbus_connection_call (self->bus, GC_BUS, self->client, GC_ICLIENT, "Start", NULL, NULL,
                            G_DBUS_CALL_FLAGS_NONE, 15000, self->cancel, gc_reply, g_object_ref (self));
    break;
  default:
    break; /* started: LocationUpdated from here on */
  }
}

/* ---- the Location portal ---- */

static void
portal_location (GDBusConnection *bus, const char *sender, const char *path, const char *iface, const char *signal,
                 GVariant *params, gpointer data)
{
  NaviPosition *self = data;
  const char *handle;
  g_autoptr (GVariant) dict = NULL;
  if (!self->running || !g_variant_is_of_type (params, G_VARIANT_TYPE ("(oa{sv})")))
    return;
  g_variant_get (params, "(&o@a{sv})", &handle, &dict);
  if (g_strcmp0 (handle, self->session))
    return;
  double lat, lng, acc = 1e9, speed = -1, heading = -1;
  if (!g_variant_lookup (dict, "Latitude", "d", &lat) || !g_variant_lookup (dict, "Longitude", "d", &lng))
    return;
  g_variant_lookup (dict, "Accuracy", "d", &acc);
  g_variant_lookup (dict, "Speed", "d", &speed);
  g_variant_lookup (dict, "Heading", "d", &heading);
  real_fix (self, lat, lng, acc, speed, heading);
}

static void
portal_answer (GDBusConnection *bus, const char *sender, const char *path, const char *iface, const char *signal,
               GVariant *params, gpointer data)
{
  NaviPosition *self = data;
  guint32 response = 2;
  if (g_variant_is_of_type (params, G_VARIANT_TYPE ("(ua{sv})")))
    g_variant_get (params, "(u@a{sv})", &response, NULL);
  if (response == 1 && self->running)
    g_signal_emit (self, signals[SIGNAL_DENIED], 0);
  else if (response != 0)
    g_message ("location portal: no location");
}

static void
portal_reply (GObject *src, GAsyncResult *res, gpointer data)
{
  NaviPosition *self = data;
  g_autoptr (GError) error = NULL;
  g_autoptr (GVariant) v = g_dbus_connection_call_finish (G_DBUS_CONNECTION (src), res, &error);
  if (!v && !g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    g_message ("location portal: %s", error->message);
  else if (v && self->running && self->step == 0) {
    self->step = 1;
    GVariantBuilder opts;
    g_variant_builder_init (&opts, G_VARIANT_TYPE_VARDICT);
    g_variant_builder_add (&opts, "{sv}", "handle_token", g_variant_new_string (self->token));
    g_dbus_connection_call (self->bus, PORTAL, PORTAL_PATH, PORTAL_ILOC, "Start",
                            g_variant_new ("(osa{sv})", self->session, "", &opts), NULL, G_DBUS_CALL_FLAGS_NONE, -1,
                            self->cancel, portal_reply, g_object_ref (self));
  }
  g_object_unref (self);
}

static void
portal_open (NaviPosition *self)
{
  g_autofree char *sender = g_strdup (g_dbus_connection_get_unique_name (self->bus) + 1);
  g_strdelimit (sender, ".", '_');
  g_free (self->token);
  g_free (self->session);
  g_free (self->request);
  self->token = g_strdup_printf ("navi%u", g_random_int_range (1, 0x3fffffff));
  self->session = g_strdup_printf (PORTAL_PATH "/session/%s/%s", sender, self->token);
  self->request = g_strdup_printf (PORTAL_PATH "/request/%s/%s", sender, self->token);
  /* subscribed before the session exists, so neither the answer nor the first fix comes first */
  self->portal_fix_sub = g_dbus_connection_signal_subscribe (self->bus, PORTAL, PORTAL_ILOC, "LocationUpdated",
                                                             PORTAL_PATH, NULL, G_DBUS_SIGNAL_FLAGS_NONE,
                                                             portal_location, self, NULL);
  self->portal_answer_sub = g_dbus_connection_signal_subscribe (self->bus, PORTAL, "org.freedesktop.portal.Request",
                                                                "Response", self->request, NULL,
                                                                G_DBUS_SIGNAL_FLAGS_NONE, portal_answer, self, NULL);
  GVariantBuilder opts;
  g_variant_builder_init (&opts, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_add (&opts, "{sv}", "session_handle_token", g_variant_new_string (self->token));
  g_variant_builder_add (&opts, "{sv}", "distance-threshold", g_variant_new_uint32 (0));
  g_variant_builder_add (&opts, "{sv}", "time-threshold", g_variant_new_uint32 (0));
  g_variant_builder_add (&opts, "{sv}", "accuracy", g_variant_new_uint32 (PORTAL_EXACT));
  self->step = 0;
  g_dbus_connection_call (self->bus, PORTAL, PORTAL_PATH, PORTAL_ILOC, "CreateSession",
                          g_variant_new ("(a{sv})", &opts), NULL, G_DBUS_CALL_FLAGS_NONE, -1, self->cancel,
                          portal_reply, g_object_ref (self));
}

/* ---- starting and stopping ---- */

static void
got_bus (GObject *src, GAsyncResult *res, gpointer data)
{
  NaviPosition *self = data;
  g_autoptr (GError) error = NULL;
  GDBusConnection *bus = g_bus_get_finish (res, &error);
  if (!bus) {
    if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
      g_message ("location: no bus: %s", error->message);
  } else if (self->running && !self->bus) {
    self->bus = bus;
    self->step = 0;
    if (sandboxed ())
      portal_open (self);
    else
      gc_step (self);
  } else {
    g_object_unref (bus);
  }
  g_object_unref (self);
}

void
navi_position_start (NaviPosition *self)
{
  if (self->running)
    return;
  self->running = TRUE;
  self->cancel = g_cancellable_new ();
  g_bus_get (sandboxed () ? G_BUS_TYPE_SESSION : G_BUS_TYPE_SYSTEM, self->cancel, got_bus, g_object_ref (self));
}

void
navi_position_stop (NaviPosition *self)
{
  if (!self->running)
    return;
  self->running = FALSE;
  g_cancellable_cancel (self->cancel);
  g_clear_object (&self->cancel);
  if (self->bus) {
    if (self->gc_sub)
      g_dbus_connection_signal_unsubscribe (self->bus, self->gc_sub);
    if (self->portal_fix_sub)
      g_dbus_connection_signal_unsubscribe (self->bus, self->portal_fix_sub);
    if (self->portal_answer_sub)
      g_dbus_connection_signal_unsubscribe (self->bus, self->portal_answer_sub);
    self->gc_sub = self->portal_fix_sub = self->portal_answer_sub = 0;
    if (self->client) {
      g_dbus_connection_call (self->bus, GC_BUS, self->client, GC_ICLIENT, "Stop", NULL, NULL,
                              G_DBUS_CALL_FLAGS_NONE, -1, NULL, ignore_reply, NULL);
      g_dbus_connection_call (self->bus, GC_BUS, GC_MANAGER, GC_IMGR, "DeleteClient",
                              g_variant_new ("(o)", self->client), NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL,
                              ignore_reply, NULL);
    }
    if (self->session)
      g_dbus_connection_call (self->bus, PORTAL, self->session, "org.freedesktop.portal.Session", "Close", NULL,
                              NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, ignore_reply, NULL);
  }
  g_clear_pointer (&self->client, g_free);
  g_clear_pointer (&self->session, g_free);
  g_clear_pointer (&self->request, g_free);
  g_clear_object (&self->bus);
}

gboolean
navi_position_get_fix (NaviPosition *self, NaviFix *fix)
{
  if (fix)
    *fix = self->fix;
  return self->have_fix;
}

void
navi_position_set_last_known (NaviPosition *self, double lat, double lng)
{
  if (self->have_fix)
    return;
  NaviFix f = { lat, lng, 5000, -1, -1, g_get_monotonic_time (), NAVI_FIX_LAST_KNOWN };
  emit_fix (self, &f);
}

/* ---- the simulated drive ---- */

/* the speed to drive at s: the street's own (Valhalla's length over time), down to a turning speed before each turn */
static double
sim_target_speed (NaviPosition *self, double s)
{
  NaviRoute *r = self->sim;
  if (!r->n_maneuvers)
    return 13.9;
  guint i = navi_route_maneuver_at (r, s);
  const NaviManeuver *m = &r->maneuvers[i];
  double v = m->time > 1 ? m->length / m->time : 13.9;
  v = CLAMP (v, 4.0, 36.0);
  if (i + 1 < r->n_maneuvers) {
    const NaviManeuver *n = &r->maneuvers[i + 1];
    double turn;
    switch (n->type) {
    case NAVI_TURN_LEFT: case NAVI_TURN_RIGHT: turn = 6.5; break;
    case NAVI_TURN_SHARP_LEFT: case NAVI_TURN_SHARP_RIGHT: case NAVI_TURN_UTURN_LEFT: case NAVI_TURN_UTURN_RIGHT:
      turn = 4; break;
    case NAVI_TURN_ROUNDABOUT_ENTER: case NAVI_TURN_ROUNDABOUT_EXIT: turn = 7; break;
    case NAVI_TURN_SLIGHT_LEFT: case NAVI_TURN_SLIGHT_RIGHT: case NAVI_TURN_RAMP_LEFT: case NAVI_TURN_RAMP_RIGHT:
    case NAVI_TURN_EXIT_LEFT: case NAVI_TURN_EXIT_RIGHT: turn = 12; break;
    case NAVI_TURN_DESTINATION: case NAVI_TURN_DESTINATION_LEFT: case NAVI_TURN_DESTINATION_RIGHT: turn = 0; break;
    default: turn = v; break;
    }
    /* braking at 2 m/s2 toward the turn: v^2 = turn^2 + 2 a d */
    double d = MAX (0.0, n->s - s - 5);
    v = MIN (v, sqrt (turn * turn + 2 * 2.0 * d));
  }
  return v;
}

static gboolean
sim_tick (gpointer data)
{
  NaviPosition *self = data;
  NaviRoute *r = self->sim;
  double dt = SIM_STEP_MS / 1000.0 * self->sim_factor;
  double target = sim_target_speed (self, self->sim_s);
  /* 1.5 m/s2 up, 2.5 down */
  double dv = target - self->sim_v;
  self->sim_v += CLAMP (dv, -2.5 * dt, 1.5 * dt);
  self->sim_v = MAX (self->sim_v, 0.0);
  self->sim_s = MIN (self->sim_s + self->sim_v * dt, r->length);
  if (self->sim_s >= r->length)
    self->sim_v = 0;
  self->sim_since_fix += SIM_STEP_MS;
  if (self->sim_since_fix >= self->sim_interval) {
    self->sim_since_fix = 0;
    NaviFix f = { 0, 0, 5, self->sim_v * self->sim_factor, navi_path_heading_at (r->points, r->n_points, self->sim_s, 8),
                  g_get_monotonic_time (), NAVI_FIX_SIMULATED };
    navi_path_point_at (r->points, r->n_points, self->sim_s, &f.lat, &f.lng, &self->sim_hint);
    emit_fix (self, &f);
  }
  return G_SOURCE_CONTINUE;
}

void
navi_position_simulate (NaviPosition *self, NaviRoute *route, double speed_factor, guint interval_ms)
{
  g_clear_handle_id (&self->sim_timer, g_source_remove);
  g_clear_pointer (&self->sim, navi_route_unref);
  if (!route)
    return;
  self->sim = navi_route_ref (route);
  self->sim_factor = speed_factor > 0 ? speed_factor : 1;
  self->sim_interval = interval_ms ? interval_ms : 1000;
  self->sim_s = 0;
  self->sim_v = 0;
  self->sim_hint = 0;
  self->sim_since_fix = self->sim_interval; /* a fix at once */
  self->sim_timer = g_timeout_add (SIM_STEP_MS, sim_tick, self);
  sim_tick (self);
}

gboolean
navi_position_is_simulating (NaviPosition *self)
{
  return self->sim != NULL;
}

/* ---- GObject ---- */

static void
navi_position_dispose (GObject *object)
{
  NaviPosition *self = NAVI_POSITION (object);
  navi_position_stop (self);
  navi_position_simulate (self, NULL, 1, 0);
  G_OBJECT_CLASS (navi_position_parent_class)->dispose (object);
}

static void
navi_position_finalize (GObject *object)
{
  NaviPosition *self = NAVI_POSITION (object);
  g_free (self->token);
  G_OBJECT_CLASS (navi_position_parent_class)->finalize (object);
}

static void
navi_position_class_init (NaviPositionClass *klass)
{
  GObjectClass *oc = G_OBJECT_CLASS (klass);
  oc->dispose = navi_position_dispose;
  oc->finalize = navi_position_finalize;
  signals[SIGNAL_FIX] = g_signal_new ("fix", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                                      G_TYPE_NONE, 1, G_TYPE_POINTER);
  signals[SIGNAL_DENIED] = g_signal_new ("denied", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                                         G_TYPE_NONE, 0);
}

static void
navi_position_init (NaviPosition *self)
{
}

NaviPosition *
navi_position_new (void)
{
  return g_object_new (NAVI_TYPE_POSITION, NULL);
}
