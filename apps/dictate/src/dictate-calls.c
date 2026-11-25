/* dictate-calls: see dictate-calls.h. */
#include "dictate-calls.h"

#include <json-glib/json-glib.h>
#include <stdio.h>
#include <string.h>

GDBusConnection *
dictate_calls_bus (GCancellable *cancellable, GError **error)
{
  const char *address = g_getenv ("DICTATE_DIALER_BUS");

  if (address && *address)
    return g_dbus_connection_new_for_address_sync (address, G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT
                                                            | G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION,
                                                   NULL, cancellable, error);
  return g_bus_get_sync (G_BUS_TYPE_SESSION, cancellable, error);
}

void
dictate_call_free (DictateCall *call)
{
  g_free (call->number);
  g_free (call);
}

gboolean
dictate_call_is_ringing (const DictateCall *call)
{
  return call->state == DICTATE_CALL_INCOMING || call->state == DICTATE_CALL_WAITING;
}

GPtrArray *
dictate_calls_parse (GVariant *calls)
{
  GPtrArray *list = g_ptr_array_new_with_free_func ((GDestroyNotify) dictate_call_free);
  GVariantIter iter;
  GVariant *dict;

  if (!calls || !g_variant_is_of_type (calls, G_VARIANT_TYPE ("aa{sv}")))
    return list;
  g_variant_iter_init (&iter, calls);
  while ((dict = g_variant_iter_next_value (&iter)))
    {
      DictateCall *call = g_new0 (DictateCall, 1);
      const char *number = "";
      guint32 state = DICTATE_CALL_DISCONNECTED;

      g_variant_lookup (dict, "id", "u", &call->id);
      g_variant_lookup (dict, "number", "&s", &number);
      g_variant_lookup (dict, "presentation", "u", &call->presentation);
      g_variant_lookup (dict, "direction", "u", &call->direction);
      g_variant_lookup (dict, "state", "u", &state);
      g_variant_lookup (dict, "connected", "x", &call->connected);
      call->number = g_strdup (number);
      call->state = state;
      g_variant_unref (dict);
      if (call->state >= DICTATE_CALL_DISCONNECTING)
        dictate_call_free (call);
      else
        g_ptr_array_add (list, call);
    }
  return list;
}

const char *
dictate_calls_context (GPtrArray *calls)
{
  const char *now = NULL;

  for (guint i = 0; calls && i < calls->len; i++)
    {
      DictateCall *c = calls->pdata[i];
      if (dictate_call_is_ringing (c))
        return "a call is ringing";
      if (c->state == DICTATE_CALL_ACTIVE)
        now = "a call is going on";
      else if (c->state == DICTATE_CALL_ONHOLD && !now)
        now = "a call is on hold";
      else if ((c->state == DICTATE_CALL_DIALING || c->state == DICTATE_CALL_CONNECTING) && !now)
        now = "a call is being made, not answered yet";
    }
  return now;
}

/* ---------------- what is said ---------------- */

typedef struct {
  const char *code;
  const char *named;            /* %s: the contact's name */
  const char *unknown, *withheld;
  const char *contact;          /* a contact, the name not said (private mode on the speaker) */
} Announcement;

