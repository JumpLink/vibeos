/* The timers in the Clock's service (timer-service.h). */
#include "timer-service.h"
#include "alarm-klaxon.h"
#include "clock-notifier.h"
#include "elapsed.h"
#include "phone.h"
#include "timer-model.h"

#include <errno.h>
#include <glib/gi18n.h>
#include <glib/gstdio.h>
#include <glib-unix.h>
#include <sys/timerfd.h>
#include <unistd.h>

#define INTERFACE "neo.Clock.Timers"
#define OBJECT_PATH "/neo/Clock/Timers"
#define APP_ID "neo.Clock"
#define RINGING_APP_ID "neo.Clock.Ringing"
#define MAX_LENGTH ((99 * 3600 + 99 * 60 + 99) * 1000LL)        /* TimerSetupView's 99h 99m 99s */

/* sets of timer states, for timers_in */
#define IN(state) (1u << (state))
#define UNEXPIRED (IN (TIMER_RUNNING) | IN (TIMER_PAUSED))

static const char introspection[] =
  "<node>"
  "  <interface name='neo.Clock.Timers'>"
  /* every timer, the newest first: id u, state u (TimerState: 1 running, 2 paused, 3 expired, 4 reset, 5 missed),
   * length x, total-length x, last-start x, remaining x, label s, delete-after-use b. Times in ms; last-start is on
   * the elapsed clock (CLOCK_BOOTTIME), the same in every process: the time left now is remaining - (now -
   * last-start) for a running, expired or missed timer, and remaining for the others (timer_remaining) */
  "    <method name='GetTimers'><arg name='timers' type='aa{sv}' direction='out'/></method>"
  /* a new timer, reset; its id */
  "    <method name='AddTimer'>"
  "      <arg name='length' type='x' direction='in'/><arg name='label' type='s' direction='in'/>"
  "      <arg name='delete_after_use' type='b' direction='in'/><arg name='id' type='u' direction='out'/>"
  "    </method>"
  "    <method name='Start'><arg name='id' type='u' direction='in'/></method>"
  "    <method name='Pause'><arg name='id' type='u' direction='in'/></method>"
  /* back to its length; one that rang and was asked for by another app (delete-after-use) is deleted instead */
  "    <method name='Reset'><arg name='id' type='u' direction='in'/></method>"
  /* a minute more; one that ran out runs again with a minute */
  "    <method name='AddMinute'><arg name='id' type='u' direction='in'/></method>"
  "    <method name='Delete'><arg name='id' type='u' direction='in'/></method>"
  "    <method name='SetLabel'>"
  "      <arg name='id' type='u' direction='in'/><arg name='label' type='s' direction='in'/>"
  "    </method>"
  /* Stop on the ringing screen: every timer that rings is reset (or deleted, as Reset) */
  "    <method name='ResetExpired'/>"
  /* the timers changed: GetTimers again */
  "    <signal name='Changed'/>"
  "  </interface>"
  "</node>";

struct _TimerService {
  GDBusConnection *session, *system;
  GSettings *settings;
  char *store_path;
  TimerStore *store;
  AlarmKlaxon *klaxon;
  ClockNotifier *notifier;
  GDBusNodeInfo *node;
  guint registration;

  int expiry_fd;                /* CLOCK_BOOTTIME, at the end of the running timer that ends first */
  gint64 sent_wakeup;           /* what neo-wakeup was last told (Unix s); -1 for nothing yet */
  guint wakeup_watch;

  GHashTable *ringing;          /* the ids of the expired timers it rings for */
  int sleep_inhibitor;          /* logind's sleep lock while they ring, or -1 */
  gboolean foreground;

  /* the three notifications, and the timer each one's buttons act on */
  guint32 unexpired_id, missed_id, expired_id;
  guint unexpired_timer, missed_timer, expired_timer;
};

static void update (TimerService *self);

/* --- the timers */

static int
by_expiry (gconstpointer a, gconstpointer b, gpointer now)
{
  return timer_compare_expiry (*(Timer **) a, *(Timer **) b, *(gint64 *) now);
}

