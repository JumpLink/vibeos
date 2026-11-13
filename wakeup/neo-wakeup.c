/* neo-wakeup: wakes the phone from suspend at the times apps ask for.
 *
 * A sleeping phone runs no code, so an app's own timer cannot ring an alarm at 7:00: something has to bring the phone
 * out of suspend first. The kernel does that for a timer on CLOCK_REALTIME_ALARM: as the phone suspends it programs
 * the real-time clock chip with the earliest such timer, and the chip's interrupt wakes the phone (on the S10e the
 * PMIC's RTC, which s10e-sleepd's AOD clock already wakes by). Setting one needs CAP_WAKE_ALARM, which apps do not
 * have, so this service holds that one timer for all of them.
 *
 * An app keeps its own timer for what it does at that time and tells this service when that is (Set). At the
 * earliest time asked for, the phone wakes; this service then holds it awake for HOLD_SECONDS (a logind "sleep"
 * block inhibitor, which s10e-sleepd honours) so the app's own timer gets to fire and the app can take over: an alarm
 * clock takes an inhibitor of its own while it rings.
 *
 * Wake-ups an app marks "alarm-clock" are alarms the user set. The earliest of them is the NextAlarmClock property,
 * which the status bar's alarm icon and the lock screen show.
 *
 * Nothing is stored. After a reboot, or a restart of this service, apps set their wake-ups again: they watch for the
 * bus name to appear.
 *
 * D-Bus, system bus: neo.Wakeup at /neo/Wakeup, interface neo.Wakeup
 *   Set (s name, x time, a{sv} options)
 *       wake the phone at @time (Unix seconds); a time of 0 removes the wake-up. @name is the caller's own key: a later
 *       Set with the same name from the same user replaces it. Options:
 *         "alarm-clock"  b  it is an alarm the user set
 *         "app"          s  the desktop app id that shows it (tapping the next alarm opens that app)
 *   NextAlarmClock (xs), read-only
 *       the earliest alarm clock's time and app, (0, "") when there is none
 *
 * Try it:  busctl call neo.Wakeup /neo/Wakeup neo.Wakeup Set 'sxa{sv}' test $(($(date +%s) + 60)) 0
 */
#include <errno.h>
#include <string.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>

#include <gio/gio.h>
#include <gio/gunixfdlist.h>
#include <glib-unix.h>

#define BUS_NAME        "neo.Wakeup"
#define OBJECT_PATH     "/neo/Wakeup"
#define INTERFACE       "neo.Wakeup"
#define HOLD_SECONDS    30      /* how long the phone stays up after a wake-up, for the app to take over */
#define MAX_PER_USER    64      /* a runaway app cannot fill the table */

static const char introspection_xml[] =
  "<node>"
  "  <interface name='" INTERFACE "'>"
  "    <method name='Set'>"
  "      <arg name='name' type='s' direction='in'/>"
  "      <arg name='time' type='x' direction='in'/>"
  "      <arg name='options' type='a{sv}' direction='in'/>"
  "    </method>"
  "    <property name='NextAlarmClock' type='(xs)' access='read'/>"
  "  </interface>"
  "</node>";

/* one app's wake-up */
typedef struct {
  guint32 uid;                  /* who asked: names are per user */
  char *name;
  gint64 time;                  /* Unix seconds */
  gboolean alarm_clock;
  char *app;                    /* a desktop app id, or NULL */
} WakeUp;

static GHashTable *wakeups;     /* "uid/name" -> WakeUp */
static int timer_fd = -1;       /* the CLOCK_REALTIME_ALARM timer, armed for the earliest wake-up */
static GDBusConnection *bus;
static GDBusNodeInfo *introspection;

/* the NextAlarmClock last announced */
static gint64 next_alarm_time;
static char *next_alarm_app;

/* the logind inhibitor that keeps the phone up after a wake-up: its file descriptor, and the timeout that closes it */
static int hold_fd = -1;
static guint hold_id;

static void
wake_up_free (gpointer data)
{
  WakeUp *wake_up = data;

  g_free (wake_up->name);
  g_free (wake_up->app);
  g_free (wake_up);
}

static gint64
now_seconds (void)
{
  return g_get_real_time () / G_USEC_PER_SEC;
}

/* --- the phone held awake after a wake-up */

static gboolean
release_hold (gpointer data)
{
  if (hold_fd >= 0)
    close (hold_fd);
  hold_fd = -1;
  hold_id = 0;
  return G_SOURCE_REMOVE;
}

