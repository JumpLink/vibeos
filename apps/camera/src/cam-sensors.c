/* cam-sensors: see cam-sensors.h. */
#include "cam-sensors.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define TILT_POLL_MS     66       /* SENSOR_DELAY_UI */
#define THERMAL_POLL_S   5
#define LOW_BATTERY      15.0     /* % (Aperture's BatteryManager check) */

struct _CamSensors {
  GObject       parent;
  gboolean      active, locating, leveling;
  GCancellable *cancel;

  GDBusProxy   *accel;
  gboolean      claimed;
  int           orientation;

  GDBusConnection *session;
  guint         display_signal;
  int           display_rotation;

  char         *tilt_dir;       /* the raw IIO accelerometer, if readable */
  double        tilt_scale;
  double        matrix[9];
  double        tilt;
  guint         tilt_timer;

  GDBusProxy   *battery;
  gboolean      low, charging;

  CamThermal    thermal;
  guint         thermal_timer;

  GDBusConnection *system;
  gboolean      geo_available, geo_pending;
  char         *geo_client;
  guint         geo_signal;
  CamLocation   location;
};

G_DEFINE_FINAL_TYPE (CamSensors, cam_sensors, G_TYPE_OBJECT)

enum { PROP_0, PROP_ORIENTATION, PROP_DISPLAY_ROTATION, PROP_TILT, PROP_BATTERY, PROP_THERMAL, N_PROPS };
static GParamSpec *props[N_PROPS];

enum { SIG_LOCATION_REFUSED, N_SIGNALS };
static guint signals[N_SIGNALS];

/* ---- orientation (iio-sensor-proxy) ---------------------------------------------------------------------------- */

static void
read_orientation (CamSensors *self)
{
  g_autoptr(GVariant) value = self->accel ? g_dbus_proxy_get_cached_property (self->accel,
                                                                             "AccelerometerOrientation") : NULL;
  const char *name = value ? g_variant_get_string (value, NULL) : NULL;
  int orientation = self->orientation;

  /* the clockwise turn of the phone, as OrientationEventListener (90: the left edge up) */
  if (!g_strcmp0 (name, "normal"))
    orientation = 0;
  else if (!g_strcmp0 (name, "left-up"))
    orientation = 90;
  else if (!g_strcmp0 (name, "bottom-up"))
    orientation = 180;
  else if (!g_strcmp0 (name, "right-up"))
    orientation = 270;
  if (orientation != self->orientation)
    {
      self->orientation = orientation;
      g_object_notify_by_pspec (G_OBJECT (self), props[PROP_ORIENTATION]);
    }
}

static void
on_accel_changed (GDBusProxy *proxy, GVariant *changed, GStrv invalidated, gpointer data)
{
  read_orientation (data);
}

static void
claim (CamSensors *self, gboolean claim)
{
  g_autoptr(GVariant) has = NULL;

  if (!self->accel || claim == self->claimed)
    return;
  has = g_dbus_proxy_get_cached_property (self->accel, "HasAccelerometer");
  if (claim && !(has && g_variant_get_boolean (has)))
    return;
  self->claimed = claim;
  g_dbus_proxy_call (self->accel, claim ? "ClaimAccelerometer" : "ReleaseAccelerometer", NULL,
                     G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);
  if (claim)
    read_orientation (self);
}

static void
on_accel_proxy (GObject *object, GAsyncResult *result, gpointer data)
{
  g_autoptr(GError) error = NULL;
  GDBusProxy *proxy = g_dbus_proxy_new_for_bus_finish (result, &error);
  CamSensors *self;

  if (!proxy)
    return;
  self = data;
  self->accel = proxy;
  g_signal_connect (proxy, "g-properties-changed", G_CALLBACK (on_accel_changed), self);
  if (self->active)
    claim (self, TRUE);
}

/* ---- the screen's turn (Mutter's DisplayConfig) ---------------------------------------------------------------- */

/* GetCurrentState's logical monitors: (x, y, scale, transform, primary, monitors, properties). The transform is
 * Mutter's (wl_output's): 1 is a 90° turn, given to a phone with its left edge up (meta_orientation_to_transform),
 * so a quarter of it is the clockwise turn of the phone the screen is laid out for, the orientation's own unit.
 * The flipped transforms (4…7) count as their turn. GNOME Shell and Phosh both serve DisplayConfig; elsewhere the
 * screen is taken as upright. */
