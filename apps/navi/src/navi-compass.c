/* NaviCompass: which way the screen's top points, from iio-sensor-proxy's compass (see navi-compass.h). */
#include "navi-compass.h"

#include <math.h>
#include <string.h>

#define SP_NAME          "net.hadess.SensorProxy"
#define SP_PATH          "/net/hadess/SensorProxy"
#define SP_IFACE         "net.hadess.SensorProxy"
#define SP_COMPASS_PATH  "/net/hadess/SensorProxy/Compass"
#define SP_COMPASS_IFACE "net.hadess.SensorProxy.Compass"
#define DEAD_BAND        1.0   /* degrees: the magnetometer's jitter is a few hundredths; a turn by hand is more */

struct _NaviCompass {
  GObject       parent;
  GDBusProxy   *compass, *accel;
  GCancellable *cancel;
  gboolean      started;          /* the proxies were asked for */
  gboolean      active, landscape;
  gboolean      claimed_compass, claimed_accel;
  gboolean      have_device;
  double        device_heading;   /* the device's top edge, from magnetic north */
  char         *orientation;      /* AccelerometerOrientation */
  gboolean      have_heading;
  double        heading;          /* what was last told ("changed") */
};

enum { SIG_CHANGED, N_SIGNALS };
static guint signals[N_SIGNALS];

G_DEFINE_FINAL_TYPE (NaviCompass, navi_compass, G_TYPE_OBJECT)

/* ---- the arithmetic ---- */

int
navi_compass_screen_turn (gboolean landscape, const char *orientation)
{
  /* portrait: the device's own top, also when the accelerometer says "bottom-up" (with the rotation locked the
   * screen does not turn, and a portrait screen does not say which way up it is) */
  if (!landscape)
    return 0;
  /* landscape: a phone turned on its side; "normal" and "bottom-up" are a tablet whose own top is a long edge */
  static const struct { const char *name; int turn; } turns[] = {
    { "normal", 0 }, { "right-up", 90 }, { "bottom-up", 180 }, { "left-up", 270 },
  };
  for (guint i = 0; orientation && i < G_N_ELEMENTS (turns); i++)
    if (!strcmp (orientation, turns[i].name))
      return turns[i].turn;
  return -1;   /* "undefined": the accelerometer has not said yet */
}

double
navi_compass_screen_heading (double device_heading, int turn)
{
  double h = fmod (device_heading + turn, 360);
  return h < 0 ? h + 360 : h;
}

NaviHeadingSource
navi_heading_pick (double speed, double course, double moving, gboolean have_compass, double compass,
                   double *heading)
{
  if (speed > moving && course >= 0) {
    *heading = course;
    return NAVI_HEADING_COURSE;
  }
  if (have_compass) {
    *heading = compass;
    return NAVI_HEADING_COMPASS;
  }
  return NAVI_HEADING_NONE;
}

/* ---- the heading ---- */

static void
update (NaviCompass *self)
{
  int turn = navi_compass_screen_turn (self->landscape, self->orientation);
  gboolean have = self->active && self->have_device && turn >= 0;
  double h = have ? navi_compass_screen_heading (self->device_heading, turn) : 0;
  if (have == self->have_heading && (!have || fabs (remainder (h - self->heading, 360)) < DEAD_BAND))
    return;
  self->have_heading = have;
  self->heading = h;
  g_signal_emit (self, signals[SIG_CHANGED], 0);
}

static gboolean
cached_bool (GDBusProxy *proxy, const char *name)
{
  g_autoptr (GVariant) v = proxy ? g_dbus_proxy_get_cached_property (proxy, name) : NULL;
  return v && g_variant_is_of_type (v, G_VARIANT_TYPE_BOOLEAN) && g_variant_get_boolean (v);
}

static void
read_compass (NaviCompass *self)
{
  g_autoptr (GVariant) v = g_dbus_proxy_get_cached_property (self->compass, "CompassHeading");
  double h = v && g_variant_is_of_type (v, G_VARIANT_TYPE_DOUBLE) ? g_variant_get_double (v) : -1;
  self->have_device = self->claimed_compass && h >= 0 && h <= 360;
  self->device_heading = self->have_device ? h : 0;
}

static void
read_accel (NaviCompass *self)
{
  g_autoptr (GVariant) v = g_dbus_proxy_get_cached_property (self->accel, "AccelerometerOrientation");
  g_free (self->orientation);
  self->orientation = v && g_variant_is_of_type (v, G_VARIANT_TYPE_STRING) ? g_variant_dup_string (v, NULL) : NULL;
}

static void
on_call_done (GObject *source, GAsyncResult *res, gpointer data)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (GVariant) ret = g_dbus_proxy_call_finish (G_DBUS_PROXY (source), res, &error);
  if (error && !g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    g_message ("compass: %s", error->message);
}

static void
call (GDBusProxy *proxy, const char *method)
{
  g_dbus_proxy_call (proxy, method, NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, on_call_done, NULL);
}

