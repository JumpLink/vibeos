/* The stopwatch's client: see stopwatch-client.h. */
#include "stopwatch-client.h"

#define SERVICE   "neo.Clock.Alarms"
#define PATH      "/neo/Clock/Stopwatch"
#define INTERFACE "neo.Clock.Stopwatch"

struct _StopwatchClient
{
  GObject parent;
  GDBusProxy *proxy;
  Stopwatch *stopwatch;
  GCancellable *fetching;
};

G_DEFINE_FINAL_TYPE (StopwatchClient, stopwatch_client, G_TYPE_OBJECT)

enum { CHANGED, N_SIGNALS };
static guint signals[N_SIGNALS];

static void
on_stopwatch (GObject *source, GAsyncResult *result, gpointer data)
{
  StopwatchClient *self = data;
  g_autoptr(GError) error = NULL;
  g_autoptr(GVariant) reply = g_dbus_proxy_call_finish (G_DBUS_PROXY (source), result, &error);
  g_autoptr(GVariant) dict = NULL;
  g_autoptr(GVariant) laps = NULL;
  Stopwatch *stopwatch = self->stopwatch;

  if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    return;
  g_clear_object (&self->fetching);
  if (!reply)
    {
      g_warning ("the clock service did not answer: %s", error->message);
      return;
    }
  dict = g_variant_get_child_value (reply, 0);
  g_variant_lookup (dict, "state", "u", &stopwatch->state);
  g_variant_lookup (dict, "last-start", "x", &stopwatch->last_start);
  g_variant_lookup (dict, "accumulated", "x", &stopwatch->accumulated);
  g_array_set_size (stopwatch->laps, 0);
  if ((laps = g_variant_lookup_value (dict, "laps", G_VARIANT_TYPE ("ax"))))
    {
      gsize n;
      const gint64 *totals = g_variant_get_fixed_array (laps, &n, sizeof (gint64));

      g_array_append_vals (stopwatch->laps, totals, n);
    }
  g_signal_emit (self, signals[CHANGED], 0);
}

static void
fetch (StopwatchClient *self)
{
  if (self->fetching)
    g_cancellable_cancel (self->fetching);
  g_clear_object (&self->fetching);
  self->fetching = g_cancellable_new ();
  g_dbus_proxy_call (self->proxy, "Get", NULL, G_DBUS_CALL_FLAGS_NONE, -1, self->fetching, on_stopwatch, self);
}

static void
on_signal (GDBusProxy *proxy, const char *sender, const char *name, GVariant *parameters, StopwatchClient *self)
{
  if (g_str_equal (name, "Changed"))
    fetch (self);
}

static void
on_owner (GDBusProxy *proxy, GParamSpec *pspec, StopwatchClient *self)
{
  g_autofree char *owner = g_dbus_proxy_get_name_owner (proxy);

  if (owner)
    fetch (self);
}

static void
stopwatch_client_dispose (GObject *object)
{
  StopwatchClient *self = STOPWATCH_CLIENT (object);

  if (self->fetching)
    g_cancellable_cancel (self->fetching);
  g_clear_object (&self->fetching);
  g_clear_object (&self->proxy);
  g_clear_pointer (&self->stopwatch, stopwatch_free);
  G_OBJECT_CLASS (stopwatch_client_parent_class)->dispose (object);
}

static void
stopwatch_client_class_init (StopwatchClientClass *klass)
{
  G_OBJECT_CLASS (klass)->dispose = stopwatch_client_dispose;
  signals[CHANGED] = g_signal_new ("changed", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0,
                                   NULL, NULL, NULL, G_TYPE_NONE, 0);
}

static void
stopwatch_client_init (StopwatchClient *self)
{
  g_autoptr(GError) error = NULL;

  self->stopwatch = stopwatch_new ();
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

StopwatchClient *
stopwatch_client_get (void)
{
  static StopwatchClient *client;

  if (!client)
    client = g_object_new (STOPWATCH_TYPE_CLIENT, NULL);
  return client;
}

const Stopwatch *
stopwatch_client_get_stopwatch (StopwatchClient *self)
{
  return self->stopwatch;
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
call (StopwatchClient *self, const char *method)
{
  g_dbus_proxy_call (self->proxy, method, NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, on_done, (gpointer) method);
}

void
stopwatch_client_start (StopwatchClient *self)
{
  call (self, "Start");
}

void
stopwatch_client_pause (StopwatchClient *self)
{
  call (self, "Pause");
}

void
stopwatch_client_reset (StopwatchClient *self)
{
  call (self, "Reset");
}

void
stopwatch_client_lap (StopwatchClient *self)
{
  call (self, "Lap");
}
