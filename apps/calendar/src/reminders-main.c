/* neo-calendar-reminders: the Calendar's reminders, as Etar's AlertService and Android's AlarmManager ring them
 * (spec/calendar.md §10).
 *
 * It runs from login, whether or not the app is open (a systemd user service, de.yesman.Calendar.Reminders on the
 * session bus). It reads the shown calendars' events of the coming weeks from evolution-data-server (CalStore),
 * and at each reminder's time posts a notification: the event's title, its time and place (AlertUtils
 * .formatTimeLocation), a tap opening the event in the app, and Snooze. The phone may be asleep then: neo-wakeup
 * wakes it at the next reminder's time (the RTC), and a CLOCK_REALTIME timerfd here does the rest.
 *
 * Reminders that came due while nothing ran (the phone was off) still notify while their event has not ended, as
 * Etar's missed alerts do; what was notified is kept in $XDG_STATE_HOME/neo-calendar/reminders.ini, so a restart
 * does not notify it again. The first start notifies nothing that is already past (no flood on installing).
 *
 * The "Notifications" setting off: nothing is posted (the reminders are still marked as handled).
 * Etar's other notification actions are left out: Map (no maps app to hand the place to), Call (no number parsing),
 * Email guests (no guests). The notification's sound and vibration are the shell's. */
#include "cal-alerts.h"
#include "cal-format.h"
#include "cal-store.h"

#include <errno.h>
#include <glib/gi18n.h>
#include <glib-unix.h>
#include <locale.h>
#include <sys/timerfd.h>
#include <unistd.h>

#define BUS_NAME        "de.yesman.Calendar.Reminders"
#define APP_ID          "de.yesman.Calendar"
#define NOTIFICATIONS   "org.freedesktop.Notifications"
#define NOTIFICATIONS_PATH "/org/freedesktop/Notifications"
#define LOOK_BACK       (24 * 3600)             /* missed reminders of events still going on */
#define LOOK_AHEAD      (29 * 24 * 3600)        /* the longest reminder is 4 weeks */
#define REREAD          (6 * 3600)              /* the window moves on: read it again at least this often */
#define KEEP_FIRED      (3 * 24 * 3600)         /* forget handled reminders this long after they were due */

typedef struct {
  char *calendar, *uid, *rid, *occurrence;
  gint64 start;
} Posted;

typedef struct {
  GMainLoop *loop;
  CalStore *store;
  GSettings *settings;
  GDBusConnection *session, *system;
  GCancellable *query;
  GPtrArray *events;                    /* the last read occurrences */
  GHashTable *fired;                    /* cal_alert_key -> due time: handled */
  GHashTable *snoozed;                  /* occurrence key -> when to notify again */
  GHashTable *posted;                   /* notification id -> Posted */
  char *state_path;
  gboolean first_run;
  int timer_fd;
  gint64 sent_wakeup;
  const char *wakeup_name;      /* the key of our wake-up in neo-wakeup */
  guint wakeup_watch, action_signal, closed_signal;
} Service;

static void reread (Service *self);
static void schedule (Service *self);

static gint64
now_seconds (void)
{
  return g_get_real_time () / G_USEC_PER_SEC;
}

static void
posted_free (gpointer data)
{
  Posted *posted = data;

  g_free (posted->calendar);
  g_free (posted->uid);
  g_free (posted->rid);
  g_free (posted->occurrence);
  g_free (posted);
}

/* --- the state */

/* handled; the due time is kept to forget it after KEEP_FIRED (a gint64 in a pointer: 64-bit only, as the phone) */
static void
mark_fired (Service *self, const char *key, gint64 time)
{
  g_hash_table_insert (self->fired, g_strdup (key), GSIZE_TO_POINTER ((gsize) time));
}

