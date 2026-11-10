/* The phone's text messages (sms-modem.h). */
#include "sms-modem.h"
#include "phone-number.h"

#include <stdlib.h>
#include <string.h>

#define MM_NAME "org.freedesktop.ModemManager1"
#define MM_PATH "/org/freedesktop/ModemManager1"
#define MESSAGING_INTERFACE "org.freedesktop.ModemManager1.Modem.Messaging"
#define SMS_INTERFACE "org.freedesktop.ModemManager1.Sms"
#define GPP_INTERFACE "org.freedesktop.ModemManager1.Modem.Modem3gpp"
#define SEND_TIMEOUT_MS (5 * 60 * 1000) /* BugleGservicesKeys' SMS send timeout */

/* MMSmsState and MMSmsPduType */
enum { SMS_STATE_RECEIVING = 2, SMS_STATE_RECEIVED = 3 };
enum { PDU_DELIVER = 1, PDU_STATUS_REPORT = 3, PDU_CDMA_DELIVER = 4 };

struct _SmsModem {
  gint refs;                    /* the service's, and one per send in flight: a reply may come after the free */
  GDBusConnection *system;
  SmsModemCallbacks callbacks;
  gpointer data;
  GCancellable *cancellable;

  GDBusObjectManager *manager;  /* ModemManager's modems, followed through ModemManager going and coming back */
  GDBusObject *object;          /* the modem we use, or NULL */
  GDBusProxy *messaging;
  GDBusProxy *gpp;              /* the network it is on */
  char *country;
  GHashTable *watched;          /* path -> its Sms proxy (NULL while it is made): a message not yet handed over */
  GHashTable *handed;           /* the paths handed over, so a List after an Added gives each once */
};

/* --- the network's country */

static void
update_country (SmsModem *self)
{
  g_autoptr(GVariant) code = self->gpp ? g_dbus_proxy_get_cached_property (self->gpp, "OperatorCode") : NULL;
  const char *mcc_mnc = code ? g_variant_get_string (code, NULL) : "";

  g_clear_pointer (&self->country, g_free);
  /* "26203": the first three digits are the country (ITU E.212) */
  if (strlen (mcc_mnc) >= 3)
    {
      g_autofree char *mcc = g_strndup (mcc_mnc, 3);

      self->country = g_strdup (phone_number_country_for_mcc (atoi (mcc)));
    }
}

static void
on_gpp_properties (GDBusProxy *proxy, GVariant *changed, GStrv invalidated, gpointer data)
{
  update_country (data);
}

/* --- messages coming in */

static char *
string_property (GDBusProxy *proxy, const char *name)
{
  g_autoptr(GVariant) value = g_dbus_proxy_get_cached_property (proxy, name);

  return value && g_variant_is_of_type (value, G_VARIANT_TYPE_STRING) ? g_variant_dup_string (value, NULL) : NULL;
}

static guint
uint_property (GDBusProxy *proxy, const char *name)
{
  g_autoptr(GVariant) value = g_dbus_proxy_get_cached_property (proxy, name);

  return value && g_variant_is_of_type (value, G_VARIANT_TYPE_UINT32) ? g_variant_get_uint32 (value) : 0;
}

/* a binary message: no text, only data. The one that comes is an MMS notification (a WAP push to port 2948), which
 * is mmsd-tng's to read and fetch; as a text message it would be an empty bubble */
static gboolean
is_binary (GDBusProxy *proxy)
{
  g_autoptr(GVariant) data = g_dbus_proxy_get_cached_property (proxy, "Data");
  g_autofree char *text = string_property (proxy, "Text");

  return (!text || !*text) && data && g_variant_is_of_type (data, G_VARIANT_TYPE_BYTESTRING) &&
         g_variant_n_children (data) > 0;
}

/* the service centre's timestamp, ISO 8601 with the network's offset ("2026-11-08T01:02:03+01"), as Unix ms */
static gint64
timestamp_ms (const char *text)
{
  g_autoptr(GDateTime) time = text && *text ? g_date_time_new_from_iso8601 (text, NULL) : NULL;

  return time ? g_date_time_to_unix_usec (time) / 1000 : 0;
}

/* the watched table's values: a proxy, or NULL while it is made */
static void
unref_proxy (gpointer proxy)
{
  if (proxy)
    g_object_unref (proxy);
}