static void
on_display_state (GObject *object, GAsyncResult *result, gpointer data)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(GVariant) reply = g_dbus_connection_call_finish (G_DBUS_CONNECTION (object), result, &error);
  g_autoptr(GVariant) logical = NULL;
  CamSensors *self;
  GVariantIter iter;
  guint32 transform, chosen = 0;
  gboolean primary, found = FALSE;
  int rotation;

  if (!reply)
    return;
  self = data;
  logical = g_variant_get_child_value (reply, 2);
  g_variant_iter_init (&iter, logical);
  while (g_variant_iter_next (&iter, "(iiduba(ssss)@a{sv})", NULL, NULL, NULL, &transform, &primary, NULL, NULL))
    if (!found || primary)
      {
        chosen = transform;
        if (primary)
          break;
        found = TRUE;
      }
  rotation = (int) (chosen % 4) * 90;
  if (rotation != self->display_rotation)
    {
      self->display_rotation = rotation;
      g_object_notify_by_pspec (G_OBJECT (self), props[PROP_DISPLAY_ROTATION]);
    }
}

static void
read_display (CamSensors *self)
{
  g_dbus_connection_call (self->session, "org.gnome.Mutter.DisplayConfig", "/org/gnome/Mutter/DisplayConfig",
                          "org.gnome.Mutter.DisplayConfig", "GetCurrentState", NULL, NULL, G_DBUS_CALL_FLAGS_NONE,
                          -1, self->cancel, on_display_state, self);
}

static void
on_monitors_changed (GDBusConnection *connection, const char *sender, const char *path, const char *interface,
                     const char *signal, GVariant *parameters, gpointer data)
{
  read_display (data);
}

/* ---- tilt (a raw IIO accelerometer) ---------------------------------------------------------------------------- */

static double
read_number (const char *dir, const char *name, double fallback)
{
  g_autofree char *path = g_build_filename (dir, name, NULL);
  g_autofree char *text = NULL;

  if (!g_file_get_contents (path, &text, NULL, NULL))
    return fallback;
  return g_ascii_strtod (text, NULL);
}

static void
find_tilt_sensor (CamSensors *self)
{
  g_autoptr(GDir) dir = g_dir_open ("/sys/bus/iio/devices", 0, NULL);
  const char *name;

  while (dir && (name = g_dir_read_name (dir)))
    {
      g_autofree char *path = g_build_filename ("/sys/bus/iio/devices", name, NULL);
      g_autofree char *x = g_build_filename (path, "in_accel_x_raw", NULL);
      g_autofree char *matrix = NULL;
      g_autofree char *matrix_path = NULL;

      if (access (x, R_OK) != 0)
        continue;
      self->tilt_scale = read_number (path, "in_accel_scale", 1);
      /* the mount matrix turns the sensor's axes into the phone's (x right, y up, z out of the screen) */
      for (int i = 0; i < 9; i++)
        self->matrix[i] = i % 4 == 0;
      matrix_path = g_build_filename (path, "in_accel_mount_matrix", NULL);
      if (!g_file_get_contents (matrix_path, &matrix, NULL, NULL))
        {
          g_free (matrix_path);
          matrix_path = g_build_filename (path, "mount_matrix", NULL);
          g_file_get_contents (matrix_path, &matrix, NULL, NULL);
        }
      if (matrix)
        {
          g_auto(GStrv) parts = g_strsplit_set (matrix, ",;", -1);
          if (g_strv_length (parts) == 9)
            for (int i = 0; i < 9; i++)
              self->matrix[i] = g_ascii_strtod (parts[i], NULL);
        }
      self->tilt_dir = g_steal_pointer (&path);
      return;
    }
}

static gboolean
on_tilt_poll (gpointer data)
{
  CamSensors *self = data;
  double raw[3], v[3];

  raw[0] = read_number (self->tilt_dir, "in_accel_x_raw", 0) * self->tilt_scale;
  raw[1] = read_number (self->tilt_dir, "in_accel_y_raw", 0) * self->tilt_scale;
  raw[2] = read_number (self->tilt_dir, "in_accel_z_raw", 0) * self->tilt_scale;
  for (int i = 0; i < 3; i++)
    v[i] = self->matrix[i * 3] * raw[0] + self->matrix[i * 3 + 1] * raw[1] + self->matrix[i * 3 + 2] * raw[2];
  /* OrientationEventListener: X = -x, Y = -y; flat on a table (z dominating) gives no angle */
  double X = -v[0], Y = -v[1], Z = -v[2];
  if ((X * X + Y * Y) * 4 >= Z * Z)
    {
      double angle = 90 - atan2 (-Y, X) * 180 / G_PI;
      angle = fmod (angle + 720, 360);
      if (fabs (angle - self->tilt) > 0.1)
        {
          self->tilt = angle;
          g_object_notify_by_pspec (G_OBJECT (self), props[PROP_TILT]);
        }
    }
  return G_SOURCE_CONTINUE;
}