static void
load_state (Service *self)
{
  g_autoptr(GKeyFile) file = g_key_file_new ();
  g_auto(GStrv) keys = NULL;

  if (!g_key_file_load_from_file (file, self->state_path, G_KEY_FILE_NONE, NULL))
    {
      self->first_run = TRUE;
      return;
    }
  keys = g_key_file_get_keys (file, "fired", NULL, NULL);
  for (int i = 0; keys && keys[i]; i++)
    mark_fired (self, keys[i], g_key_file_get_int64 (file, "fired", keys[i], NULL));
  g_strfreev (keys);
  keys = g_key_file_get_keys (file, "snoozed", NULL, NULL);
  for (int i = 0; keys && keys[i]; i++)
    {
      gint64 *time = g_new (gint64, 1);

      *time = g_key_file_get_int64 (file, "snoozed", keys[i], NULL);
      g_hash_table_insert (self->snoozed, g_strdup (keys[i]), time);
    }
}

static void
save_state (Service *self)
{
  g_autoptr(GKeyFile) file = g_key_file_new ();
  g_autoptr(GError) error = NULL;
  g_autofree char *dir = g_path_get_dirname (self->state_path);
  GHashTableIter iter;
  gpointer key, value;
  gint64 forget = now_seconds () - KEEP_FIRED;

  g_hash_table_iter_init (&iter, self->fired);
  while (g_hash_table_iter_next (&iter, &key, &value))
    {
      gint64 time = (gint64) GPOINTER_TO_SIZE (value);

      if (time < forget)
        g_hash_table_iter_remove (&iter);
      else
        g_key_file_set_int64 (file, "fired", key, time);
    }
  g_hash_table_iter_init (&iter, self->snoozed);
  while (g_hash_table_iter_next (&iter, &key, &value))
    g_key_file_set_int64 (file, "snoozed", key, *(gint64 *) value);
  g_mkdir_with_parents (dir, 0700);
  if (!g_key_file_save_to_file (file, self->state_path, &error))
    g_warning ("%s", error->message);
}

/* --- notifications */

static CalEvent *
find_occurrence (Service *self, const char *occurrence)
{
  for (guint i = 0; self->events && i < self->events->len; i++)
    {
      g_autofree char *key = cal_alert_occurrence_key (self->events->pdata[i]);

      if (g_str_equal (key, occurrence))
        return self->events->pdata[i];
    }
  return NULL;
}

static guint32
replaces_for (Service *self, const char *occurrence)
{
  GHashTableIter iter;
  gpointer id;
  Posted *posted;

  g_hash_table_iter_init (&iter, self->posted);
  while (g_hash_table_iter_next (&iter, &id, (gpointer *) &posted))
    if (g_str_equal (posted->occurrence, occurrence))
      return GPOINTER_TO_UINT (id);
  return 0;
}

/* AlertReceiver.buildBasicNotification: the title (or "(No title)"), the time and place, no timestamp; a tap opens
 * the event; Snooze */
