/* The alarms in the Clock's service. From login, whether or not the Clock app is open, it:
 *
 *  - keeps the alarms (the store, ~/.local/share/vibeos.Clock/alarms.ini) and takes each instance through its
 *    states (alarm-engine.c) when they are due;
 *  - wakes the phone for them: vibeos-wakeup arms the RTC for the next change, and for the next alarm, which it also
 *    shows in the status bar (its NextAlarmClock);
 *  - rings: the sound and vibration (alarm-klaxon.c), the notification with Snooze and Dismiss, and the ringing
 *    screen (the app as vibeos.Clock.Ringing), full screen over the lock screen when the phone was asleep or locked;
 *  - serves the Clock app and the ringing screen over D-Bus (vibeos.Clock.Alarms, below): the app never writes
 *    the store itself. */
#include "alarm-service.h"
#include "alarm-engine.h"
#include "alarm-klaxon.h"
#include "alarm-notifications.h"
#include "elapsed.h"
#include "phone.h"

#include <errno.h>
#include <glib/gi18n.h>
#include <glib/gstdio.h>
#include <glib-unix.h>
#include <sys/timerfd.h>
#include <unistd.h>

#define INTERFACE "vibeos.Clock.Alarms"
#define OBJECT_PATH "/vibeos/Clock/Alarms"
#define APP_ID "vibeos.Clock"
#define RINGING_APP_ID "vibeos.Clock.Ringing"

static const char introspection[] =
  "<node>"
  "  <interface name='vibeos.Clock.Alarms'>"
  /* every alarm, with its coming instance if it has one: id u, hour i, minute i, days u (Monday bit 0), enabled b,
   * vibrate b, label s, ringtone s (absent: the default sound; empty: silent), delete-after-use b; instance u,
   * instance-state i (AlarmState), instance-time x (Unix) */
  "    <method name='GetAlarms'><arg name='alarms' type='aa{sv}' direction='out'/></method>"
  /* a new alarm from those keys (id and the instance ones aside); its id */
  "    <method name='AddAlarm'>"
  "      <arg name='alarm' type='a{sv}' direction='in'/><arg name='id' type='u' direction='out'/>"
  "    </method>"
  /* the keys given change, the others stay; "ringtone" as "default" goes back to the default sound */
  "    <method name='UpdateAlarm'>"
  "      <arg name='id' type='u' direction='in'/><arg name='changes' type='a{sv}' direction='in'/>"
  "    </method>"
  "    <method name='DeleteAlarm'><arg name='id' type='u' direction='in'/></method>"
  "    <method name='Snooze'><arg name='instance' type='u' direction='in'/></method>"
  /* a ringing, snoozed or missed instance is over; one still to come is skipped ("Dismiss now") */
  "    <method name='Dismiss'><arg name='instance' type='u' direction='in'/></method>"
  /* the alarms or their instances changed: GetAlarms again */
  "    <signal name='Changed'/>"
  /* the instance ringing now (instance u, label s, time x, alarm u), or empty */
  "    <property name='Ringing' type='a{sv}' access='read'/>"
  /* when the next alarm goes off (Unix), or 0 */
  "    <property name='NextAlarm' type='x' access='read'/>"
  "  </interface>"
  "</node>";

struct _AlarmService {
  GSettings *settings;
  char *store_path;
  AlarmEngine *engine;
  AlarmKlaxon *klaxon;
  AlarmNotifications *notifications;
  gboolean dirty;               /* the store changed since it was saved */

  GDBusConnection *session, *system;
  GDBusNodeInfo *node;
  guint registration;

  int timer_fd;                 /* CLOCK_REALTIME, at the next change; cancelled when the clock is set */
  gint64 clock_offset;          /* the wall clock less the elapsed one, to tell a clock set from a resume */
  GFileMonitor *zone_monitor;   /* /etc/localtime: the time zone changed */
  gint64 sent_alarm, sent_change;       /* what vibeos-wakeup was last told */
  guint wakeup_watch;

  guint ringing;                /* the ringing instance, or 0 */
  int sleep_inhibitor;          /* logind's sleep lock while it rings, or -1 */
  gint64 published_next;
  guint published_ringing;
};

