/* The phone's modem (modem.h). */
#include "modem.h"
#include "phone-number.h"

#include <stdlib.h>
#include <string.h>

#define MM_NAME "org.freedesktop.ModemManager1"
#define MM_PATH "/org/freedesktop/ModemManager1"
#define MODEM_INTERFACE "org.freedesktop.ModemManager1.Modem"
#define VOICE_INTERFACE "org.freedesktop.ModemManager1.Modem.Voice"
#define GPP_INTERFACE "org.freedesktop.ModemManager1.Modem.Modem3gpp"
#define NOT_SUPPORTED_ERROR "org.freedesktop.ModemManager1.Error.MobileEquipment.NotSupported"

struct _Modem {
  GDBusConnection *system;
  ModemCallbacks callbacks;
  gpointer data;
  GCancellable *cancellable;

  GDBusObjectManager *manager;  /* ModemManager's modems; it follows ModemManager going and coming back itself */
  GDBusObject *object;          /* the modem we use, or NULL */
  GDBusProxy *voice;            /* its Voice interface */
  GDBusProxy *gpp;              /* its Modem3gpp interface: the network it is on */
  char *country;
};

gboolean
modem_error_is_unsupported (const GError *error)
{
  g_autofree char *name = NULL;

  if (!error || !g_dbus_error_is_remote_error (error))
    return FALSE;
  name = g_dbus_error_get_remote_error (error);
  return g_strcmp0 (name, NOT_SUPPORTED_ERROR) == 0;
}

/* --- the network's country */

static void
update_country (Modem *self)
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

/* --- the calls */

static void
on_voice_signal (GDBusProxy *proxy, const char *sender, const char *signal, GVariant *parameters, gpointer data)
{
  Modem *self = data;
  const char *path;

  if (g_str_equal (signal, "CallAdded"))
    {
      g_variant_get (parameters, "(&o)", &path);
      self->callbacks.call_added (path, self->data);
    }
  else if (g_str_equal (signal, "CallDeleted"))
    {
      g_variant_get (parameters, "(&o)", &path);
      self->callbacks.call_deleted (path, self->data);
    }
}

static void
on_list_calls (GObject *source, GAsyncResult *result, gpointer data)
{
  g_autoptr(GVariant) reply = g_dbus_proxy_call_finish (G_DBUS_PROXY (source), result, NULL);
  Modem *self = data;
  g_autoptr(GVariantIter) paths = NULL;
  const char *path;

  if (!reply)
    return;
  /* calls up before we came (the service restarted in a call): the same as calls just added */
  g_variant_get (reply, "(ao)", &paths);
  while (g_variant_iter_next (paths, "&o", &path))
    self->callbacks.call_added (path, self->data);
}

/* --- which modem */

static void
drop_modem (Modem *self)
{
  gboolean had = self->voice != NULL;

  if (self->voice)
    g_signal_handlers_disconnect_by_data (self->voice, self);
  if (self->gpp)
    g_signal_handlers_disconnect_by_data (self->gpp, self);
  g_clear_object (&self->voice);
  g_clear_object (&self->gpp);
  g_clear_object (&self->object);
  g_clear_pointer (&self->country, g_free);
  if (had)
    self->callbacks.available_changed (FALSE, self->data);
}

static void
use_modem (Modem *self, GDBusObject *object)
{
  self->object = g_object_ref (object);
  self->voice = G_DBUS_PROXY (g_dbus_object_get_interface (object, VOICE_INTERFACE));
  self->gpp = (GDBusProxy *) g_dbus_object_get_interface (object, GPP_INTERFACE);
  g_signal_connect (self->voice, "g-signal", G_CALLBACK (on_voice_signal), self);
  if (self->gpp)
    g_signal_connect (self->gpp, "g-properties-changed", G_CALLBACK (on_gpp_properties), self);
  update_country (self);
  g_message ("modem: %s can make calls", g_dbus_object_get_object_path (object));
  self->callbacks.available_changed (TRUE, self->data);
  g_dbus_proxy_call (self->voice, "ListCalls", NULL, G_DBUS_CALL_FLAGS_NONE, -1, self->cancellable, on_list_calls,
                     self);
}

/* the first modem with a Voice interface; ModemManager adds that interface only once the modem is enabled, and takes
 * it away again when the modem goes, so this runs again on every such change */