/* the timers in @states (IN bits), in DeskClock's EXPIRY_COMPARATOR order: the one that ends first first */
static GPtrArray *
timers_in (TimerService *self, guint states, gint64 now)
{
  GPtrArray *timers = g_ptr_array_new ();

  for (guint i = 0; i < self->store->timers->len; i++)
    {
      Timer *timer = self->store->timers->pdata[i];

      if (states & IN (timer->state))
        g_ptr_array_add (timers, timer);
    }
  g_ptr_array_sort_with_data (timers, by_expiry, &now);
  return timers;
}

/* TimerModel.doResetOrDeleteTimer */
static void
reset_or_delete (TimerService *self, Timer *timer)
{
  if ((timer->state == TIMER_EXPIRED || timer->state == TIMER_MISSED) && timer->delete_after_use)
    timer_store_remove (self->store, timer->id);
  else
    timer_reset (timer);
}

/* resetOrDeleteExpiredTimers, resetUnexpiredTimers, resetMissedTimers */
static void
reset_all (TimerService *self, guint states)
{
  g_autoptr(GPtrArray) timers = timers_in (self, states, elapsed_now ());

  /* (backwards: a deleted one goes from the store, and so from under this array's pointers) */
  for (guint i = timers->len; i-- > 0;)
    reset_or_delete (self, timers->pdata[i]);
}

/* the running timers that have run out start to ring */
static void
expire_due (TimerService *self)
{
  gint64 now = elapsed_now (), wall = elapsed_wall_now ();

  for (guint i = 0; i < self->store->timers->len; i++)
    {
      Timer *timer = self->store->timers->pdata[i];

      if (timer->state == TIMER_RUNNING && timer_expiration (timer) <= now)
        timer_expire (timer, now, wall);
    }
}

/* --- ringing */

/* TimerModel.updateRinger: the klaxon starts with the first timer to expire and stops with the last one reset; each
 * new one brings the ringing screen up (the shell raises it if it is there already) */
static void
sync_ringing (TimerService *self)
{
  g_autoptr(GPtrArray) expired = timers_in (self, IN (TIMER_EXPIRED), 0);
  g_autoptr(GHashTable) before = g_steal_pointer (&self->ringing);
  gboolean was_ringing = g_hash_table_size (before) > 0;
  gboolean new_one = FALSE;

  self->ringing = g_hash_table_new (NULL, NULL);
  for (guint i = 0; i < expired->len; i++)
    {
      gpointer id = GUINT_TO_POINTER (((Timer *) expired->pdata[i])->id);

      g_hash_table_add (self->ringing, id);
      if (!g_hash_table_contains (before, id))
        new_one = TRUE;
    }

  if (expired->len && !was_ringing)
    {
      if (self->sleep_inhibitor < 0)
        self->sleep_inhibitor = phone_inhibit_sleep (self->system, "A timer is ringing");
      alarm_klaxon_start (self->klaxon, NULL, g_settings_get_boolean (self->settings, "timer-vibrate"));
    }
  else if (!expired->len && was_ringing)
    {
      alarm_klaxon_stop (self->klaxon);
      phone_allow_sleep (&self->sleep_inhibitor);
    }
  if (new_one)
    phone_show_full_screen (self->session, RINGING_APP_ID);
}

/* --- the notifications (TimerNotificationBuilder) */

static const char *
label_or (const Timer *timer, const char *fallback)
{
  return *timer->label ? timer->label : fallback;
}

