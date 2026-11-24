/* vibeos-clock-alarms: the Clock's service, for everything of the Clock that must go on with the app closed. It runs in
 * the session from login (vibeos-clock-alarms.service) and holds three parts, each serving the app over D-Bus:
 *
 *  - the alarms (alarm-service.c, vibeos.Clock.Alarms);
 *  - the timers (timer-service.c, vibeos.Clock.Timers);
 *  - the stopwatch (stopwatch-service.c, vibeos.Clock.Stopwatch).
 *
 * They are one process under one bus name (still vibeos.Clock.Alarms, the name the alarms began with), so that
 * there is one thing to start and to restart, and so that what they share is in one place, below: whether the app
 * is in front, and when the wall clock was set. */
#include "alarm-klaxon.h"
#include "alarm-service.h"
#include "elapsed.h"
#include "stopwatch-service.h"
#include "timer-service.h"

#include <errno.h>
#include <glib/gi18n.h>
#include <glib/gstdio.h>
#include <glib-unix.h>
#include <locale.h>
#include <sys/timerfd.h>
#include <unistd.h>

#define BUS_NAME "vibeos.Clock.Alarms"
#define APP_ID "vibeos.Clock"
#define OBJECT_PATH "/vibeos/Clock/App"

static const char introspection[] =
  "<node>"
  "  <interface name='vibeos.Clock.App'>"
  /* the Clock app's window came to the front, or left it. While it is in front the app shows the timers and the
   * stopwatch itself, so their notifications are not shown. An app that quits or crashes in front counts as gone */
  "    <method name='SetForeground'><arg name='foreground' type='b' direction='in'/></method>"
  /* a sound tried out at the alarm volume, looping, for @milliseconds or (0) until StopSound or the caller goes: the
   * ringtone picker's preview and the alarm volume slider's. @sound is a file URI, "" for the default alarm sound, or
   * "timer" for the default timer sound. Nothing plays while an alarm or a timer rings, and one starting stops it */
  "    <method name='PlaySound'>"
  "      <arg name='sound' type='s' direction='in'/><arg name='milliseconds' type='u' direction='in'/>"
  "    </method>"
  "    <method name='StopSound'/>"
  "  </interface>"
  "</node>";

typedef struct {
  GMainLoop *loop;
  GDBusConnection *session;
  AlarmService *alarms;
  TimerService *timers;
  StopwatchService *stopwatch;

  GHashTable *in_front;         /* the bus names of the apps in front -> their name watch */
  AlarmKlaxon *preview;         /* PlaySound's */
  guint preview_watch, preview_stop;
  int clock_fd;                 /* CLOCK_REALTIME, never due: cancelled when the clock is set */
  gint64 clock_offset;          /* the wall clock less the elapsed one */
} Clock;

/* --- the app in front */

static void
foreground_changed (Clock *self)
{
  gboolean foreground = g_hash_table_size (self->in_front) > 0;

  timer_service_set_foreground (self->timers, foreground);
  stopwatch_service_set_foreground (self->stopwatch, foreground);
}

static void
on_app_vanished (GDBusConnection *bus, const char *name, gpointer data)
{
  Clock *self = data;

  g_hash_table_remove (self->in_front, name);
  foreground_changed (self);
}

/* --- sounds tried out */

static void
stop_sound (Clock *self)
{
  g_clear_handle_id (&self->preview_stop, g_source_remove);
  if (self->preview_watch)
    g_bus_unwatch_name (self->preview_watch);
  self->preview_watch = 0;
  alarm_klaxon_stop (self->preview);
}

static gboolean
on_sound_time (gpointer data)
{
  Clock *self = data;

  self->preview_stop = 0;
  stop_sound (self);
  return G_SOURCE_REMOVE;
}

static void
on_listener_vanished (GDBusConnection *bus, const char *name, gpointer data)
{
  /* an app that quits or crashes mid-preview does not leave the sound looping */
  stop_sound (data);
}

static void
play_sound (Clock *self, GDBusConnection *bus, const char *sender, const char *sound, guint milliseconds)
{
  g_autofree char *timer = NULL;

  stop_sound (self);
  if (g_str_equal (sound, "timer") && !(timer = alarm_klaxon_default_sound (ALARM_KLAXON_TIMER)))
    return;
  alarm_klaxon_start (self->preview, timer ? timer : *sound ? sound : NULL, FALSE);
  self->preview_watch = g_bus_watch_name_on_connection (bus, sender, G_BUS_NAME_WATCHER_FLAGS_NONE, NULL,
                                                        on_listener_vanished, self, NULL);
  if (milliseconds)
    self->preview_stop = g_timeout_add (milliseconds, on_sound_time, self);
}