static void
choose_modem (Modem *self)
{
  g_autolist(GDBusObject) objects = self->manager ? g_dbus_object_manager_get_objects (self->manager) : NULL;
  GDBusObject *chosen = NULL;
  g_autoptr(GDBusInterface) gpp = NULL;

  for (GList *l = objects; l && !chosen; l = l->next)
    {
      g_autoptr(GDBusInterface) voice = g_dbus_object_get_interface (l->data, VOICE_INTERFACE);

      if (voice)
        chosen = l->data;
    }
  if (chosen && chosen == self->object)
    {
      /* the same modem: its Modem3gpp may have come since */
      gpp = g_dbus_object_get_interface (chosen, GPP_INTERFACE);
      if (gpp && !self->gpp)
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
  Modem *self = data;

  if (!manager)
    {
      if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        g_warning ("modem: cannot reach ModemManager: %s", error->message);
      return;
    }
  self->manager = manager;
  g_signal_connect (manager, "object-added", G_CALLBACK (on_object_changed), self);
  g_signal_connect (manager, "object-removed", G_CALLBACK (on_object_changed), self);
  g_signal_connect (manager, "interface-added", G_CALLBACK (on_interface_changed), self);
  g_signal_connect (manager, "interface-removed", G_CALLBACK (on_interface_changed), self);
  /* ModemManager gone: its objects go (object-removed); back: they come again (object-added) */
  choose_modem (self);
}

Modem *
modem_new (GDBusConnection *system, const ModemCallbacks *callbacks, gpointer data)
{
  Modem *self = g_new0 (Modem, 1);

  self->system = g_object_ref (system);
  self->callbacks = *callbacks;
  self->data = data;
  self->cancellable = g_cancellable_new ();
  /* DO_NOT_AUTO_START: ModemManager runs from boot; a service that asked for it would start it on a phone that has
   * it off on purpose */
  g_dbus_object_manager_client_new (system, G_DBUS_OBJECT_MANAGER_CLIENT_FLAGS_DO_NOT_AUTO_START, MM_NAME, MM_PATH,
                                    NULL, NULL, NULL, self->cancellable, on_manager, self);
  return self;
}

void
modem_free (Modem *self)
{
  g_cancellable_cancel (self->cancellable);
  if (self->manager)
    g_signal_handlers_disconnect_by_data (self->manager, self);
  if (self->voice)
    g_signal_handlers_disconnect_by_data (self->voice, self);
  if (self->gpp)
    g_signal_handlers_disconnect_by_data (self->gpp, self);
  g_clear_object (&self->voice);
  g_clear_object (&self->gpp);
  g_clear_object (&self->object);
  g_clear_object (&self->manager);
  g_clear_object (&self->cancellable);
  g_object_unref (self->system);
  g_free (self->country);
  g_free (self);
}

gboolean
modem_is_available (Modem *self)
{
  return self->voice != NULL;
}

const char *
modem_get_country (Modem *self)
{
  return self->country;
}

gboolean
modem_get_emergency_only (Modem *self)
{
  g_autoptr(GVariant) only = self->voice ? g_dbus_proxy_get_cached_property (self->voice, "EmergencyOnly") : NULL;

  return only && g_variant_get_boolean (only);
}

char *
modem_get_model (Modem *self)
{
  g_autoptr(GDBusInterface) modem = self->object ? g_dbus_object_get_interface (self->object, MODEM_INTERFACE) : NULL;
  g_autoptr(GVariant) manufacturer = NULL, model = NULL, revision = NULL;

  if (!modem)
    return NULL;
  manufacturer = g_dbus_proxy_get_cached_property (G_DBUS_PROXY (modem), "Manufacturer");
  model = g_dbus_proxy_get_cached_property (G_DBUS_PROXY (modem), "Model");
  revision = g_dbus_proxy_get_cached_property (G_DBUS_PROXY (modem), "Revision");
  return g_strdup_printf ("%s %s %s", manufacturer ? g_variant_get_string (manufacturer, NULL) : "",
                          model ? g_variant_get_string (model, NULL) : "",
                          revision ? g_variant_get_string (revision, NULL) : "");
}

static void
on_create_call (GObject *source, GAsyncResult *result, gpointer data)
{
  g_autoptr(GTask) task = data;
  g_autoptr(GError) error = NULL;
  g_autoptr(GVariant) reply = g_dbus_proxy_call_finish (G_DBUS_PROXY (source), result, &error);
  const char *path;

  if (!reply)
    {
      g_task_return_error (task, g_steal_pointer (&error));
      return;
    }
  g_variant_get (reply, "(&o)", &path);
  g_task_return_pointer (task, g_strdup (path), g_free);
}

void
modem_create_call (Modem *self, const char *number, GCancellable *cancellable, GAsyncReadyCallback callback,
                   gpointer data)
{
  GTask *task = g_task_new (NULL, cancellable, callback, data);
  GVariantBuilder properties;

  if (!self->voice)
    {
      g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_NOT_CONNECTED, "There is no modem");
      g_object_unref (task);
      return;
    }
  g_variant_builder_init (&properties, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_add (&properties, "{sv}", "number", g_variant_new_string (number));
  g_dbus_proxy_call (self->voice, "CreateCall", g_variant_new ("(a{sv})", &properties), G_DBUS_CALL_FLAGS_NONE, -1,
                     cancellable, on_create_call, task);
}

char *
modem_create_call_finish (Modem *self, GAsyncResult *result, GError **error)
{
  return g_task_propagate_pointer (G_TASK (result), error);
}

static void
on_voice_method (GObject *source, GAsyncResult *result, gpointer data)
{
  g_autoptr(GTask) task = data;
  g_autoptr(GError) error = NULL;
  g_autoptr(GVariant) reply = g_dbus_proxy_call_finish (G_DBUS_PROXY (source), result, &error);

  if (reply)
    g_task_return_boolean (task, TRUE);
  else
    g_task_return_error (task, g_steal_pointer (&error));
}

void
modem_voice_method (Modem *self, const char *method, GAsyncReadyCallback callback, gpointer data)
{
  GTask *task = g_task_new (NULL, NULL, callback, data);

  if (!self->voice)
    {
      g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_NOT_CONNECTED, "There is no modem");
      g_object_unref (task);
      return;
    }
  g_dbus_proxy_call (self->voice, method, NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, on_voice_method, task);
}

gboolean
modem_voice_method_finish (Modem *self, GAsyncResult *result, GError **error)
{
  return g_task_propagate_boolean (G_TASK (result), error);
}

void
modem_delete_call (Modem *self, const char *path)
{
  if (!self->voice)
    return;
  g_dbus_proxy_call (self->voice, "DeleteCall", g_variant_new ("(o)", path), G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL,
                     NULL);
}
