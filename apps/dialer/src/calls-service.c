/* neo-dialer-calls: the Phone's service, for everything about calls that must go on with the app closed. It runs in the
 * session from login (neo-dialer-calls.service), so that a call that comes rings and shows its screen whether or not
 * the Phone app is open.
 *
 * This file is its face on D-Bus (neo.Dialer.Calls): the apps ask it to dial, answer and hang up, and read
 * the calls and the call log from it. What it does about calls is in calls.c.
 *
 * It also stands aside while gsm2sip is in gateway mode: then gsm2sipd answers the modem's calls and bridges them
 * to SIP, and a second answerer on the same modem would ring and pick up the same call. */
#include "call-log.h"
#include "calls.h"

#include <glib/gi18n.h>
#include <glib-unix.h>
#include <locale.h>

#define GATEWAY_NAME "com.callagent.Gsm2sipd"
#define GATEWAY_PATH "/com/callagent/Gsm2sipd"
#define GATEWAY_INTERFACE "com.callagent.Gsm2sipd1"
#define CALL_LOG_LIMIT 500      /* the most calls the Recents and the call history read */

static const char introspection[] =
  "<node>"
  "  <interface name='" CALLS_INTERFACE "'>"
  /* the calls, oldest first: see calls.h, calls_get_calls, for each one's keys */
  "    <property name='Calls' type='aa{sv}' access='read'/>"
  "    <property name='Muted' type='b' access='read'/>"
  "    <property name='Speaker' type='b' access='read'/>"
  /* ready, no-modem, emergency-only, gateway */
  "    <property name='Status' type='s' access='read'/>"
  /* what the modem can do besides calling: hold, dtmf */
  "    <property name='Features' type='as' access='read'/>"
  /* the network's country (ISO 3166), for formatting numbers */
  "    <property name='Country' type='s' access='read'/>"
  /* the missed calls not yet seen: the Recents tab's badge */
  "    <property name='MissedCount' type='u' access='read'/>"
  "    <method name='Dial'><arg name='number' type='s' direction='in'/>"
  "      <arg name='id' type='u' direction='out'/></method>"
  "    <method name='Answer'><arg name='id' type='u' direction='in'/></method>"
  "    <method name='AnswerAndRelease'><arg name='id' type='u' direction='in'/></method>"
  "    <method name='Decline'><arg name='id' type='u' direction='in'/></method>"
  "    <method name='Hangup'><arg name='id' type='u' direction='in'/></method>"
  "    <method name='SetHeld'><arg name='id' type='u' direction='in'/>"
  "      <arg name='held' type='b' direction='in'/></method>"
  "    <method name='SendDtmf'><arg name='id' type='u' direction='in'/>"
  "      <arg name='tones' type='s' direction='in'/></method>"
  /* "Send the following tones?" answered: the rest of the number's tones, or not */
  "    <method name='ContinuePostDial'><arg name='id' type='u' direction='in'/>"
  "      <arg name='proceed' type='b' direction='in'/></method>"
  "    <method name='SetMuted'><arg name='muted' type='b' direction='in'/></method>"
  "    <method name='SetSpeaker'><arg name='speaker' type='b' direction='in'/></method>"
  "    <method name='SilenceRinger'/>"
  /* Settings' ringtone picker: a ringtone (its URI, or 'default') played once; '' stops it */
  "    <method name='PreviewRingtone'><arg name='uri' type='s' direction='in'/></method>"
  /* the main dialpad's keypad tone while a key is held */
  "    <method name='PlayTone'><arg name='key' type='s' direction='in'/></method>"
  "    <method name='StopTone'/>"
  /* the main dialpad came up, or went: the tones' stream opens ahead of the first key, and closes */
  "    <method name='PrepareTones'/>"
  "    <method name='ReleaseTones'/>"
  /* the in-call screen came to the front, or left it; an app that quits or crashes in front counts as gone */
  "    <method name='SetInCallVisible'><arg name='visible' type='b' direction='in'/></method>"
  /* the newest calls first, each with CallLog.Calls' column names as keys (call-log.h) */
  "    <method name='GetCallLog'><arg name='entries' type='aa{sv}' direction='out'/></method>"
  "    <method name='DeleteCallLog'><arg name='ids' type='ax' direction='in'/></method>"
  "    <method name='ClearCallLog'/>"
  /* the user looked at the missed calls: they are seen, their notifications go */
  "    <method name='MarkMissedRead'/>"
  /* these calls are seen: a missed call's row opened in Recents */
  "    <method name='MarkCallsRead'><arg name='ids' type='ax' direction='in'/></method>"
  "    <signal name='CallLogChanged'/>"
  "  </interface>"
  "</node>";

