/* vibeos-unifiedpush: handing messages, new endpoints and unregistrations to the apps (org.unifiedpush.Connector2
 * at /org/unifiedpush/Connector on the app's bus name), one call at a time per app, in the order they came. Apps
 * do not wait for each other. The call starts the app if it is not running (D-Bus activation: its service file).
 *
 * The spec says not to wait for the app's reply, and nothing here does: the reply comes back asynchronously, and it
 * only says what happens next. A call that the app answers, or that it got but did not answer in 25 s, is done.
 * One that fails says why:
 *   - UnknownObject, UnknownMethod, UnknownInterface: the app runs but has no connector yet (it was just started and
 *     is still setting up): the call is made again after 0.25, 0.5, 1, 2, 4 and 8 s, then the message is dropped.
 *   - Spawn.*: the app could not be started: again after 5 and 30 s, then the message is dropped.
 *   - ServiceUnknown: the app neither runs nor can be started. If it could be started when it registered (it had a
 *     service file), it has been uninstalled, or is being upgraded: after a minute it is looked for again, and if it is
 *     still missing all its registrations are removed (there is nobody to tell). An app that never had a service file
 *     only gets messages while it runs: its messages are dropped, its registrations stay.
 *   - anything else (AccessDenied, InvalidArgs): the message is dropped.
 * Messages wait for an app in memory, at most QUEUE_MAX of them. The stream's position (vup-store.c "since") only
 * moves past a message once it has been dealt with, so the ones that were waiting come again after a restart.
 *
 * The same check for uninstalled apps runs a minute after the distributor starts, and whenever the bus says that its
 * set of service files changed (ActivatableServicesChanged).
 *
 * While a call is being made, and 2 s after the last one, the system is held awake: a logind "sleep" inhibitor of the
 * "block" kind, as the phones' own sleep daemons (s10e-sleepd) look for, since they suspend without logind and its
 * "delay" inhibitors. */
#include <unistd.h>

#include <gio/gunixfdlist.h>

#include "vup.h"

#define CALL_TIMEOUT_MS   25000
#define QUEUE_MAX         128
#define SEEN_MAX          1024
#define GONE_GRACE        60          /* s */
#define LINGER_MS         2000
#define SHORT_WAIT_MS     2000        /* a retry no longer than this keeps the system awake and Collect waiting */
#define CHECK_DELAY       60          /* s after start, the first look for uninstalled apps */

static const guint starting_delays[] = { 250, 500, 1000, 2000, 4000, 8000 };   /* ms */
static const guint spawn_delays[] = { 5000, 30000 };

typedef enum {
  ITEM_MESSAGE,
  ITEM_NEW_ENDPOINT,
  ITEM_UNREGISTERED,
} ItemKind;

typedef struct {
  ItemKind kind;
  char *token;
  char *id;                 /* a message's id on the push server */
  GBytes *message;
  gboolean done;            /* a message: dealt with (it stays in `order` until those before it are too) */
  VupTell tell;             /* an Unregistered: a confirmation, or a removal in Settings */
} Item;

typedef struct {
  char *service;
  GQueue items;             /* Item*: the head is the one being delivered */
  Item *in_flight;          /* the head, while its call is made */
  guint retry_id;
  gboolean short_wait;
  guint attempts;           /* of the head */
  gint64 missing_since;     /* monotonic µs: when the app was found neither running nor installed, 0 */
} App;

static GDBusConnection *bus, *system_bus;
static GHashTable *apps;                /* service -> App */
static GQueue order = G_QUEUE_INIT;     /* the messages (Item*) in the order they came, until committed: owns them */
static GHashTable *seen;                /* the last SEEN_MAX message ids */
static GQueue seen_order = G_QUEUE_INIT;
static guint64 delivered;
static GSList *idle_waiters;            /* GSource*: idle sources made when everything is handed over */
static guint check_id, services_subscription;

static int inhibit_fd = -1;
static gboolean inhibit_pending, inhibit_warned;
static guint linger_id;

static void app_next (App *app);

static void
item_free (Item *item)
{
  g_free (item->token);
  g_free (item->id);
  g_clear_pointer (&item->message, g_bytes_unref);
  g_free (item);
}