static const Announcement ANNOUNCEMENTS[] = {
  { "en", "Call from %s. Answer or decline?", "Call from a number not in your contacts. Answer or decline?",
    "Call from a private number. Answer or decline?", "Call from one of your contacts. Answer or decline?" },
  { "de", "Anruf von %s. Annehmen oder ablehnen?", "Anruf von einer Nummer, die nicht in deinen Kontakten ist. "
    "Annehmen oder ablehnen?", "Anruf von einer unterdrückten Nummer. Annehmen oder ablehnen?",
    "Anruf von einem deiner Kontakte. Annehmen oder ablehnen?" },
  { "es", "Llamada de %s. ¿Contestar o rechazar?", "Llamada de un número que no está en tus contactos. ¿Contestar "
    "o rechazar?", "Llamada de un número oculto. ¿Contestar o rechazar?",
    "Llamada de uno de tus contactos. ¿Contestar o rechazar?" },
  { "fr", "Appel de %s. Répondre ou refuser ?", "Appel d'un numéro qui n'est pas dans vos contacts. Répondre ou "
    "refuser ?", "Appel d'un numéro masqué. Répondre ou refuser ?", "Appel d'un de vos contacts. Répondre ou "
    "refuser ?" },
  { "it", "Chiamata da %s. Rispondere o rifiutare?", "Chiamata da un numero che non è nei tuoi contatti. Rispondere "
    "o rifiutare?", "Chiamata da un numero privato. Rispondere o rifiutare?",
    "Chiamata da uno dei tuoi contatti. Rispondere o rifiutare?" },
  { "ru", "Звонит %s. Ответить или отклонить?", "Звонок с номера, которого нет в контактах. Ответить или "
    "отклонить?", "Звонок со скрытого номера. Ответить или отклонить?",
    "Звонит один из ваших контактов. Ответить или отклонить?" },
};

static const Announcement *
announcement_in (const char *language)
{
  const Announcement *a = &ANNOUNCEMENTS[0];

  for (guint i = 0; i < G_N_ELEMENTS (ANNOUNCEMENTS); i++)
    if (language && g_str_has_prefix (language, ANNOUNCEMENTS[i].code))
      a = &ANNOUNCEMENTS[i];
  return a;
}

char *
dictate_calls_announcement (const char *language, const char *who, gboolean withheld)
{
  const Announcement *a = announcement_in (language);

  if (who && *who)
    return g_strdup_printf (a->named, who);
  return g_strdup (withheld ? a->withheld : a->unknown);
}

char *
dictate_calls_announce_text (const char *language, const char *current)
{
  g_autoptr (JsonNode) root = current ? json_from_string (current, NULL) : NULL;
  JsonObject *o = root && JSON_NODE_HOLDS_OBJECT (root) ? json_node_get_object (root) : NULL;
  JsonArray *calls = o && json_object_has_member (o, "calls") ? json_object_get_array_member (o, "calls") : NULL;

  for (guint i = 0; calls && i < json_array_get_length (calls); i++)
    {
      JsonObject *c = json_array_get_object_element (calls, i);

      if (!c || !g_str_has_prefix (json_object_get_string_member_with_default (c, "state", ""), "ringing"))
        continue;
      /* private mode on the speaker: the tool gave no name (dictate-tools.c) */
      if (json_object_has_member (o, "names_withheld")
          && json_object_get_boolean_member_with_default (c, "in_contacts", FALSE))
        return g_strdup (announcement_in (language)->contact);
      return dictate_calls_announcement (language,
                                         json_object_get_boolean_member_with_default (c, "in_contacts", FALSE)
                                         ? json_object_get_string_member_with_default (c, "with", NULL) : NULL,
                                         json_object_get_boolean_member_with_default (c, "private_number", FALSE));
    }
  return NULL;
}

/* ---------------- when ---------------- */

static gboolean
parse_range (const char *range, int *from, int *to)
{
  int h1, m1, h2, m2;

  if (!range || sscanf (range, "%d:%d-%d:%d", &h1, &m1, &h2, &m2) != 4 || h1 < 0 || h1 > 23 || h2 < 0 || h2 > 23
      || m1 < 0 || m1 > 59 || m2 < 0 || m2 > 59)
    return FALSE;
  *from = h1 * 60 + m1, *to = h2 * 60 + m2;
  return *from != *to;
}