typedef struct {
  GMainLoop *loop;
  GDBusConnection *session, *system;
  GSettings *settings;
  CallLog *log;
  Calls *calls;
  guint registration;
  GDBusProxy *gateway;
  char *visible_sender;         /* the in-call screen in front, and its name watch */
  guint visible_watch;
} Service;

/* --- telling the apps */

static GVariant *
get_property (GDBusConnection *bus, const char *sender, const char *path, const char *interface,
              const char *property, GError **error, gpointer data)
{
  Service *self = data;

  if (g_str_equal (property, "Calls"))
    return calls_get_calls (self->calls);
  if (g_str_equal (property, "Muted"))
    return g_variant_new_boolean (calls_get_muted (self->calls));
  if (g_str_equal (property, "Speaker"))
    return g_variant_new_boolean (calls_get_speaker (self->calls));
  if (g_str_equal (property, "Status"))
    return g_variant_new_string (calls_get_status (self->calls));
  if (g_str_equal (property, "Features"))
    {
      g_auto(GStrv) features = calls_get_features (self->calls);

      return g_variant_new_strv ((const char * const *) features, -1);
    }
  if (g_str_equal (property, "Country"))
    return g_variant_new_string (calls_get_country (self->calls));
  if (g_str_equal (property, "MissedCount"))
    return g_variant_new_uint32 (call_log_count_new_missed (self->log));
  return NULL;
}

/* every property at once: they are few and small, and an app reads them together anyway */
static void
emit_properties (Service *self)
{
  static const char * const names[] = { "Calls", "Muted", "Speaker", "Status", "Features", "Country",
                                        "MissedCount", NULL };
  GVariantBuilder changed;

  if (!self->registration)
    return;
  g_variant_builder_init (&changed, G_VARIANT_TYPE_VARDICT);
  for (guint i = 0; names[i]; i++)
    g_variant_builder_add (&changed, "{sv}", names[i],
                           get_property (self->session, NULL, CALLS_PATH, CALLS_INTERFACE, names[i], NULL, self));
  g_dbus_connection_emit_signal (self->session, NULL, CALLS_PATH, "org.freedesktop.DBus.Properties",
                                 "PropertiesChanged",
                                 g_variant_new ("(sa{sv}as)", CALLS_INTERFACE, &changed, NULL), NULL);
}

static void
on_calls_changed (gpointer data)
{
  emit_properties (data);
}

static void
on_log_changed (gpointer data)
{
  Service *self = data;

  if (!self->registration)
    return;
  g_dbus_connection_emit_signal (self->session, NULL, CALLS_PATH, CALLS_INTERFACE, "CallLogChanged", NULL, NULL);
  emit_properties (self);
}

/* --- the in-call screen in front */

static void
set_visible_sender (Service *self, const char *sender);

static void
on_visible_vanished (GDBusConnection *bus, const char *name, gpointer data)
{
  set_visible_sender (data, NULL);
}

static void
set_visible_sender (Service *self, const char *sender)
{
  if (self->visible_watch)
    g_bus_unwatch_name (self->visible_watch);
  self->visible_watch = 0;
  g_clear_pointer (&self->visible_sender, g_free);
  if (sender)
    {
      self->visible_sender = g_strdup (sender);
      self->visible_watch = g_bus_watch_name_on_connection (self->session, sender, G_BUS_NAME_WATCHER_FLAGS_NONE,
                                                            NULL, on_visible_vanished, self, NULL);
    }
  calls_set_incall_visible (self->calls, sender != NULL);
}

/* --- the apps' asks */