/* takes a logind "sleep" block inhibitor (or keeps the one held) for HOLD_SECONDS from now */
static void
hold_awake (const char *why)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(GUnixFDList) fds = NULL;
  g_autoptr(GVariant) reply = NULL;

  if (hold_id)
    g_source_remove (hold_id);
  hold_id = g_timeout_add_seconds (HOLD_SECONDS, release_hold, NULL);
  if (hold_fd >= 0)
    return;

  reply = g_dbus_connection_call_with_unix_fd_list_sync (bus, "org.freedesktop.login1", "/org/freedesktop/login1",
                                                         "org.freedesktop.login1.Manager", "Inhibit",
                                                         g_variant_new ("(ssss)", "sleep", "neo-wakeup", why, "block"),
                                                         G_VARIANT_TYPE ("(h)"), G_DBUS_CALL_FLAGS_NONE, -1, NULL,
                                                         &fds, NULL, &error);
  if (!reply)
    {
      g_warning ("could not hold the phone awake: %s", error->message);
      return;
    }
  hold_fd = g_unix_fd_list_get (fds, 0, NULL);
}

/* --- the timer */

static void
announce_next_alarm_clock (void)
{
  GVariantBuilder changed;

  g_variant_builder_init (&changed, G_VARIANT_TYPE ("a{sv}"));
  g_variant_builder_add (&changed, "{sv}", "NextAlarmClock",
                         g_variant_new ("(xs)", next_alarm_time, next_alarm_app ? next_alarm_app : ""));
  g_dbus_connection_emit_signal (bus, NULL, OBJECT_PATH, "org.freedesktop.DBus.Properties", "PropertiesChanged",
                                 g_variant_new ("(sa{sv}as)", INTERFACE, &changed, NULL), NULL);
}

/* arms the timer for the earliest wake-up, and announces the earliest alarm clock if it changed */
static void
reschedule (void)
{
  GHashTableIter iter;
  WakeUp *wake_up, *earliest = NULL, *alarm_clock = NULL;
  struct itimerspec spec = { 0 };

  g_hash_table_iter_init (&iter, wakeups);
  while (g_hash_table_iter_next (&iter, NULL, (gpointer *) &wake_up))
    {
      if (!earliest || wake_up->time < earliest->time)
        earliest = wake_up;
      if (wake_up->alarm_clock && (!alarm_clock || wake_up->time < alarm_clock->time))
        alarm_clock = wake_up;
    }

  /* an absolute time on the wall clock: the kernel moves it with the clock (a new time zone does not move it, as
   * Unix time has none). A zero spec disarms the timer */
  if (earliest)
    spec.it_value.tv_sec = earliest->time;
  if (timerfd_settime (timer_fd, TFD_TIMER_ABSTIME, &spec, NULL) < 0)
    g_warning ("could not set the wake-up timer: %s", g_strerror (errno));

  if ((alarm_clock ? alarm_clock->time : 0) != next_alarm_time
      || g_strcmp0 (alarm_clock ? alarm_clock->app : NULL, next_alarm_app) != 0)
    {
      next_alarm_time = alarm_clock ? alarm_clock->time : 0;
      g_free (next_alarm_app);
      next_alarm_app = alarm_clock ? g_strdup (alarm_clock->app) : NULL;
      announce_next_alarm_clock ();
    }
}

/* the timer went off (the phone may just have woken for it): what was due is done, the phone stays up a while */
static gboolean
on_timer (int fd, GIOCondition condition, gpointer data)
{
  guint64 expirations;
  GHashTableIter iter;
  WakeUp *wake_up;
  gint64 now = now_seconds ();
  g_autoptr(GString) names = g_string_new (NULL);

  if (read (fd, &expirations, sizeof expirations) < 0 && errno != EAGAIN)
    g_warning ("reading the wake-up timer: %s", g_strerror (errno));

  g_hash_table_iter_init (&iter, wakeups);
  while (g_hash_table_iter_next (&iter, NULL, (gpointer *) &wake_up))
    {
      if (wake_up->time > now)
        continue;
      g_string_append_printf (names, "%s%s", names->len ? ", " : "", wake_up->name);
      g_hash_table_iter_remove (&iter);
    }

  if (names->len)
    {
      g_autofree char *why = g_strdup_printf ("woke the phone for %s", names->str);

      g_message ("%s", why);
      hold_awake (why);
    }
  reschedule ();
  return G_SOURCE_CONTINUE;
}

/* --- D-Bus */

static gboolean
caller_uid (GDBusMethodInvocation *invocation, guint32 *uid)
{
  g_autoptr(GVariant) reply = NULL;

  /* the bus daemon knows who is on the other end of a connection */
  reply = g_dbus_connection_call_sync (bus, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus",
                                       "GetConnectionUnixUser",
                                       g_variant_new ("(s)", g_dbus_method_invocation_get_sender (invocation)),
                                       G_VARIANT_TYPE ("(u)"), G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL);
  if (!reply)
    return FALSE;
  g_variant_get (reply, "(u)", uid);
  return TRUE;
}