static App *
app_get (const char *service)
{
  App *app = g_hash_table_lookup (apps, service);

  if (!app)
    {
      app = g_new0 (App, 1);
      app->service = g_strdup (service);
      g_queue_init (&app->items);
      g_hash_table_insert (apps, app->service, app);
    }
  return app;
}

/* --- holding the system awake */

static gboolean
busy (void)
{
  GHashTableIter iter;
  App *app;

  g_hash_table_iter_init (&iter, apps);
  while (g_hash_table_iter_next (&iter, NULL, (gpointer *) &app))
    if (app->in_flight || (app->retry_id && app->short_wait))
      return TRUE;
  return FALSE;
}

static gboolean
on_linger_over (gpointer data)
{
  linger_id = 0;
  if (inhibit_fd >= 0)
    close (inhibit_fd);
  inhibit_fd = -1;
  return G_SOURCE_REMOVE;
}

static void
on_inhibit (GObject *source, GAsyncResult *result, gpointer data)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(GUnixFDList) fds = NULL;
  g_autoptr(GVariant) reply = g_dbus_connection_call_with_unix_fd_list_finish (G_DBUS_CONNECTION (source), &fds,
                                                                               result, &error);

  inhibit_pending = FALSE;
  if (!reply)
    {
      if (!inhibit_warned)
        g_warning ("cannot hold the system awake while delivering: %s", error->message);
      inhibit_warned = TRUE;
      return;
    }
  inhibit_fd = g_unix_fd_list_get (fds, 0, NULL);
  if (!busy () && !linger_id)
    linger_id = g_timeout_add (LINGER_MS, on_linger_over, NULL);
}

static void
hold_awake (void)
{
  g_clear_handle_id (&linger_id, g_source_remove);
  if (inhibit_fd >= 0 || inhibit_pending || !system_bus)
    return;
  inhibit_pending = TRUE;
  g_dbus_connection_call_with_unix_fd_list (system_bus, "org.freedesktop.login1", "/org/freedesktop/login1",
                                            "org.freedesktop.login1.Manager", "Inhibit",
                                            g_variant_new ("(ssss)", "sleep", "vibeos-unifiedpush",
                                                           "Delivering push messages", "block"),
                                            G_VARIANT_TYPE ("(h)"), G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL,
                                            on_inhibit, NULL);
}

/* after each call: when nothing is being delivered any more, the system may sleep in LINGER_MS, and whoever waits
 * for that (Collect) hears of it */
static void
check_idle (void)
{
  GSList *waiters;

  if (busy ())
    return;
  if (inhibit_fd >= 0 && !linger_id)
    linger_id = g_timeout_add (LINGER_MS, on_linger_over, NULL);
  waiters = g_steal_pointer (&idle_waiters);
  for (GSList *l = waiters; l; l = l->next)
    {
      g_source_attach (l->data, NULL);
      g_source_unref (l->data);
    }
  g_slist_free (waiters);
}

gboolean
vup_deliver_is_idle (void)
{
  return !busy ();
}

void
vup_deliver_when_idle (GSourceFunc func, gpointer data)
{
  GSource *source = g_idle_source_new ();

  g_source_set_callback (source, func, data, NULL);
  idle_waiters = g_slist_append (idle_waiters, source);
  check_idle ();
}

guint
vup_deliver_count (void)
{
  return (guint) delivered;
}

/* --- the stream's position */

static void
commit (void)
{
  g_autoptr(GPtrArray) done = g_ptr_array_new ();
  Item *item;
  char *since = NULL;

  while ((item = g_queue_peek_head (&order)) && item->done)
    {
      g_queue_pop_head (&order);
      g_free (since);
      since = g_steal_pointer (&item->id);
      item_free (item);
    }
  if (since)
    vup_store_set_since (since);
  g_free (since);
  for (GList *l = order.head; l; l = l->next)
    if (((Item *) l->data)->done)
      g_ptr_array_add (done, ((Item *) l->data)->id);
  vup_store_set_done (done);
}