typedef struct _AlarmService Service;

static void publish (Service *self);

/* --- the alarms as D-Bus dictionaries */

static GVariant *
alarm_to_variant (Service *self, const Alarm *alarm)
{
  AlarmStore *store = alarm_engine_get_store (self->engine);
  AlarmInstance *instance = alarm_store_instance_of (store, alarm->id);
  GVariantBuilder dict;

  g_variant_builder_init (&dict, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_add (&dict, "{sv}", "id", g_variant_new_uint32 (alarm->id));
  g_variant_builder_add (&dict, "{sv}", "hour", g_variant_new_int32 (alarm->hour));
  g_variant_builder_add (&dict, "{sv}", "minute", g_variant_new_int32 (alarm->minute));
  g_variant_builder_add (&dict, "{sv}", "days", g_variant_new_uint32 (alarm->days));
  g_variant_builder_add (&dict, "{sv}", "enabled", g_variant_new_boolean (alarm->enabled));
  g_variant_builder_add (&dict, "{sv}", "vibrate", g_variant_new_boolean (alarm->vibrate));
  g_variant_builder_add (&dict, "{sv}", "label", g_variant_new_string (alarm->label ? alarm->label : ""));
  if (alarm->ringtone)
    g_variant_builder_add (&dict, "{sv}", "ringtone", g_variant_new_string (alarm->ringtone));
  g_variant_builder_add (&dict, "{sv}", "delete-after-use", g_variant_new_boolean (alarm->delete_after_use));
  if (instance)
    {
      g_autoptr(GDateTime) time = alarm_instance_time (instance);

      g_variant_builder_add (&dict, "{sv}", "instance", g_variant_new_uint32 (instance->id));
      g_variant_builder_add (&dict, "{sv}", "instance-state", g_variant_new_int32 (instance->state));
      g_variant_builder_add (&dict, "{sv}", "instance-time", g_variant_new_int64 (g_date_time_to_unix (time)));
    }
  return g_variant_builder_end (&dict);
}

/* the keys of @changes into @alarm; FALSE (and @error) for a value out of range */
static gboolean
alarm_apply (Alarm *alarm, GVariant *changes, GError **error)
{
  GVariantIter iter;
  const char *key;
  GVariant *value;

  g_variant_iter_init (&iter, changes);
  while (g_variant_iter_loop (&iter, "{&sv}", &key, &value))
    {
      if (g_str_equal (key, "hour") && g_variant_is_of_type (value, G_VARIANT_TYPE_INT32))
        alarm->hour = g_variant_get_int32 (value);
      else if (g_str_equal (key, "minute") && g_variant_is_of_type (value, G_VARIANT_TYPE_INT32))
        alarm->minute = g_variant_get_int32 (value);
      else if (g_str_equal (key, "days") && g_variant_is_of_type (value, G_VARIANT_TYPE_UINT32))
        alarm->days = g_variant_get_uint32 (value) & ALARM_EVERY_DAY;
      else if (g_str_equal (key, "enabled") && g_variant_is_of_type (value, G_VARIANT_TYPE_BOOLEAN))
        alarm->enabled = g_variant_get_boolean (value);
      else if (g_str_equal (key, "vibrate") && g_variant_is_of_type (value, G_VARIANT_TYPE_BOOLEAN))
        alarm->vibrate = g_variant_get_boolean (value);
      else if (g_str_equal (key, "delete-after-use") && g_variant_is_of_type (value, G_VARIANT_TYPE_BOOLEAN))
        alarm->delete_after_use = g_variant_get_boolean (value);
      else if (g_str_equal (key, "label") && g_variant_is_of_type (value, G_VARIANT_TYPE_STRING))
        {
          g_free (alarm->label);
          alarm->label = g_variant_dup_string (value, NULL);
        }
      else if (g_str_equal (key, "ringtone") && g_variant_is_of_type (value, G_VARIANT_TYPE_STRING))
        {
          g_free (alarm->ringtone);
          alarm->ringtone = g_str_equal (g_variant_get_string (value, NULL), "default")
                            ? NULL : g_variant_dup_string (value, NULL);
        }
      else
        {
          g_set_error (error, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS, "bad key or type: %s", key);
          return FALSE;
        }
    }
  if (alarm->hour < 0 || alarm->hour > 23 || alarm->minute < 0 || alarm->minute > 59)
    {
      g_set_error (error, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS, "no such time: %d:%d", alarm->hour,
                   alarm->minute);
      return FALSE;
    }
  return TRUE;
}

/* --- ringing */

static void
start_ringing (Service *self, AlarmInstance *instance)
{
  self->ringing = instance->id;
  /* logind keeps the phone from sleeping while it rings: vibeos-wakeup's hold on waking lasts only 30 s */
  if (self->sleep_inhibitor < 0)
    self->sleep_inhibitor = phone_inhibit_sleep (self->system, "An alarm is ringing");
  alarm_klaxon_start (self->klaxon, instance->ringtone, instance->vibrate);
  /* full screen: the ringing screen over the lock screen if the phone is asleep or locked; in use, the notification's
   * heads-up is the alarm */
  phone_show_full_screen (self->session, RINGING_APP_ID);
}

static void
stop_ringing (Service *self)
{
  self->ringing = 0;
  alarm_klaxon_stop (self->klaxon);
  phone_allow_sleep (&self->sleep_inhibitor);
}

/* --- the engine's changes */

static void
on_instance_changed (AlarmEngine *engine, guint id, int state, Service *self)
{
  AlarmInstance *instance = alarm_store_find_instance (alarm_engine_get_store (engine), id);

  /* (one ringing on after a change of the clock rings on as it was) */
  if (state == ALARM_STATE_FIRED && id != self->ringing)
    start_ringing (self, instance);
  else if (state != ALARM_STATE_FIRED && id == self->ringing)
    stop_ringing (self);
  alarm_notifications_show (self->notifications, instance);
}

static void
on_instance_removed (AlarmEngine *engine, guint id, Service *self)
{
  if (id == self->ringing)
    stop_ringing (self);
  alarm_notifications_clear (self->notifications, id);
}

static void
on_store_changed (AlarmEngine *engine, Service *self)
{
  self->dirty = TRUE;
}

/* --- the user, from the notifications */

static void
on_notification (guint instance_id, const char *action, gpointer data)
{
  Service *self = data;
  AlarmInstance *instance = alarm_store_find_instance (alarm_engine_get_store (self->engine), instance_id);

  if (!instance)
    return;
  if (g_str_equal (action, "dismiss"))
    alarm_engine_dismiss (self->engine, instance_id);
  else if (g_str_equal (action, "snooze"))
    alarm_engine_snooze (self->engine, instance_id);
  else if (g_str_equal (action, "open"))
    {
      guint alarm_id = instance->alarm_id;

      if (instance->state == ALARM_STATE_FIRED)
        phone_start_app (RINGING_APP_ID);
      else
        {
          /* a missed one tapped is seen: over (SHOW_AND_DISMISS_ALARM_ACTION) */
          if (instance->state == ALARM_STATE_MISSED)
            alarm_engine_dismiss (self->engine, instance_id);
          /* the Clock on its Alarm tab, at the alarm */
          phone_open_app (self->session, APP_ID, "show-alarm", g_variant_new_uint32 (alarm_id));
        }
    }
  else if (g_str_equal (action, "closed"))
    {
      /* swiped away: the upcoming one stays hidden, a missed one is over; the ringing one cannot go (ongoing) */
      if (instance->state == ALARM_STATE_LOW_NOTIFICATION)
        alarm_engine_hide_notification (self->engine, instance_id);
      else if (instance->state == ALARM_STATE_MISSED)
        alarm_engine_dismiss (self->engine, instance_id);
      else if (instance->state == ALARM_STATE_FIRED)
        alarm_notifications_show (self->notifications, instance);
    }
  publish (self);
}

/* --- the times */

static void
tell_wakeup (Service *self, const char *name, gint64 time, gboolean alarm_clock, gint64 *sent)
{
  if (*sent == time)
    return;
  *sent = time;
  phone_wake_at (self->system, name, time, alarm_clock);
}

static gint64
unix_time (GDateTime *time)
{
  return time ? g_date_time_to_unix (time) : 0;
}

static GVariant *
ringing_to_variant (Service *self)
{
  AlarmInstance *instance = alarm_engine_ringing (self->engine);
  GVariantBuilder dict;

  g_variant_builder_init (&dict, G_VARIANT_TYPE_VARDICT);
  if (instance)
    {
      g_autoptr(GDateTime) time = alarm_instance_time (instance);

      g_variant_builder_add (&dict, "{sv}", "instance", g_variant_new_uint32 (instance->id));
      g_variant_builder_add (&dict, "{sv}", "alarm", g_variant_new_uint32 (instance->alarm_id));
      g_variant_builder_add (&dict, "{sv}", "label", g_variant_new_string (instance->label ? instance->label : ""));
      g_variant_builder_add (&dict, "{sv}", "time", g_variant_new_int64 (g_date_time_to_unix (time)));
    }
  return g_variant_builder_end (&dict);
}

/* after every change: save, and schedule the next one (here and in the RTC); tell the app */
static void
publish (Service *self)
{
  g_autoptr(GDateTime) change = alarm_engine_next_change (self->engine);
  AlarmInstance *next = alarm_engine_next_alarm (self->engine);
  g_autoptr(GDateTime) next_time = next ? alarm_instance_time (next) : NULL;
  struct itimerspec at = { 0 };
  GVariantBuilder changed;
  gboolean properties = FALSE;

  if (self->dirty)
    {
      g_autoptr(GError) error = NULL;

      self->dirty = FALSE;
      if (!alarm_store_save (alarm_engine_get_store (self->engine), self->store_path, &error))
        g_warning ("cannot save the alarms: %s", error->message);
      g_dbus_connection_emit_signal (self->session, NULL, OBJECT_PATH, INTERFACE, "Changed", NULL, NULL);
    }

  /* CANCEL_ON_SET: a change of the clock wakes it too, to look at every instance again. A time already gone is
   * due at once (1 s after the epoch, as 0 disarms) */
  if (change)
    at.it_value.tv_sec = MAX (1, g_date_time_to_unix (change));
  if (timerfd_settime (self->timer_fd, TFD_TIMER_ABSTIME | TFD_TIMER_CANCEL_ON_SET, &at, NULL) < 0)
    g_warning ("cannot set the alarm timer: %s", g_strerror (errno));

  /* the phone wakes for both: the alarm is also the status bar's next alarm, the change is not */
  tell_wakeup (self, "alarm", unix_time (next_time), TRUE, &self->sent_alarm);
  tell_wakeup (self, "change", unix_time (change), FALSE, &self->sent_change);

  g_variant_builder_init (&changed, G_VARIANT_TYPE_VARDICT);
  if (unix_time (next_time) != self->published_next)
    {
      self->published_next = unix_time (next_time);
      g_variant_builder_add (&changed, "{sv}", "NextAlarm", g_variant_new_int64 (self->published_next));
      properties = TRUE;
    }
  if (self->ringing != self->published_ringing)
    {
      self->published_ringing = self->ringing;
      g_variant_builder_add (&changed, "{sv}", "Ringing", ringing_to_variant (self));
      properties = TRUE;
    }
  if (properties)
    g_dbus_connection_emit_signal (self->session, NULL, OBJECT_PATH, "org.freedesktop.DBus.Properties",
                                   "PropertiesChanged",
                                   g_variant_new ("(sa{sv}as)", INTERFACE, &changed, NULL), NULL);
  else
    g_variant_builder_clear (&changed);
}

static gboolean
on_timer (int fd, GIOCondition condition, gpointer data)
{
  Service *self = data;
  guint64 expirations;
  gint64 offset = elapsed_wall_now () - elapsed_now ();
  gboolean cancelled = read (fd, &expirations, sizeof expirations) < 0 && errno == ECANCELED;
  gboolean moved = ABS (offset - self->clock_offset) > 1000;

  self->clock_offset = offset;
  /* The kernel cancels the timer at every resume from sleep too, and fixing every instance then would post their
   * notifications again, or ring a fired one anew. Both clocks went on through the sleep, so the clock was set only
   * if the wall clock moved against the elapsed one; else this is a tick, which does what came due meanwhile. */
  if (cancelled && moved)
    {
      /* the clock was set: every instance anew, as after a reboot (ACTION_TIME_CHANGED) */
      g_message ("the clock was set");
      alarm_engine_fix (self->engine);
    }
  else
    alarm_engine_tick (self->engine);
  publish (self);
  return G_SOURCE_CONTINUE;
}

static void
on_zone_changed (GFileMonitor *monitor, GFile *file, GFile *other, GFileMonitorEvent event, gpointer data)
{
  Service *self = data;

  if (event != G_FILE_MONITOR_EVENT_CHANGES_DONE_HINT && event != G_FILE_MONITOR_EVENT_CREATED
      && event != G_FILE_MONITOR_EVENT_RENAMED && event != G_FILE_MONITOR_EVENT_MOVED_IN)
    return;
  /* the wall-clock times stay, the moments move (ACTION_TIMEZONE_CHANGED) */
  g_message ("the time zone changed");
  alarm_engine_fix (self->engine);
  publish (self);
}

static void
on_wakeup_appeared (GDBusConnection *bus, const char *name, const char *owner, gpointer data)
{
  Service *self = data;

  /* vibeos-wakeup (re)started: it knows nothing of ours */
  self->sent_alarm = self->sent_change = -1;
  publish (self);
}

static void
apply_settings (Service *self)
{
  AlarmEngineSettings settings = {
    .snooze_minutes = g_settings_get_int (self->settings, "snooze-duration"),
    .auto_silence_minutes = g_settings_get_int (self->settings, "auto-silence"),
  };

  alarm_engine_set_settings (self->engine, &settings);
}

static void
on_settings_changed (GSettings *settings, const char *key, Service *self)
{
  apply_settings (self);
  /* a new auto-silence moves the ringing one's end */
  publish (self);
}

/* --- D-Bus */

static void
on_method_call (GDBusConnection *bus, const char *sender, const char *path, const char *interface,
                const char *method, GVariant *parameters, GDBusMethodInvocation *invocation, gpointer data)
{
  Service *self = data;
  AlarmStore *store = alarm_engine_get_store (self->engine);
  g_autoptr(GError) error = NULL;

  if (g_str_equal (method, "GetAlarms"))
    {
      GVariantBuilder alarms;

      g_variant_builder_init (&alarms, G_VARIANT_TYPE ("aa{sv}"));
      for (guint i = 0; i < store->alarms->len; i++)
        g_variant_builder_add_value (&alarms, alarm_to_variant (self, store->alarms->pdata[i]));
      g_dbus_method_invocation_return_value (invocation, g_variant_new ("(aa{sv})", &alarms));
      return;
    }
  if (g_str_equal (method, "AddAlarm"))
    {
      g_autoptr(GVariant) values = g_variant_get_child_value (parameters, 0);
      g_autoptr(Alarm) alarm = alarm_new ();
      guint id;

      /* a new alarm (Alarm's constructor): on, vibrating, the default sound */
      alarm->enabled = TRUE;
      alarm->vibrate = TRUE;
      if (!alarm_apply (alarm, values, &error))
        {
          g_dbus_method_invocation_return_gerror (invocation, error);
          return;
        }
      id = alarm_engine_add_alarm (self->engine, alarm);
      publish (self);
      g_dbus_method_invocation_return_value (invocation, g_variant_new ("(u)", id));
      return;
    }
  if (g_str_equal (method, "UpdateAlarm"))
    {
      g_autoptr(GVariant) values = NULL;
      g_autoptr(Alarm) alarm = NULL;
      Alarm *stored;
      guint id;

      g_variant_get (parameters, "(u@a{sv})", &id, &values);
      if (!(stored = alarm_store_find_alarm (store, id)))
        {
          g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS,
                                                 "no alarm %u", id);
          return;
        }
      alarm = alarm_copy (stored);
      if (!alarm_apply (alarm, values, &error))
        {
          g_dbus_method_invocation_return_gerror (invocation, error);
          return;
        }
      alarm_engine_update_alarm (self->engine, alarm);
      publish (self);
      g_dbus_method_invocation_return_value (invocation, NULL);
      return;
    }

  /* the rest take an id */
  {
    guint id;
    gboolean done;

    g_variant_get (parameters, "(u)", &id);
    if (g_str_equal (method, "DeleteAlarm"))
      done = alarm_engine_delete_alarm (self->engine, id);
    else if (g_str_equal (method, "Snooze"))
      done = alarm_engine_snooze (self->engine, id);
    else
      done = alarm_engine_dismiss (self->engine, id);
    publish (self);
    if (done)
      g_dbus_method_invocation_return_value (invocation, NULL);
    else
      g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS,
                                             "%s: no such alarm or instance, or not now: %u", method, id);
  }
}