/* the running and paused timers': the one that ends first, or how many there are */
static void
show_unexpired (TimerService *self, GPtrArray *timers, gint64 now)
{
  const char *running_actions[] = { "default", "", "pause", _("Pause"), "add-minute", _("Add 1 min"), NULL };
  const char *paused_actions[] = { "default", "", "start", _("Resume"), "reset", _("Reset"), NULL };
  const char *several_actions[] = { "default", "", "reset-all", _("Reset all timers"), NULL };
  Timer *timer = timers->pdata[0];
  gboolean running = timer->state == TIMER_RUNNING;       /* (the first one runs if any does) */
  gint64 remaining = timer_remaining (timer, now);
  g_autofree char *title = NULL, *body = NULL;
  const char * const *actions;
  GVariant *hints = NULL;

  if (timers->len > 1)
    {
      title = g_strdup_printf (running ? _("%u timers") : _("%u timers paused"), timers->len);
      actions = several_actions;
    }
  else if (running)
    {
      title = g_strdup (label_or (timer, _("Timer")));
      actions = running_actions;
    }
  else
    {
      title = g_strdup (_("Timer paused"));
      actions = paused_actions;
    }

  /* running, the time left counts down in the header, a second ahead as the app rounds it up
   * (getChronometerBase); paused, it is the text */
  if (running)
    hints = clock_notifier_header_hints (elapsed_wall_now () + remaining + 1000, TRUE, NULL);
  else
    body = clock_notifier_format_elapsed (remaining / 1000);
  self->unexpired_timer = timer->id;
  /* Android's channel is "Timers" at default importance: in the list, no heads-up (alarm-notifications.c) */
  self->unexpired_id = clock_notifier_notify (self->notifier, self->unexpired_id, title, body, actions,
                                              CLOCK_URGENCY_LOW, hints);
}

/* buildHeadsUp: the ringing timers' */
static void
show_expired (TimerService *self, GPtrArray *timers)
{
  const char *one_actions[] = { "default", "", "stop", _("Stop"), "add-minute", _("Add 1 min"), NULL };
  const char *several_actions[] = { "default", "", "stop", _("Stop all timers"), NULL };
  Timer *timer = timers->pdata[0];
  g_autofree char *title = NULL;

  if (timers->len > 1)
    title = g_strdup_printf (_("%u timers expired"), timers->len);
  else
    title = g_strdup (label_or (timer, _("Time's up")));
  self->expired_timer = timer->id;
  /* the firing channel: it peeks, and stays while they ring */
  self->expired_id = clock_notifier_notify (self->notifier, self->expired_id, title, NULL,
                                            timers->len > 1 ? several_actions : one_actions, CLOCK_URGENCY_CRITICAL,
                                            NULL);
}

/* buildMissed */
static void
show_missed (TimerService *self, GPtrArray *timers)
{
  const char *one_actions[] = { "default", "", "reset", _("Reset"), NULL };
  const char *several_actions[] = { "default", "", "reset-all", _("Reset all timers"), NULL };
  Timer *timer = timers->pdata[0];
  g_autofree char *title = NULL;

  if (timers->len > 1)
    title = g_strdup_printf (_("%u timers missed"), timers->len);
  else if (*timer->label)
    title = g_strdup_printf (_("Missed timer: %s"), timer->label);
  else
    title = g_strdup (_("Missed timer"));
  self->missed_timer = timer->id;
  self->missed_id = clock_notifier_notify (self->notifier, self->missed_id, title, NULL,
                                           timers->len > 1 ? several_actions : one_actions, CLOCK_URGENCY_LOW, NULL);
}

static void
close_notification (TimerService *self, guint32 *id)
{
  clock_notifier_close (self->notifier, *id);
  *id = 0;
}

/* TimerModel's updateNotification, updateMissedNotification and updateHeadsUpNotification */
static void
update_notifications (TimerService *self)
{
  gint64 now = elapsed_now ();
  g_autoptr(GPtrArray) unexpired = timers_in (self, UNEXPIRED, now);
  g_autoptr(GPtrArray) missed = timers_in (self, IN (TIMER_MISSED), now);
  g_autoptr(GPtrArray) expired = timers_in (self, IN (TIMER_EXPIRED), now);

  /* the app in front shows its timers itself; the ringing ones' notification shows anyway */
  if (unexpired->len && !self->foreground)
    show_unexpired (self, unexpired, now);
  else
    close_notification (self, &self->unexpired_id);
  if (missed->len && !self->foreground)
    show_missed (self, missed);
  else
    close_notification (self, &self->missed_id);
  if (expired->len)
    show_expired (self, expired);
  else
    close_notification (self, &self->expired_id);
}