static void
remember_seen (const char *id)
{
  char *copy;

  if (g_hash_table_contains (seen, id))
    return;
  copy = g_strdup (id);
  g_hash_table_add (seen, copy);
  g_queue_push_tail (&seen_order, copy);
  if (g_queue_get_length (&seen_order) > SEEN_MAX)
    g_hash_table_remove (seen, g_queue_pop_head (&seen_order));
}

gboolean
vup_deliver_seen (const char *id)
{
  return g_hash_table_contains (seen, id);
}

/* the item at the head of the app's queue is finished with (delivered or not): it goes, the next one is sent */
static void
finish_head (App *app)
{
  Item *item = g_queue_pop_head (&app->items);

  app->attempts = 0;
  if (item->kind == ITEM_MESSAGE)
    {
      g_clear_pointer (&item->message, g_bytes_unref);
      item->done = TRUE;
      commit ();
    }
  else
    item_free (item);
}

/* --- calls */

static gboolean
on_retry (gpointer data)
{
  App *app = data;

  app->retry_id = 0;
  app->short_wait = FALSE;
  app_next (app);
  check_idle ();
  return G_SOURCE_REMOVE;
}

static void
retry_later (App *app, guint ms)
{
  g_clear_handle_id (&app->retry_id, g_source_remove);
  app->short_wait = ms <= SHORT_WAIT_MS;
  app->retry_id = g_timeout_add (ms, on_retry, app);
}

static const char *
item_name (Item *item)
{
  return item->kind == ITEM_MESSAGE ? "message" : item->kind == ITEM_NEW_ENDPOINT ? "new endpoint" : "unregistration";
}

static gboolean
service_is_activatable (const char *service)
{
  GList *list = vup_store_list ();
  gboolean activatable = FALSE;

  for (GList *l = list; l && !activatable; l = l->next)
    activatable = g_str_equal (((VupRegistration *) l->data)->service, service)
                  && ((VupRegistration *) l->data)->activatable;
  g_list_free (list);
  return activatable;
}

/* every registration of an app that is gone; nobody is told, there is nobody */
static void
app_gone (App *app)
{
  GList *list = vup_store_list ();
  guint removed = 0;

  for (GList *l = list; l; l = l->next)
    {
      VupRegistration *reg = l->data;
      if (g_str_equal (reg->service, app->service))
        {
          vup_registration_removed (reg, VUP_TELL_NOBODY);
          removed++;
        }
    }
  g_list_free (list);
  g_message ("%s is not installed any more: its %u registrations are removed", app->service, removed);
  app->missing_since = 0;
  /* what still waits was for those registrations: app_next drops it */
  g_clear_handle_id (&app->retry_id, g_source_remove);
  app->short_wait = FALSE;
  app_next (app);
}

/* the bus cannot start the app, and it does not run */
static void
app_missing (App *app)
{
  Item *item = g_queue_peek_head (&app->items);
  gint64 now = g_get_monotonic_time ();

  if (item->kind == ITEM_UNREGISTERED)
    {
      finish_head (app);
      app_next (app);
      return;
    }
  if (!service_is_activatable (app->service))
    {
      g_message ("%s does not run, and has no D-Bus service file to start it by: %s %s dropped", app->service,
                 item_name (item), item->id ? item->id : "");
      finish_head (app);
      app_next (app);
      return;
    }
  if (!app->missing_since)
    app->missing_since = now;
  if (now - app->missing_since >= GONE_GRACE * G_USEC_PER_SEC)
    {
      app_gone (app);
      return;
    }
  g_message ("%s cannot be started (its D-Bus service file is gone): looking again in %" G_GINT64_FORMAT " s",
             app->service, GONE_GRACE - (now - app->missing_since) / G_USEC_PER_SEC);
  retry_later (app, (GONE_GRACE * G_USEC_PER_SEC - (now - app->missing_since)) / 1000 + 100);
}