static void
on_method_call (GDBusConnection *bus, const char *sender, const char *path, const char *interface,
                const char *method, GVariant *parameters, GDBusMethodInvocation *invocation, gpointer data)
{
  Service *self = data;
  g_autoptr(GError) error = NULL;
  gboolean done = TRUE;
  guint id = 0;
  gboolean flag;

  if (g_str_equal (method, "Dial"))
    {
      const char *number;

      g_variant_get (parameters, "(&s)", &number);
      id = calls_dial (self->calls, number, &error);
      if (id)
        {
          g_dbus_method_invocation_return_value (invocation, g_variant_new ("(u)", id));
          return;
        }
      done = FALSE;
    }
  else if (g_str_equal (method, "Answer"))
    {
      g_variant_get (parameters, "(u)", &id);
      done = calls_answer (self->calls, id, &error);
    }
  else if (g_str_equal (method, "AnswerAndRelease"))
    {
      g_variant_get (parameters, "(u)", &id);
      done = calls_answer_and_release (self->calls, id, &error);
    }
  else if (g_str_equal (method, "Decline"))
    {
      g_variant_get (parameters, "(u)", &id);
      done = calls_decline (self->calls, id, &error);
    }
  else if (g_str_equal (method, "Hangup"))
    {
      g_variant_get (parameters, "(u)", &id);
      done = calls_hangup (self->calls, id, &error);
    }
  else if (g_str_equal (method, "SetHeld"))
    {
      g_variant_get (parameters, "(ub)", &id, &flag);
      done = calls_set_held (self->calls, id, flag, &error);
    }
  else if (g_str_equal (method, "SendDtmf"))
    {
      const char *tones;

      g_variant_get (parameters, "(u&s)", &id, &tones);
      done = calls_send_dtmf (self->calls, id, tones, &error);
    }
  else if (g_str_equal (method, "ContinuePostDial"))
    {
      g_variant_get (parameters, "(ub)", &id, &flag);
      done = calls_continue_post_dial (self->calls, id, flag, &error);
    }
  else if (g_str_equal (method, "SetMuted"))
    {
      g_variant_get (parameters, "(b)", &flag);
      calls_set_muted (self->calls, flag);
    }
  else if (g_str_equal (method, "SetSpeaker"))
    {
      g_variant_get (parameters, "(b)", &flag);
      calls_set_speaker (self->calls, flag);
    }
  else if (g_str_equal (method, "SilenceRinger"))
    calls_silence_ringer (self->calls);
  else if (g_str_equal (method, "PreviewRingtone"))
    {
      const char *uri;

      g_variant_get (parameters, "(&s)", &uri);
      calls_preview_ringtone (self->calls, *uri ? uri : NULL);
    }
  else if (g_str_equal (method, "PlayTone"))
    {
      const char *key;

      g_variant_get (parameters, "(&s)", &key);
      calls_play_tone (self->calls, key[0]);
    }
  else if (g_str_equal (method, "StopTone"))
    calls_stop_tone (self->calls);
  else if (g_str_equal (method, "PrepareTones"))
    calls_prepare_tones (self->calls);
  else if (g_str_equal (method, "ReleaseTones"))
    calls_release_tones (self->calls);
  else if (g_str_equal (method, "SetInCallVisible"))
    {
      g_variant_get (parameters, "(b)", &flag);
      if (flag)
        set_visible_sender (self, sender);
      else if (g_strcmp0 (sender, self->visible_sender) == 0)
        set_visible_sender (self, NULL);
    }
  else if (g_str_equal (method, "GetCallLog"))
    {
      g_autoptr(GPtrArray) entries = call_log_list (self->log, CALL_LOG_LIMIT);
      GVariantBuilder out;

      g_variant_builder_init (&out, G_VARIANT_TYPE ("aa{sv}"));
      for (guint i = 0; i < entries->len; i++)
        g_variant_builder_add_value (&out, call_log_entry_to_variant (entries->pdata[i]));
      g_dbus_method_invocation_return_value (invocation, g_variant_new ("(aa{sv})", &out));
      return;
    }
  else if (g_str_equal (method, "DeleteCallLog"))
    {
      g_autoptr(GVariant) ids = g_variant_get_child_value (parameters, 0);
      gsize n;
      const gint64 *values = g_variant_get_fixed_array (ids, &n, sizeof (gint64));

      if (call_log_delete (self->log, values, n))
        on_log_changed (self);
    }
  else if (g_str_equal (method, "ClearCallLog"))
    {
      call_log_clear (self->log);
      calls_missed_seen (self->calls);
      on_log_changed (self);
    }
  else if (g_str_equal (method, "MarkMissedRead"))
    calls_missed_seen (self->calls);
  else if (g_str_equal (method, "MarkCallsRead"))
    {
      g_autoptr(GVariant) ids = g_variant_get_child_value (parameters, 0);
      gsize n;
      const gint64 *values = g_variant_get_fixed_array (ids, &n, sizeof (gint64));

      calls_calls_seen (self->calls, values, n);
    }

  if (done)
    g_dbus_method_invocation_return_value (invocation, NULL);
  else
    g_dbus_method_invocation_return_gerror (invocation, error);
}

static const GDBusInterfaceVTable vtable = { .method_call = on_method_call, .get_property = get_property };

/* --- gsm2sip's gateway mode */

static void
sync_gateway (Service *self)
{
  g_autofree char *owner = self->gateway ? g_dbus_proxy_get_name_owner (self->gateway) : NULL;
  g_autoptr(GVariant) enabled = owner ? g_dbus_proxy_get_cached_property (self->gateway, "Enabled") : NULL;

  calls_set_gateway (self->calls, enabled && g_variant_get_boolean (enabled));
}

/* Two handlers, because the signals differ: g-properties-changed passes (changed, invalidated), notify:: passes
 * (pspec). One handler for both read the user data from the wrong argument when the gateway came or went. */
static void
on_gateway_properties (GDBusProxy *proxy, GVariant *changed, GStrv invalidated, gpointer data)
{
  sync_gateway (data);
}