static void
on_notification (guint32 id, const char *action, gpointer data)
{
  TimerService *self = data;
  gint64 now = elapsed_now (), wall = elapsed_wall_now ();
  guint timer_id;
  Timer *timer;

  /* (a notification of ours already replaced: nothing) */
  if (id == self->unexpired_id)
    timer_id = self->unexpired_timer;
  else if (id == self->missed_id)
    timer_id = self->missed_timer;
  else if (id == self->expired_id)
    timer_id = self->expired_timer;
  else
    return;
  timer = timer_store_find (self->store, timer_id);

  if (g_str_equal (action, "closed") || g_str_equal (action, "gone"))
    {
      gboolean ongoing = id != self->missed_id;

      if (id == self->unexpired_id)
        self->unexpired_id = 0;
      else if (id == self->missed_id)
        self->missed_id = 0;
      else
        self->expired_id = 0;
      /* swiped away: the running and the ringing timers' come back, as Android lets no one swipe an ongoing
       * notification; the missed one stays away until the timers next change */
      if (ongoing && g_str_equal (action, "closed"))
        update_notifications (self);
      return;
    }
  if (g_str_equal (action, "default"))
    {
      /* the ringing ones open the ringing screen (ExpiredTimersActivity), the others the app at the timer */
      if (id == self->expired_id)
        phone_start_app (RINGING_APP_ID);
      else
        phone_open_app (self->session, APP_ID, "show-timer", g_variant_new_uint32 (timer_id));
      return;
    }

  if (g_str_equal (action, "stop"))
    reset_all (self, IN (TIMER_EXPIRED));
  else if (g_str_equal (action, "reset-all"))
    reset_all (self, id == self->missed_id ? IN (TIMER_MISSED) : UNEXPIRED);
  else if (!timer)
    return;
  else if (g_str_equal (action, "pause"))
    timer_pause (timer, now);
  else if (g_str_equal (action, "start"))
    timer_start (timer, now, wall);
  else if (g_str_equal (action, "add-minute"))
    timer_add_minute (timer, now, wall);
  else if (g_str_equal (action, "reset"))
    reset_or_delete (self, timer);
  update (self);
}

static void
on_notifications_restart (gpointer data)
{
  TimerService *self = data;

  self->unexpired_id = self->missed_id = self->expired_id = 0;
  update_notifications (self);
}

/* --- the times */

/* the timerfd at the first running timer's end, and neo-wakeup at that second (the RTC counts whole seconds: it
 * wakes the phone up to a second early, and neo-wakeup holds it up while the timerfd fires) */
static void
schedule (TimerService *self)
{
  gint64 now = elapsed_now ();
  Timer *next = NULL;
  struct itimerspec at = { 0 };
  gint64 wakeup = 0;

  for (guint i = 0; i < self->store->timers->len; i++)
    {
      Timer *timer = self->store->timers->pdata[i];

      if (timer->state == TIMER_RUNNING && (!next || timer_expiration (timer) < timer_expiration (next)))
        next = timer;
    }
  if (next)
    {
      gint64 end = MAX (timer_expiration (next), 0);

      /* (and 1 ns: a time of 0 would disarm it; one already over is due at once) */
      at.it_value.tv_sec = end / 1000;
      at.it_value.tv_nsec = end % 1000 * 1000000 + 1;
      wakeup = (elapsed_wall_now () + end - now) / 1000;
    }
  if (timerfd_settime (self->expiry_fd, TFD_TIMER_ABSTIME, &at, NULL) < 0)
    g_warning ("cannot set the timer expiry: %s", g_strerror (errno));
  if (wakeup != self->sent_wakeup)
    {
      self->sent_wakeup = wakeup;
      phone_wake_at (self->system, "timer", wakeup, FALSE);
    }
}

static gboolean
on_expiry (int fd, GIOCondition condition, gpointer data)
{
  TimerService *self = data;
  guint64 expirations;

  if (read (fd, &expirations, sizeof expirations) < 0 && errno != EAGAIN)
    g_warning ("timer expiry: %s", g_strerror (errno));
  expire_due (self);
  update (self);
  return G_SOURCE_CONTINUE;
}