static void
on_method_call (GDBusConnection *bus, const char *sender, const char *path, const char *interface,
                const char *method, GVariant *parameters, GDBusMethodInvocation *invocation, gpointer data)
{
  Clock *self = data;
  gboolean foreground;

  if (g_str_equal (method, "PlaySound"))
    {
      const char *sound;
      guint milliseconds;

      g_variant_get (parameters, "(&su)", &sound, &milliseconds);
      play_sound (self, bus, sender, sound, milliseconds);
      g_dbus_method_invocation_return_value (invocation, NULL);
      return;
    }
  if (g_str_equal (method, "StopSound"))
    {
      stop_sound (self);
      g_dbus_method_invocation_return_value (invocation, NULL);
      return;
    }
  g_variant_get (parameters, "(b)", &foreground);
  if (foreground && !g_hash_table_contains (self->in_front, sender))
    g_hash_table_insert (self->in_front, g_strdup (sender),
                         GUINT_TO_POINTER (g_bus_watch_name_on_connection (bus, sender, G_BUS_NAME_WATCHER_FLAGS_NONE,
                                                                           NULL, on_app_vanished, self, NULL)));
  else if (!foreground)
    g_hash_table_remove (self->in_front, sender);
  foreground_changed (self);
  g_dbus_method_invocation_return_value (invocation, NULL);
}

static const GDBusInterfaceVTable vtable = { .method_call = on_method_call };

static void
unwatch (gpointer watch)
{
  g_bus_unwatch_name (GPOINTER_TO_UINT (watch));
}

/* --- the wall clock set */

static void
arm_clock_watch (Clock *self)
{
  /* 2100: a time that never comes. The timer is there to be cancelled (CANCEL_ON_SET) */
  struct itimerspec never = { .it_value.tv_sec = 4102444800 };

  if (timerfd_settime (self->clock_fd, TFD_TIMER_ABSTIME | TFD_TIMER_CANCEL_ON_SET, &never, NULL) < 0)
    g_warning ("cannot watch the clock: %s", g_strerror (errno));
}

static gboolean
on_clock_set (int fd, GIOCondition condition, gpointer data)
{
  Clock *self = data;
  gint64 offset = elapsed_wall_now () - elapsed_now ();
  guint64 expirations;

  /* The kernel cancels the timer at every resume from sleep too. Both clocks went on through the sleep, so the
   * clock was set only if the wall clock moved against the elapsed one. (The alarms watch the clock themselves, at
   * their next change.) */
  if (read (fd, &expirations, sizeof expirations) < 0 && errno == ECANCELED
      && ABS (offset - self->clock_offset) > 1000)
    {
      timer_service_time_set (self->timers);
      stopwatch_service_time_set (self->stopwatch);
    }
  self->clock_offset = offset;
  arm_clock_watch (self);
  return G_SOURCE_CONTINUE;
}

/* --- start */

static void
on_name_lost (GDBusConnection *bus, const char *name, gpointer data)
{
  Clock *self = data;

  /* another one runs: two would ring twice */
  g_warning ("%s is taken: quitting", name);
  g_main_loop_quit (self->loop);
}

int
main (int argc, char **argv)
{
  Clock clock = { 0 };
  Clock *self = &clock;
  g_autofree char *dir = g_build_filename (g_get_user_data_dir (), APP_ID, NULL);
  g_autofree char *alarms = g_build_filename (dir, "alarms.ini", NULL);
  g_autofree char *timers = g_build_filename (dir, "timers.ini", NULL);
  g_autofree char *stopwatch = g_build_filename (dir, "stopwatch.ini", NULL);
  g_autoptr(GDBusConnection) system = NULL;
  g_autoptr(GSettings) settings = NULL;
  g_autoptr(GDBusNodeInfo) node = NULL;
  g_autoptr(GError) error = NULL;

  setlocale (LC_ALL, "");
  bindtextdomain (GETTEXT_PACKAGE, LOCALEDIR);
  bind_textdomain_codeset (GETTEXT_PACKAGE, "UTF-8");
  textdomain (GETTEXT_PACKAGE);
  self->loop = g_main_loop_new (NULL, FALSE);
  self->session = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, &error);
  if (!self->session)
    g_error ("no session bus: %s", error->message);
  system = g_bus_get_sync (G_BUS_TYPE_SYSTEM, NULL, NULL);
  settings = g_settings_new (APP_ID);
  g_mkdir_with_parents (dir, 0700);

  if (!(self->alarms = alarm_service_new (self->session, system, settings, alarms, &error))
      || !(self->timers = timer_service_new (self->session, system, settings, timers, &error))
      || !(self->stopwatch = stopwatch_service_new (self->session, stopwatch, &error)))
    g_error ("cannot serve the Clock: %s", error->message);

  self->in_front = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, unwatch);
  self->preview = alarm_klaxon_new (settings, ALARM_KLAXON_PREVIEW);
  node = g_dbus_node_info_new_for_xml (introspection, NULL);
  if (!g_dbus_connection_register_object (self->session, OBJECT_PATH, node->interfaces[0], &vtable, self, NULL,
                                          &error))
    g_error ("cannot serve %s: %s", OBJECT_PATH, error->message);
  self->clock_fd = timerfd_create (CLOCK_REALTIME, TFD_NONBLOCK | TFD_CLOEXEC);
  self->clock_offset = elapsed_wall_now () - elapsed_now ();
  arm_clock_watch (self);
  g_unix_fd_add (self->clock_fd, G_IO_IN, on_clock_set, self);

  /* the name last: whoever calls finds every part there */
  g_bus_own_name_on_connection (self->session, BUS_NAME, G_BUS_NAME_OWNER_FLAGS_DO_NOT_QUEUE, NULL, on_name_lost,
                                self, NULL);
  g_main_loop_run (self->loop);
  return 0;
}
