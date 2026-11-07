/* The timers' client: see timers-client.h. */
#include "timers-client.h"

#define SERVICE   "de.yesman.Clock.Alarms"
#define PATH      "/de/yesman/Clock/Timers"
#define INTERFACE "de.yesman.Clock.Timers"

struct _TimersClient
{
  GObject parent;
  GDBusProxy *proxy;
  GPtrArray *timers;            /* Timer; NULL before the first answer */
  GCancellable *fetching;
};

G_DEFINE_FINAL_TYPE (TimersClient, timers_client, G_TYPE_OBJECT)

enum { CHANGED, N_SIGNALS };
static guint signals[N_SIGNALS];

/* --- the list */

static Timer *
timer_from_variant (GVariant *dict)
{
  Timer *timer = timer_new (0, 0, NULL, FALSE);
  const char *label;

  g_variant_lookup (dict, "id", "u", &timer->id);
  g_variant_lookup (dict, "state", "u", &timer->state);
  g_variant_lookup (dict, "length", "x", &timer->length);
  g_variant_lookup (dict, "total-length", "x", &timer->total_length);
  g_variant_lookup (dict, "last-start", "x", &timer->last_start);
  g_variant_lookup (dict, "remaining", "x", &timer->remaining);
  g_variant_lookup (dict, "delete-after-use", "b", &timer->delete_after_use);
  if (g_variant_lookup (dict, "label", "&s", &label))
    timer_set_label (timer, label);
  return timer;
}

static void
on_timers (GObject *source, GAsyncResult *result, gpointer data)
{
  TimersClient *self = data;
  g_autoptr(GError) error = NULL;
  g_autoptr(GVariant) reply = g_dbus_proxy_call_finish (G_DBUS_PROXY (source), result, &error);
  g_autoptr(GVariantIter) iter = NULL;
  GVariant *dict;

  if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    return;
  g_clear_object (&self->fetching);
  if (!reply)
    {
      g_warning ("the clock service did not answer: %s", error->message);
      return;
    }
  if (!self->timers)
    self->timers = g_ptr_array_new_with_free_func ((GDestroyNotify) timer_free);
  g_ptr_array_set_size (self->timers, 0);
  g_variant_get (reply, "(aa{sv})", &iter);
  while ((dict = g_variant_iter_next_value (iter)))
    {
      g_ptr_array_add (self->timers, timer_from_variant (dict));
      g_variant_unref (dict);
    }
  g_signal_emit (self, signals[CHANGED], 0);
}

static void
fetch (TimersClient *self)
{
  /* a newer list is coming: the answer to an older request would only be overtaken by it */
  if (self->fetching)
    g_cancellable_cancel (self->fetching);
  g_clear_object (&self->fetching);
  self->fetching = g_cancellable_new ();
  g_dbus_proxy_call (self->proxy, "GetTimers", NULL, G_DBUS_CALL_FLAGS_NONE, -1, self->fetching, on_timers, self);
}

static void
on_signal (GDBusProxy *proxy, const char *sender, const char *name, GVariant *parameters, TimersClient *self)
{
  if (g_str_equal (name, "Changed"))
    fetch (self);
}

/* the service came back (it restarts on failure): what it holds may have changed */
static void
on_owner (GDBusProxy *proxy, GParamSpec *pspec, TimersClient *self)
{
  g_autofree char *owner = g_dbus_proxy_get_name_owner (proxy);

  if (owner)
    fetch (self);
}

/* --- GObject */

static void
timers_client_dispose (GObject *object)
{
  TimersClient *self = TIMERS_CLIENT (object);

  if (self->fetching)
    g_cancellable_cancel (self->fetching);
  g_clear_object (&self->fetching);
  g_clear_object (&self->proxy);
  g_clear_pointer (&self->timers, g_ptr_array_unref);
  G_OBJECT_CLASS (timers_client_parent_class)->dispose (object);
}

static void
timers_client_class_init (TimersClientClass *klass)
{
  G_OBJECT_CLASS (klass)->dispose = timers_client_dispose;
  signals[CHANGED] = g_signal_new ("changed", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0,
                                   NULL, NULL, NULL, G_TYPE_NONE, 0);
}

static void
timers_client_init (TimersClient *self)
{
  g_autoptr(GError) error = NULL;

  self->proxy = g_dbus_proxy_new_for_bus_sync (G_BUS_TYPE_SESSION, G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES, NULL,
                                               SERVICE, PATH, INTERFACE, NULL, &error);
  if (!self->proxy)
    {
      g_warning ("no session bus for the clock service: %s", error->message);
      return;
    }
  g_signal_connect (self->proxy, "g-signal", G_CALLBACK (on_signal), self);
  g_signal_connect (self->proxy, "notify::g-name-owner", G_CALLBACK (on_owner), self);
  fetch (self);
}

TimersClient *
timers_client_get (void)
{
  static TimersClient *client;

  if (!client)
    client = g_object_new (TIMERS_TYPE_CLIENT, NULL);
  return client;
}

GPtrArray *
timers_client_get_timers (TimersClient *self)
{
  return self->timers;
}

Timer *
timers_client_find (TimersClient *self, guint id)
{
  if (!self->timers)
    return NULL;
  for (guint i = 0; i < self->timers->len; i++)
    if (((Timer *) self->timers->pdata[i])->id == id)
      return self->timers->pdata[i];
  return NULL;
}

/* --- requests */

typedef struct
{
  TimersAdded added;
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
      g_warning ("AddTimer: %s", error->message);
      return;
    }
  g_variant_get (reply, "(u)", &id);
  if (adding->added)
    adding->added (id, adding->data);
}

void
timers_client_add (TimersClient *self, gint64 length, const char *label, TimersAdded added, gpointer data)
{
  Adding *adding = g_new (Adding, 1);

  *adding = (Adding) { added, data };
  g_dbus_proxy_call (self->proxy, "AddTimer", g_variant_new ("(xsb)", length, label ? label : "", FALSE),
                     G_DBUS_CALL_FLAGS_NONE, -1, NULL, on_added, adding);
}

static void
on_done (GObject *source, GAsyncResult *result, gpointer data)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(GVariant) reply = g_dbus_proxy_call_finish (G_DBUS_PROXY (source), result, &error);

  if (!reply)
    g_warning ("%s: %s", (const char *) data, error->message);
}

static void
call (TimersClient *self, const char *method, GVariant *parameters)
{
  g_dbus_proxy_call (self->proxy, method, parameters, G_DBUS_CALL_FLAGS_NONE, -1, NULL, on_done, (gpointer) method);
}

void
timers_client_start (TimersClient *self, guint id)
{
  call (self, "Start", g_variant_new ("(u)", id));
}

void
timers_client_pause (TimersClient *self, guint id)
{
  call (self, "Pause", g_variant_new ("(u)", id));
}

void
timers_client_reset (TimersClient *self, guint id)
{
  call (self, "Reset", g_variant_new ("(u)", id));
}

void
timers_client_add_minute (TimersClient *self, guint id)
{
  call (self, "AddMinute", g_variant_new ("(u)", id));
}

void
timers_client_delete (TimersClient *self, guint id)
{
  call (self, "Delete", g_variant_new ("(u)", id));
}

void
timers_client_set_label (TimersClient *self, guint id, const char *label)
{
  call (self, "SetLabel", g_variant_new ("(us)", id, label));
}

void
timers_client_reset_expired (TimersClient *self)
{
  call (self, "ResetExpired", NULL);
}
