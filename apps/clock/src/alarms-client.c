/* The alarm service's client: see alarms-client.h. */
#include "alarms-client.h"

#define SERVICE   "vibeos.Clock.Alarms"
#define PATH      "/vibeos/Clock/Alarms"
#define INTERFACE "vibeos.Clock.Alarms"

struct _AlarmsClient
{
  GObject parent;
  GDBusProxy *proxy;
  GPtrArray *alarms;            /* ClockAlarm */
  GCancellable *fetching;
};

G_DEFINE_FINAL_TYPE (AlarmsClient, alarms_client, G_TYPE_OBJECT)

enum { CHANGED, RINGING_CHANGED, NEXT_ALARM_CHANGED, N_SIGNALS };
static guint signals[N_SIGNALS];

void
clock_alarm_free (ClockAlarm *alarm)
{
  g_free (alarm->label);
  g_free (alarm->ringtone);
  g_free (alarm);
}

/* --- the list */

static ClockAlarm *
alarm_from_variant (GVariant *dict)
{
  ClockAlarm *alarm = g_new0 (ClockAlarm, 1);
  const char *text;

  g_variant_lookup (dict, "id", "u", &alarm->id);
  g_variant_lookup (dict, "hour", "i", &alarm->hour);
  g_variant_lookup (dict, "minute", "i", &alarm->minute);
  g_variant_lookup (dict, "days", "u", &alarm->days);
  g_variant_lookup (dict, "enabled", "b", &alarm->enabled);
  g_variant_lookup (dict, "vibrate", "b", &alarm->vibrate);
  g_variant_lookup (dict, "delete-after-use", "b", &alarm->delete_after_use);
  alarm->label = g_variant_lookup (dict, "label", "&s", &text) ? g_strdup (text) : g_strdup ("");
  if (g_variant_lookup (dict, "ringtone", "&s", &text))
    alarm->ringtone = g_strdup (text);
  g_variant_lookup (dict, "instance", "u", &alarm->instance);
  g_variant_lookup (dict, "instance-state", "i", &alarm->instance_state);
  g_variant_lookup (dict, "instance-time", "x", &alarm->instance_time);
  return alarm;
}

/* by hour, then minute, then the newest (highest id) first */
static int
by_time (gconstpointer a, gconstpointer b)
{
  const ClockAlarm *x = *(ClockAlarm **) a, *y = *(ClockAlarm **) b;

  if (x->hour != y->hour)
    return x->hour - y->hour;
  if (x->minute != y->minute)
    return x->minute - y->minute;
  return x->id < y->id ? 1 : x->id > y->id ? -1 : 0;
}

static void
on_alarms (GObject *source, GAsyncResult *result, gpointer data)
{
  AlarmsClient *self = data;
  g_autoptr(GError) error = NULL;
  g_autoptr(GVariant) reply = g_dbus_proxy_call_finish (G_DBUS_PROXY (source), result, &error);
  g_autoptr(GVariantIter) iter = NULL;
  GVariant *dict;

  if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    return;
  g_clear_object (&self->fetching);
  if (!reply)
    {
      g_warning ("the alarm service did not answer: %s", error->message);
      return;
    }
  g_ptr_array_set_size (self->alarms, 0);
  g_variant_get (reply, "(aa{sv})", &iter);
  while ((dict = g_variant_iter_next_value (iter)))
    {
      g_ptr_array_add (self->alarms, alarm_from_variant (dict));
      g_variant_unref (dict);
    }
  g_ptr_array_sort (self->alarms, by_time);
  g_signal_emit (self, signals[CHANGED], 0);
}

static void
fetch (AlarmsClient *self)
{
  /* a newer list is coming: the answer to an older request would only be overtaken by it */
  if (self->fetching)
    g_cancellable_cancel (self->fetching);
  g_clear_object (&self->fetching);
  self->fetching = g_cancellable_new ();
  g_dbus_proxy_call (self->proxy, "GetAlarms", NULL, G_DBUS_CALL_FLAGS_NONE, -1, self->fetching, on_alarms, self);
}