static void
on_gateway_owner (GObject *proxy, GParamSpec *pspec, gpointer data)
{
  sync_gateway (data);
}

static void
on_gateway_proxy (GObject *source, GAsyncResult *result, gpointer data)
{
  Service *self = data;
  g_autoptr(GError) error = NULL;

  self->gateway = g_dbus_proxy_new_finish (result, &error);
  if (!self->gateway)
    return;
  g_signal_connect (self->gateway, "g-properties-changed", G_CALLBACK (on_gateway_properties), self);
  g_signal_connect (self->gateway, "notify::g-name-owner", G_CALLBACK (on_gateway_owner), self);
  sync_gateway (self);
}

/* --- the service */

static void
on_name_acquired (GDBusConnection *bus, const char *name, gpointer data)
{
  g_message ("serving %s", name);
}

static void
on_name_lost (GDBusConnection *bus, const char *name, gpointer data)
{
  Service *self = data;

  /* another one has it: two services would ring and answer the same call */
  g_warning ("lost the bus name %s", name);
  g_main_loop_quit (self->loop);
}

static gboolean
on_signal (gpointer data)
{
  g_main_loop_quit (((Service *) data)->loop);
  return G_SOURCE_REMOVE;
}

/* the system bus, or for the tests a bus of their own with a ModemManager of their own (tests/mock-modem.py) */
static GDBusConnection *
system_bus (GError **error)
{
  const char *address = g_getenv ("NEO_DIALER_SYSTEM_BUS");

  if (address && *address)
    return g_dbus_connection_new_for_address_sync (address, G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT
                                                            | G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION,
                                                   NULL, NULL, error);
  return g_bus_get_sync (G_BUS_TYPE_SYSTEM, NULL, error);
}

int
main (int argc, char **argv)
{
  static const CallsCallbacks callbacks = { .changed = on_calls_changed, .log_changed = on_log_changed };
  Service service = { 0 };
  g_autoptr(GError) error = NULL;
  g_autoptr(GDBusNodeInfo) node = NULL;
  g_autofree char *log_path = g_build_filename (g_get_user_data_dir (), "neo-dialer", "calls.db", NULL);
  g_autofree char *gnome_calls = g_build_filename (g_get_user_data_dir (), "calls", "records.db", NULL);
  guint owner;

  setlocale (LC_ALL, "");
  bindtextdomain (GETTEXT_PACKAGE, LOCALEDIR);
  bind_textdomain_codeset (GETTEXT_PACKAGE, "UTF-8");
  textdomain (GETTEXT_PACKAGE);

  service.loop = g_main_loop_new (NULL, FALSE);
  service.session = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, &error);
  if (!service.session)
    g_error ("no session bus: %s", error->message);
  service.system = system_bus (&error);
  if (!service.system)
    g_error ("no system bus: %s", error->message);
  service.settings = g_settings_new (DIALER_APP_ID);
  service.log = call_log_open (log_path, gnome_calls, &error);
  if (!service.log)
    g_error ("%s", error->message);
  service.calls = calls_new (service.session, service.system, service.settings, service.log, &callbacks, &service);

  node = g_dbus_node_info_new_for_xml (introspection, NULL);
  service.registration = g_dbus_connection_register_object (service.session, CALLS_PATH, node->interfaces[0],
                                                            &vtable, &service, NULL, &error);
  if (!service.registration)
    g_error ("%s", error->message);
  owner = g_bus_own_name_on_connection (service.session, CALLS_BUS_NAME, G_BUS_NAME_OWNER_FLAGS_NONE,
                                        on_name_acquired, on_name_lost, &service, NULL);
  g_dbus_proxy_new (service.system, G_DBUS_PROXY_FLAGS_DO_NOT_AUTO_START, NULL, GATEWAY_NAME, GATEWAY_PATH,
                    GATEWAY_INTERFACE, NULL, on_gateway_proxy, &service);

  g_unix_signal_add (SIGTERM, on_signal, &service);
  g_unix_signal_add (SIGINT, on_signal, &service);
  g_main_loop_run (service.loop);

  g_bus_unown_name (owner);
  g_dbus_connection_unregister_object (service.session, service.registration);
  service.registration = 0;
  if (service.visible_watch)
    g_bus_unwatch_name (service.visible_watch);
  g_free (service.visible_sender);
  if (service.gateway)
    g_signal_handlers_disconnect_by_data (service.gateway, &service);
  g_clear_object (&service.gateway);
  calls_free (service.calls);
  call_log_free (service.log);
  g_object_unref (service.settings);
  g_object_unref (service.system);
  g_object_unref (service.session);
  g_main_loop_unref (service.loop);
  return 0;
}
