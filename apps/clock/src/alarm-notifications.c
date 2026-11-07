/* The alarm notifications (alarm-notifications.h), over org.freedesktop.Notifications. */
#include "alarm-notifications.h"

#include <gio/gio.h>
#include <glib/gi18n.h>

#define NOTIFICATIONS "org.freedesktop.Notifications"
#define NOTIFICATIONS_PATH "/org/freedesktop/Notifications"

/* org.freedesktop.Notifications' urgencies and the reason a notification closed */
enum { URGENCY_LOW = 0, URGENCY_NORMAL = 1, URGENCY_CRITICAL = 2 };
enum { CLOSED_DISMISSED = 2 };

typedef struct {
  guint32 id;                   /* the server's id for it */
  AlarmInstance *instance;      /* what it shows: shown again if the server restarts */
} Shown;

struct _AlarmNotifications {
  GDBusConnection *bus;
  AlarmNotificationCallback callback;
  gpointer data;
  GHashTable *shown;            /* instance id -> Shown */
  guint action_signal, closed_signal, watch;
};

static void
shown_free (Shown *shown)
{
  alarm_instance_free (shown->instance);
  g_free (shown);
}

char *
alarm_format_time (GDateTime *time)
{
  g_autoptr(GSettings) interface = g_settings_new ("org.gnome.desktop.interface");

  /* the skeletons "EHm" and "Ehma" */
  if (g_settings_get_enum (interface, "clock-format") == 0 /* 24h */)
    return g_date_time_format (time, "%a %H:%M");
  return g_date_time_format (time, "%a %-l:%M %p");
}

/* AlarmInstance.getLabelOrDefault */
static const char *
label_or_default (const AlarmInstance *instance)
{
  return instance->label && *instance->label ? instance->label : _("Alarm");
}

/* AlarmUtils.getAlarmText: the time, and " - label" if it has one */
static char *
alarm_text (const AlarmInstance *instance, gboolean include_label)
{
  g_autoptr(GDateTime) time = alarm_instance_time (instance);
  g_autofree char *text = alarm_format_time (time);

  if (!include_label || !instance->label || !*instance->label)
    return g_steal_pointer (&text);
  return g_strdup_printf ("%s - %s", text, instance->label);
}

static guint32
notify (AlarmNotifications *self, guint32 replaces, const char *title, const char *body, const char * const *actions,
        guchar urgency)
{
  GVariantBuilder hints;
  g_autofree char *markup = g_markup_escape_text (body, -1);
  g_autoptr(GError) error = NULL;
  g_autoptr(GVariant) reply = NULL;
  guint32 id = 0;

  g_variant_builder_init (&hints, G_VARIANT_TYPE_VARDICT);
  /* the Clock's: its name and icon */
  g_variant_builder_add (&hints, "{sv}", "desktop-entry", g_variant_new_string ("de.yesman.Clock"));
  /* it stays when a button is pressed; we take it down when its state moves on */
  g_variant_builder_add (&hints, "{sv}", "resident", g_variant_new_boolean (TRUE));
  /* VISIBILITY_PUBLIC: all of it shows on the lock screen */
  g_variant_builder_add (&hints, "{sv}", "x-gnome-privacy-scope", g_variant_new_string ("system"));
  g_variant_builder_add (&hints, "{sv}", "urgency", g_variant_new_byte (urgency));

  /* a sync call: the shell answers at once, and the alarm's sound has already started when this runs */
  reply = g_dbus_connection_call_sync (self->bus, NOTIFICATIONS, NOTIFICATIONS_PATH, NOTIFICATIONS, "Notify",
                                       g_variant_new ("(susss^asa{sv}i)", "Clock", replaces, "", title, markup,
                                                      actions, &hints, 0),
                                       G_VARIANT_TYPE ("(u)"), G_DBUS_CALL_FLAGS_NONE, 3000, NULL, &error);
  if (reply)
    g_variant_get (reply, "(u)", &id);
  else
    g_warning ("notifications: %s", error->message);
  return id;
}