static void
on_signal (GDBusProxy *proxy, const char *sender, const char *name, GVariant *parameters, AlarmsClient *self)
{
  if (g_str_equal (name, "Changed"))
    fetch (self);
}

static gboolean
has_key (GVariant *dict, const char *key)
{
  g_autoptr(GVariant) value = g_variant_lookup_value (dict, key, NULL);

  return value != NULL;
}

static void
on_properties (GDBusProxy *proxy, GVariant *changed, GStrv invalidated, AlarmsClient *self)
{
  if (has_key (changed, "Ringing"))
    g_signal_emit (self, signals[RINGING_CHANGED], 0);
  if (has_key (changed, "NextAlarm"))
    g_signal_emit (self, signals[NEXT_ALARM_CHANGED], 0);
}

/* the service went away (it restarts on failure) or came back: what it holds may have changed */
static void
on_owner (GDBusProxy *proxy, GParamSpec *pspec, AlarmsClient *self)
{
  g_autofree char *owner = g_dbus_proxy_get_name_owner (proxy);

  if (owner)
    fetch (self);
  g_signal_emit (self, signals[RINGING_CHANGED], 0);
  g_signal_emit (self, signals[NEXT_ALARM_CHANGED], 0);
}

/* --- GObject */

static void
alarms_client_dispose (GObject *object)
{
  AlarmsClient *self = ALARMS_CLIENT (object);

  if (self->fetching)
    g_cancellable_cancel (self->fetching);
  g_clear_object (&self->fetching);
  g_clear_object (&self->proxy);
  g_clear_pointer (&self->alarms, g_ptr_array_unref);
  G_OBJECT_CLASS (alarms_client_parent_class)->dispose (object);
}

static void
alarms_client_class_init (AlarmsClientClass *klass)
{
  G_OBJECT_CLASS (klass)->dispose = alarms_client_dispose;
  signals[CHANGED] = g_signal_new ("changed", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0,
                                   NULL, NULL, NULL, G_TYPE_NONE, 0);
  signals[RINGING_CHANGED] = g_signal_new ("ringing-changed", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0,
                                           NULL, NULL, NULL, G_TYPE_NONE, 0);
  signals[NEXT_ALARM_CHANGED] = g_signal_new ("next-alarm-changed", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0,
                                              NULL, NULL, NULL, G_TYPE_NONE, 0);
}

static void
alarms_client_init (AlarmsClient *self)
{
  g_autoptr(GError) error = NULL;

  self->alarms = g_ptr_array_new_with_free_func ((GDestroyNotify) clock_alarm_free);
  /* synchronous: the app has nothing to show before it, and the service answers at once */
  self->proxy = g_dbus_proxy_new_for_bus_sync (G_BUS_TYPE_SESSION, G_DBUS_PROXY_FLAGS_NONE, NULL,
                                               SERVICE, PATH, INTERFACE, NULL, &error);
  if (!self->proxy)
    {
      g_warning ("no session bus for the alarm service: %s", error->message);
      return;
    }
  g_signal_connect (self->proxy, "g-signal", G_CALLBACK (on_signal), self);
  g_signal_connect (self->proxy, "g-properties-changed", G_CALLBACK (on_properties), self);
  g_signal_connect (self->proxy, "notify::g-name-owner", G_CALLBACK (on_owner), self);
  fetch (self);
}

AlarmsClient *
alarms_client_get (void)
{
  static AlarmsClient *client;

  if (!client)
    client = g_object_new (ALARMS_TYPE_CLIENT, NULL);
  return client;
}

GPtrArray *
alarms_client_get_alarms (AlarmsClient *self)
{
  return self->alarms;
}

ClockAlarm *
alarms_client_find (AlarmsClient *self, guint id)
{
  for (guint i = 0; i < self->alarms->len; i++)
    {
      ClockAlarm *alarm = g_ptr_array_index (self->alarms, i);

      if (alarm->id == id)
        return alarm;
    }
  return NULL;
}

gint64
alarms_client_get_next_alarm (AlarmsClient *self)
{
  g_autoptr(GVariant) value = self->proxy ? g_dbus_proxy_get_cached_property (self->proxy, "NextAlarm") : NULL;

  return value ? g_variant_get_int64 (value) : 0;
}