static guint
count_for_user (guint32 uid)
{
  GHashTableIter iter;
  WakeUp *wake_up;
  guint count = 0;

  g_hash_table_iter_init (&iter, wakeups);
  while (g_hash_table_iter_next (&iter, NULL, (gpointer *) &wake_up))
    count += wake_up->uid == uid;
  return count;
}

static void
handle_set (GDBusMethodInvocation *invocation, GVariant *parameters)
{
  const char *name;
  gint64 time;
  g_autoptr(GVariant) options = NULL;
  guint32 uid;
  g_autofree char *key = NULL;
  WakeUp *wake_up;

  g_variant_get (parameters, "(&sx@a{sv})", &name, &time, &options);
  if (!caller_uid (invocation, &uid))
    {
      g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR, G_DBUS_ERROR_ACCESS_DENIED,
                                             "who is calling is unknown");
      return;
    }
  key = g_strdup_printf ("%u/%s", uid, name);

  /* 0 removes it; so does a time already gone (the app's own timer has it) */
  if (time <= now_seconds ())
    {
      g_hash_table_remove (wakeups, key);
      reschedule ();
      g_dbus_method_invocation_return_value (invocation, NULL);
      return;
    }

  if (!g_hash_table_contains (wakeups, key) && count_for_user (uid) >= MAX_PER_USER)
    {
      g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR, G_DBUS_ERROR_LIMITS_EXCEEDED,
                                             "at most %d wake-ups per user", MAX_PER_USER);
      return;
    }

  wake_up = g_new0 (WakeUp, 1);
  wake_up->uid = uid;
  wake_up->name = g_strdup (name);
  wake_up->time = time;
  g_variant_lookup (options, "alarm-clock", "b", &wake_up->alarm_clock);
  g_variant_lookup (options, "app", "s", &wake_up->app);
  g_hash_table_replace (wakeups, g_steal_pointer (&key), wake_up);
  reschedule ();
  g_dbus_method_invocation_return_value (invocation, NULL);
}

static void
on_method_call (GDBusConnection *connection, const char *sender, const char *object_path,
                const char *interface_name, const char *method_name, GVariant *parameters,
                GDBusMethodInvocation *invocation, gpointer data)
{
  if (g_strcmp0 (method_name, "Set") == 0)
    handle_set (invocation, parameters);
  else
    g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_METHOD,
                                           "no method %s", method_name);
}

static GVariant *
on_get_property (GDBusConnection *connection, const char *sender, const char *object_path,
                 const char *interface_name, const char *property_name, GError **error, gpointer data)
{
  if (g_strcmp0 (property_name, "NextAlarmClock") == 0)
    return g_variant_new ("(xs)", next_alarm_time, next_alarm_app ? next_alarm_app : "");
  return NULL;
}

static const GDBusInterfaceVTable vtable = { .method_call = on_method_call, .get_property = on_get_property };

static void
on_bus_acquired (GDBusConnection *connection, const char *name, gpointer data)
{
  g_autoptr(GError) error = NULL;

  bus = connection;
  if (!g_dbus_connection_register_object (connection, OBJECT_PATH, introspection->interfaces[0], &vtable,
                                          NULL, NULL, &error))
    g_error ("could not export %s: %s", OBJECT_PATH, error->message);
}

static void
on_name_lost (GDBusConnection *connection, const char *name, gpointer data)
{
  g_printerr ("could not own %s on the system bus (is the D-Bus policy installed?)\n", name);
  g_main_loop_quit (data);
}

int
main (void)
{
  g_autoptr(GMainLoop) loop = g_main_loop_new (NULL, FALSE);

  /* without CAP_WAKE_ALARM the kernel refuses this timer: nothing else here would be worth running */
  timer_fd = timerfd_create (CLOCK_REALTIME_ALARM, TFD_NONBLOCK | TFD_CLOEXEC);
  if (timer_fd < 0)
    {
      g_printerr ("no CLOCK_REALTIME_ALARM timer (%s): neo-wakeup needs CAP_WAKE_ALARM\n", g_strerror (errno));
      return 1;
    }
  g_unix_fd_add (timer_fd, G_IO_IN, on_timer, NULL);

  wakeups = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, wake_up_free);
  introspection = g_dbus_node_info_new_for_xml (introspection_xml, NULL);
  g_bus_own_name (G_BUS_TYPE_SYSTEM, BUS_NAME, G_BUS_NAME_OWNER_FLAGS_NONE, on_bus_acquired, NULL, on_name_lost,
                  loop, NULL);
  g_main_loop_run (loop);
  return 1;
}
