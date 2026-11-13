/* neo-calendar-reminders: the Calendar's reminders, as Etar's AlertService and Android's AlarmManager ring them
 * (spec/calendar.md §10).
 *
 * It runs from login, whether or not the app is open (a systemd user service, neo.Calendar.Reminders on the
 * session bus). It reads the shown calendars' events of the coming weeks from evolution-data-server (CalStore),
 * and at each reminder's time posts a notification: the event's title, its time and place (AlertUtils
 * .formatTimeLocation), a tap opening the event in the app. The phone may be asleep then: neo-wakeup wakes it at
 * the next reminder's time (the RTC), and a CLOCK_REALTIME timerfd here does the rest.
 *
 * Its buttons are AlertReceiver.buildBasicNotification's, in its order and at most three: Map (the place looked up
 * in the maps app, when the location reads as a place and something opens geo: links), Call (the number in the
 * location, when something opens tel: links), Email guests (the app's quick responses, then the mail app: only
 * with guests to write to and a mail app), Snooze (not when the default snooze delay is "None"). Snooze waits the
 * default snooze delay, or with "Ask for reminder snooze delay" asks the app for its Snooze delay dialog, which
 * answers with Snooze() here. The notification is resident: Map, Call and Email guests leave it, as Etar's do; a
 * tap, Snooze and a swipe dismiss it.
 *
 * A notified reminder is fired until it is dismissed (CalendarAlerts' STATE_FIRED): by a tap, Snooze, a swipe, or
 * the app's reminders page (AlertActivity), which reads them with GetFired() and dismisses with Dismiss() and
 * DismissAll(). The Pop-up notification setting has the app open that page as each reminder notifies (Etar's
 * full-screen intent). FiredChanged() tells the page when the list changed. A fired reminder whose event ended
 * more than a day ago (the oldest this service reads) is forgotten. A new notification server (the next login, the
 * shell restarted) has none of the fired ones' notifications: they are posted again, quietly, as AlertService posts
 * the fired ones again after a boot.
 *
 * Reminders that came due while nothing ran (the phone was off) still notify while their event has not ended, as
 * Etar's missed alerts do; what was notified and what is fired are kept in $XDG_STATE_HOME/neo-calendar
 * /reminders.ini, so a restart does not notify it again. The first start notifies nothing that is already past (no
 * flood on installing).
 *
 * The "Notifications" setting off: nothing is posted (the reminders are still marked as handled); nor for an
 * event Skip Reminders skips (cal_alert_skipped). The notification channel's sound and vibration are the
 * Notifications page's: the sound goes to the shell as the notification's sound hint, and the vibration (Android's
 * default pattern) to feedbackd, unless its profile is silent, on a phone that can vibrate. Times are in the home
 * time zone when it is used, as the app's. */
#include "cal-alerts.h"
#include "cal-format.h"
#include "cal-store.h"

#include <errno.h>
#include <glib/gi18n.h>
#include <glib-unix.h>
#include <locale.h>
#include <sys/timerfd.h>
#include <unistd.h>

#define BUS_NAME        "neo.Calendar.Reminders"
#define OBJECT_PATH     "/neo/Calendar/Reminders"
#define APP_ID          "neo.Calendar"
#define APP_PATH        "/neo/Calendar"
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
  GHashTable *shown;                    /* occurrence key -> its notification's id (0: unknown): fired, not
                                         * dismissed */
  char *state_path;
  char *region;                         /* the phone's region, for the links in a location */
  char *server;                         /* the notification server the ids in shown are from: the bus's GUID and
                                         * its unique name */
  guint object_id, server_watch;
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
  g_strfreev (keys);
  self->server = g_key_file_get_string (file, "service", "notifications", NULL);
  keys = g_key_file_get_keys (file, "shown", NULL, NULL);
  for (int i = 0; keys && keys[i]; i++)
    g_hash_table_insert (self->shown, g_strdup (keys[i]),
                         GUINT_TO_POINTER ((guint) g_key_file_get_uint64 (file, "shown", keys[i], NULL)));
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
  g_hash_table_iter_init (&iter, self->shown);
  while (g_hash_table_iter_next (&iter, &key, &value))
    g_key_file_set_uint64 (file, "shown", key, GPOINTER_TO_UINT (value));
  if (self->server)
    g_key_file_set_string (file, "service", "notifications", self->server);
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

