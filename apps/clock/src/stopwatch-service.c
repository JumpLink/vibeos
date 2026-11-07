/* The stopwatch in the Clock's service (stopwatch-service.h). */
#include "stopwatch-service.h"
#include "clock-notifier.h"
#include "elapsed.h"
#include "phone.h"
#include "stopwatch-model.h"

#include <glib/gi18n.h>
#include <glib/gstdio.h>

#define INTERFACE "de.yesman.Clock.Stopwatch"
#define OBJECT_PATH "/de/yesman/Clock/Stopwatch"
#define APP_ID "de.yesman.Clock"

static const char introspection[] =
  "<node>"
  "  <interface name='de.yesman.Clock.Stopwatch'>"
  /* state u (0 reset, 1 running, 2 paused), last-start x, accumulated x, laps ax. Times in ms; last-start is on the
   * elapsed clock (CLOCK_BOOTTIME): the time on it now is accumulated + (now - last-start) while running, and
   * accumulated otherwise. laps: the time on it at the end of each lap, the first lap first */
  "    <method name='Get'><arg name='stopwatch' type='a{sv}' direction='out'/></method>"
  "    <method name='Start'/>"
  "    <method name='Pause'/>"
  "    <method name='Reset'/>"
  /* a lap ends now; an error when it is not running or has all the laps it can (98) */
  "    <method name='Lap'/>"
  /* it changed: Get again */
  "    <signal name='Changed'/>"
  "  </interface>"
  "</node>";

struct _StopwatchService {
  GDBusConnection *session;
  char *store_path;
  Stopwatch *stopwatch;
  ClockNotifier *notifier;
  GDBusNodeInfo *node;
  guint registration;
  gboolean foreground;
  guint32 notification;         /* the server's id for ours, or 0 */
};

/* --- the notification (StopwatchNotificationBuilder) */

/* StopwatchModel.updateNotification: none while it is reset or the app is in front */
static void
update_notification (StopwatchService *self)
{
  const char *running_actions[] = { "default", "", "pause", _("Pause"), "lap", _("Lap"), NULL };
  const char *paused_actions[] = { "default", "", "start", _("Resume"), "reset", _("Reset"), NULL };
  Stopwatch *stopwatch = self->stopwatch;
  gint64 total = stopwatch_total (stopwatch, elapsed_now ());
  g_autofree char *body = NULL;
  GVariant *hints;

  if (stopwatch->state == STOPWATCH_RESET || self->foreground)
    {
      clock_notifier_close (self->notifier, self->notification);
      self->notification = 0;
      return;
    }

  if (stopwatch->state == STOPWATCH_RUNNING)
    {
      /* the time on it counts up in the header; the text is the lap it is in, once there are laps */
      if (stopwatch->laps->len)
        body = g_strdup_printf (_("Lap %u"), stopwatch->laps->len + 1);
      hints = clock_notifier_header_hints (elapsed_wall_now () - total, FALSE, NULL);
      /* no Lap at the limit */
      if (!stopwatch_can_lap (stopwatch))
        running_actions[4] = NULL;
    }
  else
    {
      body = clock_notifier_format_elapsed (total / 1000);
      hints = clock_notifier_header_hints (0, FALSE, _("Paused"));
    }
  /* Android's channel is "Stopwatch" at default importance: in the list, no heads-up (alarm-notifications.c) */
  self->notification = clock_notifier_notify (self->notifier, self->notification, _("Stopwatch"), body,
                                              stopwatch->state == STOPWATCH_RUNNING ? running_actions : paused_actions,
                                              CLOCK_URGENCY_LOW, hints);
}

/* --- changes */

/* after every change: save, the notification, and tell the app */
static void
update (StopwatchService *self)
{
  g_autoptr(GError) error = NULL;

  if (!stopwatch_save (self->stopwatch, self->store_path, &error))
    g_warning ("cannot save the stopwatch: %s", error->message);
  update_notification (self);
  g_dbus_connection_emit_signal (self->session, NULL, OBJECT_PATH, INTERFACE, "Changed", NULL, NULL);
}

static void
on_notification (guint32 id, const char *action, gpointer data)
{
  StopwatchService *self = data;
  gint64 now = elapsed_now ();

  if (id != self->notification)
    return;
  if (g_str_equal (action, "closed") || g_str_equal (action, "gone"))
    {
      self->notification = 0;
      /* swiped away: it comes back, as Android's is ongoing */
      if (g_str_equal (action, "closed"))
        update_notification (self);
      return;
    }
  if (g_str_equal (action, "default"))
    {
      phone_open_app (self->session, APP_ID, "show-stopwatch", NULL);
      return;
    }
  if (g_str_equal (action, "pause"))
    stopwatch_pause (self->stopwatch, now);
  else if (g_str_equal (action, "start"))
    stopwatch_start (self->stopwatch, now, elapsed_wall_now ());
  else if (g_str_equal (action, "lap"))
    stopwatch_lap (self->stopwatch, now);
  else if (g_str_equal (action, "reset"))
    stopwatch_reset (self->stopwatch);
  update (self);
}