static void
notify (Service *self, CalEvent *event)
{
  g_autofree char *occurrence = cal_alert_occurrence_key (event);
  g_autofree char *body = cal_format_reminder_text (event->start, event->all_day, event->location, now_seconds ());
  g_autofree char *markup = g_markup_escape_text (body, -1);
  const char *title = event->title && *event->title ? event->title : _("(No title)");
  int snooze = g_settings_get_int (self->settings, "default-snooze");
  const char *actions_snooze[] = { "default", _("Open"), "snooze", _("Snooze"), NULL };
  const char *actions_plain[] = { "default", _("Open"), NULL };
  GVariantBuilder hints;
  g_autoptr(GError) error = NULL;
  g_autoptr(GVariant) reply = NULL;
  guint32 id;
  Posted *posted;

  if (!g_settings_get_boolean (self->settings, "notifications"))
    return;
  g_variant_builder_init (&hints, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_add (&hints, "{sv}", "desktop-entry", g_variant_new_string (APP_ID));
  g_variant_builder_add (&hints, "{sv}", "category", g_variant_new_string ("x-gnome.calendar"));
  g_variant_builder_add (&hints, "{sv}", "x-gnome-privacy-scope", g_variant_new_string ("system"));
  g_variant_builder_add (&hints, "{sv}", "urgency", g_variant_new_byte (1));
  reply = g_dbus_connection_call_sync (self->session, NOTIFICATIONS, NOTIFICATIONS_PATH, NOTIFICATIONS, "Notify",
                                       g_variant_new ("(susss^asa{sv}i)", _("Calendar"),
                                                      replaces_for (self, occurrence), "", title, markup,
                                                      snooze >= 0 ? actions_snooze : actions_plain, &hints, -1),
                                       G_VARIANT_TYPE ("(u)"), G_DBUS_CALL_FLAGS_NONE, 5000, NULL, &error);
  if (!reply)
    {
      g_warning ("notify: %s", error->message);
      return;
    }
  g_variant_get (reply, "(u)", &id);
  posted = g_new0 (Posted, 1);
  posted->calendar = g_strdup (event->calendar);
  posted->uid = g_strdup (event->uid);
  posted->rid = g_strdup (event->rid);
  posted->occurrence = g_steal_pointer (&occurrence);
  posted->start = event->start;
  g_hash_table_replace (self->posted, GUINT_TO_POINTER (id), posted);
}

static void
close_notification (Service *self, guint32 id)
{
  g_dbus_connection_call (self->session, NOTIFICATIONS, NOTIFICATIONS_PATH, NOTIFICATIONS, "CloseNotification",
                          g_variant_new ("(u)", id), NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);
}

static void
on_action (GDBusConnection *bus, const char *sender, const char *path, const char *interface, const char *signal,
           GVariant *parameters, gpointer data)
{
  Service *self = data;
  guint32 id;
  const char *action;
  Posted *posted;

  g_variant_get (parameters, "(u&s)", &id, &action);
  posted = g_hash_table_lookup (self->posted, GUINT_TO_POINTER (id));
  if (!posted)
    return;
  if (g_str_equal (action, "default"))
    {
      /* the event's info in the app (its "open-event" action; D-Bus activation starts it) */
      GVariantBuilder parameter;

      g_variant_builder_init (&parameter, G_VARIANT_TYPE ("av"));
      g_variant_builder_add (&parameter, "v", g_variant_new ("(sssx)", posted->calendar, posted->uid,
                                                             posted->rid ? posted->rid : "", posted->start));
      g_dbus_connection_call (self->session, APP_ID, "/de/yesman/Calendar", "org.freedesktop.Application",
                              "ActivateAction", g_variant_new ("(sava{sv})", "open-event", &parameter, NULL), NULL,
                              G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);
    }
  else if (g_str_equal (action, "snooze"))
    {
      /* SnoozeAlarmsService: again after the default snooze delay */
      gint64 *time = g_new (gint64, 1);

      *time = now_seconds () + (gint64) MAX (g_settings_get_int (self->settings, "default-snooze"), 1) * 60;
      g_hash_table_replace (self->snoozed, g_strdup (posted->occurrence), time);
      save_state (self);
      schedule (self);
    }
  close_notification (self, id);
  g_hash_table_remove (self->posted, GUINT_TO_POINTER (id));
}

static void
on_closed (GDBusConnection *bus, const char *sender, const char *path, const char *interface, const char *signal,
           GVariant *parameters, gpointer data)
{
  Service *self = data;
  guint32 id, reason;

  g_variant_get (parameters, "(uu)", &id, &reason);
  g_hash_table_remove (self->posted, GUINT_TO_POINTER (id));
}

/* --- the times */

static void
tell_wakeup (Service *self, gint64 time)
{
  GVariantBuilder options;

  if (!self->system || time == self->sent_wakeup)
    return;
  self->sent_wakeup = time;
  g_variant_builder_init (&options, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_add (&options, "{sv}", "app", g_variant_new_string (APP_ID));
  g_dbus_connection_call (self->system, "de.yesman.Wakeup", "/de/yesman/Wakeup", "de.yesman.Wakeup", "Set",
                          g_variant_new ("(sxa{sv})", self->wakeup_name, time, &options), NULL,
                          G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);
}

static void
on_wakeup_appeared (GDBusConnection *bus, const char *name, const char *owner, gpointer data)
{
  Service *self = data;

  /* neo-wakeup (re)started: it knows nothing of ours */
  self->sent_wakeup = -1;
  schedule (self);
}

/* notify what is due, then set the timer (and the RTC) for what comes next */
static void
schedule (Service *self)
{
  gint64 now = now_seconds (), next_alert = 0, next;
  g_autoptr(GPtrArray) due = g_ptr_array_new_with_free_func ((GDestroyNotify) cal_alert_free);
  struct itimerspec at = { 0 };
  GHashTableIter iter;
  gpointer key, value;
  gboolean changed = FALSE;

  if (self->events)
    {
      cal_alerts_compute (self->events, self->fired, now, due, &next_alert);
      for (guint i = 0; i < due->len; i++)
        {
          CalAlert *alert = due->pdata[i];
          const CalCalendar *calendar = cal_store_find_calendar (self->store, alert->event->calendar);

          /* a calendar whose reminders are off (ESourceAlarms include-me), and the first start: handled, quietly */
          if (!self->first_run && (!calendar || calendar->alarms))
            notify (self, alert->event);
          for (guint k = 0; k < alert->keys->len; k++)
            mark_fired (self, alert->keys->pdata[k], alert->time);
          changed = TRUE;
        }
      if (self->first_run && cal_store_is_ready (self->store))
        {
          self->first_run = FALSE;
          changed = TRUE;
        }
    }

  /* the snoozed ones whose time came (still there: an occurrence that is gone is dropped) */
  g_hash_table_iter_init (&iter, self->snoozed);
  while (g_hash_table_iter_next (&iter, &key, &value))
    {
      gint64 time = *(gint64 *) value;

      if (time <= now)
        {
          CalEvent *event = find_occurrence (self, key);

          if (event)
            notify (self, event);
          if (event || (self->events && cal_store_is_ready (self->store)))
            {
              g_hash_table_iter_remove (&iter);
              changed = TRUE;
            }
        }
      else if (!next_alert || time < next_alert)
        next_alert = time;
    }
  if (changed)
    save_state (self);

  /* the timer: the next reminder, or the next read of the coming weeks */
  next = now + REREAD;
  if (next_alert && next_alert < next)
    next = next_alert;
  at.it_value.tv_sec = next;
  if (timerfd_settime (self->timer_fd, TFD_TIMER_ABSTIME | TFD_TIMER_CANCEL_ON_SET, &at, NULL) < 0)
    g_warning ("timerfd: %s", g_strerror (errno));
  /* and the RTC, so that a sleeping phone wakes for it */
  tell_wakeup (self, next_alert);
}

static gboolean
on_timer (int fd, GIOCondition condition, gpointer data)
{
  Service *self = data;
  guint64 expirations;

  /* ECANCELED: the clock was set; either way, look again */
  if (read (fd, &expirations, sizeof expirations) < 0 && errno != ECANCELED && errno != EAGAIN)
    g_warning ("timerfd: %s", g_strerror (errno));
  reread (self);
  return G_SOURCE_CONTINUE;
}

/* --- the events */

static void
on_events (GPtrArray *events, gpointer data)
{
  Service *self = data;

  if (!events)
    return;
  g_clear_pointer (&self->events, g_ptr_array_unref);
  self->events = g_ptr_array_ref (events);
  schedule (self);
}

static void
reread (Service *self)
{
  gint64 now = now_seconds ();

  g_cancellable_cancel (self->query);
  g_clear_object (&self->query);
  self->query = g_cancellable_new ();
  cal_store_query (self->store, now - LOOK_BACK, now + LOOK_AHEAD, self->query, on_events, self);
}

static void
on_store_changed (CalStore *store, Service *self)
{
  reread (self);
}

static void
on_settings_changed (GSettings *settings, const char *key, Service *self)
{
  schedule (self);
}

static void
on_name_lost (GDBusConnection *bus, const char *name, gpointer data)
{
  Service *self = data;

  /* another one runs, or the bus went away */
  g_message ("%s lost", name);
  g_main_loop_quit (self->loop);
}

static gboolean
on_term (gpointer data)
{
  g_main_loop_quit (((Service *) data)->loop);
  return G_SOURCE_REMOVE;
}

int
main (int argc, char **argv)
{
  Service service = { 0 }, *self = &service;
  g_autoptr(GError) error = NULL;

  setlocale (LC_ALL, "");
  bindtextdomain (GETTEXT_PACKAGE, LOCALEDIR);
  bind_textdomain_codeset (GETTEXT_PACKAGE, "UTF-8");
  textdomain (GETTEXT_PACKAGE);

  self->session = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, &error);
  if (!self->session)
    g_error ("session bus: %s", error->message);
  /* neo-wakeup is on the system bus; without it (a desktop) reminders still notify while the machine is up */
  self->system = g_bus_get_sync (G_BUS_TYPE_SYSTEM, NULL, NULL);
  self->loop = g_main_loop_new (NULL, FALSE);
  self->settings = g_settings_new (APP_ID);
  self->fired = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  self->snoozed = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
  self->posted = g_hash_table_new_full (NULL, NULL, NULL, posted_free);
  self->state_path = g_build_filename (g_get_user_state_dir (), "neo-calendar", "reminders.ini", NULL);
  self->sent_wakeup = -1;
  /* a test rig's service (tests/try-calendar.sh) wakes the phone under a key of its own, which the rig clears: it
   * must not move the real service's wake-up, nor leave one behind for its test events */
  self->wakeup_name = g_getenv ("CAL_RIG_DIR") ? "calendar-reminder-rig" : "calendar-reminder";
  load_state (self);

  self->timer_fd = timerfd_create (CLOCK_REALTIME, TFD_NONBLOCK | TFD_CLOEXEC);
  g_unix_fd_add (self->timer_fd, G_IO_IN, on_timer, self);
  self->action_signal = g_dbus_connection_signal_subscribe (self->session, NULL, NOTIFICATIONS, "ActionInvoked",
                                                            NOTIFICATIONS_PATH, NULL, G_DBUS_SIGNAL_FLAGS_NONE,
                                                            on_action, self, NULL);
  self->closed_signal = g_dbus_connection_signal_subscribe (self->session, NULL, NOTIFICATIONS,
                                                            "NotificationClosed", NOTIFICATIONS_PATH, NULL,
                                                            G_DBUS_SIGNAL_FLAGS_NONE, on_closed, self, NULL);
  if (self->system)
    self->wakeup_watch = g_bus_watch_name_on_connection (self->system, "de.yesman.Wakeup",
                                                         G_BUS_NAME_WATCHER_FLAGS_NONE, on_wakeup_appeared, NULL,
                                                         self, NULL);
  g_bus_own_name_on_connection (self->session, BUS_NAME, G_BUS_NAME_OWNER_FLAGS_NONE, NULL, on_name_lost, self,
                                NULL);

  /* every calendar shown in the app (Android rings the visible calendars' reminders) */
  self->store = cal_store_new (FALSE);
  g_signal_connect (self->store, "changed", G_CALLBACK (on_store_changed), self);
  g_signal_connect (self->store, "calendars-changed", G_CALLBACK (on_store_changed), self);
  g_signal_connect (self->settings, "changed", G_CALLBACK (on_settings_changed), self);
  g_unix_signal_add (SIGTERM, on_term, self);
  g_unix_signal_add (SIGINT, on_term, self);
  reread (self);

  g_main_loop_run (self->loop);

  save_state (self);
  g_cancellable_cancel (self->query);
  g_clear_object (&self->query);
  g_clear_object (&self->store);
  g_clear_pointer (&self->events, g_ptr_array_unref);
  g_object_unref (self->settings);
  return 0;
}