/* the channel's sound: "default" the sound theme's message-new-instant (Android's default notification sound),
 * "" none, else that file; the shell plays the hint */
static void
add_sound (Service *self, GVariantBuilder *hints)
{
  g_autofree char *sound = g_settings_get_string (self->settings, "reminder-sound");

  if (!*sound)
    g_variant_builder_add (hints, "{sv}", "suppress-sound", g_variant_new_boolean (TRUE));
  else if (g_str_equal (sound, "default"))
    g_variant_builder_add (hints, "{sv}", "sound-name", g_variant_new_string ("message-new-instant"));
  else
    g_variant_builder_add (hints, "{sv}", "sound-file", g_variant_new_string (sound));
}

/* the channel's vibration: Notification.DEFAULT_VIBRATE ({0, 250, 250, 250}) through feedbackd, which its silent
 * profile stops (Android's silent ringer mode); quiet still vibrates (vibrate mode) */
static void
vibrate (Service *self)
{
  GVariantBuilder pattern;
  g_autoptr(GVariant) profile = NULL;

  if (!g_settings_get_boolean (self->settings, "reminder-vibrate") || !cal_alerts_can_vibrate ())
    return;
  profile = g_dbus_connection_call_sync (self->session, "org.sigxcpu.Feedback", "/org/sigxcpu/Feedback",
                                         "org.freedesktop.DBus.Properties", "Get",
                                         g_variant_new ("(ss)", "org.sigxcpu.Feedback", "Profile"),
                                         G_VARIANT_TYPE ("(v)"), G_DBUS_CALL_FLAGS_NONE, 2000, NULL, NULL);
  if (profile)
    {
      g_autoptr(GVariant) value = NULL;

      g_variant_get (profile, "(v)", &value);
      if (g_variant_is_of_type (value, G_VARIANT_TYPE_STRING) && g_str_equal (g_variant_get_string (value, NULL),
                                                                              "silent"))
        return;
    }
  g_variant_builder_init (&pattern, G_VARIANT_TYPE ("a(du)"));
  g_variant_builder_add (&pattern, "(du)", 1.0, 250);
  g_variant_builder_add (&pattern, "(du)", 0.0, 250);
  g_variant_builder_add (&pattern, "(du)", 1.0, 250);
  g_dbus_connection_call (self->session, "org.sigxcpu.Feedback", "/org/sigxcpu/Feedback",
                          "org.sigxcpu.Feedback.Haptic", "Vibrate", g_variant_new ("(sa(du))", APP_ID, &pattern),
                          NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);
}

/* the app's actions (it is D-Bus activatable: this starts it when it is not running) */
static void
app_action (Service *self, const char *name, GVariant *parameter)
{
  GVariantBuilder parameters;

  g_variant_builder_init (&parameters, G_VARIANT_TYPE ("av"));
  if (parameter)
    g_variant_builder_add (&parameters, "v", parameter);
  g_dbus_connection_call (self->session, APP_ID, APP_PATH, "org.freedesktop.Application", "ActivateAction",
                          g_variant_new ("(sava{sv})", name, &parameters, NULL), NULL, G_DBUS_CALL_FLAGS_NONE, -1,
                          NULL, NULL, NULL);
}

static gboolean
can_open (const char *scheme)
{
  g_autoptr(GAppInfo) app = g_app_info_get_default_for_uri_scheme (scheme);

  return app != NULL;
}

/* the calendar owner's address, to leave out of the guests to write to */
static const char *
owner_of (Service *self, const CalEvent *event)
{
  const CalCalendar *calendar = cal_store_find_calendar (self->store, event->calendar);

  return calendar ? calendar->email : NULL;
}

static void
fired_changed (Service *self)
{
  g_dbus_connection_emit_signal (self->session, NULL, OBJECT_PATH, BUS_NAME, "FiredChanged", NULL, NULL);
}

/* The notification server's ids are its own. Another server (the shell restarted, the next login: unique names
 * start over on each bus, so the bus's GUID too) has none of these notifications, and gives their ids to others',
 * whose buttons must not act here nor be closed from here: the fired ones' ids are then forgotten (and those
 * notifications posted again, quietly, by adopt_shown, as AlertService posts the fired ones again after a boot). */