gboolean
dictate_calls_quiet_now (GSettings *settings)
{
  g_autofree char *range = g_settings_get_string (settings, "quiet-hours");
  g_autoptr (GDateTime) now = g_date_time_new_now_local ();
  int from, to, minute = g_date_time_get_hour (now) * 60 + g_date_time_get_minute (now);
  const char *test = g_getenv ("DICTATE_TEST_NOW");

  /* for the tests: the time of day, HH:MM */
  if (test)
    {
      int h, m;
      if (sscanf (test, "%d:%d", &h, &m) == 2)
        minute = h * 60 + m;
    }
  if (!parse_range (range, &from, &to))
    return FALSE;
  /* "22:00-07:00" goes over midnight */
  return from < to ? minute >= from && minute < to : minute >= from || minute < to;
}

gboolean
dictate_calls_may_announce (GSettings *settings, gboolean open)
{
  if (!g_settings_get_boolean (settings, "access-calls"))
    return FALSE;
  if (!open && !g_settings_get_boolean (settings, "announce-calls"))
    return FALSE;
  return !dictate_calls_quiet_now (settings) || g_settings_get_boolean (settings, "quiet-calls");
}

/* ---------------- watching ---------------- */

struct _DictateCallWatch {
  GObject parent_instance;
  GCancellable *cancellable;
  GDBusConnection *bus;
  guint subscription, name_watch;
  GPtrArray *calls;             /* DictateCall: as last seen */
};

G_DEFINE_FINAL_TYPE (DictateCallWatch, dictate_call_watch, G_TYPE_OBJECT)

enum { SIGNAL_RINGING, SIGNAL_STOPPED, SIGNAL_CHANGED, N_SIGNALS };
static guint signals[N_SIGNALS];

static DictateCall *
find_call (GPtrArray *calls, guint id)
{
  for (guint i = 0; calls && i < calls->len; i++)
    if (((DictateCall *) calls->pdata[i])->id == id)
      return calls->pdata[i];
  return NULL;
}

static void
take_calls (DictateCallWatch *self, GVariant *value)
{
  g_autoptr (GPtrArray) was = g_steal_pointer (&self->calls);

  self->calls = dictate_calls_parse (value);
  for (guint i = 0; was && i < was->len; i++)
    {
      DictateCall *old = was->pdata[i], *now = find_call (self->calls, old->id);
      if (dictate_call_is_ringing (old) && (!now || !dictate_call_is_ringing (now)))
        g_signal_emit (self, signals[SIGNAL_STOPPED], 0, old->id);
    }
  for (guint i = 0; i < self->calls->len; i++)
    {
      DictateCall *now = self->calls->pdata[i], *old = find_call (was, now->id);
      if (dictate_call_is_ringing (now) && (!old || !dictate_call_is_ringing (old)))
        g_signal_emit (self, signals[SIGNAL_RINGING], 0, now->id);
    }
  g_signal_emit (self, signals[SIGNAL_CHANGED], 0);
}

static void
on_properties (GDBusConnection *bus, const char *sender, const char *path, const char *interface,
               const char *signal, GVariant *parameters, gpointer data)
{
  g_autoptr (GVariant) changed = NULL;
  g_autoptr (GVariant) calls = NULL;
  const char *iface;

  if (!g_variant_is_of_type (parameters, G_VARIANT_TYPE ("(sa{sv}as)")))
    return;
  g_variant_get (parameters, "(&s@a{sv}as)", &iface, &changed, NULL);
  if (g_strcmp0 (iface, DICTATE_CALLS_INTERFACE) != 0)
    return;
  calls = g_variant_lookup_value (changed, "Calls", G_VARIANT_TYPE ("aa{sv}"));
  if (calls)
    take_calls (data, calls);
}

static void
on_got (GObject *source, GAsyncResult *result, gpointer data)
{
  g_autoptr (GVariant) reply = g_dbus_connection_call_finish (G_DBUS_CONNECTION (source), result, NULL);
  g_autoptr (GVariant) value = NULL;

  if (!reply)
    return;
  g_variant_get (reply, "(v)", &value);
  take_calls (data, value);
}