gboolean
alarms_client_get_ringing (AlarmsClient *self, guint *instance, guint *alarm, char **label, gint64 *time)
{
  g_autoptr(GVariant) value = self->proxy ? g_dbus_proxy_get_cached_property (self->proxy, "Ringing") : NULL;
  const char *text = "";

  if (!value || !g_variant_lookup (value, "instance", "u", instance))
    return FALSE;
  g_variant_lookup (value, "alarm", "u", alarm);
  g_variant_lookup (value, "label", "&s", &text);
  g_variant_lookup (value, "time", "x", time);
  *label = g_strdup (text);
  return TRUE;
}

/* --- requests */

typedef struct
{
  AlarmsAdded added;
  gpointer data;
} Adding;

static void
on_added (GObject *source, GAsyncResult *result, gpointer data)
{
  g_autofree Adding *adding = data;
  g_autoptr(GError) error = NULL;
  g_autoptr(GVariant) reply = g_dbus_proxy_call_finish (G_DBUS_PROXY (source), result, &error);
  guint id;

  if (!reply)
    {
      g_warning ("AddAlarm: %s", error->message);
      return;
    }
  g_variant_get (reply, "(u)", &id);
  if (adding->added)
    adding->added (id, adding->data);
}

void
alarms_client_add (AlarmsClient *self, const ClockAlarm *alarm, AlarmsAdded added, gpointer data)
{
  GVariantBuilder dict;
  Adding *adding = g_new (Adding, 1);

  *adding = (Adding) { added, data };
  g_variant_builder_init (&dict, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_add (&dict, "{sv}", "hour", g_variant_new_int32 (alarm->hour));
  g_variant_builder_add (&dict, "{sv}", "minute", g_variant_new_int32 (alarm->minute));
  g_variant_builder_add (&dict, "{sv}", "days", g_variant_new_uint32 (alarm->days));
  g_variant_builder_add (&dict, "{sv}", "enabled", g_variant_new_boolean (alarm->enabled));
  g_variant_builder_add (&dict, "{sv}", "vibrate", g_variant_new_boolean (alarm->vibrate));
  g_variant_builder_add (&dict, "{sv}", "label", g_variant_new_string (alarm->label ? alarm->label : ""));
  if (alarm->ringtone)
    g_variant_builder_add (&dict, "{sv}", "ringtone", g_variant_new_string (alarm->ringtone));
  g_dbus_proxy_call (self->proxy, "AddAlarm", g_variant_new ("(a{sv})", &dict), G_DBUS_CALL_FLAGS_NONE, -1,
                     NULL, on_added, adding);
}

static void
on_done (GObject *source, GAsyncResult *result, gpointer data)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(GVariant) reply = g_dbus_proxy_call_finish (G_DBUS_PROXY (source), result, &error);

  if (!reply)
    g_warning ("%s: %s", (const char *) data, error->message);
}

void
alarms_client_update (AlarmsClient *self, guint id, GVariant *changes)
{
  g_dbus_proxy_call (self->proxy, "UpdateAlarm", g_variant_new ("(u@a{sv})", id, changes), G_DBUS_CALL_FLAGS_NONE,
                     -1, NULL, on_done, "UpdateAlarm");
}

void
alarms_client_delete (AlarmsClient *self, guint id)
{
  g_dbus_proxy_call (self->proxy, "DeleteAlarm", g_variant_new ("(u)", id), G_DBUS_CALL_FLAGS_NONE, -1, NULL,
                     on_done, "DeleteAlarm");
}

void
alarms_client_snooze (AlarmsClient *self, guint instance)
{
  g_dbus_proxy_call (self->proxy, "Snooze", g_variant_new ("(u)", instance), G_DBUS_CALL_FLAGS_NONE, -1, NULL,
                     on_done, "Snooze");
}

void
alarms_client_dismiss (AlarmsClient *self, guint instance)
{
  g_dbus_proxy_call (self->proxy, "Dismiss", g_variant_new ("(u)", instance), G_DBUS_CALL_FLAGS_NONE, -1, NULL,
                     on_done, "Dismiss");
}