static void
check_server (Service *self)
{
  g_autoptr(GVariant) reply = NULL;
  g_autofree char *server = NULL;
  GHashTableIter iter;
  gpointer key;

  reply = g_dbus_connection_call_sync (self->session, "org.freedesktop.DBus", "/org/freedesktop/DBus",
                                       "org.freedesktop.DBus", "GetNameOwner", g_variant_new ("(s)", NOTIFICATIONS),
                                       G_VARIANT_TYPE ("(s)"), G_DBUS_CALL_FLAGS_NONE, 2000, NULL, NULL);
  if (reply)
    {
      const char *owner;

      g_variant_get (reply, "(&s)", &owner);
      server = g_strdup_printf ("%s %s", g_dbus_connection_get_guid (self->session), owner);
    }
  if (g_strcmp0 (server, self->server) == 0)
    return;
  g_hash_table_iter_init (&iter, self->shown);
  while (g_hash_table_iter_next (&iter, &key, NULL))
    g_hash_table_iter_replace (&iter, GUINT_TO_POINTER (0));
  g_hash_table_remove_all (self->posted);
  g_free (self->server);
  self->server = g_steal_pointer (&server);
  save_state (self);
}

/* AlertReceiver.buildBasicNotification (the expanding notification of an event that is on, the one with buttons):
 * the title (or "(No title)"), the time and place, no timestamp; a tap opens the event; Map, Call, Email guests and
 * Snooze as they apply, at most three */
static void
notify (Service *self, CalEvent *event, gboolean quiet)
{
  g_autofree char *occurrence = cal_alert_occurrence_key (event);
  g_autofree char *body = cal_format_reminder_text (event->start, event->all_day, event->location, now_seconds ());
  g_autofree char *markup = g_markup_escape_text (body, -1);
  g_autofree char *geo = NULL, *tel = NULL, *mail = NULL;
  const char *title = event->title && *event->title ? event->title : _("(No title)");
  CalAlertAction actions[CAL_ALERT_MAX_ACTIONS];
  static const char *const names[] = { "map", "call", "email", "snooze" };
  const char *labels[] = { _("Map"), _("Call"), _("Email guests"), _("Snooze") };
  GPtrArray *buttons = g_ptr_array_new ();
  GVariantBuilder hints;
  g_autoptr(GError) error = NULL;
  g_autoptr(GVariant) reply = NULL;
  guint32 id;
  int n;
  Posted *posted;

  if (!g_settings_get_boolean (self->settings, "notifications"))
    {
      g_ptr_array_unref (buttons);
      return;
    }
  cal_alert_location_links (event->location, self->region, &geo, &tel);
  mail = cal_event_mail_uri (event, owner_of (self, event), NULL);
  n = cal_alert_actions (geo && can_open ("geo"), tel && can_open ("tel"), mail && can_open ("mailto"),
                         g_settings_get_int (self->settings, "default-snooze") >= 0, actions);
  g_ptr_array_add (buttons, (gpointer) "default");
  g_ptr_array_add (buttons, (gpointer) _("Open"));
  for (int i = 0; i < n; i++)
    {
      g_ptr_array_add (buttons, (gpointer) names[actions[i]]);
      g_ptr_array_add (buttons, (gpointer) labels[actions[i]]);
    }
  g_ptr_array_add (buttons, NULL);

  g_variant_builder_init (&hints, G_VARIANT_TYPE_VARDICT);
  if (quiet)
    g_variant_builder_add (&hints, "{sv}", "suppress-sound", g_variant_new_boolean (TRUE));
  else
    add_sound (self, &hints);
  g_variant_builder_add (&hints, "{sv}", "desktop-entry", g_variant_new_string (APP_ID));
  g_variant_builder_add (&hints, "{sv}", "category", g_variant_new_string ("x-gnome.calendar"));
  g_variant_builder_add (&hints, "{sv}", "x-gnome-privacy-scope", g_variant_new_string ("system"));
  g_variant_builder_add (&hints, "{sv}", "urgency", g_variant_new_byte (1));
  /* a button leaves it up (Map, Call, Email guests); the service closes it for the rest */
  g_variant_builder_add (&hints, "{sv}", "resident", g_variant_new_boolean (TRUE));
  reply = g_dbus_connection_call_sync (self->session, NOTIFICATIONS, NOTIFICATIONS_PATH, NOTIFICATIONS, "Notify",
                                       g_variant_new ("(susss^asa{sv}i)", _("Calendar"),
                                                      replaces_for (self, occurrence), "", title, markup,
                                                      (const char *const *) buttons->pdata, &hints, -1),
                                       G_VARIANT_TYPE ("(u)"), G_DBUS_CALL_FLAGS_NONE, 5000, NULL, &error);
  g_ptr_array_unref (buttons);
  if (!reply)
    {
      g_warning ("notify: %s", error->message);
      return;
    }
  g_variant_get (reply, "(u)", &id);
  check_server (self);
  if (!quiet)
    vibrate (self);
  posted = g_new0 (Posted, 1);
  posted->calendar = g_strdup (event->calendar);
  posted->uid = g_strdup (event->uid);
  posted->rid = g_strdup (event->rid);
  posted->occurrence = g_strdup (occurrence);
  posted->start = event->start;
  g_hash_table_replace (self->posted, GUINT_TO_POINTER (id), posted);
  g_hash_table_replace (self->shown, g_steal_pointer (&occurrence), GUINT_TO_POINTER (id));
  save_state (self);
  fired_changed (self);
  /* Pop-up notification: the reminders page over the app (setFullScreenIntent(AlertActivity)) */
  if (!quiet && g_settings_get_boolean (self->settings, "popup"))
    app_action (self, "show-alerts", NULL);
}