static void
unwatch (SmsModem *self, const char *path)
{
  GDBusProxy *proxy = g_hash_table_lookup (self->watched, path);

  if (proxy)
    g_signal_handlers_disconnect_by_data (proxy, self);
  g_hash_table_remove (self->watched, path);
}

/* a message is handed over once it is RECEIVED, and so is a delivery report, which is then deleted (it is no
 * message: it says what became of one sent from here); anything else (a message being sent, a draft someone
 * stored) is not ours */
static void
check (SmsModem *self, GDBusProxy *proxy)
{
  g_autofree char *path = g_strdup (g_dbus_proxy_get_object_path (proxy));
  guint state = uint_property (proxy, "State"), type = uint_property (proxy, "PduType");
  g_autofree char *number = NULL, *text = NULL, *timestamp = NULL;

  if (state == SMS_STATE_RECEIVING)
    return;                     /* the other parts are still to come */
  if (state == SMS_STATE_RECEIVED && type == PDU_STATUS_REPORT)
    {
      number = string_property (proxy, "Number");
      g_hash_table_add (self->handed, g_strdup (path));
      g_message ("sms: %s is a delivery report (reference %u, state 0x%x)", path,
                 uint_property (proxy, "MessageReference"), uint_property (proxy, "DeliveryState"));
      if (self->callbacks.status_report)
        self->callbacks.status_report (number ? number : "", uint_property (proxy, "MessageReference"),
                                       uint_property (proxy, "DeliveryState"), self->data);
      unwatch (self, path);
      sms_modem_delete (self, path);
      return;
    }
  if (state != SMS_STATE_RECEIVED || (type != PDU_DELIVER && type != PDU_CDMA_DELIVER))
    {
      unwatch (self, path);
      return;
    }
  if (is_binary (proxy))
    {
      /* left on the modem, not deleted: mmsd-tng takes it from there */
      g_message ("sms: %s is binary (an MMS notification): left to mmsd-tng", path);
      unwatch (self, path);
      return;
    }
  number = string_property (proxy, "Number");
  text = string_property (proxy, "Text");
  timestamp = string_property (proxy, "Timestamp");
  g_hash_table_add (self->handed, g_strdup (path));
  unwatch (self, path);
  g_message ("sms: %s came", path);
  self->callbacks.received (path, number ? number : "", text ? text : "", timestamp_ms (timestamp), self->data);
}

static void
on_sms_properties (GDBusProxy *proxy, GVariant *changed, GStrv invalidated, gpointer data)
{
  check (data, proxy);
}

typedef struct
{
  SmsModem *self;
  char *path;
} Watch;

static void
on_sms_proxy (GObject *source, GAsyncResult *result, gpointer data)
{
  Watch *watch = data;
  g_autoptr(GError) error = NULL;
  GDBusProxy *proxy = g_dbus_proxy_new_finish (result, &error);
  SmsModem *self = watch->self;

  if (!proxy)
    {
      if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        {
          g_warning ("sms: %s: %s", watch->path, error->message);
          g_hash_table_remove (self->watched, watch->path);
        }
    }
  else if (!g_hash_table_contains (self->watched, watch->path))
    g_object_unref (proxy);     /* the modem went meanwhile */
  else
    {
      g_hash_table_replace (self->watched, g_strdup (watch->path), proxy);
      g_signal_connect (proxy, "g-properties-changed", G_CALLBACK (on_sms_properties), self);
      check (self, proxy);
    }
  g_free (watch->path);
  g_free (watch);
}

static void
watch_sms (SmsModem *self, const char *path)
{
  Watch *watch;

  if (g_hash_table_contains (self->watched, path) || g_hash_table_contains (self->handed, path))
    return;
  g_hash_table_insert (self->watched, g_strdup (path), NULL);
  watch = g_new0 (Watch, 1);
  watch->self = self;
  watch->path = g_strdup (path);
  g_dbus_proxy_new (self->system, G_DBUS_PROXY_FLAGS_DO_NOT_AUTO_START, NULL, MM_NAME, path, SMS_INTERFACE,
                    self->cancellable, on_sms_proxy, watch);
}