static void
close_notification (AlarmNotifications *self, guint32 id)
{
  g_dbus_connection_call (self->bus, NOTIFICATIONS, NOTIFICATIONS_PATH, NOTIFICATIONS, "CloseNotification",
                          g_variant_new ("(u)", id), NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);
}

void
alarm_notifications_show (AlarmNotifications *self, const AlarmInstance *instance)
{
  /* actions: key, label pairs; "default" is a tap on the notification */
  const char *upcoming_actions[] = { "default", "", "dismiss", _("Dismiss"), NULL };
  const char *ringing_actions[] = { "default", "", "snooze", _("Snooze"), "dismiss", _("Dismiss"), NULL };
  const char *missed_actions[] = { "default", "", NULL };
  Shown *shown = g_hash_table_lookup (self->shown, GUINT_TO_POINTER (instance->id));
  g_autofree char *title = NULL, *body = NULL;
  const char * const *actions;
  guchar urgency;

  /* Android's channels are "Upcoming" and "Snoozing" at default importance: in the list, no heads-up. Here a
   * normal notification peeks, so these are low ones (under "Silent" in the shade) */
  switch (instance->state)
    {
    case ALARM_STATE_LOW_NOTIFICATION:
    case ALARM_STATE_HIGH_NOTIFICATION:
      title = g_strdup (_("Upcoming alarm"));
      body = alarm_text (instance, TRUE);
      actions = upcoming_actions;
      urgency = URGENCY_LOW;
      break;
    case ALARM_STATE_SNOOZE:
      {
        g_autoptr(GDateTime) time = alarm_instance_time (instance);
        g_autofree char *until = alarm_format_time (time);

        title = g_strdup (label_or_default (instance));
        body = g_strdup_printf (_("Snoozing until %s"), until);
        actions = upcoming_actions;
        urgency = URGENCY_LOW;
        break;
      }
    case ALARM_STATE_FIRED:
      title = g_strdup (label_or_default (instance));
      body = alarm_text (instance, FALSE);
      actions = ringing_actions;
      urgency = URGENCY_CRITICAL;     /* the firing channel: it peeks, and stays while it rings */
      break;
    case ALARM_STATE_MISSED:
      title = g_strdup (_("Missed alarm"));
      body = alarm_text (instance, TRUE);
      actions = missed_actions;
      urgency = URGENCY_NORMAL;       /* the missed channel is high importance: it peeks */
      break;
    default:
      /* silent, hidden, dismissed: none */
      alarm_notifications_clear (self, instance->id);
      return;
    }

  if (!shown)
    {
      shown = g_new0 (Shown, 1);
      g_hash_table_insert (self->shown, GUINT_TO_POINTER (instance->id), shown);
    }
  else
    alarm_instance_free (shown->instance);
  shown->instance = alarm_instance_copy (instance);
  shown->id = notify (self, shown->id, title, body, actions, urgency);
}

void
alarm_notifications_clear (AlarmNotifications *self, guint instance_id)
{
  Shown *shown = g_hash_table_lookup (self->shown, GUINT_TO_POINTER (instance_id));

  if (!shown)
    return;
  if (shown->id)
    close_notification (self, shown->id);
  g_hash_table_remove (self->shown, GUINT_TO_POINTER (instance_id));
}

/* the instance a notification of ours shows, or 0 */
static guint
instance_of (AlarmNotifications *self, guint32 id)
{
  GHashTableIter iter;
  gpointer key, value;

  g_hash_table_iter_init (&iter, self->shown);
  while (g_hash_table_iter_next (&iter, &key, &value))
    if (((Shown *) value)->id == id)
      return GPOINTER_TO_UINT (key);
  return 0;
}