static void
close_notification (Service *self, guint32 id)
{
  g_dbus_connection_call (self->session, NOTIFICATIONS, NOTIFICATIONS_PATH, NOTIFICATIONS, "CloseNotification",
                          g_variant_new ("(u)", id), NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);
}

/* STATE_DISMISSED: off the reminders page, and its notification closed (DismissAlarmsService) */
static gboolean
dismiss (Service *self, const char *occurrence)
{
  gpointer id;
  gboolean was = g_hash_table_lookup_extended (self->shown, occurrence, NULL, &id);
  guint32 posted = replaces_for (self, occurrence);

  if (posted)
    {
      close_notification (self, posted);
      g_hash_table_remove (self->posted, GUINT_TO_POINTER (posted));
    }
  else if (was && GPOINTER_TO_UINT (id))
    /* posted by this service before a restart: the server may still show it */
    close_notification (self, GPOINTER_TO_UINT (id));
  g_hash_table_remove (self->shown, occurrence);
  return was;
}

/* SnoozeAlarmsService: dismissed now, notified again after @minutes */
static void
snooze (Service *self, const char *occurrence, int minutes)
{
  gint64 *time = g_new (gint64, 1);

  *time = now_seconds () + (gint64) MAX (minutes, 0) * 60;
  g_hash_table_replace (self->snoozed, g_strdup (occurrence), time);
  dismiss (self, occurrence);
  save_state (self);
  fired_changed (self);
  schedule (self);
}

/* the event as Email guests needs it in the app: (title, organizer, owner, [(address, name, status)]) */
static GVariant *
mail_parameter (Service *self, const CalEvent *event)
{
  GVariantBuilder guests;

  g_variant_builder_init (&guests, G_VARIANT_TYPE ("a(ssu)"));
  for (guint i = 0; i < event->attendees->len; i++)
    {
      const CalAttendee *guest = event->attendees->pdata[i];

      if (guest->email)
        g_variant_builder_add (&guests, "(ssu)", guest->email, guest->name ? guest->name : "", guest->status);
    }
  return g_variant_new ("(sssa(ssu))", event->title ? event->title : "", event->organizer ? event->organizer : "",
                        owner_of (self, event) ? owner_of (self, event) : "", &guests);
}

/* Map and Call: the location read again (AlertReceiver's MAP_ACTION and CALL_ACTION), the link opened; an event
 * changed since leaves nothing to open */
static void
open_link (Service *self, const char *occurrence, gboolean map)
{
  CalEvent *event = find_occurrence (self, occurrence);
  g_autofree char *geo = NULL, *tel = NULL;
  g_autoptr(GError) error = NULL;

  if (!event)
    return;
  cal_alert_location_links (event->location, self->region, &geo, &tel);
  if ((map ? geo : tel) && !g_app_info_launch_default_for_uri (map ? geo : tel, NULL, &error))
    g_warning ("%s: %s", map ? geo : tel, error->message);
}