static void
on_messaging_signal (GDBusProxy *proxy, const char *sender, const char *signal, GVariant *parameters, gpointer data)
{
  SmsModem *self = data;
  const char *path;
  gboolean received;

  if (g_str_equal (signal, "Added"))
    {
      /* received FALSE: one made here (ours, being sent) */
      g_variant_get (parameters, "(&ob)", &path, &received);
      if (received)
        watch_sms (self, path);
    }
  else if (g_str_equal (signal, "Deleted"))
    {
      g_variant_get (parameters, "(&o)", &path);
      unwatch (self, path);
      g_hash_table_remove (self->handed, path);
    }
}

static void
on_list (GObject *source, GAsyncResult *result, gpointer data)
{
  g_autoptr(GVariant) reply = g_dbus_proxy_call_finish (G_DBUS_PROXY (source), result, NULL);
  g_autoptr(GVariantIter) paths = NULL;
  const char *path;

  if (!reply)
    return;
  /* messages that came while nobody listened (the service was not running): as if they came now */
  g_variant_get (reply, "(ao)", &paths);
  while (g_variant_iter_next (paths, "&o", &path))
    watch_sms (data, path);
}

/* --- which modem */

static void
drop_modem (SmsModem *self)
{
  gboolean had = self->messaging != NULL;
  GHashTableIter iter;
  gpointer proxy;

  if (self->messaging)
    g_signal_handlers_disconnect_by_data (self->messaging, self);
  if (self->gpp)
    g_signal_handlers_disconnect_by_data (self->gpp, self);
  g_hash_table_iter_init (&iter, self->watched);
  while (g_hash_table_iter_next (&iter, NULL, &proxy))
    if (proxy)
      g_signal_handlers_disconnect_by_data (proxy, self);
  g_hash_table_remove_all (self->watched);
  g_hash_table_remove_all (self->handed);
  g_clear_object (&self->messaging);
  g_clear_object (&self->gpp);
  g_clear_object (&self->object);
  g_clear_pointer (&self->country, g_free);
  if (had && self->callbacks.available_changed)
    self->callbacks.available_changed (FALSE, self->data);
}

static void
use_modem (SmsModem *self, GDBusObject *object)
{
  self->object = g_object_ref (object);
  self->messaging = G_DBUS_PROXY (g_dbus_object_get_interface (object, MESSAGING_INTERFACE));
  self->gpp = (GDBusProxy *) g_dbus_object_get_interface (object, GPP_INTERFACE);
  g_signal_connect (self->messaging, "g-signal", G_CALLBACK (on_messaging_signal), self);
  if (self->gpp)
    g_signal_connect (self->gpp, "g-properties-changed", G_CALLBACK (on_gpp_properties), self);
  update_country (self);
  g_message ("sms: %s has messages", g_dbus_object_get_object_path (object));
  self->callbacks.available_changed (TRUE, self->data);
  g_dbus_proxy_call (self->messaging, "List", NULL, G_DBUS_CALL_FLAGS_NONE, -1, self->cancellable, on_list, self);
}

/* the first modem with a Messaging interface, which ModemManager adds once the modem is enabled; this runs again
 * on every change of ModemManager's objects */
static void
choose_modem (SmsModem *self)
{
  g_autolist(GDBusObject) objects = self->manager ? g_dbus_object_manager_get_objects (self->manager) : NULL;
  GDBusObject *chosen = NULL;

  for (GList *l = objects; l && !chosen; l = l->next)
    {
      g_autoptr(GDBusInterface) messaging = g_dbus_object_get_interface (l->data, MESSAGING_INTERFACE);

      if (messaging)
        chosen = l->data;
    }
  if (chosen && chosen == self->object)
    {
      /* the same modem: its Modem3gpp may have come since */
      g_autoptr(GDBusInterface) gpp = self->gpp ? NULL : g_dbus_object_get_interface (chosen, GPP_INTERFACE);

      if (gpp)
        {
          self->gpp = G_DBUS_PROXY (g_steal_pointer (&gpp));
          g_signal_connect (self->gpp, "g-properties-changed", G_CALLBACK (on_gpp_properties), self);
          update_country (self);
        }
      return;
    }
  drop_modem (self);
  if (chosen)
    use_modem (self, chosen);
}

/* Two handlers, because the signals differ: object-added/-removed pass (object), interface-added/-removed pass
 * (object, interface). One handler for both read the user data from the wrong argument on object-added, and the
 * service crashed whenever a modem appeared after it started (at boot on the POCO, at every ModemManager restart). */