static GVariant *
on_get_property (GDBusConnection *bus, const char *sender, const char *path, const char *interface,
                 const char *property, GError **error, gpointer data)
{
  Service *self = data;

  if (g_str_equal (property, "Ringing"))
    return ringing_to_variant (self);
  return g_variant_new_int64 (self->published_next);
}

static const GDBusInterfaceVTable vtable = { .method_call = on_method_call, .get_property = on_get_property };

/* --- start */

/* the store, or a new one if it cannot be read: the unreadable file is kept aside, never overwritten */
static AlarmStore *
load_store (const char *path)
{
  g_autoptr(GError) error = NULL;
  AlarmStore *store = alarm_store_load (path, &error);
  g_autofree char *aside = NULL;

  if (store)
    return store;
  aside = g_strconcat (path, ".unreadable", NULL);
  g_warning ("cannot read %s (%s): kept as %s, starting with a new store", path, error->message, aside);
  g_rename (path, aside);
  return alarm_store_load (path, NULL);
}

AlarmService *
alarm_service_new (GDBusConnection *session, GDBusConnection *system, GSettings *settings, const char *store_path,
                   GError **error)
{
  Service *self = g_new0 (Service, 1);
  g_autoptr(GFile) localtime = g_file_new_for_path ("/etc/localtime");

  self->timer_fd = self->sleep_inhibitor = -1;
  self->sent_alarm = self->sent_change = -1;
  self->session = g_object_ref (session);
  self->system = system ? g_object_ref (system) : NULL;
  self->node = g_dbus_node_info_new_for_xml (introspection, NULL);
  self->store_path = g_strdup (store_path);
  self->settings = g_object_ref (settings);
  self->engine = alarm_engine_new (load_store (self->store_path));
  apply_settings (self);
  self->klaxon = alarm_klaxon_new (self->settings, ALARM_KLAXON_ALARM);
  self->notifications = alarm_notifications_new (on_notification, self);

  g_signal_connect (self->engine, "instance-changed", G_CALLBACK (on_instance_changed), self);
  g_signal_connect (self->engine, "instance-removed", G_CALLBACK (on_instance_removed), self);
  g_signal_connect (self->engine, "store-changed", G_CALLBACK (on_store_changed), self);
  g_signal_connect (self->settings, "changed", G_CALLBACK (on_settings_changed), self);

  self->registration = g_dbus_connection_register_object (self->session, OBJECT_PATH, self->node->interfaces[0],
                                                          &vtable, self, NULL, error);
  if (!self->registration)
    return NULL;
  self->timer_fd = timerfd_create (CLOCK_REALTIME, TFD_NONBLOCK | TFD_CLOEXEC);
  self->clock_offset = elapsed_wall_now () - elapsed_now ();
  g_unix_fd_add (self->timer_fd, G_IO_IN, on_timer, self);
  self->zone_monitor = g_file_monitor_file (localtime, G_FILE_MONITOR_WATCH_MOVES, NULL, NULL);
  if (self->zone_monitor)
    g_signal_connect (self->zone_monitor, "changed", G_CALLBACK (on_zone_changed), self);
  if (self->system)
    self->wakeup_watch = g_bus_watch_name_on_connection (self->system, "vibeos.Wakeup",
                                                         G_BUS_NAME_WATCHER_FLAGS_NONE, on_wakeup_appeared, NULL,
                                                         self, NULL);

  /* as after a reboot: whatever came due while nothing ran (the phone off, the session out) */
  alarm_engine_fix (self->engine);
  publish (self);
  return self;
}