static void
on_notifications_restart (gpointer data)
{
  StopwatchService *self = data;

  self->notification = 0;
  update_notification (self);
}

void
stopwatch_service_set_foreground (StopwatchService *self, gboolean foreground)
{
  if (self->foreground == foreground)
    return;
  self->foreground = foreground;
  update_notification (self);
}

void
stopwatch_service_time_set (StopwatchService *self)
{
  stopwatch_update_after_time_set (self->stopwatch, elapsed_now (), elapsed_wall_now ());
  /* the notification's chronometer is on the wall clock: it moves */
  update (self);
}

/* --- D-Bus */

static GVariant *
stopwatch_to_variant (const Stopwatch *stopwatch)
{
  GVariantBuilder dict;

  g_variant_builder_init (&dict, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_add (&dict, "{sv}", "state", g_variant_new_uint32 (stopwatch->state));
  g_variant_builder_add (&dict, "{sv}", "last-start", g_variant_new_int64 (stopwatch->last_start));
  g_variant_builder_add (&dict, "{sv}", "accumulated", g_variant_new_int64 (stopwatch->accumulated));
  g_variant_builder_add (&dict, "{sv}", "laps",
                         g_variant_new_fixed_array (G_VARIANT_TYPE_INT64, stopwatch->laps->data, stopwatch->laps->len,
                                                    sizeof (gint64)));
  return g_variant_builder_end (&dict);
}

static void
on_method_call (GDBusConnection *bus, const char *sender, const char *path, const char *interface,
                const char *method, GVariant *parameters, GDBusMethodInvocation *invocation, gpointer data)
{
  StopwatchService *self = data;
  gint64 now = elapsed_now ();

  if (g_str_equal (method, "Get"))
    {
      g_dbus_method_invocation_return_value (invocation, g_variant_new ("(@a{sv})",
                                                                        stopwatch_to_variant (self->stopwatch)));
      return;
    }
  if (g_str_equal (method, "Start"))
    stopwatch_start (self->stopwatch, now, elapsed_wall_now ());
  else if (g_str_equal (method, "Pause"))
    stopwatch_pause (self->stopwatch, now);
  else if (g_str_equal (method, "Reset"))
    stopwatch_reset (self->stopwatch);
  else if (!stopwatch_lap (self->stopwatch, now))
    {
      g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR, G_DBUS_ERROR_FAILED,
                                             "no lap: the stopwatch is not running, or has %d laps",
                                             STOPWATCH_MAX_LAPS);
      return;
    }
  update (self);
  g_dbus_method_invocation_return_value (invocation, NULL);
}

static const GDBusInterfaceVTable vtable = { .method_call = on_method_call };

/* --- start */

/* the stopwatch in the store, or a reset one if it cannot be read: the unreadable file is kept aside */
static Stopwatch *
load_store (const char *path)
{
  g_autoptr(GError) error = NULL;
  Stopwatch *stopwatch = stopwatch_load (path, &error);
  g_autofree char *aside = NULL;

  if (stopwatch)
    return stopwatch;
  aside = g_strconcat (path, ".unreadable", NULL);
  g_warning ("cannot read %s (%s): kept as %s, starting reset", path, error->message, aside);
  g_rename (path, aside);
  return stopwatch_new ();
}

StopwatchService *
stopwatch_service_new (GDBusConnection *session, const char *store_path, GError **error)
{
  StopwatchService *self = g_new0 (StopwatchService, 1);
  g_autofree char *boot = elapsed_boot_id ();

  self->session = g_object_ref (session);
  self->store_path = g_strdup (store_path);
  self->stopwatch = load_store (store_path);
  self->notifier = clock_notifier_new (on_notification, on_notifications_restart, self);

  self->node = g_dbus_node_info_new_for_xml (introspection, NULL);
  self->registration = g_dbus_connection_register_object (session, OBJECT_PATH, self->node->interfaces[0], &vtable,
                                                          self, NULL, error);
  if (!self->registration)
    return NULL;

  /* saved in another boot: the elapsed clock started again, the wall clock says for how long it was off, and a
   * running stopwatch counts that time (StopwatchModel's updateAfterReboot) */
  if (*self->stopwatch->boot_id && *boot && !g_str_equal (self->stopwatch->boot_id, boot))
    stopwatch_update_after_reboot (self->stopwatch, elapsed_now (), elapsed_wall_now ());
  g_free (self->stopwatch->boot_id);
  self->stopwatch->boot_id = g_steal_pointer (&boot);
  update (self);
  return self;
}