static void
sync_tilt (CamSensors *self)
{
  gboolean want = self->active && self->leveling && self->tilt_dir;

  if (want && !self->tilt_timer)
    self->tilt_timer = g_timeout_add (TILT_POLL_MS, on_tilt_poll, self);
  else if (!want)
    g_clear_handle_id (&self->tilt_timer, g_source_remove);
}

/* ---- battery (UPower) ------------------------------------------------------------------------------------------ */

static void
read_battery (CamSensors *self)
{
  g_autoptr(GVariant) present = g_dbus_proxy_get_cached_property (self->battery, "IsPresent");
  g_autoptr(GVariant) percent = g_dbus_proxy_get_cached_property (self->battery, "Percentage");
  g_autoptr(GVariant) state = g_dbus_proxy_get_cached_property (self->battery, "State");
  gboolean low = present && g_variant_get_boolean (present) && percent && g_variant_get_double (percent) <= LOW_BATTERY;
  /* UPower's states: 1 charging */
  gboolean charging = state && g_variant_get_uint32 (state) == 1;

  if (low != self->low || charging != self->charging)
    {
      self->low = low;
      self->charging = charging;
      g_object_notify_by_pspec (G_OBJECT (self), props[PROP_BATTERY]);
    }
}

static void
on_battery_changed (GDBusProxy *proxy, GVariant *changed, GStrv invalidated, gpointer data)
{
  read_battery (data);
}

static void
on_battery_proxy (GObject *object, GAsyncResult *result, gpointer data)
{
  GDBusProxy *proxy = g_dbus_proxy_new_for_bus_finish (result, NULL);
  CamSensors *self;

  if (!proxy)
    return;
  self = data;
  self->battery = proxy;
  g_signal_connect (proxy, "g-properties-changed", G_CALLBACK (on_battery_changed), self);
  read_battery (self);
}

/* ---- thermal zones --------------------------------------------------------------------------------------------- */

/* One zone against its own trip points: the first passive one is where the kernel starts throttling (moderate),
 * the last passive or the hot one where it throttles hard (severe), ten degrees short of critical is critical, and
 * critical itself is an emergency. Android asks the thermal HAL the same question; this is the kernel's answer. */
static CamThermal
zone_status (const char *zone)
{
  double temp = read_number (zone, "temp", -1);
  double first_passive = G_MAXDOUBLE, last_passive = G_MAXDOUBLE, hot = G_MAXDOUBLE, critical = G_MAXDOUBLE;

  if (temp < 0)
    return CAM_THERMAL_NONE;
  for (int i = 0; i < 32; i++)
    {
      g_autofree char *type_name = g_strdup_printf ("trip_point_%d_type", i);
      g_autofree char *temp_name = g_strdup_printf ("trip_point_%d_temp", i);
      g_autofree char *type_path = g_build_filename (zone, type_name, NULL);
      g_autofree char *type = NULL;
      double trip;

      if (!g_file_get_contents (type_path, &type, NULL, NULL))
        break;
      g_strstrip (type);
      trip = read_number (zone, temp_name, G_MAXDOUBLE);
      if (trip <= 0)
        continue;
      if (!strcmp (type, "passive"))
        {
          first_passive = MIN (first_passive, trip);
          last_passive = last_passive == G_MAXDOUBLE ? trip : MAX (last_passive, trip);
        }
      else if (!strcmp (type, "hot"))
        hot = MIN (hot, trip);
      else if (!strcmp (type, "critical"))
        critical = MIN (critical, trip);
    }
  if (temp >= critical)
    return CAM_THERMAL_EMERGENCY;
  if (critical != G_MAXDOUBLE && temp >= critical - 10000)
    return CAM_THERMAL_CRITICAL;
  if (temp >= hot || (last_passive != first_passive && temp >= last_passive))
    return CAM_THERMAL_SEVERE;
  if (temp >= first_passive)
    return CAM_THERMAL_MODERATE;
  return CAM_THERMAL_NONE;
}