static void
on_action (GDBusConnection *bus, const char *sender, const char *path, const char *interface, const char *signal,
           GVariant *parameters, gpointer data)
{
  Service *self = data;
  guint32 id;
  const char *action;
  Posted *posted;
  g_autofree char *occurrence = NULL;

  if (!g_variant_is_of_type (parameters, G_VARIANT_TYPE ("(us)")))
    return;
  g_variant_get (parameters, "(u&s)", &id, &action);
  posted = g_hash_table_lookup (self->posted, GUINT_TO_POINTER (id));
  if (!posted)
    return;
  occurrence = g_strdup (posted->occurrence);
  if (g_str_equal (action, "default"))
    {
      /* the event's info in the app (its "open-event" action), and the reminder dismissed */
      app_action (self, "open-event", g_variant_new ("(sssx)", posted->calendar, posted->uid,
                                                     posted->rid ? posted->rid : "", posted->start));
      dismiss (self, occurrence);
      save_state (self);
      fired_changed (self);
    }
  else if (g_str_equal (action, "snooze"))
    {
      int minutes = g_settings_get_int (self->settings, "default-snooze");

      /* SnoozeDelayActivity: the app asks how long; its answer comes back as Snooze() */
      if (g_settings_get_boolean (self->settings, "custom-snooze"))
        app_action (self, "snooze-delay", g_variant_new ("(si)", occurrence, minutes));
      else
        snooze (self, occurrence, minutes);
    }
  else if (g_str_equal (action, "map") || g_str_equal (action, "call"))
    open_link (self, occurrence, g_str_equal (action, "map"));
  else if (g_str_equal (action, "email"))
    {
      /* QuickResponseActivity, in the app: a quick response, then the mail app; nothing is sent from here */
      CalEvent *event = find_occurrence (self, occurrence);

      if (event)
        app_action (self, "email-guests", mail_parameter (self, event));
    }
}

static void
on_closed (GDBusConnection *bus, const char *sender, const char *path, const char *interface, const char *signal,
           GVariant *parameters, gpointer data)
{
  Service *self = data;
  guint32 id, reason;
  Posted *posted;

  if (!g_variant_is_of_type (parameters, G_VARIANT_TYPE ("(uu)")))
    return;
  g_variant_get (parameters, "(uu)", &id, &reason);
  posted = g_hash_table_lookup (self->posted, GUINT_TO_POINTER (id));
  /* swiped away (reason 2, dismissed by the user): the delete intent, DismissAlarmsService */
  if (posted && reason == 2 && g_hash_table_remove (self->shown, posted->occurrence))
    {
      save_state (self);
      fired_changed (self);
    }
  g_hash_table_remove (self->posted, GUINT_TO_POINTER (id));
}

/* --- the reminders page's interface */

static const char introspection[] =
  "<node>"
  "  <interface name='neo.Calendar.Reminders'>"
  /* the fired reminders not dismissed, by start and title (CalendarAlerts.DEFAULT_SORT_ORDER): each its occurrence
   * key and calendar, uid, rid, start, end, all-day, title, location, color (the calendar's, 0xRRGGBB, as
   * CalendarAlerts.CALENDAR_COLOR), recurring (it has a repeat rule, CalendarAlerts.RRULE: AlertAdapter's repeat
   * icon) */
  "    <method name='GetFired'><arg type='a(sa{sv})' direction='out'/></method>"
  "    <method name='Dismiss'><arg name='occurrences' type='as' direction='in'/></method>"
  "    <method name='DismissAll'/>"
  "    <method name='Snooze'><arg name='occurrence' type='s' direction='in'/>"
  "      <arg name='minutes' type='i' direction='in'/></method>"
  "    <signal name='FiredChanged'/>"
  "  </interface>"
  "</node>";

static int
compare_fired (gconstpointer a, gconstpointer b)
{
  const CalEvent *x = *(CalEvent * const *) a, *y = *(CalEvent * const *) b;

  if (x->start != y->start)
    return x->start < y->start ? -1 : 1;
  return g_strcmp0 (x->title, y->title);
}