static void
on_reply (GObject *source, GAsyncResult *result, gpointer data)
{
  App *app = data;
  g_autoptr(GError) error = NULL;
  g_autoptr(GVariant) reply = g_dbus_connection_call_finish (G_DBUS_CONNECTION (source), result, &error);
  Item *item = app->in_flight;
  VupRegistration *reg;

  app->in_flight = NULL;
  if (item != g_queue_peek_head (&app->items))
    goto next;      /* it was taken off while the call was made (the registration went) */

  if (reply || g_error_matches (error, G_DBUS_ERROR, G_DBUS_ERROR_NO_REPLY)
      || g_error_matches (error, G_DBUS_ERROR, G_DBUS_ERROR_TIMEOUT)
      || g_error_matches (error, G_DBUS_ERROR, G_DBUS_ERROR_TIMED_OUT)
      || g_error_matches (error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT))
    {
      if (error)
        g_message ("%s did not answer the %s %s: taken as delivered", app->service, item_name (item),
                   item->id ? item->id : "");
      app->missing_since = 0;
      if (item->kind == ITEM_MESSAGE && (reg = vup_store_by_token (item->token)))
        {
          reg->messages++;
          reg->last_message = g_get_real_time () / G_USEC_PER_SEC;
          delivered++;
          vup_store_save_soon ();
          g_debug ("message %s delivered to %s", item->id, app->service);
        }
      finish_head (app);
    }
  else if (g_error_matches (error, G_DBUS_ERROR, G_DBUS_ERROR_SERVICE_UNKNOWN)
           || g_error_matches (error, G_DBUS_ERROR, G_DBUS_ERROR_NAME_HAS_NO_OWNER))
    {
      app_missing (app);
      check_idle ();
      return;
    }
  else if (g_error_matches (error, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_OBJECT)
           || g_error_matches (error, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_METHOD)
           || g_error_matches (error, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_INTERFACE))
    {
      app->missing_since = 0;
      if (app->attempts < G_N_ELEMENTS (starting_delays))
        {
          retry_later (app, starting_delays[app->attempts++]);
          check_idle ();
          return;
        }
      g_warning ("%s has no UnifiedPush connector (%s): %s %s dropped", app->service, error->message,
                 item_name (item), item->id ? item->id : "");
      finish_head (app);
    }
  else if (error->domain == G_DBUS_ERROR && error->code >= G_DBUS_ERROR_SPAWN_EXEC_FAILED
           && error->code <= G_DBUS_ERROR_SPAWN_NO_MEMORY)
    {
      app->missing_since = 0;
      if (app->attempts < G_N_ELEMENTS (spawn_delays))
        {
          g_warning ("%s could not be started: %s; trying again in %u s", app->service, error->message,
                     spawn_delays[app->attempts] / 1000);
          retry_later (app, spawn_delays[app->attempts++]);
          check_idle ();
          return;
        }
      g_warning ("%s could not be started: %s; %s %s dropped", app->service, error->message, item_name (item),
                 item->id ? item->id : "");
      finish_head (app);
    }
  else
    {
      g_warning ("%s: %s; %s %s dropped", app->service, error->message, item_name (item), item->id ? item->id : "");
      finish_head (app);
    }

next:
  app_next (app);
  check_idle ();
}

