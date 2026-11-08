/* The Phone service's notifications (notifier.h). */
#include "notifier.h"
#include "call-types.h"

/* whose notifications they are; the Messaging builds this file too, with its own */
#ifndef NOTIFIER_APP_ID
#define NOTIFIER_APP_ID DIALER_APP_ID
#define NOTIFIER_APP_NAME "Phone"
#endif

#define NOTIFICATIONS "org.freedesktop.Notifications"
#define NOTIFICATIONS_PATH "/org/freedesktop/Notifications"
#define CLOSED_DISMISSED 2              /* NotificationClosed's reason: the user closed it */

struct _Notifier {
  GDBusConnection *bus;
  NotifierAction action;
  NotifierRestart restart;
  gpointer data;
  GHashTable *ours;                     /* the ids we posted that are still up */
  guint action_signal, closed_signal, watch;
  gboolean server_seen;
};

guint32
notifier_notify (Notifier *self, guint32 replaces, const char *title, const char *body,
                       const char * const *actions, NotifierUrgency urgency, GVariant *extra)
{
  const char *none[] = { NULL };
  g_autofree char *markup = g_markup_escape_text (body ? body : "", -1);
  g_autoptr(GVariant) extra_ref = extra ? g_variant_ref_sink (extra) : NULL;
  g_autoptr(GError) error = NULL;
  g_autoptr(GVariant) reply = NULL;
  GVariantBuilder hints;
  guint32 id = 0;

  g_variant_builder_init (&hints, G_VARIANT_TYPE_VARDICT);
  /* the app's: its name and icon */
  g_variant_builder_add (&hints, "{sv}", "desktop-entry", g_variant_new_string (NOTIFIER_APP_ID));
  g_variant_builder_add (&hints, "{sv}", "resident", g_variant_new_boolean (TRUE));
  g_variant_builder_add (&hints, "{sv}", "x-gnome-privacy-scope", g_variant_new_string ("system"));
  g_variant_builder_add (&hints, "{sv}", "urgency", g_variant_new_byte (urgency));
  if (extra_ref)
    {
      GVariantIter iter;
      const char *key;
      GVariant *value;

      g_variant_iter_init (&iter, extra_ref);
      while (g_variant_iter_loop (&iter, "{&sv}", &key, &value))
        g_variant_builder_add (&hints, "{sv}", key, value);
    }

  /* a sync call: the shell answers at once, and the caller wants the id */
  reply = g_dbus_connection_call_sync (self->bus, NOTIFICATIONS, NOTIFICATIONS_PATH, NOTIFICATIONS, "Notify",
                                       g_variant_new ("(susss^asa{sv}i)", NOTIFIER_APP_NAME, replaces, "", title,
                                                      markup, actions ? actions : none, &hints, 0),
                                       G_VARIANT_TYPE ("(u)"), G_DBUS_CALL_FLAGS_NONE, 3000, NULL, &error);
  if (!reply)
    {
      g_warning ("notifications: %s", error->message);
      return 0;
    }
  g_variant_get (reply, "(u)", &id);
  /* (a notification the server no longer had comes back as a new one) */
  if (replaces && id != replaces)
    g_hash_table_remove (self->ours, GUINT_TO_POINTER (replaces));
  g_hash_table_add (self->ours, GUINT_TO_POINTER (id));
  return id;
}

GVariant *
notifier_header_hints (gint64 chronometer, const char *subtext, gboolean no_peek)
{
  GVariantBuilder hints;

  g_variant_builder_init (&hints, G_VARIANT_TYPE_VARDICT);
  if (chronometer)
    g_variant_builder_add (&hints, "{sv}", "x-neo-chronometer", g_variant_new_int64 (chronometer));
  if (subtext)
    g_variant_builder_add (&hints, "{sv}", "x-neo-subtext", g_variant_new_string (subtext));
  if (no_peek)
    g_variant_builder_add (&hints, "{sv}", "x-neo-no-peek", g_variant_new_boolean (TRUE));
  return g_variant_builder_end (&hints);
}

void
notifier_close (Notifier *self, guint32 id)
{
  if (!id)
    return;
  g_dbus_connection_call (self->bus, NOTIFICATIONS, NOTIFICATIONS_PATH, NOTIFICATIONS, "CloseNotification",
                          g_variant_new ("(u)", id), NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);
}

static void
on_action_invoked (GDBusConnection *bus, const char *sender, const char *path, const char *interface,
                   const char *signal, GVariant *parameters, gpointer data)
{
  Notifier *self = data;
  const char *action;
  guint32 id;

  g_variant_get (parameters, "(u&s)", &id, &action);
  if (g_hash_table_contains (self->ours, GUINT_TO_POINTER (id)))
    self->action (id, action, self->data);
}

static void
on_notification_closed (GDBusConnection *bus, const char *sender, const char *path, const char *interface,
                        const char *signal, GVariant *parameters, gpointer data)
{
  Notifier *self = data;
  guint32 id, reason;

  g_variant_get (parameters, "(uu)", &id, &reason);
  if (!g_hash_table_remove (self->ours, GUINT_TO_POINTER (id)))
    return;
  self->action (id, reason == CLOSED_DISMISSED ? "closed" : "gone", self->data);
}

static void
on_server_appeared (GDBusConnection *bus, const char *name, const char *owner, gpointer data)
{
  Notifier *self = data;

  /* the first time is the start: nothing was shown yet */
  if (!self->server_seen)
    {
      self->server_seen = TRUE;
      return;
    }
  self->restart (self->data);
}

static void
on_server_vanished (GDBusConnection *bus, const char *name, gpointer data)
{
  Notifier *self = data;

  /* its notifications went with it */
  g_hash_table_remove_all (self->ours);
  self->server_seen = TRUE;
}

Notifier *
notifier_new (NotifierAction action, NotifierRestart restart, gpointer data)
{
  Notifier *self = g_new0 (Notifier, 1);

  self->bus = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, NULL);
  self->action = action;
  self->restart = restart;
  self->data = data;
  self->ours = g_hash_table_new (NULL, NULL);
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
notifier_free (Notifier *self)
{
  g_bus_unwatch_name (self->watch);
  g_dbus_connection_signal_unsubscribe (self->bus, self->action_signal);
  g_dbus_connection_signal_unsubscribe (self->bus, self->closed_signal);
  g_hash_table_unref (self->ours);
  g_object_unref (self->bus);
  g_free (self);
}