static gboolean
on_thermal_poll (gpointer data)
{
  CamSensors *self = data;
  g_autoptr(GDir) dir = g_dir_open ("/sys/class/thermal", 0, NULL);
  CamThermal worst = CAM_THERMAL_NONE;
  const char *name;

  while (dir && (name = g_dir_read_name (dir)))
    if (g_str_has_prefix (name, "thermal_zone"))
      {
        g_autofree char *zone = g_build_filename ("/sys/class/thermal", name, NULL);
        worst = MAX (worst, zone_status (zone));
      }
  if (worst != self->thermal)
    {
      self->thermal = worst;
      g_object_notify_by_pspec (G_OBJECT (self), props[PROP_THERMAL]);
    }
  return G_SOURCE_CONTINUE;
}

/* ---- location (GeoClue2) --------------------------------------------------------------------------------------- */

static GVariant *
get_property (CamSensors *self, const char *path, const char *iface, const char *name)
{
  g_autoptr(GVariant) reply = g_dbus_connection_call_sync (self->system, "org.freedesktop.GeoClue2", path,
                                                           "org.freedesktop.DBus.Properties", "Get",
                                                           g_variant_new ("(ss)", iface, name),
                                                           G_VARIANT_TYPE ("(v)"), G_DBUS_CALL_FLAGS_NONE, 2000,
                                                           NULL, NULL);
  GVariant *value = NULL;

  if (reply)
    g_variant_get (reply, "(v)", &value);
  return value;
}

static void
on_location (GDBusConnection *bus, const char *sender, const char *path, const char *iface, const char *signal,
             GVariant *params, gpointer data)
{
  CamSensors *self = data;
  const char *new_path;
  g_autoptr(GVariant) lat = NULL, lon = NULL, alt = NULL, stamp = NULL;

  g_variant_get (params, "(&o&o)", NULL, &new_path);
  lat = get_property (self, new_path, "org.freedesktop.GeoClue2.Location", "Latitude");
  lon = get_property (self, new_path, "org.freedesktop.GeoClue2.Location", "Longitude");
  alt = get_property (self, new_path, "org.freedesktop.GeoClue2.Location", "Altitude");
  stamp = get_property (self, new_path, "org.freedesktop.GeoClue2.Location", "Timestamp");
  if (!lat || !lon)
    return;
  self->location.valid = TRUE;
  self->location.latitude = g_variant_get_double (lat);
  self->location.longitude = g_variant_get_double (lon);
  self->location.altitude = alt ? g_variant_get_double (alt) : -G_MAXDOUBLE;
  self->location.time = g_get_real_time () / G_USEC_PER_SEC;
  if (stamp)
    {
      guint64 seconds = 0, micro = 0;
      g_variant_get (stamp, "(tt)", &seconds, &micro);
      if (seconds)
        self->location.time = seconds;
    }
}

static void
set_client_property (CamSensors *self, const char *name, GVariant *value)
{
  g_dbus_connection_call (self->system, "org.freedesktop.GeoClue2", self->geo_client,
                          "org.freedesktop.DBus.Properties", "Set",
                          g_variant_new ("(ssv)", "org.freedesktop.GeoClue2.Client", name, value), NULL,
                          G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);
}

static void sync_location (CamSensors *self);

static void
drop_client (CamSensors *self)
{
  if (self->geo_signal)
    g_dbus_connection_signal_unsubscribe (self->system, self->geo_signal);
  self->geo_signal = 0;
  g_clear_pointer (&self->geo_client, g_free);
}

static void
on_geo_started (GObject *object, GAsyncResult *result, gpointer data)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(GVariant) reply = g_dbus_connection_call_finish (G_DBUS_CONNECTION (object), result, &error);
  CamSensors *self = data;

  if (reply || g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    return;
  /* the agent (the shell's location prompt) or the configuration said no: Aperture's refused permission */
  g_message ("location: %s", error->message);
  drop_client (self);
  g_signal_emit (self, signals[SIG_LOCATION_REFUSED], 0);
}