/* the claims follow what is wanted: the compass while active, the accelerometer while active in landscape */
static void
sync_claims (NaviCompass *self)
{
  gboolean want = self->active && cached_bool (self->compass, "HasCompass");
  if (want != self->claimed_compass) {
    call (self->compass, want ? "ClaimCompass" : "ReleaseCompass");
    self->claimed_compass = want;
  }
  want = self->active && self->landscape && cached_bool (self->accel, "HasAccelerometer");
  if (want != self->claimed_accel) {
    call (self->accel, want ? "ClaimAccelerometer" : "ReleaseAccelerometer");
    self->claimed_accel = want;
  }
  if (self->compass)
    read_compass (self);
  if (self->accel)
    read_accel (self);
  update (self);
}

static void
on_props (GDBusProxy *proxy, GVariant *changed, GStrv invalidated, NaviCompass *self)
{
  if (proxy == self->compass)
    read_compass (self);
  else
    read_accel (self);
  update (self);
}

static void
on_owner (GDBusProxy *proxy, GParamSpec *pspec, NaviCompass *self)
{
  /* iio-sensor-proxy restarted: its claims went with it */
  g_autofree char *owner = g_dbus_proxy_get_name_owner (proxy);
  if (proxy == self->compass)
    self->claimed_compass = FALSE;
  else
    self->claimed_accel = FALSE;
  if (owner)
    sync_claims (self);
}

static void
on_proxy (GObject *source, GAsyncResult *res, gpointer data)
{
  g_autoptr (GError) error = NULL;
  GDBusProxy *proxy = g_dbus_proxy_new_for_bus_finish (res, &error);
  if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    return;
  NaviCompass *self = data;
  if (!proxy) {
    g_message ("compass: no sensor service (%s)", error->message);
    return;
  }
  if (!strcmp (g_dbus_proxy_get_object_path (proxy), SP_COMPASS_PATH))
    self->compass = proxy;
  else
    self->accel = proxy;
  g_signal_connect (proxy, "g-properties-changed", G_CALLBACK (on_props), self);
  g_signal_connect (proxy, "notify::g-name-owner", G_CALLBACK (on_owner), self);
  sync_claims (self);
}

static void
start (NaviCompass *self)
{
  if (self->started)
    return;
  self->started = TRUE;
  g_dbus_proxy_new_for_bus (G_BUS_TYPE_SYSTEM, G_DBUS_PROXY_FLAGS_NONE, NULL, SP_NAME, SP_COMPASS_PATH,
                            SP_COMPASS_IFACE, self->cancel, on_proxy, self);
  g_dbus_proxy_new_for_bus (G_BUS_TYPE_SYSTEM, G_DBUS_PROXY_FLAGS_NONE, NULL, SP_NAME, SP_PATH, SP_IFACE,
                            self->cancel, on_proxy, self);
}

void
navi_compass_set_active (NaviCompass *self, gboolean active)
{
  if (self->active == !!active)
    return;
  self->active = !!active;
  if (self->active)
    start (self);
  sync_claims (self);
}

void
navi_compass_set_landscape (NaviCompass *self, gboolean landscape)
{
  if (self->landscape == !!landscape)
    return;
  self->landscape = !!landscape;
  sync_claims (self);
}

gboolean
navi_compass_get_heading (NaviCompass *self, double *heading)
{
  if (self->have_heading)
    *heading = self->heading;
  return self->have_heading;
}

/* ---- GObject ---- */

static void
navi_compass_dispose (GObject *object)
{
  NaviCompass *self = NAVI_COMPASS (object);
  g_cancellable_cancel (self->cancel);
  /* the claims go back now (iio-sensor-proxy would also drop them when the process leaves the bus) */
  if (self->claimed_compass)
    call (self->compass, "ReleaseCompass");
  if (self->claimed_accel)
    call (self->accel, "ReleaseAccelerometer");
  self->claimed_compass = self->claimed_accel = FALSE;
  if (self->compass)
    g_signal_handlers_disconnect_by_data (self->compass, self);
  if (self->accel)
    g_signal_handlers_disconnect_by_data (self->accel, self);
  g_clear_object (&self->compass);
  g_clear_object (&self->accel);
  g_clear_object (&self->cancel);
  G_OBJECT_CLASS (navi_compass_parent_class)->dispose (object);
}

static void
navi_compass_finalize (GObject *object)
{
  g_free (NAVI_COMPASS (object)->orientation);
  G_OBJECT_CLASS (navi_compass_parent_class)->finalize (object);
}

static void
navi_compass_class_init (NaviCompassClass *klass)
{
  GObjectClass *oc = G_OBJECT_CLASS (klass);
  oc->dispose = navi_compass_dispose;
  oc->finalize = navi_compass_finalize;
  signals[SIG_CHANGED] = g_signal_new ("changed", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                                       G_TYPE_NONE, 0);
}

static void
navi_compass_init (NaviCompass *self)
{
  self->cancel = g_cancellable_new ();
}

NaviCompass *
navi_compass_new (void)
{
  return g_object_new (NAVI_TYPE_COMPASS, NULL);
}