/* the service started (or is there at the start): its calls now */
static void
on_appeared (GDBusConnection *bus, const char *name, const char *owner, gpointer data)
{
  DictateCallWatch *self = data;

  g_dbus_connection_call (bus, DICTATE_CALLS_NAME, DICTATE_CALLS_PATH, "org.freedesktop.DBus.Properties", "Get",
                          g_variant_new ("(ss)", DICTATE_CALLS_INTERFACE, "Calls"), G_VARIANT_TYPE ("(v)"),
                          G_DBUS_CALL_FLAGS_NO_AUTO_START, 5000, self->cancellable, on_got, self);
}

static void
on_vanished (GDBusConnection *bus, const char *name, gpointer data)
{
  take_calls (data, NULL);
}

static void
on_bus (GObject *source, GAsyncResult *result, gpointer data)
{
  g_autoptr (DictateCallWatch) self = data;
  g_autoptr (GError) error = NULL;
  GTask *task = G_TASK (result);
  GDBusConnection *bus = g_task_propagate_pointer (task, &error);

  if (!bus)
    {
      if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        g_message ("calls: no bus for the call service: %s", error->message);
      return;
    }
  self->bus = bus;
  self->subscription = g_dbus_connection_signal_subscribe (bus, DICTATE_CALLS_NAME,
                                                           "org.freedesktop.DBus.Properties", "PropertiesChanged",
                                                           DICTATE_CALLS_PATH, NULL, G_DBUS_SIGNAL_FLAGS_NONE,
                                                           on_properties, self, NULL);
  self->name_watch = g_bus_watch_name_on_connection (bus, DICTATE_CALLS_NAME, G_BUS_NAME_WATCHER_FLAGS_NONE,
                                                     on_appeared, on_vanished, self, NULL);
}

static void
bus_thread (GTask *task, gpointer source, gpointer data, GCancellable *cancellable)
{
  GError *error = NULL;
  GDBusConnection *bus = dictate_calls_bus (cancellable, &error);

  if (bus)
    g_task_return_pointer (task, bus, g_object_unref);
  else
    g_task_return_error (task, error);
}

GPtrArray *
dictate_call_watch_calls (DictateCallWatch *self)
{
  return self->calls;
}

static void
dictate_call_watch_dispose (GObject *object)
{
  DictateCallWatch *self = DICTATE_CALL_WATCH (object);

  g_cancellable_cancel (self->cancellable);
  if (self->subscription)
    g_dbus_connection_signal_unsubscribe (self->bus, self->subscription);
  self->subscription = 0;
  g_clear_handle_id (&self->name_watch, g_bus_unwatch_name);
  g_clear_object (&self->bus);
  g_clear_object (&self->cancellable);
  g_clear_pointer (&self->calls, g_ptr_array_unref);
  G_OBJECT_CLASS (dictate_call_watch_parent_class)->dispose (object);
}

static void
dictate_call_watch_class_init (DictateCallWatchClass *klass)
{
  G_OBJECT_CLASS (klass)->dispose = dictate_call_watch_dispose;
  signals[SIGNAL_RINGING] = g_signal_new ("ringing", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL,
                                          NULL, G_TYPE_NONE, 1, G_TYPE_UINT);
  signals[SIGNAL_STOPPED] = g_signal_new ("stopped", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL,
                                          NULL, G_TYPE_NONE, 1, G_TYPE_UINT);
  signals[SIGNAL_CHANGED] = g_signal_new ("changed", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL,
                                          NULL, G_TYPE_NONE, 0);
}

static void
dictate_call_watch_init (DictateCallWatch *self)
{
  self->cancellable = g_cancellable_new ();
  self->calls = g_ptr_array_new_with_free_func ((GDestroyNotify) dictate_call_free);
}

DictateCallWatch *
dictate_call_watch_new (void)
{
  DictateCallWatch *self = g_object_new (DICTATE_TYPE_CALL_WATCH, NULL);
  /* a bus at an address (the tests') is opened on a thread: it may take a moment */
  g_autoptr (GTask) task = g_task_new (NULL, self->cancellable, on_bus, g_object_ref (self));

  g_task_run_in_thread (task, bus_thread);
  return self;
}