static void
on_object_changed (GDBusObjectManager *manager, GDBusObject *object, gpointer data)
{
  choose_modem (data);
}

static void
on_interface_changed (GDBusObjectManager *manager, GDBusObject *object, GDBusInterface *interface, gpointer data)
{
  choose_modem (data);
}

static void
on_manager (GObject *source, GAsyncResult *result, gpointer data)
{
  g_autoptr(GError) error = NULL;
  GDBusObjectManager *manager = g_dbus_object_manager_client_new_finish (result, &error);
  SmsModem *self = data;

  if (!manager)
    {
      if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        g_warning ("sms: cannot reach ModemManager: %s", error->message);
      return;
    }
  self->manager = manager;
  g_signal_connect (manager, "object-added", G_CALLBACK (on_object_changed), self);
  g_signal_connect (manager, "object-removed", G_CALLBACK (on_object_changed), self);
  g_signal_connect (manager, "interface-added", G_CALLBACK (on_interface_changed), self);
  g_signal_connect (manager, "interface-removed", G_CALLBACK (on_interface_changed), self);
  choose_modem (self);
}

static SmsModem *
sms_modem_ref (SmsModem *self)
{
  g_atomic_int_inc (&self->refs);
  return self;
}

static void
sms_modem_unref (SmsModem *self)
{
  if (!g_atomic_int_dec_and_test (&self->refs))
    return;
  g_clear_object (&self->cancellable);
  g_hash_table_unref (self->watched);
  g_hash_table_unref (self->handed);
  g_object_unref (self->system);
  g_free (self);
}

SmsModem *
sms_modem_new (GDBusConnection *system, const SmsModemCallbacks *callbacks, gpointer data)
{
  SmsModem *self = g_new0 (SmsModem, 1);

  self->refs = 1;
  self->system = g_object_ref (system);
  self->callbacks = *callbacks;
  self->data = data;
  self->cancellable = g_cancellable_new ();
  self->watched = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, unref_proxy);
  self->handed = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  g_dbus_object_manager_client_new (system, G_DBUS_OBJECT_MANAGER_CLIENT_FLAGS_DO_NOT_AUTO_START, MM_NAME, MM_PATH,
                                    NULL, NULL, NULL, self->cancellable, on_manager, self);
  return self;
}

void
sms_modem_free (SmsModem *self)
{
  if (!self)
    return;
  g_cancellable_cancel (self->cancellable);
  if (self->manager)
    g_signal_handlers_disconnect_by_data (self->manager, self);
  /* told nothing more: the modem is gone for whoever freed it */
  self->callbacks.available_changed = NULL;
  drop_modem (self);
  g_clear_object (&self->manager);
  sms_modem_unref (self);
}

gboolean
sms_modem_is_available (SmsModem *self)
{
  return self->messaging != NULL;
}

const char *
sms_modem_get_country (SmsModem *self)
{
  return self->country;
}

/* --- sending */

void
sms_modem_delete (SmsModem *self, const char *path)
{
  if (!self->messaging)
    return;
  g_dbus_proxy_call (self->messaging, "Delete", g_variant_new ("(o)", path), G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL,
                     NULL);
}

/* a send's: the Sms object made for it, and whether a report was asked for */
typedef struct
{
  char *path;
  gboolean delivery_report;
} Send;

static void
send_free (gpointer data)
{
  Send *send = data;

  g_free (send->path);
  g_free (send);
}

/* the message is out, and the reference its report will carry is read before the Sms object goes */
static void
on_reference (GObject *source, GAsyncResult *result, gpointer data)
{
  g_autoptr(GTask) task = data;
  SmsModem *self = g_object_get_data (G_OBJECT (task), "modem");
  Send *send = g_task_get_task_data (task);
  g_autoptr(GError) error = NULL;
  g_autoptr(GVariant) reply = g_dbus_connection_call_finish (G_DBUS_CONNECTION (source), result, &error);
  g_autoptr(GVariant) value = NULL;
  gssize reference = -1;

  sms_modem_delete (self, send->path);
  if (reply)
    {
      g_variant_get (reply, "(v)", &value);
      if (g_variant_is_of_type (value, G_VARIANT_TYPE_UINT32))
        reference = g_variant_get_uint32 (value);
    }
  if (reference < 0)
    g_warning ("sms: %s was sent, but its reference is unknown (%s): its delivery report will not be matched",
               send->path, error ? error->message : "not a number");
  /* sent, whatever became of the reference */
  g_task_return_int (task, reference);
}