static GVariant *
get_fired (Service *self)
{
  g_autoptr(GPtrArray) events = g_ptr_array_new ();
  GVariantBuilder list;
  GHashTableIter iter;
  gpointer key;

  g_hash_table_iter_init (&iter, self->shown);
  while (g_hash_table_iter_next (&iter, &key, NULL))
    {
      CalEvent *event = find_occurrence (self, key);

      if (event)
        g_ptr_array_add (events, event);
    }
  g_ptr_array_sort (events, compare_fired);
  g_variant_builder_init (&list, G_VARIANT_TYPE ("a(sa{sv})"));
  for (guint i = 0; i < events->len; i++)
    {
      CalEvent *event = events->pdata[i];
      const CalCalendar *calendar = cal_store_find_calendar (self->store, event->calendar);
      g_autofree char *occurrence = cal_alert_occurrence_key (event);
      GVariantBuilder fields;

      g_variant_builder_init (&fields, G_VARIANT_TYPE_VARDICT);
      g_variant_builder_add (&fields, "{sv}", "calendar", g_variant_new_string (event->calendar));
      g_variant_builder_add (&fields, "{sv}", "uid", g_variant_new_string (event->uid ? event->uid : ""));
      g_variant_builder_add (&fields, "{sv}", "rid", g_variant_new_string (event->rid ? event->rid : ""));
      g_variant_builder_add (&fields, "{sv}", "start", g_variant_new_int64 (event->start));
      g_variant_builder_add (&fields, "{sv}", "end", g_variant_new_int64 (event->end));
      g_variant_builder_add (&fields, "{sv}", "all-day", g_variant_new_boolean (event->all_day));
      g_variant_builder_add (&fields, "{sv}", "title", g_variant_new_string (event->title ? event->title : ""));
      g_variant_builder_add (&fields, "{sv}", "location",
                             g_variant_new_string (event->location ? event->location : ""));
      g_variant_builder_add (&fields, "{sv}", "color", g_variant_new_uint32 (calendar ? calendar->color
                                                                                          : event->color));
      g_variant_builder_add (&fields, "{sv}", "recurring", g_variant_new_boolean (event->rrule && *event->rrule));
      g_variant_builder_add (&list, "(sa{sv})", occurrence, &fields);
    }
  return g_variant_new ("(a(sa{sv}))", &list);
}

static void
on_method (GDBusConnection *bus, const char *sender, const char *path, const char *interface, const char *method,
           GVariant *parameters, GDBusMethodInvocation *invocation, gpointer data)
{
  Service *self = data;

  if (g_str_equal (method, "GetFired"))
    {
      g_dbus_method_invocation_return_value (invocation, get_fired (self));
      return;
    }
  if (g_str_equal (method, "Dismiss"))
    {
      g_autofree const char **occurrences = NULL;
      gboolean changed = FALSE;

      g_variant_get (parameters, "(^a&s)", &occurrences);
      for (int i = 0; occurrences[i]; i++)
        changed |= dismiss (self, occurrences[i]);
      if (changed)
        {
          save_state (self);
          fired_changed (self);
        }
    }
  else if (g_str_equal (method, "DismissAll"))
    {
      /* AlertActivity's Dismiss all: every notification of the app cancelled (nm.cancelAll), every fired one
       * dismissed */
      GHashTableIter iter;
      gpointer id;

      /* every posted one is a fired one (posted only goes with shown) */
      g_hash_table_iter_init (&iter, self->shown);
      while (g_hash_table_iter_next (&iter, NULL, &id))
        if (GPOINTER_TO_UINT (id))
          close_notification (self, GPOINTER_TO_UINT (id));
      g_hash_table_remove_all (self->posted);
      g_hash_table_remove_all (self->shown);
      save_state (self);
      fired_changed (self);
    }
  else if (g_str_equal (method, "Snooze"))
    {
      const char *occurrence;
      int minutes;

      g_variant_get (parameters, "(&si)", &occurrence, &minutes);
      snooze (self, occurrence, minutes);
    }
  g_dbus_method_invocation_return_value (invocation, NULL);
}

static const GDBusInterfaceVTable vtable = { on_method, NULL, NULL, { 0 } };

/* the fired ones from before a restart: their buttons work again once their events are read, those of another
 * notification server posted again (quietly); those whose events are gone (ended more than LOOK_BACK ago, deleted)
 * are forgotten */