static void
on_geo_client (GObject *object, GAsyncResult *result, gpointer data)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(GVariant) reply = g_dbus_connection_call_finish (G_DBUS_CONNECTION (object), result, &error);
  CamSensors *self;

  if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    return;
  self = data;
  self->geo_pending = FALSE;
  if (!reply)
    {
      g_message ("location: %s", error->message);
      g_signal_emit (self, signals[SIG_LOCATION_REFUSED], 0);
      return;
    }
  g_variant_get (reply, "(o)", &self->geo_client);
  self->geo_signal = g_dbus_connection_signal_subscribe (self->system, "org.freedesktop.GeoClue2",
                                                         "org.freedesktop.GeoClue2.Client", "LocationUpdated",
                                                         self->geo_client, NULL, G_DBUS_SIGNAL_FLAGS_NONE,
                                                         on_location, self, NULL);
  set_client_property (self, "DesktopId", g_variant_new_string ("de.yesman.Camera"));
  /* GCLUE_ACCURACY_LEVEL_EXACT; updates every metre or second, as Aperture's location request */
  set_client_property (self, "RequestedAccuracyLevel", g_variant_new_uint32 (8));
  set_client_property (self, "DistanceThreshold", g_variant_new_uint32 (1));
  set_client_property (self, "TimeThreshold", g_variant_new_uint32 (1));
  g_dbus_connection_call (self->system, "org.freedesktop.GeoClue2", self->geo_client,
                          "org.freedesktop.GeoClue2.Client", "Start", NULL, NULL, G_DBUS_CALL_FLAGS_NONE, -1,
                          self->cancel, on_geo_started, self);
  /* turned off while the client was being made */
  sync_location (self);
}

static void
sync_location (CamSensors *self)
{
  gboolean want = self->active && self->locating && self->system;

  if (want && !self->geo_client && !self->geo_pending)
    {
      self->geo_pending = TRUE;
      g_dbus_connection_call (self->system, "org.freedesktop.GeoClue2", "/org/freedesktop/GeoClue2/Manager",
                              "org.freedesktop.GeoClue2.Manager", "GetClient", NULL, G_VARIANT_TYPE ("(o)"),
                              G_DBUS_CALL_FLAGS_NONE, -1, self->cancel, on_geo_client, self);
    }
  else if (!want && self->geo_client)
    {
      g_dbus_connection_call (self->system, "org.freedesktop.GeoClue2", self->geo_client,
                              "org.freedesktop.GeoClue2.Client", "Stop", NULL, NULL, G_DBUS_CALL_FLAGS_NONE, -1,
                              NULL, NULL, NULL);
      drop_client (self);
    }
}

/* GeoClue running or startable on the system bus */
static gboolean
find_geoclue (CamSensors *self)
{
  const char *methods[] = { "ListNames", "ListActivatableNames" };

  if (!self->system)
    return FALSE;
  for (int i = 0; i < 2; i++)
    {
      g_autoptr(GVariant) reply = g_dbus_connection_call_sync (self->system, "org.freedesktop.DBus",
                                                               "/org/freedesktop/DBus", "org.freedesktop.DBus",
                                                               methods[i], NULL, G_VARIANT_TYPE ("(as)"),
                                                               G_DBUS_CALL_FLAGS_NONE, 2000, NULL, NULL);
      g_autofree const char **names = NULL;

      if (!reply)
        continue;
      g_variant_get (reply, "(^a&s)", &names);
      if (g_strv_contains (names, "org.freedesktop.GeoClue2"))
        return TRUE;
    }
  return FALSE;
}

/* ---- the object ------------------------------------------------------------------------------------------------ */

static void
cam_sensors_dispose (GObject *object)
{
  CamSensors *self = CAM_SENSORS (object);

  cam_sensors_set_active (self, FALSE);
  g_cancellable_cancel (self->cancel);
  g_clear_object (&self->cancel);
  if (self->display_signal)
    g_dbus_connection_signal_unsubscribe (self->session, self->display_signal);
  self->display_signal = 0;
  g_clear_object (&self->session);
  g_clear_object (&self->accel);
  g_clear_object (&self->battery);
  g_clear_object (&self->system);
  g_clear_pointer (&self->tilt_dir, g_free);
  G_OBJECT_CLASS (cam_sensors_parent_class)->dispose (object);
}

static void
cam_sensors_get_property (GObject *object, guint id, GValue *value, GParamSpec *pspec)
{
  CamSensors *self = CAM_SENSORS (object);

  switch (id)
    {
    case PROP_ORIENTATION:
      g_value_set_int (value, cam_sensors_get_orientation (self));
      break;
    case PROP_DISPLAY_ROTATION:
      g_value_set_int (value, cam_sensors_get_display_rotation (self));
      break;
    case PROP_TILT:
      g_value_set_double (value, cam_sensors_get_tilt (self));
      break;
    case PROP_BATTERY:
      g_value_set_boolean (value, cam_sensors_get_battery_low (self));
      break;
    case PROP_THERMAL:
      g_value_set_int (value, cam_sensors_get_thermal (self));
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, id, pspec);
    }
}