static void
on_sent (GObject *source, GAsyncResult *result, gpointer data)
{
  g_autoptr(GTask) task = data;
  SmsModem *self = g_object_get_data (G_OBJECT (task), "modem");
  Send *send = g_task_get_task_data (task);
  g_autoptr(GError) error = NULL;
  g_autoptr(GVariant) reply = g_dbus_connection_call_finish (G_DBUS_CONNECTION (source), result, &error);

  if (reply && send->delivery_report)
    {
      /* ModemManager set it before it answered Send (the network's TP-MR for the last part) */
      g_dbus_connection_call (self->system, MM_NAME, send->path, "org.freedesktop.DBus.Properties", "Get",
                              g_variant_new ("(ss)", SMS_INTERFACE, "MessageReference"), G_VARIANT_TYPE ("(v)"),
                              G_DBUS_CALL_FLAGS_NONE, -1, NULL, on_reference, g_steal_pointer (&task));
      return;
    }
  /* kept in the store, not on the modem, either way (a no-op once the modem went) */
  sms_modem_delete (self, send->path);
  if (!reply)
    g_task_return_error (task, g_steal_pointer (&error));
  else
    g_task_return_int (task, -1);
}

static void
on_created (GObject *source, GAsyncResult *result, gpointer data)
{
  g_autoptr(GTask) task = data;
  SmsModem *self = g_object_get_data (G_OBJECT (task), "modem");
  Send *send = g_task_get_task_data (task);
  g_autoptr(GError) error = NULL;
  g_autoptr(GVariant) reply = g_dbus_proxy_call_finish (G_DBUS_PROXY (source), result, &error);
  GCancellable *cancellable;
  const char *path;

  if (!reply)
    {
      g_task_return_error (task, g_steal_pointer (&error));
      return;
    }
  g_variant_get (reply, "(&o)", &path);
  send->path = g_strdup (path);
  cancellable = g_task_get_cancellable (task);
  g_dbus_connection_call (self->system, MM_NAME, path, SMS_INTERFACE, "Send", NULL, NULL, G_DBUS_CALL_FLAGS_NONE,
                          SEND_TIMEOUT_MS, cancellable, on_sent, g_steal_pointer (&task));
}

void
sms_modem_send (SmsModem *self, const char *number, const char *text, gboolean delivery_report,
                GCancellable *cancellable, GAsyncReadyCallback callback, gpointer data)
{
  GTask *task;
  GVariantBuilder properties;
  Send *send;

  /* the modem's own: freeing it stops the send */
  if (!cancellable)
    cancellable = self->cancellable;
  task = g_task_new (NULL, cancellable, callback, data);
  g_task_set_source_tag (task, sms_modem_send);
  /* the modem is not a GObject, so not the task's source object */
  g_object_set_data_full (G_OBJECT (task), "modem", sms_modem_ref (self), (GDestroyNotify) sms_modem_unref);
  send = g_new0 (Send, 1);
  send->delivery_report = delivery_report;
  g_task_set_task_data (task, send, send_free);
  if (!self->messaging)
    {
      g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_NOT_CONNECTED, "No modem");
      g_object_unref (task);
      return;
    }
  g_variant_builder_init (&properties, G_VARIANT_TYPE ("a{sv}"));
  g_variant_builder_add (&properties, "{sv}", "number", g_variant_new_string (number));
  g_variant_builder_add (&properties, "{sv}", "text", g_variant_new_string (text));
  /* only when asked: ModemManager's default is none, and a message without it is made as it always was */
  if (delivery_report)
    g_variant_builder_add (&properties, "{sv}", "delivery-report-request", g_variant_new_boolean (TRUE));
  g_dbus_proxy_call (self->messaging, "Create", g_variant_new ("(a{sv})", &properties), G_DBUS_CALL_FLAGS_NONE, -1,
                     cancellable, on_created, task);
}

gboolean
sms_modem_send_finish (SmsModem *self, GAsyncResult *result, int *reference, GError **error)
{
  GError *failure = NULL;
  gssize value = g_task_propagate_int (G_TASK (result), &failure);

  if (reference)
    *reference = failure ? -1 : (int) value;
  if (failure)
    {
      g_propagate_error (error, failure);
      return FALSE;
    }
  return TRUE;
}