static GVariant *
item_parameters (Item *item, VupRegistration *reg)
{
  GVariantBuilder options;

  g_variant_builder_init (&options, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_add (&options, "{sv}", "token", g_variant_new_string (item->token));
  if (item->kind == ITEM_MESSAGE)
    {
      g_variant_builder_add (&options, "{sv}", "message",
                             g_variant_new_from_bytes (G_VARIANT_TYPE_BYTESTRING, item->message, TRUE));
      g_variant_builder_add (&options, "{sv}", "id", g_variant_new_string (item->id));
    }
  else if (item->kind == ITEM_NEW_ENDPOINT)
    g_variant_builder_add (&options, "{sv}", "endpoint", g_variant_new_take_string (vup_store_endpoint (reg)));
  return g_variant_new ("(a{sv})", &options);
}

static void
app_next (App *app)
{
  Item *item;
  VupRegistration *reg;
  const char *method;

  while (!app->in_flight && !app->retry_id && (item = g_queue_peek_head (&app->items)))
    {
      reg = vup_store_by_token (item->token);
      if (item->kind != ITEM_UNREGISTERED && !reg)
        {
          finish_head (app);      /* the registration went while it waited */
          continue;
        }
      method = item->kind == ITEM_MESSAGE ? "Message" : item->kind == ITEM_NEW_ENDPOINT ? "NewEndpoint" : "Unregistered";
      app->in_flight = item;
      hold_awake ();
      g_dbus_connection_call (bus, app->service, VUP_CONNECTOR_PATH, VUP_CONNECTOR_IFACE, method,
                              item_parameters (item, reg), NULL, G_DBUS_CALL_FLAGS_NONE, CALL_TIMEOUT_MS, NULL,
                              on_reply, app);
    }
}

static void
enqueue (const char *service, Item *item)
{
  App *app = app_get (service);

  g_queue_push_tail (&app->items, item);
  app_next (app);
}

/* a message from the stream: to the app whose topic it is, or dropped */
void
vup_deliver_received (const char *id, const char *topic, GBytes *message)
{
  VupRegistration *reg = vup_store_by_topic (topic);
  Item *item = g_new0 (Item, 1);
  gsize size = g_bytes_get_size (message);
  App *app;

  remember_seen (id);
  item->kind = ITEM_MESSAGE;
  item->id = g_strdup (id);
  g_queue_push_tail (&order, item);
  if (!reg)
    {
      g_debug ("message %s for %s, which is not registered: dropped", id, vup_redact (topic));
      item->done = TRUE;
      commit ();
      return;
    }
  if (size == 0 || size > VUP_MESSAGE_MAX)
    {
      g_warning ("message %s for %s: %" G_GSIZE_FORMAT " bytes, not 1 to %u: dropped", id, reg->service, size,
                 VUP_MESSAGE_MAX);
      item->done = TRUE;
      commit ();
      return;
    }
  app = app_get (reg->service);
  if (g_queue_get_length (&app->items) >= QUEUE_MAX)
    {
      g_warning ("message %s for %s: %u are waiting for it already: dropped", id, reg->service, QUEUE_MAX);
      item->done = TRUE;
      commit ();
      return;
    }
  item->token = g_strdup (reg->token);
  item->message = g_bytes_ref (message);
  g_debug ("message %s for %s (%s), %" G_GSIZE_FORMAT " bytes", id, reg->service, vup_redact (reg->token), size);
  enqueue (reg->service, item);
}

void
vup_deliver_new_endpoint (VupRegistration *reg, gboolean answering)
{
  App *app = app_get (reg->service);
  Item *item;

  /* one waiting already gives the endpoint as it is when it is sent; but each Register gets a NewEndpoint of its
   * own, as the spec has it (an app may wait for one per Register) */
  for (GList *l = answering ? NULL : app->items.head; l; l = l->next)
    if (l->data != app->in_flight && ((Item *) l->data)->kind == ITEM_NEW_ENDPOINT
        && g_str_equal (((Item *) l->data)->token, reg->token))
      return;
  item = g_new0 (Item, 1);
  item->kind = ITEM_NEW_ENDPOINT;
  item->token = g_strdup (reg->token);
  enqueue (reg->service, item);
}

void
vup_deliver_unregistered (const char *service, const char *token, VupTell tell)
{
  Item *item = g_new0 (Item, 1);

  item->kind = ITEM_UNREGISTERED;
  item->token = g_strdup (token);
  item->tell = tell;
  enqueue (service, item);
}

/* a registration goes: what waits for it goes too (a call already made finishes on its own) */
void
vup_deliver_forget (VupRegistration *reg)
{
  App *app = g_hash_table_lookup (apps, reg->service);
  GList *l, *next;

  if (!app)
    return;
  for (l = app->items.head; l; l = next)
    {
      Item *item = l->data;

      next = l->next;
      if (item == app->in_flight || item->kind == ITEM_UNREGISTERED || g_strcmp0 (item->token, reg->token) != 0)
        continue;
      if (l == app->items.head)
        {
          /* it was waiting to be tried again */
          app->attempts = 0;
          g_clear_handle_id (&app->retry_id, g_source_remove);
          app->short_wait = FALSE;
        }
      g_queue_delete_link (&app->items, l);
      if (item->kind == ITEM_MESSAGE)
        {
          g_clear_pointer (&item->message, g_bytes_unref);
          item->done = TRUE;
        }
      else
        item_free (item);
    }
  commit ();
  app_next (app);
  check_idle ();
}

/* an app registers with a token whose Unregistered has not reached it yet. A removal in Settings stands: an app
 * started for that very Unregistered registers as it starts, before its connector can take the call, and must not
 * undo the removal (VUP_TELL_REMOVED: the Register is answered, and the Unregistered follows). The confirmation of
 * the app's own Unregister is no longer true, and is not sent (VUP_TELL_CONFIRM); nor is it once it is on its way
 * (libvibeos then unregisters the token again). VUP_TELL_NOBODY: none was waiting */
VupTell
vup_deliver_unregistered_waiting (const char *service, const char *token)
{
  App *app = g_hash_table_lookup (apps, service);
  gboolean cancelled = FALSE;
  GList *l, *next;

  for (l = app ? app->items.head : NULL; l; l = l->next)
    {
      Item *item = l->data;

      if (item->kind == ITEM_UNREGISTERED && item->tell == VUP_TELL_REMOVED && g_strcmp0 (item->token, token) == 0)
        return VUP_TELL_REMOVED;
    }
  for (l = app ? app->items.head : NULL; l; l = next)
    {
      Item *item = l->data;

      next = l->next;
      if (item == app->in_flight || item->kind != ITEM_UNREGISTERED || g_strcmp0 (item->token, token) != 0)
        continue;
      if (l == app->items.head)
        {
          app->attempts = 0;
          g_clear_handle_id (&app->retry_id, g_source_remove);
          app->short_wait = FALSE;
        }
      g_queue_delete_link (&app->items, l);
      item_free (item);
      cancelled = TRUE;
    }
  if (cancelled)
    {
      app_next (app);
      check_idle ();
    }
  return cancelled ? VUP_TELL_CONFIRM : VUP_TELL_NOBODY;
}

guint
vup_deliver_pending (const char *service)
{
  App *app = g_hash_table_lookup (apps, service);
  guint count = 0;

  for (GList *l = app ? app->items.head : NULL; l; l = l->next)
    if (((Item *) l->data)->kind == ITEM_MESSAGE)
      count++;
  return count;
}

/* --- uninstalled apps */

static gboolean on_check (gpointer data);

static void
on_names (GObject *source, GAsyncResult *result, gpointer data)
{
  g_autoptr(GVariant) activatable = data;
  g_autoptr(GVariant) running = g_dbus_connection_call_finish (G_DBUS_CONNECTION (source), result, NULL);
  g_autofree const char **activatable_names = NULL, **running_names = NULL;
  g_autoptr(GHashTable) present = g_hash_table_new (g_str_hash, g_str_equal);
  g_autoptr(GHashTable) gone = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  GHashTableIter iter;
  const char *service;
  GList *list;
  gboolean look_again = FALSE;
  gint64 now = g_get_monotonic_time ();

  if (!running)
    return;
  g_variant_get (activatable, "(^a&s)", &activatable_names);
  g_variant_get (running, "(^a&s)", &running_names);
  for (guint i = 0; activatable_names[i]; i++)
    g_hash_table_add (present, (gpointer) activatable_names[i]);
  for (guint i = 0; running_names[i]; i++)
    g_hash_table_add (present, (gpointer) running_names[i]);

  list = vup_store_list ();
  for (GList *l = list; l; l = l->next)
    {
      VupRegistration *reg = l->data;
      App *app;

      if (!reg->activatable || g_hash_table_contains (present, reg->service))
        {
          if ((app = g_hash_table_lookup (apps, reg->service)) && g_hash_table_contains (present, reg->service))
            app->missing_since = 0;
          continue;
        }
      app = app_get (reg->service);
      if (!app->missing_since)
        {
          app->missing_since = now;
          g_message ("%s has no D-Bus service file any more: looking again in %u s", reg->service, GONE_GRACE);
        }
      if (now - app->missing_since >= GONE_GRACE * G_USEC_PER_SEC)
        g_hash_table_add (gone, g_strdup (reg->service));
      else
        look_again = TRUE;
    }
  g_list_free (list);
  /* after the walk through the registrations: app_gone removes some */
  g_hash_table_iter_init (&iter, gone);
  while (g_hash_table_iter_next (&iter, (gpointer *) &service, NULL))
    app_gone (app_get (service));
  if (look_again && !check_id)
    check_id = g_timeout_add_seconds (GONE_GRACE + 1, on_check, NULL);
}

static void
on_activatable_names (GObject *source, GAsyncResult *result, gpointer data)
{
  GVariant *activatable = g_dbus_connection_call_finish (G_DBUS_CONNECTION (source), result, NULL);

  if (!activatable)
    return;
  g_dbus_connection_call (bus, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "ListNames",
                          NULL, G_VARIANT_TYPE ("(as)"), G_DBUS_CALL_FLAGS_NONE, -1, NULL, on_names, activatable);
}

void
vup_deliver_check_installed (void)
{
  g_dbus_connection_call (bus, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus",
                          "ListActivatableNames", NULL, G_VARIANT_TYPE ("(as)"), G_DBUS_CALL_FLAGS_NONE, -1, NULL,
                          on_activatable_names, NULL);
}

static gboolean
on_check (gpointer data)
{
  check_id = 0;
  vup_deliver_check_installed ();
  return G_SOURCE_REMOVE;
}

static void
on_services_changed (GDBusConnection *connection, const char *sender, const char *path, const char *interface,
                     const char *signal, GVariant *parameters, gpointer data)
{
  g_clear_handle_id (&check_id, g_source_remove);
  check_id = g_timeout_add_seconds (5, on_check, NULL);
}

static void
on_registered_activatable (GObject *source, GAsyncResult *result, gpointer data)
{
  g_autofree char *token = data;
  g_autoptr(GVariant) names = g_dbus_connection_call_finish (G_DBUS_CONNECTION (source), result, NULL);
  g_autofree const char **list = NULL;
  VupRegistration *reg = vup_store_by_token (token);
  gboolean activatable;

  if (!names || !reg)
    return;
  g_variant_get (names, "(^a&s)", &list);
  activatable = g_strv_contains (list, reg->service);
  if (activatable != reg->activatable)
    {
      reg->activatable = activatable;
      vup_store_save ();
    }
  if (!activatable)
    g_message ("%s has no D-Bus service file: it gets messages only while it runs", reg->service);
}

/* whether the bus can start the app: asked again at every Register, as an update may have added a service file */
void
vup_deliver_check_activatable (VupRegistration *reg)
{
  g_dbus_connection_call (bus, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus",
                          "ListActivatableNames", NULL, G_VARIANT_TYPE ("(as)"), G_DBUS_CALL_FLAGS_NONE, -1, NULL,
                          on_registered_activatable, g_strdup (reg->token));
}

/* --- setting up */

void
vup_deliver_init (GDBusConnection *session_bus)
{
  g_autoptr(GError) error = NULL;
  GStrv done;

  bus = g_object_ref (session_bus);
  if (!(system_bus = g_bus_get_sync (G_BUS_TYPE_SYSTEM, NULL, &error)))
    g_warning ("no system bus, so nothing holds the system awake while delivering: %s", error->message);
  apps = g_hash_table_new (g_str_hash, g_str_equal);
  seen = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  /* the messages that were dealt with after the stream's position, before a restart: they will come again */
  done = vup_store_get_done ();
  for (guint i = 0; done && done[i]; i++)
    remember_seen (done[i]);
  services_subscription = g_dbus_connection_signal_subscribe (bus, "org.freedesktop.DBus", "org.freedesktop.DBus",
                                                              "ActivatableServicesChanged", "/org/freedesktop/DBus",
                                                              NULL, G_DBUS_SIGNAL_FLAGS_NONE, on_services_changed,
                                                              NULL, NULL);
  check_id = g_timeout_add_seconds (CHECK_DELAY, on_check, NULL);
}

void
vup_deliver_shutdown (void)
{
  g_clear_handle_id (&check_id, g_source_remove);
  g_clear_handle_id (&linger_id, g_source_remove);
  if (inhibit_fd >= 0)
    close (inhibit_fd);
  inhibit_fd = -1;
  if (services_subscription)
    g_dbus_connection_signal_unsubscribe (bus, services_subscription);
}