static void
cam_sensors_class_init (CamSensorsClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = cam_sensors_dispose;
  object_class->get_property = cam_sensors_get_property;
  props[PROP_ORIENTATION] = g_param_spec_int ("orientation", NULL, NULL, 0, 270, 0, G_PARAM_READABLE);
  props[PROP_DISPLAY_ROTATION] = g_param_spec_int ("display-rotation", NULL, NULL, 0, 270, 0, G_PARAM_READABLE);
  props[PROP_TILT] = g_param_spec_double ("tilt", NULL, NULL, -1, 360, 0, G_PARAM_READABLE);
  props[PROP_BATTERY] = g_param_spec_boolean ("battery", NULL, NULL, FALSE, G_PARAM_READABLE);
  props[PROP_THERMAL] = g_param_spec_int ("thermal", NULL, NULL, 0, CAM_THERMAL_SHUTDOWN, 0, G_PARAM_READABLE);
  g_object_class_install_properties (object_class, N_PROPS, props);
  signals[SIG_LOCATION_REFUSED] = g_signal_new ("location-refused", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0,
                                                NULL, NULL, NULL, G_TYPE_NONE, 0);
}

static void
cam_sensors_init (CamSensors *self)
{
  self->cancel = g_cancellable_new ();
  self->system = g_bus_get_sync (G_BUS_TYPE_SYSTEM, NULL, NULL);
  self->geo_available = find_geoclue (self);
  self->tilt = NAN;
  g_dbus_proxy_new_for_bus (G_BUS_TYPE_SYSTEM, G_DBUS_PROXY_FLAGS_NONE, NULL, "net.hadess.SensorProxy",
                            "/net/hadess/SensorProxy", "net.hadess.SensorProxy", self->cancel, on_accel_proxy, self);
  g_dbus_proxy_new_for_bus (G_BUS_TYPE_SYSTEM, G_DBUS_PROXY_FLAGS_NONE, NULL, "org.freedesktop.UPower",
                            "/org/freedesktop/UPower/devices/DisplayDevice", "org.freedesktop.UPower.Device",
                            self->cancel, on_battery_proxy, self);
  find_tilt_sensor (self);
  self->session = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, NULL);
  if (self->session)
    {
      self->display_signal = g_dbus_connection_signal_subscribe (self->session, NULL,
                                                                 "org.gnome.Mutter.DisplayConfig", "MonitorsChanged",
                                                                 "/org/gnome/Mutter/DisplayConfig", NULL,
                                                                 G_DBUS_SIGNAL_FLAGS_NONE, on_monitors_changed, self,
                                                                 NULL);
      read_display (self);
    }
}

CamSensors *
cam_sensors_new (void)
{
  return g_object_new (CAM_TYPE_SENSORS, NULL);
}

void
cam_sensors_set_active (CamSensors *self, gboolean active)
{
  if (active == self->active)
    return;
  self->active = active;
  claim (self, active);
  if (active && !self->thermal_timer)
    {
      self->thermal_timer = g_timeout_add_seconds (THERMAL_POLL_S, on_thermal_poll, self);
      on_thermal_poll (self);
    }
  else if (!active)
    g_clear_handle_id (&self->thermal_timer, g_source_remove);
  sync_tilt (self);
  sync_location (self);
}

void
cam_sensors_set_locating (CamSensors *self, gboolean locating)
{
  self->locating = locating;
  if (!locating)
    self->location.valid = FALSE;
  sync_location (self);
}

void
cam_sensors_set_leveling (CamSensors *self, gboolean leveling)
{
  self->leveling = leveling;
  sync_tilt (self);
}

int
cam_sensors_get_orientation (CamSensors *self)
{
  return self->orientation;
}

int
cam_sensors_get_display_rotation (CamSensors *self)
{
  return self->display_rotation;
}

gboolean
cam_sensors_has_tilt (CamSensors *self)
{
  return self->tilt_dir != NULL;
}

double
cam_sensors_get_tilt (CamSensors *self)
{
  return self->tilt;
}

gboolean
cam_sensors_get_battery_low (CamSensors *self)
{
  return self->low;
}

gboolean
cam_sensors_get_charging (CamSensors *self)
{
  return self->charging;
}

CamThermal
cam_sensors_get_thermal (CamSensors *self)
{
  return self->thermal;
}

gboolean
cam_sensors_can_locate (CamSensors *self)
{
  return self->geo_available;
}

CamLocation
cam_sensors_get_location (CamSensors *self)
{
  return self->location;
}