static void
adopt_shown (Service *self)
{
  g_autoptr(GPtrArray) again = g_ptr_array_new ();
  GHashTableIter iter;
  gpointer key, value;
  gboolean changed = FALSE;

  if (!self->events)
    return;
  check_server (self);
  g_hash_table_iter_init (&iter, self->shown);
  while (g_hash_table_iter_next (&iter, &key, &value))
    {
      CalEvent *event = find_occurrence (self, key);
      guint32 id = GPOINTER_TO_UINT (value);

      if (!event)
        {
          /* not read yet, while some calendar is still loading; else gone, and its notification with it (the
           * provider's CalendarAlerts rows go with the event, and AlertService's update takes the notification) */
          if (cal_store_is_ready (self->store))
            {
              if (id)
                {
                  close_notification (self, id);
                  g_hash_table_remove (self->posted, value);
                }
              g_hash_table_iter_remove (&iter);
              changed = TRUE;
            }
        }
      else if (!id)
        g_ptr_array_add (again, event);
      else if (!g_hash_table_contains (self->posted, value))
        {
          Posted *posted = g_new0 (Posted, 1);

          posted->calendar = g_strdup (event->calendar);
          posted->uid = g_strdup (event->uid);
          posted->rid = g_strdup (event->rid);
          posted->occurrence = g_strdup (key);
          posted->start = event->start;
          g_hash_table_insert (self->posted, GUINT_TO_POINTER (id), posted);
        }
    }
  if (changed)
    {
      save_state (self);
      fired_changed (self);
    }
  for (guint i = 0; i < again->len; i++)
    notify (self, again->pdata[i], TRUE);
}

/* the notification server came or went */
static void
on_server (GDBusConnection *bus, const char *name, const char *owner, gpointer data)
{
  adopt_shown (data);
}

static void
on_server_gone (GDBusConnection *bus, const char *name, gpointer data)
{
  check_server (data);
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
  g_dbus_connection_call (self->system, "neo.Wakeup", "/neo/Wakeup", "neo.Wakeup", "Set",
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

          g_autofree char *skip = g_settings_get_string (self->settings, "skip-reminders");

          /* a calendar whose reminders are off (ESourceAlarms include-me), a skipped event, and the first start:
           * handled, quietly */
          if (!self->first_run && (!calendar || calendar->alarms) && !cal_alert_skipped (alert->event, skip))
            notify (self, alert->event, FALSE);
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
            notify (self, event, FALSE);
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
  adopt_shown (self);
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

/* the home time zone, as the app applies it */
static void
apply_home_zone (Service *self)
{
  g_autofree char *tzid = g_settings_get_string (self->settings, "home-tz");

  cal_set_home_zone (g_settings_get_boolean (self->settings, "home-tz-enabled") ? tzid : NULL);
}

static void
on_settings_changed (GSettings *settings, const char *key, Service *self)
{
  if (g_str_equal (key, "home-tz-enabled") || g_str_equal (key, "home-tz"))
    {
      apply_home_zone (self);
      cal_store_zone_changed (self->store);
      return;
    }
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
  self->shown = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  self->region = cal_alert_region ();
  self->state_path = g_build_filename (g_get_user_state_dir (), "neo-calendar", "reminders.ini", NULL);
  apply_home_zone (self);
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
    self->wakeup_watch = g_bus_watch_name_on_connection (self->system, "neo.Wakeup",
                                                         G_BUS_NAME_WATCHER_FLAGS_NONE, on_wakeup_appeared, NULL,
                                                         self, NULL);
  {
    g_autoptr(GDBusNodeInfo) node = g_dbus_node_info_new_for_xml (introspection, &error);

    g_assert (node);
    self->object_id = g_dbus_connection_register_object (self->session, OBJECT_PATH, node->interfaces[0], &vtable,
                                                         self, NULL, &error);
    if (!self->object_id)
      g_error ("%s: %s", OBJECT_PATH, error->message);
  }
  g_bus_own_name_on_connection (self->session, BUS_NAME, G_BUS_NAME_OWNER_FLAGS_NONE, NULL, on_name_lost, self,
                                NULL);
  self->server_watch = g_bus_watch_name_on_connection (self->session, NOTIFICATIONS, G_BUS_NAME_WATCHER_FLAGS_NONE,
                                                       on_server, on_server_gone, self, NULL);

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
  g_dbus_connection_unregister_object (self->session, self->object_id);
  g_cancellable_cancel (self->query);
  g_clear_object (&self->query);
  g_clear_object (&self->store);
  g_clear_pointer (&self->events, g_ptr_array_unref);
  g_object_unref (self->settings);
  g_hash_table_unref (self->shown);
  g_hash_table_unref (self->posted);
  g_hash_table_unref (self->snoozed);
  g_hash_table_unref (self->fired);
  g_free (self->region);
  g_free (self->server);
  g_free (self->state_path);
  return 0;
}