static void
on_action_invoked (GDBusConnection *bus, const char *sender, const char *path, const char *interface,
                   const char *signal, GVariant *parameters, gpointer data)
{
  AlarmNotifications *self = data;
  const char *action;
  guint32 id;
  guint instance_id;

  g_variant_get (parameters, "(u&s)", &id, &action);
  instance_id = instance_of (self, id);
  if (instance_id)
    self->callback (instance_id, g_str_equal (action, "default") ? "open" : action, self->data);
}

static void
on_notification_closed (GDBusConnection *bus, const char *sender, const char *path, const char *interface,
                        const char *signal, GVariant *parameters, gpointer data)
{
  AlarmNotifications *self = data;
  guint32 id, reason;
  guint instance_id;
  Shown *shown;

  g_variant_get (parameters, "(uu)", &id, &reason);
  instance_id = instance_of (self, id);
  if (!instance_id)
    return;
  /* it is gone either way: a new one for the instance is a new notification */
  shown = g_hash_table_lookup (self->shown, GUINT_TO_POINTER (instance_id));
  shown->id = 0;
  if (reason == CLOSED_DISMISSED)
    self->callback (instance_id, "closed", self->data);
}

/* the notification server came (back): what was shown is shown again */
static void
on_server_appeared (GDBusConnection *bus, const char *name, const char *owner, gpointer data)
{
  AlarmNotifications *self = data;
  g_autoptr(GList) instances = g_hash_table_get_values (self->shown);
  g_autoptr(GPtrArray) copies = g_ptr_array_new_with_free_func ((GDestroyNotify) alarm_instance_free);

  for (GList *l = instances; l; l = l->next)
    {
      Shown *shown = l->data;

      if (!shown->id)
        g_ptr_array_add (copies, alarm_instance_copy (shown->instance));
    }
  for (guint i = 0; i < copies->len; i++)
    alarm_notifications_show (self, copies->pdata[i]);
}

static void
on_server_vanished (GDBusConnection *bus, const char *name, gpointer data)
{
  AlarmNotifications *self = data;
  GHashTableIter iter;
  gpointer value;

  /* its notifications went with it */
  g_hash_table_iter_init (&iter, self->shown);
  while (g_hash_table_iter_next (&iter, NULL, &value))
    ((Shown *) value)->id = 0;
}

AlarmNotifications *
alarm_notifications_new (AlarmNotificationCallback callback, gpointer data)
{
  AlarmNotifications *self = g_new0 (AlarmNotifications, 1);

  self->bus = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, NULL);
  self->callback = callback;
  self->data = data;
  self->shown = g_hash_table_new_full (NULL, NULL, NULL, (GDestroyNotify) shown_free);
  self->action_signal = g_dbus_connection_signal_subscribe (self->bus, NOTIFICATIONS, NOTIFICATIONS, "ActionInvoked",
                                                            NOTIFICATIONS_PATH, NULL, G_DBUS_SIGNAL_FLAGS_NONE,
                                                            on_action_invoked, self, NULL);
  self->closed_signal = g_dbus_connection_signal_subscribe (self->bus, NOTIFICATIONS, NOTIFICATIONS,
                                                            "NotificationClosed", NOTIFICATIONS_PATH, NULL,
                                                            G_DBUS_SIGNAL_FLAGS_NONE, on_notification_closed, self,
                                                            NULL);
  self->watch = g_bus_watch_name_on_connection (self->bus, NOTIFICATIONS, G_BUS_NAME_WATCHER_FLAGS_NONE,
                                                on_server_appeared, on_server_vanished, self, NULL);
  return self;
}

void
alarm_notifications_free (AlarmNotifications *self)
{
  g_bus_unwatch_name (self->watch);
  g_dbus_connection_signal_unsubscribe (self->bus, self->action_signal);
  g_dbus_connection_signal_unsubscribe (self->bus, self->closed_signal);
  g_hash_table_unref (self->shown);
  g_object_unref (self->bus);
  g_free (self);
}