static void
on_wakeup_appeared (GDBusConnection *bus, const char *name, const char *owner, gpointer data)
{
  TimerService *self = data;

  /* neo-wakeup (re)started: it knows nothing of ours */
  self->sent_wakeup = -1;
  schedule (self);
}

/* after every change: save, schedule the next end, ring or stop, the notifications, and tell the app */
static void
update (TimerService *self)
{
  g_autoptr(GError) error = NULL;

  if (!timer_store_save (self->store, self->store_path, &error))
    g_warning ("cannot save the timers: %s", error->message);
  schedule (self);
  sync_ringing (self);
  update_notifications (self);
  g_dbus_connection_emit_signal (self->session, NULL, OBJECT_PATH, INTERFACE, "Changed", NULL, NULL);
}

void
timer_service_set_foreground (TimerService *self, gboolean foreground)
{
  if (self->foreground == foreground)
    return;
  self->foreground = foreground;
  update_notifications (self);
}

void
timer_service_time_set (TimerService *self)
{
  gint64 now = elapsed_now (), wall = elapsed_wall_now ();

  for (guint i = 0; i < self->store->timers->len; i++)
    timer_update_after_time_set (self->store->timers->pdata[i], now, wall);
  /* the wake-up and the notification's chronometer are on the wall clock: both move */
  update (self);
}

/* --- D-Bus */

static GVariant *
timer_to_variant (const Timer *timer)
{
  GVariantBuilder dict;

  g_variant_builder_init (&dict, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_add (&dict, "{sv}", "id", g_variant_new_uint32 (timer->id));
  g_variant_builder_add (&dict, "{sv}", "state", g_variant_new_uint32 (timer->state));
  g_variant_builder_add (&dict, "{sv}", "length", g_variant_new_int64 (timer->length));
  g_variant_builder_add (&dict, "{sv}", "total-length", g_variant_new_int64 (timer->total_length));
  g_variant_builder_add (&dict, "{sv}", "last-start", g_variant_new_int64 (timer->last_start));
  g_variant_builder_add (&dict, "{sv}", "remaining", g_variant_new_int64 (timer->remaining));
  g_variant_builder_add (&dict, "{sv}", "label", g_variant_new_string (timer->label));
  g_variant_builder_add (&dict, "{sv}", "delete-after-use", g_variant_new_boolean (timer->delete_after_use));
  return g_variant_builder_end (&dict);
}

static void
on_method_call (GDBusConnection *bus, const char *sender, const char *path, const char *interface,
                const char *method, GVariant *parameters, GDBusMethodInvocation *invocation, gpointer data)
{
  TimerService *self = data;
  gint64 now = elapsed_now (), wall = elapsed_wall_now ();
  Timer *timer;
  guint id;

  if (g_str_equal (method, "GetTimers"))
    {
      GVariantBuilder timers;

      g_variant_builder_init (&timers, G_VARIANT_TYPE ("aa{sv}"));
      for (guint i = 0; i < self->store->timers->len; i++)
        g_variant_builder_add_value (&timers, timer_to_variant (self->store->timers->pdata[i]));
      g_dbus_method_invocation_return_value (invocation, g_variant_new ("(aa{sv})", &timers));
      return;
    }
  if (g_str_equal (method, "AddTimer"))
    {
      const char *label;
      gint64 length;
      gboolean delete_after_use;

      g_variant_get (parameters, "(x&sb)", &length, &label, &delete_after_use);
      if (length < TIMER_MIN_LENGTH || length > MAX_LENGTH)
        {
          g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS,
                                                 "no timer is %" G_GINT64_FORMAT " ms long", length);
          return;
        }
      timer = timer_store_add (self->store, timer_new (0, length, label, delete_after_use));
      id = timer->id;
      update (self);
      g_dbus_method_invocation_return_value (invocation, g_variant_new ("(u)", id));
      return;
    }
  if (g_str_equal (method, "ResetExpired"))
    {
      reset_all (self, IN (TIMER_EXPIRED));
      update (self);
      g_dbus_method_invocation_return_value (invocation, NULL);
      return;
    }

  /* the rest name a timer */
  g_variant_get_child (parameters, 0, "u", &id);
  if (!(timer = timer_store_find (self->store, id)))
    {
      g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS, "no timer %u", id);
      return;
    }
  if (g_str_equal (method, "Start"))
    timer_start (timer, now, wall);
  else if (g_str_equal (method, "Pause"))
    timer_pause (timer, now);
  else if (g_str_equal (method, "Reset"))
    reset_or_delete (self, timer);
  else if (g_str_equal (method, "AddMinute"))
    timer_add_minute (timer, now, wall);
  else if (g_str_equal (method, "Delete"))
    timer_store_remove (self->store, id);
  else if (g_str_equal (method, "SetLabel"))
    {
      const char *label;

      g_variant_get_child (parameters, 1, "&s", &label);
      timer_set_label (timer, label);
    }
  update (self);
  g_dbus_method_invocation_return_value (invocation, NULL);
}

static const GDBusInterfaceVTable vtable = { .method_call = on_method_call };

/* --- start */

/* the store, or a new one if it cannot be read: the unreadable file is kept aside, never overwritten */
static TimerStore *
load_store (const char *path)
{
  g_autoptr(GError) error = NULL;
  TimerStore *store = timer_store_load (path, &error);
  g_autofree char *aside = NULL;

  if (store)
    return store;
  aside = g_strconcat (path, ".unreadable", NULL);
  g_warning ("cannot read %s (%s): kept as %s, starting with no timers", path, error->message, aside);
  g_rename (path, aside);
  return timer_store_load (path, NULL);
}

/* TimerModel.updateTimersAfterReboot: the store is from another boot, so its elapsed-clock times mean nothing now */
static void
catch_up_after_reboot (TimerService *self)
{
  g_autofree char *boot = elapsed_boot_id ();
  gint64 now = elapsed_now (), wall = elapsed_wall_now ();

  if (*self->store->boot_id && *boot && !g_str_equal (self->store->boot_id, boot))
    for (guint i = 0; i < self->store->timers->len; i++)
      {
        Timer *timer = self->store->timers->pdata[i];

        timer_update_after_reboot (timer, now, wall);
        /* run out over a minute before the phone came back: missed, it does not ring */
        if (timer->state == TIMER_RUNNING && timer_remaining (timer, now) < TIMER_MISSED_THRESHOLD)
          timer_miss (timer, now, wall);
      }
  g_free (self->store->boot_id);
  self->store->boot_id = g_steal_pointer (&boot);
}

TimerService *
timer_service_new (GDBusConnection *session, GDBusConnection *system, GSettings *settings, const char *store_path,
                   GError **error)
{
  TimerService *self = g_new0 (TimerService, 1);

  self->session = g_object_ref (session);
  self->system = system ? g_object_ref (system) : NULL;
  self->settings = g_object_ref (settings);
  self->store_path = g_strdup (store_path);
  self->store = load_store (store_path);
  self->klaxon = alarm_klaxon_new (settings, ALARM_KLAXON_TIMER);
  self->notifier = clock_notifier_new (on_notification, on_notifications_restart, self);
  self->ringing = g_hash_table_new (NULL, NULL);
  self->sleep_inhibitor = -1;
  self->sent_wakeup = -1;

  self->node = g_dbus_node_info_new_for_xml (introspection, NULL);
  self->registration = g_dbus_connection_register_object (session, OBJECT_PATH, self->node->interfaces[0], &vtable,
                                                          self, NULL, error);
  if (!self->registration)
    return NULL;
  self->expiry_fd = timerfd_create (CLOCK_BOOTTIME, TFD_NONBLOCK | TFD_CLOEXEC);
  g_unix_fd_add (self->expiry_fd, G_IO_IN, on_expiry, self);
  if (system)
    self->wakeup_watch = g_bus_watch_name_on_connection (system, "neo.Wakeup", G_BUS_NAME_WATCHER_FLAGS_NONE,
                                                         on_wakeup_appeared, NULL, self, NULL);

  /* what came due while nothing ran rings now; the timerfd, set in the past, fires at once */
  catch_up_after_reboot (self);
  update (self);
  return self;
}
